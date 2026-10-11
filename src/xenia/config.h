/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CONFIG_H_
#define XENIA_CONFIG_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace config {
void SetupConfig(const std::filesystem::path& config_folder);
void LoadGameConfig(const std::string_view title_id);
// An integer a game's config sets ("GPU.draw_resolution_scale_x", say),
// without loading it - none if the game has no config or it doesn't set it.
std::optional<int64_t> GetGameConfigInteger(const std::string_view title_id,
                                            const std::string_view key);
// The same for a string ("GPU.gpu").
std::optional<std::string> GetGameConfigString(const std::string_view title_id,
                                               const std::string_view key);
void SaveConfig();
}  // namespace config

#endif  // XENIA_CONFIG_H_
