/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/game_metadata.h"

#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/mapped_memory.h"
#include "xenia/base/utf8.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/util/xdbf_utils.h"
#include "xenia/kernel/util/xex2_info.h"
#include "xenia/vfs/devices/disc_image_device.h"
#include "xenia/vfs/devices/disc_image_entry.h"

#if XE_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4244 4456 4457 4702)
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "third_party/stb/stb_image.h"
#if XE_COMPILER_MSVC
#pragma warning(pop)
#endif

namespace xe {
namespace app {

namespace {

// Bump when the cache format or what's extracted changes.
constexpr char kCacheMagic[] = "xenia-dashboard-meta 2";

// Finds the XDBF resource in a decoded image - named after the title ID, as
// KernelState::module_xdbf looks it up, or failing that any resource that
// is XDBF data.
bool FindXdbf(const xex2_header* header, const std::vector<uint8_t>& image,
              uint32_t base_address, uint32_t title_id, const uint8_t** data,
              size_t* size) {
  xex2_opt_resource_info* resource_info = nullptr;
  if (!cpu::XexModule::GetOptHeader(header, XEX_HEADER_RESOURCE_INFO,
                                    &resource_info) ||
      !resource_info || resource_info->size < 4) {
    return false;
  }
  uint32_t count = (resource_info->size - 4) / sizeof(xex2_resource);
  std::string title_id_name = fmt::format("{:08X}", title_id);
  const uint8_t* fallback_data = nullptr;
  size_t fallback_size = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const xex2_resource& resource = resource_info->resources[i];
    uint32_t address = resource.address;
    uint32_t resource_size = resource.size;
    if (address < base_address ||
        size_t(address - base_address) + resource_size > image.size()) {
      continue;
    }
    const uint8_t* resource_data = image.data() + (address - base_address);
    if (std::strncmp(resource.name, title_id_name.c_str(),
                     sizeof(resource.name)) == 0) {
      *data = resource_data;
      *size = resource_size;
      return true;
    }
    if (!fallback_data && resource_size >= 4 &&
        std::memcmp(resource_data, "XDBF", 4) == 0) {
      fallback_data = resource_data;
      fallback_size = resource_size;
    }
  }
  if (fallback_data) {
    *data = fallback_data;
    *size = fallback_size;
    return true;
  }
  return false;
}

bool DecodeIconPng(const uint8_t* png, size_t png_size, GameMetadata* out) {
  return DecodeImageRgba(png, png_size, &out->icon_rgba, &out->icon_width,
                         &out->icon_height);
}

// Reads metadata from an in-memory default.xex. `icon_png` gets the icon's
// original PNG bytes, for the cache.
bool ReadFromXex(const uint8_t* xex, size_t xex_size, GameMetadata* out,
                 std::vector<uint8_t>* icon_png) {
  std::vector<uint8_t> image;
  uint32_t base_address = 0;
  if (!cpu::XexModule::DecodeImageToHost(xex, xex_size, &image,
                                         &base_address)) {
    return false;
  }
  auto header = reinterpret_cast<const xex2_header*>(xex);
  xex2_opt_execution_info* execution_info = nullptr;
  if (cpu::XexModule::GetOptHeader(header, XEX_HEADER_EXECUTION_INFO,
                                   &execution_info) &&
      execution_info) {
    out->title_id = execution_info->title_id;
  }
  const uint8_t* xdbf_data = nullptr;
  size_t xdbf_size = 0;
  if (!FindXdbf(header, image, base_address, out->title_id, &xdbf_data,
                &xdbf_size)) {
    return false;
  }
  kernel::util::XdbfGameData db(xdbf_data, xdbf_size);
  if (!db.is_valid()) {
    return false;
  }
  // English when the game has it (some, like Lost Odyssey, default to
  // Japanese, which the dashboard font can't draw), else its default.
  out->title = db.title(XLanguage::kEnglish);
  if (out->title.empty()) {
    out->title = db.title();
  }
  kernel::util::XdbfBlock icon = db.icon();
  if (icon && icon.size) {
    icon_png->assign(icon.buffer, icon.buffer + icon.size);
    DecodeIconPng(icon_png->data(), icon_png->size(), out);
  }
  return !out->title.empty() || !out->icon_rgba.empty();
}

bool ReadGameMetadata(const std::filesystem::path& game_path,
                      GameMetadata* out, std::vector<uint8_t>* icon_png) {
  auto extension =
      xe::utf8::lower_ascii(xe::path_to_utf8(game_path.extension()));
  if (extension == ".iso") {
    vfs::DiscImageDevice device("\\Device\\Cdrom0", game_path);
    if (!device.Initialize()) {
      return false;
    }
    auto entry =
        dynamic_cast<vfs::DiscImageEntry*>(device.ResolvePath("default.xex"));
    if (!entry || !entry->mmap() ||
        entry->data_offset() + entry->data_size() > entry->mmap()->size()) {
      return false;
    }
    return ReadFromXex(entry->mmap()->data() + entry->data_offset(),
                       entry->data_size(), out, icon_png);
  }
  // An extracted game's default.xex, or a loose .xex.
  auto mapping =
      xe::MappedMemory::Open(game_path, xe::MappedMemory::Mode::kRead);
  if (!mapping) {
    return false;
  }
  return ReadFromXex(mapping->data(), mapping->size(), out, icon_png);
}

std::string CacheKey(const std::filesystem::path& game_path) {
  std::error_code ec;
  uint64_t size = std::filesystem::is_regular_file(game_path, ec)
                      ? std::filesystem::file_size(game_path, ec)
                      : 0;
  auto modified = std::filesystem::last_write_time(game_path, ec);
  std::string identity =
      fmt::format("{}|{}|{}", xe::path_to_utf8(game_path), size,
                  ec ? 0 : modified.time_since_epoch().count());
  return fmt::format("{:016x}", uint64_t(std::hash<std::string>()(identity)));
}

}  // namespace

bool DecodeImageRgba(const uint8_t* data, size_t size,
                     std::vector<uint8_t>* out_rgba, uint32_t* out_width,
                     uint32_t* out_height) {
  int width = 0, height = 0, components = 0;
  stbi_uc* pixels = stbi_load_from_memory(data, int(size), &width, &height,
                                          &components, 4);
  if (!pixels) {
    return false;
  }
  *out_width = uint32_t(width);
  *out_height = uint32_t(height);
  out_rgba->assign(pixels, pixels + size_t(width) * height * 4);
  stbi_image_free(pixels);
  return true;
}

bool LoadGameMetadataCached(const std::filesystem::path& game_path,
                            const std::filesystem::path& cache_dir,
                            GameMetadata* out) {
  *out = GameMetadata();
  std::string key = CacheKey(game_path);
  std::filesystem::path info_path = cache_dir / (key + ".txt");
  std::filesystem::path icon_path = cache_dir / (key + ".png");

  // Cached: "<magic>\n<title id hex or none>\n<title>\n", plus <key>.png.
  {
    std::ifstream info(info_path, std::ios::binary);
    std::string magic, title_id, title;
    if (info && std::getline(info, magic) && magic == kCacheMagic &&
        std::getline(info, title_id)) {
      if (title_id == "none") {
        return false;
      }
      std::getline(info, title);
      out->title = title;
      out->title_id = uint32_t(std::strtoul(title_id.c_str(), nullptr, 16));
      std::ifstream icon(icon_path, std::ios::binary);
      if (icon) {
        std::vector<uint8_t> png((std::istreambuf_iterator<char>(icon)),
                                 std::istreambuf_iterator<char>());
        DecodeIconPng(png.data(), png.size(), out);
      }
      return true;
    }
  }

  std::vector<uint8_t> icon_png;
  bool found = ReadGameMetadata(game_path, out, &icon_png);
  XELOGI("Dashboard: metadata for {}: {}", xe::path_to_utf8(game_path),
         found ? fmt::format("{:08X} \"{}\"", out->title_id, out->title)
               : std::string("none found"));

  std::error_code ec;
  if (!found && !std::filesystem::exists(game_path, ec)) {
    // Probably a drive that's temporarily unavailable - don't remember
    // "nothing found" for it.
    return false;
  }
  std::filesystem::create_directories(cache_dir, ec);
  std::ofstream info(info_path, std::ios::binary | std::ios::trunc);
  if (!found) {
    info << kCacheMagic << "\nnone\n";
    return false;
  }
  // Titles are single-line in practice - make sure of it for the format.
  std::string title = out->title;
  for (char& c : title) {
    if (c == '\n' || c == '\r') {
      c = ' ';
    }
  }
  info << kCacheMagic << "\n"
       << fmt::format("{:08X}", out->title_id) << "\n"
       << title << "\n";
  if (!icon_png.empty()) {
    std::ofstream icon(icon_path, std::ios::binary | std::ios::trunc);
    icon.write(reinterpret_cast<const char*>(icon_png.data()),
               std::streamsize(icon_png.size()));
  }
  return true;
}

}  // namespace app
}  // namespace xe
