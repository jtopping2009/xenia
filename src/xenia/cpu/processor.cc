/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/processor.h"

#include <algorithm>
#include <chrono>

#include "xenia/base/assert.h"
#include "xenia/base/atomic.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/cvar.h"
#include "xenia/base/debugging.h"
#include "xenia/base/exception_handler.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#include "xenia/base/profiling.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/breakpoint.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/module.h"
#include "xenia/cpu/ppc/ppc_decode_data.h"
#include "xenia/cpu/ppc/ppc_frontend.h"
#include "xenia/cpu/stack_walker.h"
#include "xenia/cpu/thread.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/cpu/xex_module.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
// After windows.h.
#include <tlhelp32.h>
#endif  // XE_PLATFORM_WIN32

// TODO(benvanik): based on compiler support
#include "xenia/cpu/backend/x64/x64_backend.h"

#if 0 && DEBUG
#define DEFAULT_DEBUG_FLAG true
#else
#define DEFAULT_DEBUG_FLAG false
#endif

DEFINE_bool(debug, DEFAULT_DEBUG_FLAG,
            "Allow debugging and retain debug information.", "General");
DEFINE_path(trace_function_data_path, "", "File to write trace data to.",
            "CPU");
DEFINE_int32(log_thread_stacks_interval, 0,
             "Every this many seconds, log where each guest thread is (its "
             "call stack's guest addresses) - for finding where a title that "
             "hangs is stuck. 0 to not.",
             "CPU");
DEFINE_string(watch_guest_writes, "",
              "Guest addresses (hex, comma-separated, up to 4) to watch with "
              "hardware breakpoints, logging every write to the 8 bytes at "
              "each (rounded down to 8) - what wrote it and the value - for "
              "finding what corrupts a title's memory. Empty for none.",
              "CPU");
DEFINE_bool(break_on_start, false, "Break into the debugger on startup.",
            "CPU");

namespace xe {
namespace kernel {
class XThread;
}  // namespace kernel

namespace cpu {

using xe::cpu::ppc::PPCOpcode;
using xe::kernel::XThread;

using namespace xe::literals;

class BuiltinModule : public Module {
 public:
  explicit BuiltinModule(Processor* processor)
      : Module(processor), name_("builtin") {}

  const std::string& name() const override { return name_; }
  bool is_executable() const override { return false; }

  bool ContainsAddress(uint32_t address) override {
    return (address & 0xFFFFFFF0) == 0xFFFFFFF0;
  }

 protected:
  std::unique_ptr<Function> CreateFunction(uint32_t address) override {
    return std::unique_ptr<Function>(new BuiltinFunction(this, address));
  }

