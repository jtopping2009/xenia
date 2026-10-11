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

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
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

// A game's entry in the LaunchBox Games Database
// (https://gamesdb.launchbox-app.com/), for the dashboard's box art: its
// pictures (file names on images.launchbox-app.com, empty when it has none)
// and details.
struct LaunchBoxGame {
  std::string name;
  std::string box_front;
  // Background art, behind the dashboard's covers.
  std::string fanart;
  std::string release_date;  // YYYY-MM-DD, or less.
  std::string developer;
  std::string publisher;
  std::string genres;  // "; "-separated.
  std::string players;
  std::string overview;
};

// The database's Xbox 360 games, by title. Built from LaunchBox's whole
// database (a ~100 MB download, no account needed) into a small index cached
// under `cache_dir`, and rebuilt when that's a month old.
class LaunchBoxIndex {
 public:
  // Loads the index, building it first if needed - minutes the first time,
  // so off the UI thread. `stop` cancels a download. False if it couldn't be
  // built (offline, say).
  bool Load(const std::filesystem::path& cache_dir,
            const std::atomic<bool>& stop);
  // The game whose name (or an alternate one) matches `title` - ignoring
  // case, punctuation, bracketed tags and "the"/"and", then trying just the
  // part before a " - ", ": " or " ~ " (a subtitle, or another region's
  // name). Null if none.
  const LaunchBoxGame* Find(const std::string& title) const;

 private:
  std::vector<LaunchBoxGame> games_;
  std::unordered_map<std::string, size_t> by_key_;
};

// A LaunchBox picture, downloaded once into `cache_dir` and decoded to RGBA8,
// scaled down (keeping its shape) to fit max_width x max_height.
bool LoadLaunchBoxImage(const std::string& file_name,
                        const std::filesystem::path& cache_dir,
                        uint32_t max_width, uint32_t max_height,
                        std::vector<uint8_t>* out_rgba, uint32_t* out_width,
                        uint32_t* out_height);

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_GAME_METADATA_H_
