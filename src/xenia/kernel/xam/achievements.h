/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_ACHIEVEMENTS_H_
#define XENIA_KERNEL_XAM_ACHIEVEMENTS_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "xenia/kernel/util/xdbf_utils.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
class KernelState;
}  // namespace kernel
}  // namespace xe

namespace xe {
namespace kernel {
namespace xam {

// XACHIEVEMENT_DETAILS flags.
constexpr uint32_t kAchievementShowUnachieved = 0x8;
constexpr uint32_t kAchievementAchievedOnline = 0x10000;
constexpr uint32_t kAchievementAchieved = 0x20000;

// An achievement as the game defines it, in its XDBF.
struct AchievementInfo {
  uint32_t id = 0;
  std::string label;
  std::string description;
  // Shown instead of the description while it's locked.
  std::string unachieved;
  uint32_t image_id = 0;
  uint32_t gamerscore = 0;
  uint32_t flags = 0;
  // Locked secret achievements show neither their name nor description.
  bool secret() const { return !(flags & kAchievementShowUnachieved); }
};

// The game's achievements, in `language` where it has it (else its default).
std::vector<AchievementInfo> ReadAchievements(const util::XdbfGameData& db,
                                              XLanguage language);

// What's been unlocked in a game, as saved under the storage root - one file
// per title, `achievements/<title id>.txt`. Local only: there's no Xbox Live
// to sync with.
struct AchievementProgress {
  struct Unlock {
    uint32_t id = 0;
    // Seconds since the Unix epoch.
    int64_t time = 0;
  };
  std::string title;
  // The game's totals, saved with the first unlock - for showing progress
  // without the game's XDBF to hand.
  uint32_t total_count = 0;
  uint32_t total_gamerscore = 0;
  uint32_t unlocked_gamerscore = 0;
  std::vector<Unlock> unlocked;
  const Unlock* Find(uint32_t id) const;
};
// False (and empty `out`) if nothing has been unlocked in the game.
bool LoadAchievementProgress(const std::filesystem::path& storage_root,
                             uint32_t title_id, AchievementProgress* out);

// A Unix time as a big-endian FILETIME's two halves (high, then low), as
// XACHIEVEMENT_DETAILS::ftAchieved is laid out.
void UnixTimeToGuestFileTime(int64_t time, uint32_t* high, uint32_t* low);

// Unlocks achievements in the running title (XUserWriteAchievements),
// saving any new ones and queueing a notification for each.
void UnlockAchievements(KernelState* kernel_state,
                        const std::vector<uint32_t>& ids);

// A newly unlocked achievement, for the UI to announce.
struct AchievementNotification {
  std::string label;
  uint32_t gamerscore = 0;
  // The achievement's picture, as the PNG from the XDBF (may be empty).
  std::vector<uint8_t> icon_png;
};
// Takes the notifications queued since the last call.
std::vector<AchievementNotification> TakeAchievementNotifications();

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_ACHIEVEMENTS_H_
