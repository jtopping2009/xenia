/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_SENDSPIN_SENDSPIN_FLAGS_H_
#define XENIA_APU_SENDSPIN_SENDSPIN_FLAGS_H_

#include <string>

#include "xenia/base/cvar.h"

// Base64 (standard, padded) encoding of the 32-byte X25519 identity private
// key used as this Sendspin player's Noise static key. Generated once and
// persisted here on first connect; the resulting public key (base64url,
// unpadded) is this player's client_id, which must stay stable across
// reconnects for the server to recognize a previously-paired device.
DECLARE_string(sendspin_identity_key);

// Directly assigning cvars::sendspin_identity_key only changes the in-memory
// value - it does not touch ConfigVar's separate config_value_ used when
// writing the file back out, so a runtime-generated key would silently
// vanish on every restart. This goes through ConfigVar<T>::SetConfigValue()
// (from within sendspin_flags.cc, since cv::cv_sendspin_identity_key has
// internal linkage) and then saves the config file.
void PersistSendspinIdentityKey(const std::string& value);

#endif  // XENIA_APU_SENDSPIN_SENDSPIN_FLAGS_H_