 private:
  std::string name_;
};

Processor::Processor(xe::Memory* memory, ExportResolver* export_resolver)
    : memory_(memory), export_resolver_(export_resolver) {}

Processor::~Processor() {
  if (stack_sampler_) {
    stack_sampler_stop_ = true;
    stack_sampler_->join();
    stack_sampler_.reset();
  }
  {
    auto global_lock = global_critical_region_.Acquire();
    modules_.clear();
  }

  frontend_.reset();
  backend_.reset();

  if (functions_trace_file_) {
    functions_trace_file_->Flush();
    functions_trace_file_.reset();
  }
}

bool Processor::Setup(std::unique_ptr<backend::Backend> backend) {
  // TODO(benvanik): query mode from debugger?
  debug_info_flags_ = 0;

  auto frontend = std::make_unique<ppc::PPCFrontend>(this);
  // TODO(benvanik): set options/etc.

  // Must be initialized by subclass before calling into this.
  assert_not_null(memory_);

  std::unique_ptr<Module> builtin_module(new BuiltinModule(this));
  builtin_module_ = builtin_module.get();
  modules_.push_back(std::move(builtin_module));

  if (frontend_ || backend_) {
    return false;
  }

  if (!backend) {
    return false;
  }
  if (!backend->Initialize(this)) {
    return false;
  }
  if (!frontend->Initialize()) {
    return false;
  }

  backend_ = std::move(backend);
  frontend_ = std::move(frontend);

  // Stack walker is used when profiling, debugging, and dumping.
  // Note that creation may fail, in which case we'll have to disable those
  // features.
  // The code cache may be unavailable in case of a "null" backend.
  cpu::backend::CodeCache* code_cache = backend_->code_cache();
  if (code_cache) {
    stack_walker_ = StackWalker::Create(code_cache);
  }
  bool watching = StartWriteWatches(memory_);
  if (stack_walker_ &&
      (cvars::log_thread_stacks_interval > 0 || watching)) {
    stack_sampler_ = std::make_unique<std::thread>([this, watching]() {
      auto interval =
          std::chrono::seconds(std::max(cvars::log_thread_stacks_interval, 1));
      auto next = std::chrono::steady_clock::now() + interval;
      while (!stack_sampler_stop_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (watching) {
          ArmWriteWatches();
          LogWriteWatchHits();
        }
        if (cvars::log_thread_stacks_interval <= 0 ||
            std::chrono::steady_clock::now() < next) {
          continue;
        }
        next += interval;
        LogThreadStacks();
      }
    });
  }
  if (!stack_walker_) {
    // TODO(benvanik): disable features.
    if (cvars::debug) {
      XELOGW("Disabling --debug due to lack of stack walker");
      cvars::debug = false;
    }
  }

  // Open the trace data path, if requested.
  functions_trace_path_ = cvars::trace_function_data_path;
  if (!functions_trace_path_.empty()) {
    functions_trace_file_ =
        ChunkedMappedMemoryWriter::Open(functions_trace_path_, 32_MiB, true);
  }

  return true;
}

void Processor::PreLaunch() {
  if (cvars::break_on_start) {
    // Start paused.
    XELOGI("Breaking into debugger because of --break_on_start...");
    execution_state_ = ExecutionState::kRunning;
    Pause();
  } else {
    // Start running.
    execution_state_ = ExecutionState::kRunning;
  }
}

bool Processor::AddModule(std::unique_ptr<Module> module) {
  auto global_lock = global_critical_region_.Acquire();
  modules_.push_back(std::move(module));
  return true;
}

Module* Processor::GetModule(const std::string_view name) {
  auto global_lock = global_critical_region_.Acquire();
  for (const auto& module : modules_) {
    if (module->name() == name) {
      return module.get();
    }
  }
  return nullptr;
}

std::vector<Module*> Processor::GetModules() {
  auto global_lock = global_critical_region_.Acquire();
  std::vector<Module*> clone(modules_.size());
  for (const auto& module : modules_) {
    clone.push_back(module.get());
  }
  return clone;
}

Function* Processor::DefineBuiltin(const std::string_view name,
                                   BuiltinFunction::Handler handler, void* arg0,
                                   void* arg1) {
  uint32_t address = next_builtin_address_;
  next_builtin_address_ += 4;

  Function* function;
  builtin_module_->DeclareFunction(address, &function);
  function->set_end_address(address + 4);
  function->set_name(name);

  auto builtin_function = static_cast<BuiltinFunction*>(function);
  builtin_function->SetupBuiltin(handler, arg0, arg1);

  function->set_status(Symbol::Status::kDeclared);
  return function;
}

Function* Processor::QueryFunction(uint32_t address) {
  auto entry = entry_table_.Get(address);
  if (!entry) {
    return nullptr;
  }
  return entry->function;
}

std::vector<Function*> Processor::FindFunctionsWithAddress(uint32_t address) {
  return entry_table_.FindWithAddress(address);
}

Function* Processor::ResolveFunction(uint32_t address) {
  Entry* entry;
  Entry::Status status = entry_table_.GetOrCreate(address, &entry);
  if (status == Entry::STATUS_NEW) {
    // Needs to be generated. We have the 'lock' on it and must do so now.

    // Grab symbol declaration.
    auto function = LookupFunction(address);
    if (!function) {
      entry->status = Entry::STATUS_FAILED;
      return nullptr;
    }

    if (!DemandFunction(function)) {
      entry->status = Entry::STATUS_FAILED;
      return nullptr;
    }
    entry->function = function;
    entry->end_address = function->end_address();
    status = entry->status = Entry::STATUS_READY;
  }
  if (status == Entry::STATUS_READY) {
    // Ready to use.
    return entry->function;
  } else {
    // Failed or bad state.
    return nullptr;
  }
}

Function* Processor::LookupFunction(uint32_t address) {
  // TODO(benvanik): fast reject invalid addresses/log errors.

  // Find the module that contains the address.
  Module* code_module = nullptr;
  {
    auto global_lock = global_critical_region_.Acquire();
    // TODO(benvanik): sort by code address (if contiguous) so can bsearch.
    // TODO(benvanik): cache last module low/high, as likely to be in there.
    for (const auto& module : modules_) {
      if (module->ContainsAddress(address)) {
        code_module = module.get();
        break;
      }
    }
  }
  if (!code_module) {
    // No module found that could contain the address.
    return nullptr;
  }

  return LookupFunction(code_module, address);
}

Function* Processor::LookupFunction(Module* module, uint32_t address) {
  // Atomic create/lookup symbol in module.
  // If we get back the NEW flag we must declare it now.
  Function* function = nullptr;
  auto symbol_status = module->DeclareFunction(address, &function);
  if (symbol_status == Symbol::Status::kNew) {
    // Symbol is undeclared, so declare now.
    assert_true(function->is_guest());
    if (!frontend_->DeclareFunction(static_cast<GuestFunction*>(function))) {
      function->set_status(Symbol::Status::kFailed);
      return nullptr;
    }
    function->set_status(Symbol::Status::kDeclared);
  }
  return function;
}

bool Processor::DemandFunction(Function* function) {
  // Lock function for generation. If it's already being generated
  // by another thread this will block and return DECLARED.
  auto module = function->module();
  auto symbol_status = module->DefineFunction(function);
  if (symbol_status == Symbol::Status::kNew) {
    // Symbol is undefined, so define now.
    assert_true(function->is_guest());
    if (!frontend_->DefineFunction(static_cast<GuestFunction*>(function),
                                   debug_info_flags_)) {
      function->set_status(Symbol::Status::kFailed);
      return false;
    }

    // Before we give the symbol back to the rest, let the debugger know.
    OnFunctionDefined(function);

    function->set_status(Symbol::Status::kDefined);
    symbol_status = function->status();
  }

  if (symbol_status == Symbol::Status::kFailed) {
    // Symbol likely failed.
    return false;
  }

  return true;
}

bool Processor::Execute(ThreadState* thread_state, uint32_t address) {
  SCOPE_profile_cpu_f("cpu");

  // Attempt to get the function.
  auto function = ResolveFunction(address);
  if (!function) {
    // Symbol not found in any module.
    XELOGCPU("Execute({:08X}): failed to find function", address);
    return false;
  }

  auto context = thread_state->context();

  // Pad out stack a bit, as some games seem to overwrite the caller by about
  // 16 to 32b.
  context->r[1] -= 64 + 112;

  // This could be set to anything to give us a unique identifier to track
  // re-entrancy/etc.
  uint64_t previous_lr = context->lr;
  context->lr = 0xBCBCBCBC;

  // Execute the function.
  auto result = function->Call(thread_state, uint32_t(context->lr));

  context->lr = previous_lr;
  context->r[1] += 64 + 112;

  return result;
}

bool Processor::ExecuteRaw(ThreadState* thread_state, uint32_t address) {
  SCOPE_profile_cpu_f("cpu");

  // Attempt to get the function.
  auto function = ResolveFunction(address);
  if (!function) {
    // Symbol not found in any module.
    XELOGCPU("Execute({:08X}): failed to find function", address);
    return false;
  }

  return function->Call(thread_state, 0xBCBCBCBC);
}

uint64_t Processor::Execute(ThreadState* thread_state, uint32_t address,
                            uint64_t args[], size_t arg_count) {
  SCOPE_profile_cpu_f("cpu");

  auto context = thread_state->context();
  for (size_t i = 0; i < std::min(arg_count, static_cast<size_t>(8)); ++i) {
    context->r[3 + i] = args[i];
  }

  if (arg_count > 7) {
    // Rest of the arguments go on the stack.
    // FIXME: This assumes arguments are 32 bits!
    auto stack_arg_base =
        memory()->TranslateVirtual((uint32_t)context->r[1] + 0x54 - (64 + 112));
    for (size_t i = 0; i < arg_count - 8; i++) {
      xe::store_and_swap<uint32_t>(stack_arg_base + (i * 8),
                                   (uint32_t)args[i + 8]);
    }
  }

  if (!Execute(thread_state, address)) {
    return 0xDEADBABE;
  }
  return context->r[3];
}

uint64_t Processor::ExecuteInterrupt(ThreadState* thread_state,
                                     uint32_t address, uint64_t args[],
                                     size_t arg_count) {
  SCOPE_profile_cpu_f("cpu");

  // Hold the global lock during interrupt dispatch.
  // This will block if any code is in a critical region (has interrupts
  // disabled) or if any other interrupt is executing.
  auto global_lock = global_critical_region_.Acquire();

  auto context = thread_state->context();
  assert_true(arg_count <= 5);
  for (size_t i = 0; i < arg_count; ++i) {
    context->r[3 + i] = args[i];
  }

  // TLS ptr must be zero during interrupts. Some games check this and
  // early-exit routines when under interrupts.
  auto pcr_address =
      memory_->TranslateVirtual(static_cast<uint32_t>(context->r[13]));
  uint32_t old_tls_ptr = xe::load_and_swap<uint32_t>(pcr_address);
  xe::store_and_swap<uint32_t>(pcr_address, 0);

  if (!Execute(thread_state, address)) {
    return 0xDEADBABE;
  }

  // Restores TLS ptr.
  xe::store_and_swap<uint32_t>(pcr_address, old_tls_ptr);

  return context->r[3];
}

Irql Processor::RaiseIrql(Irql new_value) {
  return static_cast<Irql>(
      xe::atomic_exchange(static_cast<uint32_t>(new_value),
                          reinterpret_cast<volatile uint32_t*>(&irql_)));
}

void Processor::LowerIrql(Irql old_value) {
  xe::atomic_exchange(static_cast<uint32_t>(old_value),
                      reinterpret_cast<volatile uint32_t*>(&irql_));
}

bool Processor::Save(ByteStream* stream) {
  stream->Write(kProcessorSaveSignature);
  return true;
}

bool Processor::Restore(ByteStream* stream) {
  if (stream->Read<uint32_t>() != kProcessorSaveSignature) {
    XELOGE("Processor::Restore - Invalid magic value!");
    return false;
  }

  // Clear cached thread data for zombie threads.
  std::vector<uint32_t> to_delete;
  for (auto& it : thread_debug_infos_) {
    if (it.second->state == ThreadDebugInfo::State::kZombie) {
      to_delete.push_back(it.first);
    }
  }
  for (uint32_t thread_id : to_delete) {
    thread_debug_infos_.erase(thread_id);
  }

  return true;
}

uint8_t* Processor::AllocateFunctionTraceData(size_t size) {
  if (!functions_trace_file_) {
    return nullptr;
  }
  return functions_trace_file_->Allocate(size);
}

void Processor::OnFunctionDefined(Function* function) {
  auto global_lock = global_critical_region_.Acquire();
  for (auto breakpoint : breakpoints_) {
    if (breakpoint->address_type() == Breakpoint::AddressType::kGuest) {
      if (function->ContainsAddress(breakpoint->guest_address())) {
        if (breakpoint->is_installed()) {
          backend_->InstallBreakpoint(breakpoint, function);
        }
      }
    }
  }
}

void Processor::OnThreadCreated(uint32_t thread_handle,
                                ThreadState* thread_state, Thread* thread) {
  auto global_lock = global_critical_region_.Acquire();
  auto thread_info = std::make_unique<ThreadDebugInfo>();
  thread_info->thread_handle = thread_handle;
  thread_info->thread_id = thread_state->thread_id();
  thread_info->thread = thread;
  thread_info->state = ThreadDebugInfo::State::kAlive;
  thread_info->suspended = false;
  thread_debug_infos_.emplace(thread_info->thread_id, std::move(thread_info));
}

void Processor::OnThreadExit(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = thread_debug_infos_.find(thread_id);
  assert_true(it != thread_debug_infos_.end());
  auto thread_info = it->second.get();
  thread_info->state = ThreadDebugInfo::State::kExited;
}

void Processor::OnThreadDestroyed(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = thread_debug_infos_.find(thread_id);
  assert_true(it != thread_debug_infos_.end());
  auto thread_info = it->second.get();
  thread_info->state = ThreadDebugInfo::State::kZombie;
  thread_info->thread = nullptr;
}

void Processor::OnThreadEnteringWait(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = thread_debug_infos_.find(thread_id);
  assert_true(it != thread_debug_infos_.end());
  auto thread_info = it->second.get();
  thread_info->state = ThreadDebugInfo::State::kWaiting;
}

void Processor::OnThreadLeavingWait(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = thread_debug_infos_.find(thread_id);
  assert_true(it != thread_debug_infos_.end());
  auto thread_info = it->second.get();
  if (thread_info->state == ThreadDebugInfo::State::kWaiting) {
    thread_info->state = ThreadDebugInfo::State::kAlive;
  }
}

std::vector<ThreadDebugInfo*> Processor::QueryThreadDebugInfos() {
  auto global_lock = global_critical_region_.Acquire();
  std::vector<ThreadDebugInfo*> result;
  for (auto& it : thread_debug_infos_) {
    result.push_back(it.second.get());
  }
  return result;
}

ThreadDebugInfo* Processor::QueryThreadDebugInfo(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  const auto& it = thread_debug_infos_.find(thread_id);
  if (it == thread_debug_infos_.end()) {
    return nullptr;
  }
  return it->second.get();
}

void Processor::AddBreakpoint(Breakpoint* breakpoint) {
  auto global_lock = global_critical_region_.Acquire();

  // Add to breakpoints map.
  breakpoints_.push_back(breakpoint);

  if (execution_state_ == ExecutionState::kRunning) {
    breakpoint->Resume();
  }
}

void Processor::RemoveBreakpoint(Breakpoint* breakpoint) {
  auto global_lock = global_critical_region_.Acquire();

  // Uninstall (if needed).
  if (execution_state_ == ExecutionState::kRunning) {
    breakpoint->Suspend();
  }

  // Remove from breakpoint map.
  auto it = std::find(breakpoints_.begin(), breakpoints_.end(), breakpoint);
  breakpoints_.erase(it);
}

Breakpoint* Processor::FindBreakpoint(uint32_t address) {
  auto global_lock = global_critical_region_.Acquire();
  for (auto breakpoint : breakpoints_) {
    if (breakpoint->address() == address) {
      return breakpoint;
    }
  }
  return nullptr;
}

void Processor::set_debug_listener(DebugListener* debug_listener) {
  if (debug_listener == debug_listener_) {
    return;
  }
  if (debug_listener_) {
    // Detach old debug listener.
    debug_listener_->OnDetached();
    debug_listener_ = nullptr;
  }
  if (debug_listener) {
    debug_listener_ = debug_listener;
  } else {
    if (execution_state_ == ExecutionState::kPaused) {
      XELOGI("Debugger detaching while execution is paused; continuing...");
      Continue();
    }
  }
}

void Processor::DemandDebugListener() {
  if (debug_listener_) {
    // Already present.
    debug_listener_->OnFocus();
    return;
  }
  if (!debug_listener_handler_) {
    XELOGE("Debugger demanded a listener but no handler was registered.");
    xe::debugging::Break();
    return;
  }
  set_debug_listener(debug_listener_handler_(this));
}

bool Processor::OnThreadBreakpointHit(Exception* ex) {
  auto global_lock = global_critical_region_.Acquire();

  // Suspend all threads (but ourselves).
  SuspendAllThreads();

  // Lookup thread info block.
  auto it = thread_debug_infos_.find(ThreadState::GetThreadID());
  if (it == thread_debug_infos_.end()) {
    // Not found - exception on a thread we don't know about?
    assert_always("UD2 on a thread we don't track");
    return false;
  }
  auto thread_info = it->second.get();

  // Run through and uninstall all breakpoint UD2s to get us back to a clean
  // state.
  if (execution_state_ != ExecutionState::kStepping) {
    SuspendAllBreakpoints();
  }

  // Update all thread states with their latest values, using the context we
  // got from the exception instead of a sampled value (as it would just show
  // the exception handler).
  UpdateThreadExecutionStates(thread_info->thread_id, ex->thread_context());

  // Walk the captured thread stack and look for breakpoints at any address in
  // the stack. We just look for the first one.
  Breakpoint* breakpoint = nullptr;
  for (size_t i = 0; i < thread_info->frames.size(); ++i) {
    auto& frame = thread_info->frames[i];
    for (auto scan_breakpoint : breakpoints_) {
      if ((scan_breakpoint->address_type() == Breakpoint::AddressType::kGuest &&
           scan_breakpoint->guest_address() == frame.guest_pc) ||
          (scan_breakpoint->address_type() == Breakpoint::AddressType::kHost &&
           scan_breakpoint->host_address() == frame.host_pc)) {
        breakpoint = scan_breakpoint;
        break;
      }
    }
    if (breakpoint) {
      breakpoint->OnHit(thread_info, frame.host_pc);
      break;
    }
  }

  // We are waiting on the debugger now. Either wait for it to continue, add a
  // new step, or direct us somewhere else.
  // The debugger will ResumeAllThreads or just resume us (depending on what
  // it wants to do).
  execution_state_ = ExecutionState::kPaused;
  thread_info->suspended = true;

  // Must unlock, or we will deadlock.
  global_lock.unlock();

  if (debug_listener_) {
    debug_listener_->OnExecutionPaused();
  }

  thread_info->thread->thread()->Suspend();

  // Apply thread context changes.
  // TODO(benvanik): apply to all threads?
#if XE_ARCH_AMD64
  ex->set_resume_pc(thread_info->host_context.rip);
#elif XE_ARCH_ARM64
  ex->set_resume_pc(thread_info->host_context.pc);
#else
#error Instruction pointer not specified for the target CPU architecture.
#endif  // XE_ARCH

  // Resume execution.
  return true;
}

void Processor::OnStepCompleted(ThreadDebugInfo* thread_info) {
  auto global_lock = global_critical_region_.Acquire();
  execution_state_ = ExecutionState::kPaused;
  if (debug_listener_) {
    debug_listener_->OnExecutionPaused();
  }

  // Note that we stay suspended.
}

bool Processor::OnUnhandledException(Exception* ex) {
  // If we have no listener return right away.
  // TODO(benvanik): DemandDebugListener()?
  if (!debug_listener_) {
    return false;
  }

  // If this isn't a managed thread, fail - let VS handle it for now.
  if (!Thread::IsInThread()) {
    return false;
  }

  auto global_lock = global_critical_region_.Acquire();

  // Suspend all guest threads (but this one).
  SuspendAllThreads();

  UpdateThreadExecutionStates(Thread::GetCurrentThreadId(),
                              ex->thread_context());

  // Stop and notify the listener.
  // This will take control.
  assert_true(execution_state_ == ExecutionState::kRunning);
  execution_state_ = ExecutionState::kPaused;

  // Notify debugger that exceution stopped.
  // debug_listener_->OnException(info);
  debug_listener_->OnExecutionPaused();

  // Suspend self.
  Thread::GetCurrentThread()->thread()->Suspend();

  return true;
}

void Processor::ShowDebugger() {
  if (debug_listener_) {
    debug_listener_->OnFocus();
  } else {
    DemandDebugListener();
  }
}

bool Processor::SuspendAllThreads() {
  auto global_lock = global_critical_region_.Acquire();
  for (auto& it : thread_debug_infos_) {
    auto thread_info = it.second.get();
    if (thread_info->suspended) {
      // Already suspended - ignore.
      continue;
    } else if (thread_info->state == ThreadDebugInfo::State::kZombie ||
               thread_info->state == ThreadDebugInfo::State::kExited) {
      // Thread is dead and cannot be suspended - ignore.
      continue;
    } else if (Thread::IsInThread() &&
               thread_info->thread_id == Thread::GetCurrentThreadId()) {
      // Can't suspend ourselves.
      continue;
    }
    auto thread = thread_info->thread;
    if (!thread->can_debugger_suspend()) {
      // Thread is a host thread, and we aren't suspending those (for now).
      continue;
    }
    bool did_suspend = thread->thread()->Suspend(nullptr);
    assert_true(did_suspend);
    thread_info->suspended = true;
  }
  return true;
}

bool Processor::ResumeThread(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = thread_debug_infos_.find(thread_id);
  if (it == thread_debug_infos_.end()) {
    return false;
  }
  auto thread_info = it->second.get();
  assert_true(thread_info->suspended);
  assert_false(thread_info->state == ThreadDebugInfo::State::kExited ||
               thread_info->state == ThreadDebugInfo::State::kZombie);
  thread_info->suspended = false;
  auto thread = thread_info->thread;
  return thread->thread()->Resume();
}

bool Processor::ResumeAllThreads() {
  auto global_lock = global_critical_region_.Acquire();
  for (auto& it : thread_debug_infos_) {
    auto thread_info = it.second.get();
    if (!thread_info->suspended) {
      // Not suspended by us - ignore.
      continue;
    } else if (thread_info->state == ThreadDebugInfo::State::kZombie ||
               thread_info->state == ThreadDebugInfo::State::kExited) {
      // Thread is dead and cannot be resumed - ignore.
      continue;
    } else if (Thread::IsInThread() &&
               thread_info->thread_id == Thread::GetCurrentThreadId()) {
      // Can't resume ourselves.
      continue;
    }
    auto thread = thread_info->thread;
    if (!thread->can_debugger_suspend()) {
      // Thread is a host thread, and we aren't suspending those (for now).
      continue;
    }
    thread_info->suspended = false;
    bool did_resume = thread->thread()->Resume();
    assert_true(did_resume);
  }
  return true;
}

void Processor::UpdateThreadExecutionStates(
    uint32_t override_thread_id, HostThreadContext* override_context) {
  auto global_lock = global_critical_region_.Acquire();
  uint64_t frame_host_pcs[64];
  xe::cpu::StackFrame cpu_frames[64];
  for (auto& it : thread_debug_infos_) {
    auto thread_info = it.second.get();
    auto thread = thread_info->thread;
    if (!thread) {
      continue;
    }

    // Grab PPC context.
    // Note that this is only up to date if --store_all_context_values is
    // enabled (or --debug).
    if (thread->can_debugger_suspend()) {
      std::memcpy(&thread_info->guest_context,
                  thread->thread_state()->context(),
                  sizeof(thread_info->guest_context));
    }

    // Grab stack trace and X64 context then resolve all symbols.
    uint64_t hash;
    HostThreadContext* in_host_context = nullptr;
    if (override_thread_id == thread_info->thread_id) {
      // If we were passed an override context we use that. Otherwise, ask the
      // stack walker for a new context.
      in_host_context = override_context;
    }
    size_t count = stack_walker_->CaptureStackTrace(
        thread->thread()->native_handle(), frame_host_pcs, 0,
        xe::countof(frame_host_pcs), in_host_context,
        &thread_info->host_context, &hash);
    stack_walker_->ResolveStack(frame_host_pcs, cpu_frames, count);
    thread_info->frames.resize(count);
    for (size_t i = 0; i < count; ++i) {
      auto& cpu_frame = cpu_frames[i];
      auto& frame = thread_info->frames[i];
      frame.host_pc = cpu_frame.host_pc;
      frame.host_function_address = cpu_frame.host_symbol.address;
      frame.guest_pc = cpu_frame.guest_pc;
      frame.guest_function_address = 0;
      frame.guest_function = nullptr;
      auto function = cpu_frame.guest_symbol.function;
      if (cpu_frame.type == cpu::StackFrame::Type::kGuest && function) {
        frame.guest_function_address = function->address();
        frame.guest_function = function;
      } else {
        std::strncpy(frame.name, cpu_frame.host_symbol.name,
                     xe::countof(frame.name));
        frame.name[xe::countof(frame.name) - 1] = 0;
      }
    }
  }
}

// --watch_guest_writes: hardware data breakpoints (the debug registers, so
// at most four) on guest addresses, in every thread of the process - guest
// code and Xenia's own threads (GPU, audio) alike - logging each write: what
// wrote (the guest code, or the Xenia function) and the value after it.
namespace {
struct WriteWatchHit {
  uint64_t host_pc;
  uint64_t value;
  uint32_t address;
  uint32_t host_thread_id;
  // The host call stack at the write (for writes from Xenia's own code).
  uint64_t stack[12];
  uint32_t stack_count;
};
constexpr uint32_t kMaxWriteWatchHits = 4096;
WriteWatchHit write_watch_hits[kMaxWriteWatchHits];
std::atomic<uint32_t> write_watch_hit_count{0};
uint32_t write_watch_logged = 0;
uint32_t write_watch_guest[4];
uint8_t* write_watch_host[4];
int write_watch_count = 0;
std::vector<uint32_t> write_watch_armed_threads;

#if XE_PLATFORM_WIN32
LONG CALLBACK WriteWatchExceptionHandler(PEXCEPTION_POINTERS ex_info) {
  if (ex_info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  auto context = ex_info->ContextRecord;
  DWORD64 triggered = context->Dr6 & 0xF;
  if (!triggered) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  for (int i = 0; i < write_watch_count; ++i) {
    if (!(triggered & (DWORD64(1) << i))) {
      continue;
    }
    uint32_t n = write_watch_hit_count.fetch_add(1);
    if (n < kMaxWriteWatchHits) {
      auto& hit = write_watch_hits[n];
      hit.host_pc = context->Rip;
      hit.address = write_watch_guest[i];
      std::memcpy(&hit.value, write_watch_host[i], 8);
      hit.host_thread_id = GetCurrentThreadId();
      // Unwound from the exception's context: the writer and its callers.
      hit.stack_count = 0;
      CONTEXT unwind = *context;
      for (; hit.stack_count < xe::countof(hit.stack); ++hit.stack_count) {
        hit.stack[hit.stack_count] = unwind.Rip;
        DWORD64 image_base;
        auto entry = RtlLookupFunctionEntry(unwind.Rip, &image_base, nullptr);
        if (!entry) {
          // A leaf function: the return address is on top of the stack.
          if (!unwind.Rsp) {
            break;
          }
          unwind.Rip = *reinterpret_cast<DWORD64*>(unwind.Rsp);
          unwind.Rsp += 8;
        } else {
          void* handler_data;
          DWORD64 establisher_frame;
          RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, unwind.Rip, entry,
                           &unwind, &handler_data, &establisher_frame,
                           nullptr);
        }
        if (!unwind.Rip) {
          ++hit.stack_count;
          break;
        }
      }
    }
  }
  context->Dr6 = 0;
  return EXCEPTION_CONTINUE_EXECUTION;
}
#endif  // XE_PLATFORM_WIN32
}  // namespace

bool Processor::StartWriteWatches(Memory* memory) {
#if XE_PLATFORM_WIN32
  std::string list = cvars::watch_guest_writes;
  size_t start = 0;
  while (start < list.size() && write_watch_count < 4) {
    size_t end = list.find(',', start);
    if (end == std::string::npos) {
      end = list.size();
    }
    std::string item = list.substr(start, end - start);
    start = end + 1;
    if (item.empty()) {
      continue;
    }
    // "p:" for a physical address, watched where Xenia's own physical memory
    // accesses (the GPU, DMA-like copies) reach it rather than through a
    // virtual view.
    bool physical = item.rfind("p:", 0) == 0;
    uint32_t address =
        uint32_t(std::strtoul(item.c_str() + (physical ? 2 : 0), nullptr,
                              16)) &
        ~7u;
    write_watch_guest[write_watch_count] = address;
    write_watch_host[write_watch_count] =
        physical ? memory->TranslatePhysical<uint8_t*>(address)
                 : memory->TranslateVirtual<uint8_t*>(address);
    XELOGI("Watching writes to {}{:08X}-{:08X}", physical ? "physical " : "",
           address, address + 7);
    ++write_watch_count;
  }
  if (!write_watch_count) {
    return false;
  }
  AddVectoredExceptionHandler(1, WriteWatchExceptionHandler);
  return true;
#else
  return false;
#endif  // XE_PLATFORM_WIN32
}

void Processor::ArmWriteWatches() {
#if XE_PLATFORM_WIN32
  // Every thread of the process not armed yet (threads come and go).
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return;
  }
  DWORD process_id = GetCurrentProcessId();
  DWORD self_id = GetCurrentThreadId();
  THREADENTRY32 entry;
  entry.dwSize = sizeof(entry);
  for (BOOL ok = Thread32First(snapshot, &entry); ok;
       ok = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != process_id ||
        entry.th32ThreadID == self_id ||
        std::find(write_watch_armed_threads.begin(),
                  write_watch_armed_threads.end(),
                  entry.th32ThreadID) != write_watch_armed_threads.end()) {
      continue;
    }
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                   THREAD_SET_CONTEXT,
                               FALSE, entry.th32ThreadID);
    if (!thread) {
      continue;
    }
    if (SuspendThread(thread) != DWORD(-1)) {
      CONTEXT context = {};
      context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      if (GetThreadContext(thread, &context)) {
        DWORD64* registers[] = {&context.Dr0, &context.Dr1, &context.Dr2,
                                &context.Dr3};
        DWORD64 control = 0;
        for (int i = 0; i < write_watch_count; ++i) {
          *registers[i] = DWORD64(write_watch_host[i]);
          // Local enable; break on writes (01); 8 bytes long (10).
          control |= DWORD64(1) << (2 * i);
          control |= DWORD64(0b01) << (16 + 4 * i);
          control |= DWORD64(0b10) << (18 + 4 * i);
        }
        context.Dr7 = control;
        if (SetThreadContext(thread, &context)) {
          write_watch_armed_threads.push_back(entry.th32ThreadID);
          XELOGI("Write watch: armed thread {:X} ({} so far)",
                 entry.th32ThreadID, write_watch_armed_threads.size());
        }
      }
      ::ResumeThread(thread);
    }
    CloseHandle(thread);
  }
  CloseHandle(snapshot);
