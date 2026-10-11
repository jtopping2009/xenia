/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/game_metadata.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/mapped_memory.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"
#include "xenia/base/utf8.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/util/xdbf_utils.h"
#include "xenia/kernel/util/xex2_info.h"
#include "xenia/vfs/devices/disc_image_device.h"
#include "xenia/vfs/devices/disc_image_entry.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
// After windows.h.
#include <urlmon.h>
#pragma comment(lib, "urlmon.lib")
#endif  // XE_PLATFORM_WIN32

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

namespace {

constexpr char kLaunchBoxUrl[] =
    "https://gamesdb.launchbox-app.com/Metadata.zip";
constexpr char kLaunchBoxImageUrl[] = "https://images.launchbox-app.com/";
constexpr char kLaunchBoxPlatform[] = "Microsoft Xbox 360";
// The index's first line - bump when its format or contents change.
constexpr char kLaunchBoxIndexMagic[] = "xenia-launchbox-xbox360 2";
constexpr auto kLaunchBoxIndexMaxAge = std::chrono::hours(24 * 30);

// Downloads `url` to `path`. The download can't be interrupted part way, but
// a stopped caller just doesn't wait on the result.
bool Download(const std::string& url, const std::filesystem::path& path) {
#if XE_PLATFORM_WIN32
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::u16string wide_url = xe::to_utf16(url);
  return SUCCEEDED(URLDownloadToFileW(
      nullptr, reinterpret_cast<const wchar_t*>(wide_url.c_str()),
      path.c_str(), 0, nullptr));
#else
  return false;
#endif  // XE_PLATFORM_WIN32
}

// Takes `member` out of the zip at `zip` into the zip's folder, with the tar
// Windows ships (which reads zips).
bool Unzip(const std::filesystem::path& zip, const std::string& member) {
#if XE_PLATFORM_WIN32
  wchar_t system[MAX_PATH];
  if (!GetSystemDirectoryW(system, MAX_PATH)) {
    return false;
  }
  std::wstring command = L"\"" +
                         (std::filesystem::path(system) / "tar.exe").wstring() +
                         L"\" -xf \"" + zip.filename().wstring() + L"\" " +
                         std::wstring(member.begin(), member.end());
  STARTUPINFOW startup = {sizeof(startup)};
  PROCESS_INFORMATION process = {};
  std::wstring folder = zip.parent_path().wstring();
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, folder.c_str(), &startup,
                      &process)) {
    return false;
  }
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 1;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return exit_code == 0;
#else
  return false;
#endif  // XE_PLATFORM_WIN32
}

// Undoes the XML escapes LaunchBox's values use.
std::string XmlUnescape(const std::string& text) {
  if (text.find('&') == std::string::npos) {
    return text;
  }
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '&') {
      out += text[i];
      continue;
    }
    size_t end = text.find(';', i);
    if (end == std::string::npos || end - i > 10) {
      out += text[i];
      continue;
    }
    std::string entity = text.substr(i + 1, end - i - 1);
    uint32_t code = 0;
    if (entity == "amp") {
      code = '&';
    } else if (entity == "lt") {
      code = '<';
    } else if (entity == "gt") {
      code = '>';
    } else if (entity == "quot") {
      code = '"';
    } else if (entity == "apos") {
      code = '\'';
    } else if (entity.size() > 1 && entity[0] == '#') {
      code = entity[1] == 'x' || entity[1] == 'X'
                 ? uint32_t(std::strtoul(entity.c_str() + 2, nullptr, 16))
                 : uint32_t(std::strtoul(entity.c_str() + 1, nullptr, 10));
    }
    if (!code) {
      out += text[i];
      continue;
    }
    // As UTF-8.
    if (code < 0x80) {
      out += char(code);
    } else if (code < 0x800) {
      out += char(0xC0 | (code >> 6));
      out += char(0x80 | (code & 0x3F));
    } else {
      out += char(0xE0 | (code >> 12));
      out += char(0x80 | ((code >> 6) & 0x3F));
      out += char(0x80 | (code & 0x3F));
    }
    i = end;
  }
  return out;
}

// The words of a title that matter for matching: lower case, without
// bracketed tags, apostrophes, punctuation, "the" and "and" ("&").
std::string TitleKey(const std::string& title) {
  std::string text;
  int depth = 0;
  for (char c : title) {
    if (c == '(' || c == '[') {
      ++depth;
    } else if (c == ')' || c == ']') {
      depth = std::max(0, depth - 1);
    } else if (!depth && c != '\'') {
      text += c == '&' ? ' ' : c;
    }
  }
  std::string key, word;
  auto flush = [&]() {
    if (!word.empty() && word != "the" && word != "and") {
      if (!key.empty()) {
        key += ' ';
      }
      key += word;
    }
    word.clear();
  };
  for (char c : text) {
    if (std::isalnum(uint8_t(c))) {
      word += char(std::tolower(uint8_t(c)));
    } else if (uint8_t(c) < 0x80) {
      flush();
    } else {
      word += c;  // Accented letters, as they are.
    }
  }
  flush();
  return key;
}

