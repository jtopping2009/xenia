/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/kernel_state.h"

#include <array>
#include <chrono>
#include <string>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xmodule.h"
#include "xenia/kernel/xnotifylistener.h"
#include "xenia/kernel/xobject.h"
#include "xenia/kernel/xthread.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif

namespace xe {
namespace kernel {

constexpr uint32_t kDeferredOverlappedDelayMillis = 100;

// This is a global object initialized with the XboxkrnlModule.
// It references the current kernel state object that all kernel methods should
// be using to stash their variables.
KernelState* shared_kernel_state_ = nullptr;

KernelState* kernel_state() { return shared_kernel_state_; }

KernelState::KernelState(Emulator* emulator)
    : emulator_(emulator),
      memory_(emulator->memory()),
      dispatch_thread_running_(false),
      dpc_list_(emulator->memory()) {
  processor_ = emulator->processor();
  file_system_ = emulator->file_system();

  app_manager_ = std::make_unique<xam::AppManager>();
  user_profile_ = std::make_unique<xam::UserProfile>();

  auto content_root = emulator_->content_root();
  if (!content_root.empty()) {
    content_root = std::filesystem::absolute(content_root);
  }
  content_manager_ = std::make_unique<xam::ContentManager>(this, content_root);

  assert_null(shared_kernel_state_);
  shared_kernel_state_ = this;

  // Hardcoded maximum of 2048 TLS slots.
  tls_bitmap_.Resize(2048);

  xam::AppManager::RegisterApps(this, app_manager_.get());
}

KernelState::~KernelState() {
  SetExecutableModule(nullptr);

  if (dispatch_thread_running_) {
    dispatch_thread_running_ = false;
    dispatch_cond_.notify_all();
    dispatch_thread_->Wait(0, 0, 0, nullptr);
  }

  executable_module_.reset();
  user_modules_.clear();
  kernel_modules_.clear();

  // Delete all objects.
  object_table_.Reset();

  // Shutdown apps.
  app_manager_.reset();

  assert_true(shared_kernel_state_ == this);
  shared_kernel_state_ = nullptr;
}

KernelState* KernelState::shared() { return shared_kernel_state_; }

uint32_t KernelState::title_id() const {
  assert_not_null(executable_module_);

  xex2_opt_execution_info* exec_info = 0;
  executable_module_->GetOptHeader(XEX_HEADER_EXECUTION_INFO, &exec_info);

  if (exec_info) {
    return exec_info->title_id;
  }

  return 0;
}

util::XdbfGameData KernelState::title_xdbf() const {
  return module_xdbf(executable_module_);
}

util::XdbfGameData KernelState::module_xdbf(
    object_ref<UserModule> exec_module) const {
  assert_not_null(exec_module);

  uint32_t resource_data = 0;
  uint32_t resource_size = 0;
  if (XSUCCEEDED(exec_module->GetSection(
          fmt::format("{:08X}", exec_module->title_id()).c_str(),
          &resource_data, &resource_size))) {
    util::XdbfGameData db(memory()->TranslateVirtual(resource_data),
                          resource_size);
    return db;
  }
  return util::XdbfGameData(nullptr, resource_size);
}

uint32_t KernelState::process_type() const {
  auto pib =
      memory_->TranslateVirtual<ProcessInfoBlock*>(process_info_block_address_);
  return pib->process_type;
}

void KernelState::set_process_type(uint32_t value) {
  auto pib =
      memory_->TranslateVirtual<ProcessInfoBlock*>(process_info_block_address_);
  pib->process_type = uint8_t(value);
}

uint32_t KernelState::AllocateTLS() { return uint32_t(tls_bitmap_.Acquire()); }

void KernelState::FreeTLS(uint32_t slot) {
  const std::vector<object_ref<XThread>> threads =
      object_table()->GetObjectsByType<XThread>();

  for (const object_ref<XThread>& thread : threads) {
    if (thread->is_guest_thread()) {
      thread->SetTLSValue(slot, 0);
    }
  }
  tls_bitmap_.Release(slot);
}

void KernelState::RegisterTitleTerminateNotification(uint32_t routine,
                                                     uint32_t priority) {
  TerminateNotification notify;
  notify.guest_routine = routine;
  notify.priority = priority;

  terminate_notifications_.push_back(notify);
}

void KernelState::RemoveTitleTerminateNotification(uint32_t routine) {
  for (auto it = terminate_notifications_.begin();
       it != terminate_notifications_.end(); it++) {
    if (it->guest_routine == routine) {
      terminate_notifications_.erase(it);
      break;
    }
  }
}

void KernelState::RegisterModule(XModule* module) {}

void KernelState::UnregisterModule(XModule* module) {}

bool KernelState::RegisterUserModule(object_ref<UserModule> module) {
  auto lock = global_critical_region_.Acquire();

  for (auto user_module : user_modules_) {
    if (user_module->path() == module->path()) {
      // Already loaded.
      return false;
    }
  }

  user_modules_.push_back(module);
  return true;
}

void KernelState::UnregisterUserModule(UserModule* module) {
  auto lock = global_critical_region_.Acquire();

  for (auto it = user_modules_.begin(); it != user_modules_.end(); it++) {
    if ((*it)->path() == module->path()) {
      user_modules_.erase(it);
      return;
    }
  }
}

bool KernelState::IsKernelModule(const std::string_view name) {
  if (name.empty()) {
    // Executing module isn't a kernel module.
    return false;
  }
  // NOTE: no global lock required as the kernel module list is static.
  for (auto kernel_module : kernel_modules_) {
    if (kernel_module->Matches(name)) {
      return true;
    }
  }
  return false;
}

object_ref<KernelModule> KernelState::GetKernelModule(
    const std::string_view name) {
  assert_true(IsKernelModule(name));

  for (auto kernel_module : kernel_modules_) {
    if (kernel_module->Matches(name)) {
      return retain_object(kernel_module.get());
    }
  }

  return nullptr;
}

object_ref<XModule> KernelState::GetModule(const std::string_view name,
                                           bool user_only) {
  if (name.empty()) {
    // NULL name = self.
    // TODO(benvanik): lookup module from caller address.
    return GetExecutableModule();
  } else if (xe::utf8::equal_case(name, "kernel32.dll")) {
    // Some games request this, for some reason. wtf.
    return nullptr;
  }

  auto global_lock = global_critical_region_.Acquire();

  if (!user_only) {
    for (auto kernel_module : kernel_modules_) {
      if (kernel_module->Matches(name)) {
        return retain_object(kernel_module.get());
      }
    }
  }

  auto path(name);

  // Resolve the path to an absolute path.
  auto entry = file_system_->ResolvePath(name);
  if (entry) {
    path = entry->absolute_path();
  }

  for (auto user_module : user_modules_) {
    if (user_module->Matches(path)) {
      return retain_object(user_module.get());
    }
  }
  return nullptr;
}

object_ref<XThread> KernelState::LaunchModule(object_ref<UserModule> module) {
  if (!module->is_executable()) {
    return nullptr;
  }

  SetExecutableModule(module);
  XELOGI("KernelState: Launching module...");

  // Create a thread to run in.
  // We start suspended so we can run the debugger prep.
  auto thread = object_ref<XThread>(
      new XThread(kernel_state(), module->stack_size(), 0,
                  module->entry_point(), 0, X_CREATE_SUSPENDED, true, true));

  // We know this is the 'main thread'.
  thread->set_name("Main XThread");

  X_STATUS result = thread->Create();
  if (XFAILED(result)) {
    XELOGE("Could not create launch thread: {:08X}", result);
    return nullptr;
  }

  // Waits for a debugger client, if desired.
  emulator()->processor()->PreLaunch();

  // Resume the thread now.
  // If the debugger has requested a suspend this will just decrement the
  // suspend count without resuming it until the debugger wants.
  thread->Resume();

  return thread;
}

object_ref<UserModule> KernelState::GetExecutableModule() {
  if (!executable_module_) {
    return nullptr;
  }
  return executable_module_;
}

void KernelState::SetExecutableModule(object_ref<UserModule> module) {
  if (module.get() == executable_module_.get()) {
    return;
  }
  executable_module_ = std::move(module);
  if (!executable_module_) {
    return;
  }

  assert_zero(process_info_block_address_);
  process_info_block_address_ = memory_->SystemHeapAlloc(0x60);

  auto pib =
      memory_->TranslateVirtual<ProcessInfoBlock*>(process_info_block_address_);
  // TODO(benvanik): figure out what this list is.
  pib->unk_04 = pib->unk_08 = 0;
  pib->unk_0C = 0x0000007F;
  pib->unk_10 = 0x001F0000;
  pib->thread_count = 0;
  pib->unk_1B = 0x06;
  pib->kernel_stack_size = 16 * 1024;
  pib->process_type = process_type_;
  // TODO(benvanik): figure out what this list is.
  pib->unk_54 = pib->unk_58 = 0;

  xex2_opt_tls_info* tls_header = nullptr;
  executable_module_->GetOptHeader(XEX_HEADER_TLS_INFO, &tls_header);
  if (tls_header) {
    auto pib = memory_->TranslateVirtual<ProcessInfoBlock*>(
        process_info_block_address_);
    pib->tls_data_size = tls_header->data_size;
    pib->tls_raw_data_size = tls_header->raw_data_size;
    pib->tls_slot_size = tls_header->slot_count * 4;
  }

  // Setup the kernel's XexExecutableModuleHandle field.
  auto export_entry = processor()->export_resolver()->GetExportByOrdinal(
      "xboxkrnl.exe", ordinals::XexExecutableModuleHandle);
  if (export_entry) {
    assert_not_zero(export_entry->variable_ptr);
    auto variable_ptr = memory()->TranslateVirtual<xe::be<uint32_t>*>(
        export_entry->variable_ptr);
    *variable_ptr = executable_module_->hmodule_ptr();
  }

  // Setup the kernel's ExLoadedImageName field
  export_entry = processor()->export_resolver()->GetExportByOrdinal(
      "xboxkrnl.exe", ordinals::ExLoadedImageName);

  if (export_entry) {
    char* variable_ptr =
        memory()->TranslateVirtual<char*>(export_entry->variable_ptr);
    xe::string_util::copy_truncating(
        variable_ptr, executable_module_->path(),
        xboxkrnl::XboxkrnlModule::kExLoadedImageNameSize);
  }
  // Spin up deferred dispatch worker.
  // TODO(benvanik): move someplace more appropriate (out of ctor, but around
  // here).
  if (!dispatch_thread_running_) {
    dispatch_thread_running_ = true;
    dispatch_thread_ =
        object_ref<XHostThread>(new XHostThread(this, 128 * 1024, 0, [this]() {
          // As we run guest callbacks the debugger must be able to suspend us.
          dispatch_thread_->set_can_debugger_suspend(true);

          auto global_lock = global_critical_region_.AcquireDeferred();
          while (dispatch_thread_running_) {
            global_lock.lock();
            if (dispatch_queue_.empty()) {
              dispatch_cond_.wait(global_lock);
              if (!dispatch_thread_running_) {
                global_lock.unlock();
                break;
              }
            }
            auto fn = std::move(dispatch_queue_.front());
            dispatch_queue_.pop_front();
            global_lock.unlock();

            fn();
          }
          return 0;
        }));
    dispatch_thread_->set_name("Kernel Dispatch");
    dispatch_thread_->Create();
  }
}

void KernelState::LoadKernelModule(object_ref<KernelModule> kernel_module) {
  auto global_lock = global_critical_region_.Acquire();
  kernel_modules_.push_back(std::move(kernel_module));
}

object_ref<UserModule> KernelState::LoadUserModule(
    const std::string_view raw_name, bool call_entry) {
  // Some games try to load relative to launch module, others specify full path.
  auto name = xe::utf8::find_name_from_guest_path(raw_name);
  std::string path(raw_name);
  if (name == raw_name) {
    assert_not_null(executable_module_);
    path = xe::utf8::join_guest_paths(
        xe::utf8::find_base_guest_path(executable_module_->path()), name);
  }

  object_ref<UserModule> module;
  {
    auto global_lock = global_critical_region_.Acquire();

    // See if we've already loaded it
    for (auto& existing_module : user_modules_) {
      if (existing_module->path() == path) {
        return existing_module;
      }
    }

    global_lock.unlock();

    // Module wasn't loaded, so load it.
    module = object_ref<UserModule>(new UserModule(this));
    X_STATUS status = module->LoadFromFile(path);
    if (XFAILED(status)) {
      object_table()->ReleaseHandle(module->handle());
      return nullptr;
    }

    global_lock.lock();

    // Putting into the listing automatically retains.
    user_modules_.push_back(module);
  }

  module->Dump();

  if (module->is_dll_module() && module->entry_point() && call_entry) {
    // Call DllMain(DLL_PROCESS_ATTACH):
    // https://msdn.microsoft.com/en-us/library/windows/desktop/ms682583%28v=vs.85%29.aspx
    uint64_t args[] = {
        module->handle(),
        1,  // DLL_PROCESS_ATTACH
        0,  // 0 because always dynamic
    };
    auto thread_state = XThread::GetCurrentThread()->thread_state();
    processor()->Execute(thread_state, module->entry_point(), args,
                         xe::countof(args));
  }

  return module;
}

void KernelState::UnloadUserModule(const object_ref<UserModule>& module,
                                   bool call_entry) {
  auto global_lock = global_critical_region_.Acquire();

  if (module->is_dll_module() && module->entry_point() && call_entry) {
    // Call DllMain(DLL_PROCESS_DETACH):
    // https://msdn.microsoft.com/en-us/library/windows/desktop/ms682583%28v=vs.85%29.aspx
    uint64_t args[] = {
        module->handle(),
        0,  // DLL_PROCESS_DETACH
        0,  // 0 for now, assume XexUnloadImage is like FreeLibrary
    };
    auto thread_state = XThread::GetCurrentThread()->thread_state();
    processor()->Execute(thread_state, module->entry_point(), args,
                         xe::countof(args));
  }

  auto iter = std::find_if(
      user_modules_.begin(), user_modules_.end(),
      [&module](const auto& e) { return e->path() == module->path(); });
  assert_true(iter != user_modules_.end());  // Unloading an unregistered module
                                             // is probably really bad
  user_modules_.erase(iter);

  // Ensure this module was not somehow registered twice
  assert_true(std::find_if(user_modules_.begin(), user_modules_.end(),
                           [&module](const auto& e) {
                             return e->path() == module->path();
                           }) == user_modules_.end());

  object_table()->ReleaseHandle(module->handle());
}

#if XE_PLATFORM_WIN32
namespace {
// Whether a suspended thread is somewhere it's safe to kill: executing
// translated guest code (which takes no host locks itself), or parked in an
// ntdll wait syscall (which covers blocking on the global lock we hold, on
// kernel objects, and sleeps).
bool IsThreadAtKillSafePoint(HANDLE thread, uintptr_t code_begin,
                             uintptr_t code_end) {
  CONTEXT context = {};
  context.ContextFlags = CONTEXT_CONTROL;
  if (!GetThreadContext(thread, &context)) {
    return false;
  }
  uintptr_t pc = uintptr_t(context.Rip);
  if (pc >= code_begin && pc < code_end) {
    return true;
  }
  static const std::array<uintptr_t, 7> wait_stubs = [] {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto stub = [ntdll](const char* name) {
      return ntdll ? uintptr_t(GetProcAddress(ntdll, name)) : uintptr_t(0);
    };
    return std::array<uintptr_t, 7>{
        stub("NtWaitForSingleObject"),   stub("NtWaitForMultipleObjects"),
        stub("NtDelayExecution"),        stub("NtWaitForAlertByThreadId"),
        stub("NtRemoveIoCompletion"),    stub("NtSignalAndWaitForSingleObject"),
        stub("NtWaitForKeyedEvent")};
  }();
  // A thread blocked in a syscall sits just past the `syscall` instruction
  // inside its ~0x20-byte stub.
  for (uintptr_t stub : wait_stubs) {
    if (stub && pc >= stub && pc < stub + 0x20) {
      return true;
    }
  }
  return false;
}
}  // namespace
#endif  // XE_PLATFORM_WIN32

bool KernelState::TerminateTitleSafely() {
#if XE_PLATFORM_WIN32
  XELOGI("KernelState::TerminateTitleSafely");
  // While this is held no other thread is inside kernel code that takes it,
  // and any thread that tries to ends up in a wait - a safe point.
  auto global_lock = global_critical_region_.Acquire();

  std::vector<XThread*> threads;
  for (auto& it : threads_by_id_) {
    XThread* thread = it.second;
    if (!XThread::IsInThread(thread) && thread->is_guest_thread() &&
        thread->is_running()) {
      threads.push_back(thread);
    }
  }
  for (XThread* thread : threads) {
    thread->thread()->Suspend();
  }

  auto code_cache = processor()->backend()->code_cache();
  uintptr_t code_begin = code_cache->execute_base_address();
  uintptr_t code_end = code_begin + code_cache->total_size();
  // Let threads caught mid-way through host code run on briefly until they
  // reach a safe point.
  std::vector<XThread*> unsafe = threads;
  for (int attempt = 0; attempt < 400 && !unsafe.empty(); ++attempt) {
    std::vector<XThread*> still_unsafe;
    for (XThread* thread : unsafe) {
      if (!IsThreadAtKillSafePoint(HANDLE(thread->thread()->native_handle()),
                                   code_begin, code_end)) {
        still_unsafe.push_back(thread);
      }
    }
    unsafe = std::move(still_unsafe);
    if (unsafe.empty()) {
      break;
    }
    for (XThread* thread : unsafe) {
      thread->thread()->Resume();
    }
    xe::threading::Sleep(std::chrono::milliseconds(1));
    for (XThread* thread : unsafe) {
      thread->thread()->Suspend();
    }
  }
  if (!unsafe.empty()) {
    for (XThread* thread : unsafe) {
      XELOGE("TerminateTitleSafely: thread {:08X} '{}' never reached a safe "
             "point",
             thread->thread_id(), thread->name());
    }
    for (XThread* thread : threads) {
      thread->thread()->Resume();
    }
    return false;
  }

  for (XThread* thread : threads) {
    XELOGI("TerminateTitleSafely: terminating thread {:08X} '{}'",
           thread->thread_id(), thread->name());
    thread->Terminate(0);
    threads_by_id_.erase(thread->thread_id());
  }

  // The rest as in TerminateTitle.
  for (size_t i = 0; i < user_modules_.size(); i++) {
    X_STATUS status = user_modules_[i]->Unload();
    assert_true(XSUCCEEDED(status));
    object_table_.RemoveHandle(user_modules_[i]->handle());
  }
  user_modules_.clear();
  object_table_.PurgeAllObjects();
  notify_listeners_.clear();
  tls_bitmap_.Reset();
  executable_module_ = nullptr;
  if (process_info_block_address_) {
    memory_->SystemHeapFree(process_info_block_address_);
    process_info_block_address_ = 0;
  }
  app_manager_->OnTitleTerminated();
  XELOGI("TerminateTitleSafely: done");
  return true;
#else
  return false;
#endif  // XE_PLATFORM_WIN32
}

void KernelState::TerminateTitle() {
  XELOGD("KernelState::TerminateTitle");
  auto global_lock = global_critical_region_.Acquire();

  // Call terminate routines.
  // TODO(benvanik): these might take arguments.
  // FIXME: Calling these will send some threads into kernel code and they'll
  // hold the lock when terminated! Do we need to wait for all threads to exit?
  /*
  if (from_guest_thread) {
    for (auto routine : terminate_notifications_) {
      auto thread_state = XThread::GetCurrentThread()->thread_state();
      processor()->Execute(thread_state, routine.guest_routine);
    }
  }
  terminate_notifications_.clear();
  */

  // Kill all guest threads.
  for (auto it = threads_by_id_.begin(); it != threads_by_id_.end();) {
    if (!XThread::IsInThread(it->second) && it->second->is_guest_thread()) {
      auto thread = it->second;

      if (thread->is_running()) {
        // Need to step the thread to a safe point (returns it to guest code
        // so it's guaranteed to not be holding any locks / in host kernel
        // code / etc). Can't do that properly if we have the lock.
        if (!emulator_->is_paused()) {
          thread->thread()->Suspend();
        }

        global_lock.unlock();
        processor_->StepToGuestSafePoint(thread->thread_id());
        thread->Terminate(0);
        global_lock.lock();
      }

      // Erase it from the thread list.
      it = threads_by_id_.erase(it);
    } else {
      ++it;
    }
  }

  // Third: Unload all user modules (including the executable).
  for (size_t i = 0; i < user_modules_.size(); i++) {
    X_STATUS status = user_modules_[i]->Unload();
    assert_true(XSUCCEEDED(status));

    object_table_.RemoveHandle(user_modules_[i]->handle());
  }
  user_modules_.clear();

  // Release all objects in the object table.
  object_table_.PurgeAllObjects();

  // Unregister all notify listeners.
  notify_listeners_.clear();

  // Clear the TLS map.
  tls_bitmap_.Reset();

  // Unset the executable module.
  executable_module_ = nullptr;

  if (process_info_block_address_) {
    memory_->SystemHeapFree(process_info_block_address_);
    process_info_block_address_ = 0;
  }

  if (XThread::IsInThread()) {
    threads_by_id_.erase(XThread::GetCurrentThread()->thread_id());

    // Now commit suicide (using Terminate, because we can't call into guest
    // code anymore).
    global_lock.unlock();
    XThread::GetCurrentThread()->Terminate(0);
  }
}

void KernelState::RegisterThread(XThread* thread) {
  auto global_lock = global_critical_region_.Acquire();
  threads_by_id_[thread->thread_id()] = thread;

  /*
  auto pib =
      memory_->TranslateVirtual<ProcessInfoBlock*>(process_info_block_address_);
  pib->thread_count = pib->thread_count + 1;
  */
}

void KernelState::UnregisterThread(XThread* thread) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = threads_by_id_.find(thread->thread_id());
  if (it != threads_by_id_.end()) {
    threads_by_id_.erase(it);
  }