#endif  // XE_PLATFORM_WIN32
}

void Processor::LogWriteWatchHits() {
  uint32_t count =
      std::min(write_watch_hit_count.load(), kMaxWriteWatchHits);
  for (; write_watch_logged < count; ++write_watch_logged) {
    const auto& hit = write_watch_hits[write_watch_logged];
    // The pc is just past the writing instruction - in guest code, that maps
    // to the guest instruction; otherwise name Xenia's function.
    std::string where;
    auto code_cache = backend_ ? backend_->code_cache() : nullptr;
    auto function = code_cache ? code_cache->LookupFunction(hit.host_pc)
                               : nullptr;
    if (function) {
      where = fmt::format("guest {:08X} [function {:08X}]",
                          function->MapMachineCodeToGuestAddress(hit.host_pc),
                          function->address());
    } else if (stack_walker_) {
      uint64_t pc = hit.host_pc;
      StackFrame frame;
      stack_walker_->ResolveStack(&pc, &frame, 1);
      where = fmt::format("host {:016X} {}", hit.host_pc,
                          frame.host_symbol.name);
    }
    auto bytes = reinterpret_cast<const uint8_t*>(&hit.value);
    XELOGI("Write {}: {:08X} = {:02X}{:02X}{:02X}{:02X} {:02X}{:02X}{:02X}{:02X}"
           " by {} (host thread {:X})",
           write_watch_logged, hit.address, bytes[0], bytes[1], bytes[2],
           bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], where,
           hit.host_thread_id);
#if XE_PLATFORM_WIN32
    // The writing thread's name, and its call stack.
    std::string thread_name;
    if (HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                                   hit.host_thread_id)) {
      PWSTR description = nullptr;
      if (SUCCEEDED(GetThreadDescription(thread, &description)) &&
          description) {
        thread_name = xe::to_utf8(std::u16string(
            reinterpret_cast<const char16_t*>(description)));
        LocalFree(description);
      }
      CloseHandle(thread);
    }
    std::string stack;
    for (uint32_t i = 0; i < hit.stack_count; ++i) {
      uint64_t pc = hit.stack[i];
      auto frame_function =
          code_cache ? code_cache->LookupFunction(pc) : nullptr;
      if (frame_function) {
        stack += fmt::format(
            "\n    guest {:08X}",
            frame_function->MapMachineCodeToGuestAddress(pc));
        continue;
      }
      StackFrame frame = {};
      if (stack_walker_) {
        stack_walker_->ResolveStack(&pc, &frame, 1);
      }
      HMODULE module = nullptr;
      char module_name[MAX_PATH] = "?";
      if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCSTR>(pc), &module)) {
        GetModuleFileNameA(module, module_name, MAX_PATH);
      }
      const char* base_name = std::strrchr(module_name, '\\');
      stack += fmt::format("\n    {}+{:X} {}",
                           base_name ? base_name + 1 : module_name,
                           pc - reinterpret_cast<uint64_t>(module),
                           frame.host_symbol.name);
    }
    XELOGI("  thread \"{}\":{}", thread_name, stack);
