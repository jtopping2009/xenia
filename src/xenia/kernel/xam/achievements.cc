/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/achievements.h"

#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"

namespace xe {
namespace kernel {
namespace xam {

namespace {

constexpr char kProgressMagic[] = "xenia-achievements 1";

// Guards the progress files (unlocks come from guest threads) and the
// notification queue.
std::mutex achievements_mutex;
std::vector<AchievementNotification> pending_notifications;

std::filesystem::path ProgressPath(const std::filesystem::path& storage_root,
                                   uint32_t title_id) {
  return storage_root / "achievements" / fmt::format("{:08X}.txt", title_id);
}

// "<magic>\n<title>\n<total count> <total gamerscore>\n" then a line per
// unlock: "<id> <unix time> <gamerscore>".
bool LoadProgressLocked(const std::filesystem::path& path,
                        AchievementProgress* out,
                        std::vector<uint32_t>* unlock_gamerscores) {
  std::ifstream file(path, std::ios::binary);
  std::string magic, totals;
  if (!file || !std::getline(file, magic) || magic != kProgressMagic ||
      !std::getline(file, out->title) || !std::getline(file, totals)) {
    return false;
  }
  std::istringstream(totals) >> out->total_count >> out->total_gamerscore;
  std::string line;
  while (std::getline(file, line)) {
    std::istringstream fields(line);
    AchievementProgress::Unlock unlock;
    uint32_t gamerscore = 0;
    if (!(fields >> unlock.id >> unlock.time)) {
      continue;
    }
    fields >> gamerscore;
    out->unlocked.push_back(unlock);
    out->unlocked_gamerscore += gamerscore;
    if (unlock_gamerscores) {
      unlock_gamerscores->push_back(gamerscore);
    }
  }
  return true;
}

}  // namespace

std::vector<AchievementInfo> ReadAchievements(const util::XdbfGameData& db,
                                              XLanguage language) {
  std::vector<AchievementInfo> achievements;
  if (!db.is_valid()) {
    return achievements;
  }
  language = db.GetExistingLanguage(language);
  for (const util::XdbfAchievementTableEntry& entry : db.GetAchievements()) {
    AchievementInfo info;
    info.id = entry.id;
    info.label = db.GetStringTableEntry(language, entry.label_id);
    info.description = db.GetStringTableEntry(language, entry.description_id);
    info.unachieved = db.GetStringTableEntry(language, entry.unachieved_id);
    info.image_id = entry.image_id;
    info.gamerscore = entry.gamerscore;
    info.flags = entry.flags;
    achievements.push_back(std::move(info));
  }
  return achievements;
}

const AchievementProgress::Unlock* AchievementProgress::Find(
    uint32_t id) const {
  for (const Unlock& unlock : unlocked) {
    if (unlock.id == id) {
      return &unlock;
    }
  }
  return nullptr;
}

bool LoadAchievementProgress(const std::filesystem::path& storage_root,
                             uint32_t title_id, AchievementProgress* out) {
  *out = AchievementProgress();
  std::lock_guard<std::mutex> lock(achievements_mutex);
  if (!LoadProgressLocked(ProgressPath(storage_root, title_id), out,
                          nullptr)) {
    *out = AchievementProgress();
    return false;
  }
  return true;
}

void UnixTimeToGuestFileTime(int64_t time, uint32_t* high, uint32_t* low) {
  // 100ns intervals since 1601.
  uint64_t file_time = uint64_t(time + 11644473600ll) * 10000000ull;
  *high = uint32_t(file_time >> 32);
  *low = uint32_t(file_time);
}

void UnlockAchievements(KernelState* kernel_state,
                        const std::vector<uint32_t>& ids) {
  Emulator* emulator = kernel_state->emulator();
  uint32_t title_id = emulator->title_id();
  if (!title_id || ids.empty()) {
    return;
  }
  // English where the game has it - the UI's font can't draw every
  // language's script.
  util::XdbfGameData db = kernel_state->title_xdbf();
  std::vector<AchievementInfo> achievements =
      ReadAchievements(db, XLanguage::kEnglish);

  std::lock_guard<std::mutex> lock(achievements_mutex);
  std::filesystem::path path = ProgressPath(emulator->storage_root(), title_id);
  AchievementProgress progress;
  std::vector<uint32_t> unlock_gamerscores;
  LoadProgressLocked(path, &progress, &unlock_gamerscores);
  progress.title = emulator->title_name();
  progress.total_count = uint32_t(achievements.size());
  progress.total_gamerscore = 0;
  for (const AchievementInfo& info : achievements) {
    progress.total_gamerscore += info.gamerscore;
  }

  bool changed = false;
  int64_t now = int64_t(std::time(nullptr));
  for (uint32_t id : ids) {
    if (progress.Find(id)) {
      continue;  // Games unlock the same one again freely.
    }
    const AchievementInfo* info = nullptr;
    for (const AchievementInfo& candidate : achievements) {
      if (candidate.id == id) {
        info = &candidate;
        break;
      }
    }
    if (!info) {
      XELOGW("Achievements: {:08X} unlocked unknown achievement {}", title_id,
             id);
      continue;
    }
    progress.unlocked.push_back({id, now});
    unlock_gamerscores.push_back(info->gamerscore);
    progress.unlocked_gamerscore += info->gamerscore;
    changed = true;
    XELOGI("Achievements: unlocked \"{}\" ({}G) in {:08X}", info->label,
           info->gamerscore, title_id);

    AchievementNotification notification;
    notification.label = info->label;
    notification.gamerscore = info->gamerscore;
    util::XdbfBlock image = db.GetEntry(util::XdbfSection::kImage,
                                        info->image_id);
    if (image) {
      notification.icon_png.assign(image.buffer, image.buffer + image.size);
    }
    pending_notifications.push_back(std::move(notification));
  }
  if (!changed) {
    return;
  }

  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  // Written beside and then moved over, so a crash mid-write can't lose the
  // unlocks already saved.
  std::filesystem::path temp_path = path;
  temp_path += ".tmp";
  {
    std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
    std::string title = progress.title;
    for (char& c : title) {
      if (c == '\n' || c == '\r') {
        c = ' ';
      }
    }
    file << kProgressMagic << "\n"
         << title << "\n"
         << progress.total_count << " " << progress.total_gamerscore << "\n";
    for (size_t i = 0; i < progress.unlocked.size(); ++i) {
      file << progress.unlocked[i].id << " " << progress.unlocked[i].time
           << " " << unlock_gamerscores[i] << "\n";
    }
    if (!file) {
      XELOGE("Achievements: couldn't write {}", xe::path_to_utf8(temp_path));
      return;
    }
  }
  std::filesystem::rename(temp_path, path, ec);
  if (ec) {
    XELOGE("Achievements: couldn't save {}: {}", xe::path_to_utf8(path),
           ec.message());
  }
}

std::vector<AchievementNotification> TakeAchievementNotifications() {
  std::lock_guard<std::mutex> lock(achievements_mutex);
  std::vector<AchievementNotification> notifications;
  notifications.swap(pending_notifications);
  return notifications;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