  /*
  auto pib =
      memory_->TranslateVirtual<ProcessInfoBlock*>(process_info_block_address_);
  pib->thread_count = pib->thread_count - 1;
  */
}

void KernelState::OnThreadExecute(XThread* thread) {
  auto global_lock = global_critical_region_.Acquire();

  // Must be called on executing thread.
  assert_true(XThread::GetCurrentThread() == thread);

  // Call DllMain(DLL_THREAD_ATTACH) for each user module:
  // https://msdn.microsoft.com/en-us/library/windows/desktop/ms682583%28v=vs.85%29.aspx
  auto thread_state = thread->thread_state();
  for (auto user_module : user_modules_) {
    if (user_module->is_dll_module() && user_module->entry_point()) {
      uint64_t args[] = {
          user_module->handle(),
          2,  // DLL_THREAD_ATTACH
          0,  // 0 because always dynamic
      };
      processor()->Execute(thread_state, user_module->entry_point(), args,
                           xe::countof(args));
    }
  }
}

void KernelState::OnThreadExit(XThread* thread) {
  auto global_lock = global_critical_region_.Acquire();

  // Must be called on executing thread.
  assert_true(XThread::GetCurrentThread() == thread);

  // Call DllMain(DLL_THREAD_DETACH) for each user module:
  // https://msdn.microsoft.com/en-us/library/windows/desktop/ms682583%28v=vs.85%29.aspx
  auto thread_state = thread->thread_state();
  for (auto user_module : user_modules_) {
    if (user_module->is_dll_module() && user_module->entry_point()) {
      uint64_t args[] = {
          user_module->handle(),
          3,  // DLL_THREAD_DETACH
          0,  // 0 because always dynamic
      };
      processor()->Execute(thread_state, user_module->entry_point(), args,
                           xe::countof(args));
    }
  }

  emulator()->processor()->OnThreadExit(thread->thread_id());
}

object_ref<XThread> KernelState::GetThreadByID(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  XThread* thread = nullptr;
  auto it = threads_by_id_.find(thread_id);
  if (it != threads_by_id_.end()) {
    thread = it->second;
  }
  return retain_object(thread);
}

void KernelState::RegisterNotifyListener(XNotifyListener* listener) {
  auto global_lock = global_critical_region_.Acquire();
  notify_listeners_.push_back(retain_object(listener));

  // Games seem to expect a few notifications on startup, only for the first
  // listener.
  // https://cs.rin.ru/forum/viewtopic.php?f=38&t=60668&hilit=resident+evil+5&start=375
  if (!has_notified_startup_ && listener->mask() & 0x00000001) {
    has_notified_startup_ = true;
    // XN_SYS_UI (on, off)
    listener->EnqueueNotification(0x00000009, 1);
    listener->EnqueueNotification(0x00000009, 0);
    // XN_SYS_SIGNINCHANGED x2
    listener->EnqueueNotification(0x0000000A, 1);
    listener->EnqueueNotification(0x0000000A, 1);
    // XN_SYS_INPUTDEVICESCHANGED x2
    listener->EnqueueNotification(0x00000012, 0);
    listener->EnqueueNotification(0x00000012, 0);
    // XN_SYS_INPUTDEVICECONFIGCHANGED x2
    listener->EnqueueNotification(0x00000013, 0);
    listener->EnqueueNotification(0x00000013, 0);
  }
}

void KernelState::UnregisterNotifyListener(XNotifyListener* listener) {
  auto global_lock = global_critical_region_.Acquire();
  for (auto it = notify_listeners_.begin(); it != notify_listeners_.end();
       ++it) {
    if ((*it).get() == listener) {
      notify_listeners_.erase(it);
      break;
    }
  }
}

void KernelState::BroadcastNotification(XNotificationID id, uint32_t data) {
  auto global_lock = global_critical_region_.Acquire();
  for (const auto& notify_listener : notify_listeners_) {
    notify_listener->EnqueueNotification(id, data);
  }
}

void KernelState::CompleteOverlapped(uint32_t overlapped_ptr, X_RESULT result) {
  CompleteOverlappedEx(overlapped_ptr, result, result, 0);
}

void KernelState::CompleteOverlappedEx(uint32_t overlapped_ptr, X_RESULT result,
                                       uint32_t extended_error,
                                       uint32_t length) {
  auto ptr = memory()->TranslateVirtual(overlapped_ptr);
  XOverlappedSetResult(ptr, result);
  XOverlappedSetExtendedError(ptr, extended_error);
  XOverlappedSetLength(ptr, length);
  X_HANDLE event_handle = XOverlappedGetEvent(ptr);
  if (event_handle) {
    auto ev = object_table()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    if (ev) {
      ev->Set(0, false);
    }
  }
  if (XOverlappedGetCompletionRoutine(ptr)) {
    X_HANDLE thread_handle = XOverlappedGetContext(ptr);
    auto thread = object_table()->LookupObject<XThread>(thread_handle);
    if (thread) {
      // Queue APC on the thread that requested the overlapped operation.
      uint32_t routine = XOverlappedGetCompletionRoutine(ptr);
      thread->EnqueueApc(routine, result, length, overlapped_ptr);
    }
  }
}

void KernelState::CompleteOverlappedImmediate(uint32_t overlapped_ptr,
                                              X_RESULT result) {
  // TODO(gibbed): there are games that check 'length' of overlapped as
  // an indication of success. WTF?
  // Setting length to -1 when not success seems to be helping.
  uint32_t length = !result ? 0 : 0xFFFFFFFF;
  CompleteOverlappedImmediateEx(overlapped_ptr, result, result, length);
}

void KernelState::CompleteOverlappedImmediateEx(uint32_t overlapped_ptr,
                                                X_RESULT result,
                                                uint32_t extended_error,
                                                uint32_t length) {
  auto ptr = memory()->TranslateVirtual(overlapped_ptr);
  XOverlappedSetContext(ptr, XThread::GetCurrentThreadHandle());
  CompleteOverlappedEx(overlapped_ptr, result, extended_error, length);
}

void KernelState::CompleteOverlappedDeferred(
    std::function<void()> completion_callback, uint32_t overlapped_ptr,
    X_RESULT result, std::function<void()> pre_callback,
    std::function<void()> post_callback) {
  CompleteOverlappedDeferredEx(std::move(completion_callback), overlapped_ptr,
                               result, result, 0, pre_callback, post_callback);
}

void KernelState::CompleteOverlappedDeferredEx(
    std::function<void()> completion_callback, uint32_t overlapped_ptr,
    X_RESULT result, uint32_t extended_error, uint32_t length,
    std::function<void()> pre_callback, std::function<void()> post_callback) {
  CompleteOverlappedDeferredEx(
      [completion_callback, result, extended_error, length](
          uint32_t& cb_extended_error, uint32_t& cb_length) -> X_RESULT {
        completion_callback();
        cb_extended_error = extended_error;
        cb_length = length;
        return result;
      },
      overlapped_ptr, pre_callback, post_callback);
}

void KernelState::CompleteOverlappedDeferred(
    std::function<X_RESULT()> completion_callback, uint32_t overlapped_ptr,
    std::function<void()> pre_callback, std::function<void()> post_callback) {
  CompleteOverlappedDeferredEx(
      [completion_callback](uint32_t& extended_error,
                            uint32_t& length) -> X_RESULT {
        auto result = completion_callback();
        extended_error = static_cast<uint32_t>(result);
        length = 0;
        return result;
      },
      overlapped_ptr, pre_callback, post_callback);
}

void KernelState::CompleteOverlappedDeferredEx(
    std::function<X_RESULT(uint32_t&, uint32_t&)> completion_callback,
    uint32_t overlapped_ptr, std::function<void()> pre_callback,
    std::function<void()> post_callback) {
  auto ptr = memory()->TranslateVirtual(overlapped_ptr);
  XOverlappedSetResult(ptr, X_ERROR_IO_PENDING);
  XOverlappedSetContext(ptr, XThread::GetCurrentThreadHandle());
  auto global_lock = global_critical_region_.Acquire();
  dispatch_queue_.push_back([this, completion_callback, overlapped_ptr,
                             pre_callback, post_callback]() {
    if (pre_callback) {
      pre_callback();
    }
    xe::threading::Sleep(
        std::chrono::milliseconds(kDeferredOverlappedDelayMillis));
    uint32_t extended_error, length;
    auto result = completion_callback(extended_error, length);
    CompleteOverlappedEx(overlapped_ptr, result, extended_error, length);
    if (post_callback) {
      post_callback();
    }
  });
  dispatch_cond_.notify_all();
}

bool KernelState::Save(ByteStream* stream) {
  XELOGD("Serializing the kernel...");
  stream->Write(kKernelSaveSignature);

  // Save the object table
  object_table_.Save(stream);

  // Write the TLS allocation bitmap
  auto tls_bitmap = tls_bitmap_.data();
  stream->Write(uint32_t(tls_bitmap.size()));
  for (size_t i = 0; i < tls_bitmap.size(); i++) {
    stream->Write<uint64_t>(tls_bitmap[i]);
  }

  // We save XThreads absolutely first, as they will execute code upon save
  // (which could modify the kernel state)
  auto threads = object_table_.GetObjectsByType<XThread>();
  uint32_t* num_threads_ptr =
      reinterpret_cast<uint32_t*>(stream->data() + stream->offset());
  stream->Write(static_cast<uint32_t>(threads.size()));

  size_t num_threads = threads.size();
  XELOGD("Serializing {} threads...", threads.size());
  for (auto thread : threads) {
    if (!thread->is_guest_thread()) {
      // Don't save host threads. They can be reconstructed on startup.
      num_threads--;
      continue;
    }

    if (!thread->Save(stream)) {
      XELOGD("Failed to save thread \"{}\"", thread->name());
      num_threads--;
    }
  }

  *num_threads_ptr = static_cast<uint32_t>(num_threads);

  // Save all other objects
  auto objects = object_table_.GetAllObjects();
  uint32_t* num_objects_ptr =
      reinterpret_cast<uint32_t*>(stream->data() + stream->offset());
  stream->Write(static_cast<uint32_t>(objects.size()));

  size_t num_objects = objects.size();
  XELOGD("Serializing {} objects...", num_objects);
  for (auto object : objects) {
    auto prev_offset = stream->offset();

    if (object->is_host_object() || object->type() == XObject::Type::Thread) {
      // Don't save host objects or save XThreads again
      num_objects--;
      continue;
    }

    stream->Write<uint32_t>(static_cast<uint32_t>(object->type()));
    if (!object->Save(stream)) {
      XELOGD("Did not save object of type {}", object->type());
      assert_always();

      // Revert backwards and overwrite if a save failed.
      stream->set_offset(prev_offset);
      num_objects--;
    }
  }

  *num_objects_ptr = static_cast<uint32_t>(num_objects);
  return true;
}

bool KernelState::Restore(ByteStream* stream) {
  // Check the magic value.
  if (stream->Read<uint32_t>() != kKernelSaveSignature) {
    return false;
  }

  // Restore the object table
  object_table_.Restore(stream);

  // Read the TLS allocation bitmap
  auto num_bitmap_entries = stream->Read<uint32_t>();
  auto& tls_bitmap = tls_bitmap_.data();
  tls_bitmap.resize(num_bitmap_entries);
  for (uint32_t i = 0; i < num_bitmap_entries; i++) {
    tls_bitmap[i] = stream->Read<uint64_t>();
  }

  uint32_t num_threads = stream->Read<uint32_t>();
  XELOGD("Loading {} threads...", num_threads);
  for (uint32_t i = 0; i < num_threads; i++) {
    auto thread = XObject::Restore(this, XObject::Type::Thread, stream);
    if (!thread) {
      // Can't continue the restore or we risk misalignment.
      assert_always();
      return false;
    }
  }

  uint32_t num_objects = stream->Read<uint32_t>();
  XELOGD("Loading {} objects...", num_objects);
  for (uint32_t i = 0; i < num_objects; i++) {
    uint32_t type = stream->Read<uint32_t>();

    auto obj = XObject::Restore(this, XObject::Type(type), stream);
    if (!obj) {
      // Can't continue the restore or we risk misalignment.
      assert_always();
      return false;
    }
  }

  return true;
}

}  // namespace kernel
}  // namespace xe