#endif  // XE_PLATFORM_WIN32
  }
  if (write_watch_hit_count.load() > kMaxWriteWatchHits &&
      write_watch_logged == kMaxWriteWatchHits) {
    XELOGI("Write watch: stopped recording after {} writes",
           kMaxWriteWatchHits);
    ++write_watch_logged;
  }
}

void Processor::LogThreadStacks() {
  // Gathered with the threads suspended and logged once they're running again:
  // a suspended thread may hold the log's lock.
  std::string text;
  {
    auto global_lock = global_critical_region_.Acquire();
    if (execution_state_ != ExecutionState::kRunning) {
      return;
    }
    SuspendAllThreads();
    UpdateThreadExecutionStates();
    for (auto& it : thread_debug_infos_) {
      auto thread_info = it.second.get();
      if (!thread_info->thread || !thread_info->suspended) {
        continue;
      }
      text += fmt::format("\n  thread {:X} ({:08X}):", thread_info->thread_id,
                          thread_info->thread_handle);
      int shown = 0;
      for (const auto& frame : thread_info->frames) {
        if (!frame.guest_pc) {
          continue;
        }
        text += fmt::format(" {:08X}[{:08X}]", frame.guest_pc,
                            frame.guest_function_address);
        if (++shown == 12) {
          break;
        }
      }
    }
    ResumeAllThreads();
  }
  XELOGI("Thread stacks (pc[function], innermost first):{}", text);
}

