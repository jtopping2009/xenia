/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_GAME_METADATA_H_
#define XENIA_APP_GAME_METADATA_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace xe {
namespace app {

// What the dashboard shows for a game that isn't running: its official title
// and icon, read out of the XDBF resource embedded in its default.xex.
struct GameMetadata {
  // In the game's default language; may be empty if the XDBF has none.
  std::string title;
  uint32_t title_id = 0;
  // The title's icon (normally 64x64) decoded to RGBA8; empty if it has none.
  std::vector<uint8_t> icon_rgba;
  uint32_t icon_width = 0;
  uint32_t icon_height = 0;
};

// Decodes a PNG or JPEG to RGBA8.
bool DecodeImageRgba(const uint8_t* data, size_t size,
                     std::vector<uint8_t>* out_rgba, uint32_t* out_width,
                     uint32_t* out_height);

// Reads the metadata for a game on the dashboard's Games list - a disc image
// (.iso), an extracted game's default.xex, or a loose .xex - caching the
// result (including "nothing found") under `cache_dir`, keyed by the file's
// path, size and modification time. Uncached, this reads and decrypts the
// whole default.xex (tens of MB, slow on a network drive), so call it off the
// UI and presentation threads. Returns false if nothing could be read.
bool LoadGameMetadataCached(const std::filesystem::path& game_path,
                            const std::filesystem::path& cache_dir,
                            GameMetadata* out);

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_GAME_METADATA_H_
