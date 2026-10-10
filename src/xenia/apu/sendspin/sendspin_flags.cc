/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/sendspin/sendspin_flags.h"

#include "xenia/config.h"

DEFINE_string(sendspin_identity_key, "",
              "Base64-encoded X25519 identity private key for this "
              "Sendspin player. Auto-generated and persisted on first "
              "connect - do not edit by hand, or the server will treat "
              "this as a new, unpaired device.",
              "Sendspin");

void PersistSendspinIdentityKey(const std::string& value) {
  auto* config_var =
      dynamic_cast<cvar::ConfigVar<std::string>*>(cv::cv_sendspin_identity_key);
  config_var->SetConfigValue(value);
  config::SaveConfig();
}