void Processor::SuspendAllBreakpoints() {
  auto global_lock = global_critical_region_.Acquire();
  for (auto breakpoint : breakpoints_) {
    breakpoint->Suspend();
  }
}

void Processor::ResumeAllBreakpoints() {
  auto global_lock = global_critical_region_.Acquire();
  for (auto breakpoint : breakpoints_) {
    breakpoint->Resume();
  }
}

void Processor::Pause() {
  {
    auto global_lock = global_critical_region_.Acquire();
    assert_true(execution_state_ == ExecutionState::kRunning);
    SuspendAllThreads();
    SuspendAllBreakpoints();
    UpdateThreadExecutionStates();
    execution_state_ = ExecutionState::kPaused;
    if (debug_listener_) {
      debug_listener_->OnExecutionPaused();
    }
  }
  DemandDebugListener();
}

void Processor::Continue() {
  auto global_lock = global_critical_region_.Acquire();
  if (execution_state_ == ExecutionState::kRunning) {
    return;
  } else if (execution_state_ == ExecutionState::kStepping) {
    assert_always("cancel stepping not done yet");
  }
  execution_state_ = ExecutionState::kRunning;
  ResumeAllBreakpoints();
  ResumeAllThreads();
  if (debug_listener_) {
    debug_listener_->OnExecutionContinued();
  }
}