// Index lines are tab-separated - keep values to one line, without tabs.
std::string IndexField(const std::string& value) {
  std::string out;
  for (char c : value) {
    if (c == '\\') {
      out += "\\\\";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\t') {
      out += ' ';
    } else if (c != '\r') {
      out += c;
    }
  }
  return out;
}

std::string IndexValue(const std::string& field) {
  std::string out;
  for (size_t i = 0; i < field.size(); ++i) {
    if (field[i] == '\\' && i + 1 < field.size()) {
      out += field[i + 1] == 'n' ? '\n' : field[i + 1];
      ++i;
    } else {
      out += field[i];
    }
  }
  return out;
}

// How much a picture's region suits a US-English dashboard - lower first.
int RegionRank(const std::string& region) {
  if (region == "North America" || region == "United States") {
    return 0;
  }
  if (region.empty() || region == "World") {
    return 1;
  }
  if (region == "Europe" || region == "United Kingdom" ||
      region == "Australia" || region == "Canada") {
    return 2;
  }
  return 3;
}

// Reads Metadata.xml a line at a time (each value is on a line of its own, or
// starts on one and runs over the next ones), writing the Xbox 360 games to
// the index at `index_path`.
bool BuildLaunchBoxIndex(const std::filesystem::path& xml_path,
                         const std::filesystem::path& index_path) {
  std::ifstream xml(xml_path, std::ios::binary);
  if (!xml) {
    return false;
  }
  struct Entry {
    LaunchBoxGame game;
    std::vector<std::string> alternate_names;
    int box_rank = 99;
  };
  std::unordered_map<std::string, Entry> games;  // By DatabaseID.
  // Records can come in any order - keep the others' parts for later.
  std::vector<std::pair<std::string, std::string>> alternate_names;
  struct Image {
    std::string id, type, region, file;
  };
  std::vector<Image> images;

  std::string record;
  std::unordered_map<std::string, std::string> fields;
  std::string open_field, open_value;
  std::string line;
  while (std::getline(xml, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!open_field.empty()) {
      // A value running over several lines.
      std::string close = "</" + open_field + ">";
      size_t end = line.find(close);
      open_value += '\n';
      open_value += end == std::string::npos ? line : line.substr(0, end);
      if (end != std::string::npos) {
        fields[open_field] = XmlUnescape(open_value);
        open_field.clear();
      }
      continue;
    }
    size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos) {
      continue;
    }
    std::string_view text(line.data() + start, line.size() - start);
    if (text == "<Game>" || text == "<GameAlternateName>" ||
        text == "<GameImage>") {
      record = std::string(text.substr(1, text.size() - 2));
      fields.clear();
      continue;
    }
    if (text == "</Game>") {
      if (fields["Platform"] == kLaunchBoxPlatform) {
        Entry& entry = games[fields["DatabaseID"]];
        LaunchBoxGame& game = entry.game;
        game.name = fields["Name"];
        game.release_date = fields["ReleaseDate"].substr(0, 10);
        game.developer = fields["Developer"];
        game.publisher = fields["Publisher"];
        game.genres = fields["Genres"];
        game.players = fields["MaxPlayers"];
        game.overview = fields["Overview"];
      }
      record.clear();
      continue;
    }
    if (text == "</GameAlternateName>") {
      alternate_names.emplace_back(fields["DatabaseID"],
                                   fields["AlternateName"]);
      record.clear();
      continue;
    }
    if (text == "</GameImage>") {
      const std::string& type = fields["Type"];
      if (type == "Box - Front" || type == "Fanart - Background") {
        images.push_back(
            {fields["DatabaseID"], type, fields["Region"], fields["FileName"]});
      }
      record.clear();
      continue;
    }
    if (record.empty() || text.size() < 3 || text[0] != '<') {
      continue;
    }
    size_t name_end = text.find('>');
    if (name_end == std::string_view::npos) {
      continue;
    }
    if (text[name_end - 1] == '/') {
      // An empty value: "<Genres />".
      continue;
    }
    // The name, without any attributes ("<AlternateName
    // xml:space="preserve">").
    std::string name(text.substr(1, name_end - 1));
    name = name.substr(0, name.find(' '));
    std::string_view rest = text.substr(name_end + 1);
    std::string close = "</" + name + ">";
    size_t end = rest.find(close);
    if (end == std::string_view::npos) {
      open_field = name;
      open_value = std::string(rest);
    } else {
      fields[name] = XmlUnescape(std::string(rest.substr(0, end)));
    }
  }
  if (games.empty()) {
    return false;
  }

  for (const auto& [id, name] : alternate_names) {
    auto it = games.find(id);
    if (it != games.end() && !name.empty()) {
      it->second.alternate_names.push_back(name);
    }
  }
  for (const Image& image : images) {
    auto it = games.find(image.id);
    if (it == games.end()) {
      continue;
    }
    Entry& entry = it->second;
    if (image.type == "Box - Front") {
      int rank = RegionRank(image.region);
      if (rank < entry.box_rank) {
        entry.box_rank = rank;
        entry.game.box_front = image.file;
      }
    } else if (entry.game.fanart.empty()) {
      entry.game.fanart = image.file;
    }
  }

  std::filesystem::path temp_path = index_path;
  temp_path += ".new";
  {
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    out << kLaunchBoxIndexMagic << "\n";
    for (const auto& [id, entry] : games) {
      const LaunchBoxGame& game = entry.game;
      std::string alternates;
      for (const std::string& name : entry.alternate_names) {
        if (!alternates.empty()) {
          alternates += '|';
        }
        alternates += IndexField(name);
      }
      out << IndexField(game.name) << '\t' << alternates << '\t'
          << IndexField(game.box_front) << '\t' << IndexField(game.fanart)
          << '\t' << IndexField(game.release_date) << '\t'
          << IndexField(game.developer) << '\t' << IndexField(game.publisher)
          << '\t' << IndexField(game.genres) << '\t'
          << IndexField(game.players) << '\t' << IndexField(game.overview)
          << '\n';
    }
    if (!out) {
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(temp_path, index_path, ec);
  XELOGI("Dashboard: LaunchBox index built: {} Xbox 360 games", games.size());
  return !ec;
}

std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    size_t tab = line.find('\t', start);
    out.push_back(line.substr(start, tab - start));
    if (tab == std::string::npos) {
      return out;
    }
    start = tab + 1;
  }
}

}  // namespace

