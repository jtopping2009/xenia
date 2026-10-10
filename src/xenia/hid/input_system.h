/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_INPUT_SYSTEM_H_
#define XENIA_HID_INPUT_SYSTEM_H_

#include <atomic>
#include <memory>
#include <vector>

#include "xenia/hid/input.h"
#include "xenia/hid/input_driver.h"
#include "xenia/xbox.h"

namespace xe {
namespace ui {
class Window;
}  // namespace ui
}  // namespace xe

namespace xe {
namespace hid {

class InputSystem {
 public:
  explicit InputSystem(xe::ui::Window* window);
  ~InputSystem();

  xe::ui::Window* window() const { return window_; }

  X_STATUS Setup();

  void AddDriver(std::unique_ptr<InputDriver> driver);

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps);
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state);
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration);
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke);

  // While set, the guest sees idle controllers and no keystrokes (see
  // xam_input.cc), like a real console while the Xbox Guide is open. Host UI
  // still reads the real state through GetState/GetKeystroke.
  void set_guest_input_blocked(bool blocked) {
    guest_input_blocked_.store(blocked, std::memory_order_relaxed);
  }
  bool guest_input_blocked() const {
    return guest_input_blocked_.load(std::memory_order_relaxed);
  }

 private:
  xe::ui::Window* window_ = nullptr;
  std::atomic<bool> guest_input_blocked_{false};

  std::vector<std::unique_ptr<InputDriver>> drivers_;
};

}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_INPUT_SYSTEM_H_