void Processor::StepHostInstruction(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  assert_true(execution_state_ == ExecutionState::kPaused);
  execution_state_ = ExecutionState::kStepping;

  auto thread_info = QueryThreadDebugInfo(thread_id);
  uint64_t new_host_pc = backend_->CalculateNextHostInstruction(
      thread_info, thread_info->frames[0].host_pc);

  assert_null(thread_info->step_breakpoint.get());
  thread_info->step_breakpoint.reset(
      new Breakpoint(this, Breakpoint::AddressType::kHost, new_host_pc,
                     [this, thread_info](Breakpoint* breakpoint,
                                         ThreadDebugInfo* breaking_thread_info,
                                         uint64_t host_address) {
                       if (thread_info != breaking_thread_info) {
                         assert_always("Step in another thread?");
                       }
                       // Our step request has completed. Remove the breakpoint
                       // and fire event.
                       breakpoint->Suspend();
                       RemoveBreakpoint(breakpoint);
                       thread_info->step_breakpoint.reset();
                       OnStepCompleted(thread_info);
                     }));
  AddBreakpoint(thread_info->step_breakpoint.get());
  thread_info->step_breakpoint->Resume();

  // ResumeAllBreakpoints();
  ResumeThread(thread_id);
}

void Processor::StepGuestInstruction(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  assert_true(execution_state_ == ExecutionState::kPaused);
  execution_state_ = ExecutionState::kStepping;

  auto thread_info = QueryThreadDebugInfo(thread_id);

  uint32_t next_pc = CalculateNextGuestInstruction(
      thread_info, thread_info->frames[0].guest_pc);

  assert_null(thread_info->step_breakpoint.get());
  thread_info->step_breakpoint.reset(
      new Breakpoint(this, Breakpoint::AddressType::kGuest, next_pc,
                     [this, thread_info](Breakpoint* breakpoint,
                                         ThreadDebugInfo* breaking_thread_info,
                                         uint64_t host_address) {
                       if (thread_info != breaking_thread_info) {
                         assert_always("Step in another thread?");
                       }
                       // Our step request has completed. Remove the breakpoint
                       // and fire event.
                       breakpoint->Suspend();
                       RemoveBreakpoint(breakpoint);
                       thread_info->step_breakpoint.reset();
                       OnStepCompleted(thread_info);
                     }));
  AddBreakpoint(thread_info->step_breakpoint.get());
  thread_info->step_breakpoint->Resume();

  // ResumeAllBreakpoints();
  ResumeThread(thread_id);
}