bool LaunchBoxIndex::Load(const std::filesystem::path& cache_dir,
                          const std::atomic<bool>& stop) {
  games_.clear();
  by_key_.clear();
  std::filesystem::path index_path = cache_dir / "launchbox_xbox360.tsv";
  std::error_code ec;
  std::filesystem::create_directories(cache_dir, ec);

  bool fresh = false;
  {
    std::ifstream index(index_path, std::ios::binary);
    std::string magic;
    if (index && std::getline(index, magic) && magic == kLaunchBoxIndexMagic) {
      auto written = std::filesystem::last_write_time(index_path, ec);
      fresh = !ec && std::filesystem::file_time_type::clock::now() - written <
                         kLaunchBoxIndexMaxAge;
    }
  }
  if (!fresh) {
    XELOGI("Dashboard: downloading the LaunchBox games database");
    std::filesystem::path zip_path = cache_dir / "launchbox_metadata.zip";
    std::filesystem::path xml_path = cache_dir / "Metadata.xml";
    // A download from a failed try is reused (it's ~100 MB) if it's recent.
    auto zip_time = std::filesystem::last_write_time(zip_path, ec);
    bool have_zip =
        !ec && std::filesystem::file_size(zip_path, ec) > 1000000 && !ec &&
        std::filesystem::file_time_type::clock::now() - zip_time <
            std::chrono::hours(24);
    bool built = (have_zip || Download(kLaunchBoxUrl, zip_path)) && !stop &&
                 Unzip(zip_path, "Metadata.xml") && !stop &&
                 BuildLaunchBoxIndex(xml_path, index_path);
    std::filesystem::remove(xml_path, ec);
    if (built) {
      std::filesystem::remove(zip_path, ec);
    } else {
      XELOGW("Dashboard: couldn't build the LaunchBox index");
      // An old index is still better than none.
    }
  }

  std::ifstream index(index_path, std::ios::binary);
  std::string line;
  if (!index || !std::getline(index, line) || line != kLaunchBoxIndexMagic) {
    return false;
  }
  // Main names first, so an alternate name never takes another game's.
  std::vector<std::vector<std::string>> alternates;
  while (std::getline(index, line)) {
    std::vector<std::string> fields = SplitTabs(line);
    if (fields.size() < 10) {
      continue;
    }
    LaunchBoxGame game;
    game.name = IndexValue(fields[0]);
    game.box_front = IndexValue(fields[2]);
    game.fanart = IndexValue(fields[3]);
    game.release_date = IndexValue(fields[4]);
    game.developer = IndexValue(fields[5]);
    game.publisher = IndexValue(fields[6]);
    game.genres = IndexValue(fields[7]);
    game.players = IndexValue(fields[8]);
    game.overview = IndexValue(fields[9]);
    std::vector<std::string> names;
    size_t start = 0;
    while (!fields[1].empty()) {
      size_t bar = fields[1].find('|', start);
      names.push_back(IndexValue(fields[1].substr(start, bar - start)));
      if (bar == std::string::npos) {
        break;
      }
      start = bar + 1;
    }
    alternates.push_back(std::move(names));
    games_.push_back(std::move(game));
  }
  auto add = [this](const std::string& name, size_t index) {
    std::string key = TitleKey(name);
    if (key.empty()) {
      return;
    }
    auto [it, inserted] = by_key_.emplace(key, index);
    // Two games with the same key ("Sonic the Hedgehog" and "Sonic the
    // Hedgehog (XBLA)"): the one named without a bracketed tag wins.
    if (!inserted && games_[it->second].name.find('(') != std::string::npos &&
        games_[index].name.find('(') == std::string::npos) {
      it->second = index;
    }
  };
  for (size_t i = 0; i < games_.size(); ++i) {
    add(games_[i].name, i);
  }
  for (size_t i = 0; i < games_.size(); ++i) {
    for (const std::string& name : alternates[i]) {
      by_key_.emplace(TitleKey(name), i);
    }
  }
  return !games_.empty();
}

