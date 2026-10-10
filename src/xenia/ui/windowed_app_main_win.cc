/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstdlib>
#include <iterator>
#include <memory>

#include "xenia/base/console.h"
#include "xenia/base/cvar.h"
#include "xenia/base/main_win.h"
#include "xenia/base/platform_win.h"
#include "xenia/ui/windowed_app.h"
#include "xenia/ui/windowed_app_context_win.h"

DEFINE_bool(enable_console, false, "Open a console window with the main window",
            "General");

int WINAPI wWinMain(HINSTANCE hinstance, HINSTANCE hinstance_prev,
                    LPWSTR command_line, int show_cmd) {
  int result;

  // Set by a previous Xenia instance relaunching itself (see
  // EmulatorWindow::RelaunchXenia) - wait for it to fully exit before touching
  // anything it may still hold (xenia.log, config, shader storage, audio and
  // controller devices), since all of those are opened well before
  // OnInitialize. Bounded so a stuck old process can't block startup forever.
  {
    wchar_t wait_pid_str[32];
    DWORD wait_pid_len = GetEnvironmentVariableW(
        L"XENIA_RELAUNCH_WAIT_PID", wait_pid_str, DWORD(std::size(wait_pid_str)));
    if (wait_pid_len && wait_pid_len < std::size(wait_pid_str)) {
      SetEnvironmentVariableW(L"XENIA_RELAUNCH_WAIT_PID", nullptr);
      DWORD wait_pid = DWORD(wcstoul(wait_pid_str, nullptr, 10));
      HANDLE old_process = wait_pid ? OpenProcess(SYNCHRONIZE, FALSE, wait_pid)
                                    : nullptr;
      if (old_process) {
        WaitForSingleObject(old_process, 15000);
        CloseHandle(old_process);
      }
    }
  }

  {
    xe::ui::Win32WindowedAppContext app_context(hinstance, show_cmd);
    // TODO(Triang3l): Initialize creates a window. Set DPI awareness via the
    // manifest.
    if (!app_context.Initialize()) {
      return EXIT_FAILURE;
    }

    std::unique_ptr<xe::ui::WindowedApp> app =
        xe::ui::GetWindowedAppCreator()(app_context);

    if (!xe::ParseWin32LaunchArguments(false, app->GetPositionalOptionsUsage(),
                                       app->GetPositionalOptions(), nullptr)) {
      return EXIT_FAILURE;
    }

    // TODO(Triang3l): Rework this, need to initialize the console properly,
    // disable has_console_attached_ by default in windowed apps, and attach
    // only if needed.
    if (cvars::enable_console) {
      xe::AttachConsole();
    }

    // Initialize COM on the UI thread with the apartment-threaded concurrency
    // model, so dialogs can be used.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
      return EXIT_FAILURE;
    }

    xe::InitializeWin32App(app->GetName());

    result =
        app->OnInitialize() ? app_context.RunMainMessageLoop() : EXIT_FAILURE;

    app->InvokeOnDestroy();
  }

  // Logging may still be needed in the destructors.
  xe::ShutdownWin32App();

  CoUninitialize();

  return result;
}