bool Processor::StepToGuestAddress(uint32_t thread_id, uint32_t pc) {
  auto functions = FindFunctionsWithAddress(pc);
  if (functions.empty()) {
    // Function hasn't been generated yet. Generate it.
    if (!ResolveFunction(pc)) {
      XELOGE(
          "Processor::StepToAddress({:08X}) - Function could not be resolved",
          pc);
      return false;
    }
  }

  // Instruct the thread to step forwards.
  threading::Fence fence;
  cpu::Breakpoint bp(
      this, Breakpoint::AddressType::kGuest, pc,
      [&fence](Breakpoint* breakpoint, ThreadDebugInfo* thread_info,
               uint64_t host_address) { fence.Signal(); });
  bp.Resume();

  // HACK
  auto thread_info = QueryThreadDebugInfo(thread_id);
  uint32_t suspend_count = 1;
  while (suspend_count) {
    thread_info->thread->thread()->Resume(&suspend_count);
  }

  fence.Wait();
  bp.Suspend();

  return true;
}

uint32_t Processor::StepIntoGuestBranchTarget(uint32_t thread_id, uint32_t pc) {
  xe::cpu::ppc::PPCDecodeData d;
  d.address = pc;
  d.code = xe::load_and_swap<uint32_t>(memory()->TranslateVirtual(d.address));
  auto opcode = xe::cpu::ppc::LookupOpcode(d.code);

  // Must be on a branch.
  assert_true(xe::cpu::ppc::GetOpcodeInfo(opcode).group ==
              xe::cpu::ppc::PPCOpcodeGroup::kB);

  auto thread_info = QueryThreadDebugInfo(thread_id);
  auto thread = thread_info->thread;
  auto context = thread->thread_state()->context();

  if (d.code == 0x4E800020) {
    // blr
    uint32_t nia = uint32_t(context->lr);
    StepToGuestAddress(thread_id, nia);
    pc = nia;
  } else if (d.code == 0x4E800420) {
    // bctr
    uint32_t nia = uint32_t(context->ctr);
    StepToGuestAddress(thread_id, nia);
    pc = nia;
  } else if (opcode == PPCOpcode::bx) {
    // bx
    uint32_t nia = d.I.ADDR();
    StepToGuestAddress(thread_id, nia);
    pc = nia;
  } else if (opcode == PPCOpcode::bcx || opcode == PPCOpcode::bcctrx ||
             opcode == PPCOpcode::bclrx) {
    threading::Fence fence;
    auto callback = [&fence, &pc](Breakpoint* breakpoint,
                                  ThreadDebugInfo* thread_info,
                                  uint64_t host_address) {
      pc = breakpoint->guest_address();
      fence.Signal();
    };

    cpu::Breakpoint bpf(this, Breakpoint::AddressType::kGuest, pc + 4,
                        callback);
    bpf.Resume();

    uint32_t nia = 0;
    if (opcode == PPCOpcode::bcx) {
      // bcx
      nia = d.B.ADDR();
    } else if (opcode == PPCOpcode::bcctrx) {
      // bcctrx
      nia = uint32_t(context->ctr);
    } else if (opcode == PPCOpcode::bclrx) {
      // bclrx
      nia = uint32_t(context->lr);
    }

    cpu::Breakpoint bpt(this, Breakpoint::AddressType::kGuest, nia, callback);
    bpt.Resume();

    // HACK
    uint32_t suspend_count = 1;
    while (suspend_count) {
      thread->thread()->Resume(&suspend_count);
    }

    fence.Wait();
    bpt.Suspend();
    bpf.Suspend();
  }

  return pc;
}