const LaunchBoxGame* LaunchBoxIndex::Find(const std::string& title) const {
  auto lookup = [this](const std::string& text) -> const LaunchBoxGame* {
    auto it = by_key_.find(TitleKey(text));
    return it != by_key_.end() ? &games_[it->second] : nullptr;
  };
  if (const LaunchBoxGame* game = lookup(title)) {
    return game;
  }
  for (const char* separator : {" - ", ": ", " ~ "}) {
    size_t at = title.find(separator);
    if (at != std::string::npos) {
      if (const LaunchBoxGame* game = lookup(title.substr(0, at))) {
        return game;
      }
    }
  }
  return nullptr;
}

bool LoadLaunchBoxImage(const std::string& file_name,
                        const std::filesystem::path& cache_dir,
                        uint32_t max_width, uint32_t max_height,
                        std::vector<uint8_t>* out_rgba, uint32_t* out_width,
                        uint32_t* out_height) {
  if (file_name.empty() ||
      file_name.find_first_of("/\\:") != std::string::npos) {
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(cache_dir, ec);
  std::filesystem::path path = cache_dir / xe::to_path(file_name);
  if (!std::filesystem::exists(path, ec)) {
    std::filesystem::path temp_path = path;
    temp_path += ".part";
    if (!Download(kLaunchBoxImageUrl + file_name, temp_path)) {
      XELOGW("Dashboard: couldn't download {}", file_name);
      return false;
    }
    std::filesystem::rename(temp_path, path, ec);
    if (ec) {
      return false;
    }
  }
  std::ifstream file(path, std::ios::binary);
  std::vector<uint8_t> encoded((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
  std::vector<uint8_t> rgba;
  uint32_t width = 0, height = 0;
  if (!DecodeImageRgba(encoded.data(), encoded.size(), &rgba, &width,
                       &height)) {
    // A broken download - fetch it again next time.
    std::filesystem::remove(path, ec);
    return false;
  }
  // Scaled down by averaging each output pixel's share of the source.
  float scale = std::min({1.0f, float(max_width) / float(width),
                          float(max_height) / float(height)});
  uint32_t new_width = std::max(1u, uint32_t(float(width) * scale + 0.5f));
  uint32_t new_height = std::max(1u, uint32_t(float(height) * scale + 0.5f));
  if (new_width == width && new_height == height) {
    *out_rgba = std::move(rgba);
  } else {
    out_rgba->assign(size_t(new_width) * new_height * 4, 0);
    for (uint32_t y = 0; y < new_height; ++y) {
      uint32_t y0 = y * height / new_height;
      uint32_t y1 = std::max(y0 + 1, (y + 1) * height / new_height);
      for (uint32_t x = 0; x < new_width; ++x) {
        uint32_t x0 = x * width / new_width;
        uint32_t x1 = std::max(x0 + 1, (x + 1) * width / new_width);
        uint32_t sum[4] = {};
        for (uint32_t sy = y0; sy < y1; ++sy) {
          const uint8_t* row = &rgba[(size_t(sy) * width + x0) * 4];
          for (uint32_t sx = x0; sx < x1; ++sx, row += 4) {
            for (int c = 0; c < 4; ++c) {
              sum[c] += row[c];
            }
          }
        }
        uint32_t count = (y1 - y0) * (x1 - x0);
        uint8_t* out = &(*out_rgba)[(size_t(y) * new_width + x) * 4];
        for (int c = 0; c < 4; ++c) {
          out[c] = uint8_t(sum[c] / count);
        }
      }
    }
  }
  *out_width = new_width;
  *out_height = new_height;
  return true;
}

}  // namespace app
}  // namespace xe