uint32_t Processor::StepToGuestSafePoint(uint32_t thread_id, bool ignore_host) {
  // This cannot be done if we're the calling thread!
  if (thread_id == ThreadState::GetThreadID()) {
    assert_always(
        "Processor::StepToSafePoint(): target thread is the calling thread!");
    return 0;
  }
  auto thread_info = QueryThreadDebugInfo(thread_id);
  auto thread = thread_info->thread;

  // Now the fun part begins: Registers are only guaranteed to be synchronized
  // with the PPC context at a basic block boundary. Unfortunately, we most
  // likely stopped the thread at some point other than a boundary. We need to
  // step forward until we reach a boundary, and then perform the save.
  uint64_t frame_host_pcs[64];
  cpu::StackFrame cpu_frames[64];
  size_t count = stack_walker_->CaptureStackTrace(
      thread->thread()->native_handle(), frame_host_pcs, 0,
      xe::countof(frame_host_pcs), nullptr, nullptr);
  stack_walker_->ResolveStack(frame_host_pcs, cpu_frames, count);
  if (count == 0) {
    return 0;
  }

  auto& first_frame = cpu_frames[0];
  if (ignore_host) {
    for (size_t i = 0; i < count; i++) {
      if (cpu_frames[i].type == cpu::StackFrame::Type::kGuest &&
          cpu_frames[i].guest_pc) {
        first_frame = cpu_frames[i];
        break;
      }
    }
  }

  // Check if we're in guest code or host code.
  uint32_t pc = 0;
  if (first_frame.type == cpu::StackFrame::Type::kGuest) {
    auto& frame = first_frame;
    if (!frame.guest_pc) {
      // Lame. The guest->host thunk is a "guest" function.
      frame = cpu_frames[1];
    }

    pc = frame.guest_pc;

    // We're in guest code.
    // First: Find a synchronizing instruction and go to it.
    xe::cpu::ppc::PPCDecodeData d;
    const xe::cpu::ppc::PPCOpcodeInfo* sync_info = nullptr;
    d.address = cpu_frames[0].guest_pc - 4;
    do {
      d.address += 4;
      d.code =
          xe::load_and_swap<uint32_t>(memory()->TranslateVirtual(d.address));
      auto& opcode_info = xe::cpu::ppc::LookupOpcodeInfo(d.code);
      if (opcode_info.type == cpu::ppc::PPCOpcodeType::kSync) {
        sync_info = &opcode_info;
        break;
      }
    } while (true);

    if (d.address != pc) {
      StepToGuestAddress(thread_id, d.address);
      pc = d.address;
    }

    // Okay. Now we're on a synchronizing instruction but we need to step
    // past it in order to get a synchronized context.
    // If we're on a branching instruction, it's guaranteed only going to have
    // two possible targets. For non-branching instructions, we can just step
    // over them.
    if (sync_info->group == xe::cpu::ppc::PPCOpcodeGroup::kB) {
      pc = StepIntoGuestBranchTarget(thread_id, d.address);
    }
  } else {
    // We're in host code. Search backwards til we can get an idea of where
    // we are.
    cpu::GuestFunction* thunk_func = nullptr;
    cpu::Export* export_data = nullptr;
    uint32_t first_pc = 0;
    for (int i = 0; i < count; i++) {
      auto& frame = cpu_frames[i];
      if (frame.type == cpu::StackFrame::Type::kGuest && frame.guest_pc) {
        auto func = frame.guest_symbol.function;
        assert_true(func->is_guest());

        if (!first_pc) {
          first_pc = frame.guest_pc;
        }

        thunk_func = reinterpret_cast<cpu::GuestFunction*>(func);
        export_data = thunk_func->export_data();
        if (export_data) {
          break;
        }
      }
    }

    // If the export is blocking, we wrap up and save inside the export thunk.
    // When we're restored, we'll call the blocking export again.
    // Otherwise, we return from the thunk and save.
    if (export_data && export_data->tags & cpu::ExportTag::kBlocking) {
      pc = thunk_func->address();
    } else if (export_data) {
      // Non-blocking. Run until we return from the thunk.
      pc = static_cast<uint32_t>(thread->thread_state()->context()->lr);
      StepToGuestAddress(thread_id, pc);
    } else if (first_pc) {
      // We're in the MMIO handler/mfmsr/something calling out of the guest
      // that doesn't use an export. If the current instruction is
      // synchronizing, we can just save here. Otherwise, step forward
      // (and call ourselves again so we run the correct logic).
      uint32_t code =
          xe::load_and_swap<uint32_t>(memory()->TranslateVirtual(first_pc));
      auto& opcode_info = xe::cpu::ppc::LookupOpcodeInfo(code);
      if (opcode_info.type == xe::cpu::ppc::PPCOpcodeType::kSync) {
        // Good to go.
        pc = first_pc;
      } else {
        // Step forward and run this logic again.
        StepToGuestAddress(thread_id, first_pc + 4);
        return StepToGuestSafePoint(thread_id, true);
      }
    } else {
      // We've managed to catch a thread before it called into the guest.
      // Set a breakpoint on its startup procedure and capture it there.
      // TODO(DrChat): Reimplement
      assert_always("Unimplemented");
      /*
      auto creation_params = thread->creation_params();
      pc = creation_params->xapi_thread_startup
               ? creation_params->xapi_thread_startup
               : creation_params->start_address;
      StepToGuestAddress(thread_id, pc);
      */
    }
  }

  return pc;
}

bool TestPpcCondition(const xe::cpu::ppc::PPCContext* context, uint32_t bo,
                      uint32_t bi, bool check_ctr, bool check_cond) {
  bool ctr_ok = true;
  if (check_ctr) {
    if (select_bits(bo, 2, 2)) {
      ctr_ok = true;
    } else {
      uint32_t new_ctr_value = static_cast<uint32_t>(context->ctr - 1);
      if (select_bits(bo, 1, 1)) {
        ctr_ok = new_ctr_value == 0;
      } else {
        ctr_ok = new_ctr_value != 0;
      }
    }
  }
  bool cond_ok = true;
  if (check_cond) {
    if (select_bits(bo, 4, 4)) {
      cond_ok = true;
    } else {
      uint8_t cr = *(reinterpret_cast<const uint8_t*>(&context->cr0) +
                     (4 * (bi >> 2)) + (bi & 3));
      if (select_bits(bo, 3, 3)) {
        cond_ok = cr != 0;
      } else {
        cond_ok = cr == 0;
      }
    }
  }
  return ctr_ok && cond_ok;
}

uint32_t Processor::CalculateNextGuestInstruction(ThreadDebugInfo* thread_info,
                                                  uint32_t current_pc) {
  xe::cpu::ppc::PPCDecodeData d;
  d.address = current_pc;
  d.code = xe::load_and_swap<uint32_t>(memory_->TranslateVirtual(d.address));
  auto opcode = xe::cpu::ppc::LookupOpcode(d.code);
  if (d.code == 0x4E800020) {
    // blr -- unconditional branch to LR.
    uint32_t target_pc = static_cast<uint32_t>(thread_info->guest_context.lr);
    return target_pc;
  } else if (d.code == 0x4E800420) {
    // bctr -- unconditional branch to CTR.
    uint32_t target_pc = static_cast<uint32_t>(thread_info->guest_context.ctr);
    return target_pc;
  } else if (opcode == PPCOpcode::bx) {
    // b/ba/bl/bla
    uint32_t target_pc = d.I.ADDR();
    return target_pc;
  } else if (opcode == PPCOpcode::bcx) {
    // bc/bca/bcl/bcla
    uint32_t target_pc = d.B.ADDR();
    bool test_passed = TestPpcCondition(&thread_info->guest_context, d.B.BO(),
                                        d.B.BI(), true, true);
    return test_passed ? target_pc : current_pc + 4;
  } else if (opcode == PPCOpcode::bclrx) {
    // bclr/bclrl
    uint32_t target_pc = static_cast<uint32_t>(thread_info->guest_context.lr);
    bool test_passed = TestPpcCondition(&thread_info->guest_context, d.XL.BO(),
                                        d.XL.BI(), true, true);
    return test_passed ? target_pc : current_pc + 4;
  } else if (opcode == PPCOpcode::bcctrx) {
    // bcctr/bcctrl
    uint32_t target_pc = static_cast<uint32_t>(thread_info->guest_context.ctr);
    bool test_passed = TestPpcCondition(&thread_info->guest_context, d.XL.BO(),
                                        d.XL.BI(), false, true);
    return test_passed ? target_pc : current_pc + 4;
  } else {
    return current_pc + 4;
  }
}

}  // namespace cpu
}  // namespace xe
