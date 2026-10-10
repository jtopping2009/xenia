/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/music_assistant/music_assistant_client.h"

#include <chrono>
#include <cstring>

#include "xenia/apu/music_assistant/http_client.h"
#include "xenia/base/logging.h"

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

namespace xe {
namespace apu {
namespace music_assistant {

namespace {

std::string BuildEnvelope(const std::string& command,
                          const std::string& args_json) {
  std::string envelope = "{\"command\":\"";
  envelope += command;
  envelope += "\",\"args\":";
  envelope += args_json.empty() ? "{}" : args_json;
  envelope += "}";
  return envelope;
}

// Reads a small set of well-known string fields off a playlist/track item
// into a MediaItem - every field is optional in the JSON, defaulting to an
// empty string if absent or the wrong type, so a partially-unexpected
// response doesn't hard-fail the whole list.
MediaItem ParseMediaItem(const rapidjson::Value& item) {
  MediaItem entry;
  if (item.HasMember("name") && item["name"].IsString()) {
    entry.name = item["name"].GetString();
  }
  if (item.HasMember("uri") && item["uri"].IsString()) {
    entry.uri = item["uri"].GetString();
  }
  if (item.HasMember("item_id") && item["item_id"].IsString()) {
    entry.item_id = item["item_id"].GetString();
  }
  if (item.HasMember("provider") && item["provider"].IsString()) {
    entry.provider_instance_id_or_domain = item["provider"].GetString();
  }
  // metadata.images[] - prefer the "thumb" (artist photo / album cover).
  if (item.HasMember("metadata") && item["metadata"].IsObject()) {
    const auto& metadata = item["metadata"];
    if (metadata.HasMember("images") && metadata["images"].IsArray()) {
      for (const auto& image : metadata["images"].GetArray()) {
        if (!image.IsObject() || !image.HasMember("proxy_id") ||
            !image["proxy_id"].IsString()) {
          continue;
        }
        bool is_thumb = image.HasMember("type") && image["type"].IsString() &&
                        std::strcmp(image["type"].GetString(), "thumb") == 0;
        if (is_thumb || entry.image_proxy_id.empty()) {
          entry.image_proxy_id = image["proxy_id"].GetString();
        }
        if (is_thumb) {
          break;
        }
      }
    }
  }
  return entry;
}

// A player's or queue's elapsed_time brought up to now: Music Assistant only
// reports it as of elapsed_time_last_updated (Unix seconds, server clock)
// rather than bumping it continuously while playing.
double ElapsedNow(const rapidjson::Value& object, bool playing) {
  if (!object.HasMember("elapsed_time") || !object["elapsed_time"].IsNumber()) {
    return 0.0;
  }
  double elapsed = object["elapsed_time"].GetDouble();
  if (playing && object.HasMember("elapsed_time_last_updated") &&
      object["elapsed_time_last_updated"].IsNumber()) {
    double now = std::chrono::duration<double>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();
    double since_update = now - object["elapsed_time_last_updated"].GetDouble();
    // Ignore anything implausible (clock skew between the machines).
    if (since_update > 0.0 && since_update < 24.0 * 60.0 * 60.0) {
      elapsed += since_update;
    }
  }
  return elapsed;
}

}  // namespace

MusicAssistantClient::MusicAssistantClient(std::string host, uint16_t port,
                                           std::string token)
    : host_(std::move(host)), port_(port), token_(std::move(token)) {}

bool MusicAssistantClient::CallCommand(const std::string& command,
                                       const std::string& args_json,
                                       std::string* out_result_json) {
  std::string body = BuildEnvelope(command, args_json);
  std::vector<std::pair<std::string, std::string>> headers = {
      {"Authorization", "Bearer " + token_},
      {"Content-Type", "application/json"},
  };
  HttpResponse response;
  if (!HttpPost(host_, port_, "/api", headers, body, &response)) {
    XELOGE("MusicAssistantClient: request failed for command '{}'", command);
    return false;
  }
  if (response.status_code != 200) {
    XELOGE("MusicAssistantClient: command '{}' returned HTTP {}: {}", command,
           response.status_code, response.body);
    return false;
  }
  *out_result_json = std::move(response.body);
  return true;
}

bool MusicAssistantClient::ListPlaylists(
    std::vector<MediaItem>* out_playlists) {
  std::string result_json;
  if (!CallCommand("music/playlists/library_items", "{}", &result_json)) {
    return false;
  }
  rapidjson::Document doc;
  if (doc.Parse(result_json.c_str()).HasParseError() || !doc.IsArray()) {
    XELOGE(
        "MusicAssistantClient: unexpected response parsing playlist list");
    return false;
  }
  std::vector<MediaItem> playlists;
  for (auto& item : doc.GetArray()) {
    playlists.push_back(ParseMediaItem(item));
  }
  *out_playlists = std::move(playlists);
  return true;
}

bool MusicAssistantClient::ListByParent(const std::string& command,
                                        const MediaItem& parent,
                                        std::vector<MediaItem>* out_items) {
  rapidjson::Document args_doc;
  args_doc.SetObject();
  auto& allocator = args_doc.GetAllocator();
  args_doc.AddMember(
      "item_id", rapidjson::Value(parent.item_id.c_str(), allocator),
      allocator);
  args_doc.AddMember(
      "provider_instance_id_or_domain",
      rapidjson::Value(parent.provider_instance_id_or_domain.c_str(),
                       allocator),
      allocator);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  args_doc.Accept(writer);

  std::string result_json;
  if (!CallCommand(command, buffer.GetString(), &result_json)) {
    return false;
  }
  rapidjson::Document doc;
  if (doc.Parse(result_json.c_str()).HasParseError() || !doc.IsArray()) {
    XELOGE("MusicAssistantClient: unexpected response parsing '{}' result",
           command);
    return false;
  }
  std::vector<MediaItem> items;
  for (auto& item : doc.GetArray()) {
    items.push_back(ParseMediaItem(item));
  }
  *out_items = std::move(items);
  return true;
}

bool MusicAssistantClient::ListPlaylistTracks(
    const MediaItem& playlist, std::vector<MediaItem>* out_tracks) {
  return ListByParent("music/playlists/playlist_tracks", playlist,
                      out_tracks);
}

bool MusicAssistantClient::ListArtists(std::vector<MediaItem>* out_artists) {
  std::string result_json;
  if (!CallCommand("music/artists/library_items", "{}", &result_json)) {
    return false;
  }
  rapidjson::Document doc;
  if (doc.Parse(result_json.c_str()).HasParseError() || !doc.IsArray()) {
    XELOGE("MusicAssistantClient: unexpected response parsing artist list");
    return false;
  }
  std::vector<MediaItem> artists;
  for (auto& item : doc.GetArray()) {
    artists.push_back(ParseMediaItem(item));
  }
  *out_artists = std::move(artists);
  return true;
}

bool MusicAssistantClient::ListArtistAlbums(
    const MediaItem& artist, std::vector<MediaItem>* out_albums) {
  return ListByParent("music/artists/artist_albums", artist, out_albums);
}

bool MusicAssistantClient::ListAlbumTracks(
    const MediaItem& album, std::vector<MediaItem>* out_tracks) {
  return ListByParent("music/albums/album_tracks", album, out_tracks);
}

bool MusicAssistantClient::FindPlayerIdByName(const std::string& name,
                                              std::string* out_player_id) {
  std::string result_json;
  if (!CallCommand("players/all", "{}", &result_json)) {
    return false;
  }
  rapidjson::Document doc;
  if (doc.Parse(result_json.c_str()).HasParseError() || !doc.IsArray()) {
    XELOGE("MusicAssistantClient: unexpected response parsing players/all");
    return false;
  }
  for (auto& item : doc.GetArray()) {
    if (!item.IsObject() || !item.HasMember("player_id") ||
        !item["player_id"].IsString() || !item.HasMember("name") ||
        !item["name"].IsString()) {
      continue;
    }
    if (name == item["name"].GetString()) {
      *out_player_id = item["player_id"].GetString();
      return true;
    }
  }
  XELOGE("MusicAssistantClient: no player named '{}' in players/all - is "
        "Xenia connected yet?",
        name);
  return false;
}

bool MusicAssistantClient::GetNowPlaying(const std::string& player_name,
                                         NowPlaying* out) {
  std::string result_json;
  if (!CallCommand("players/all", "{}", &result_json)) {
    return false;
  }
  rapidjson::Document doc;
  if (doc.Parse(result_json.c_str()).HasParseError() || !doc.IsArray()) {
    XELOGE("MusicAssistantClient: unexpected response parsing players/all");
    return false;
  }
  auto get_string = [](const rapidjson::Value& object, const char* name) {
    return object.HasMember(name) && object[name].IsString()
               ? std::string(object[name].GetString())
               : std::string();
  };
  for (auto& player : doc.GetArray()) {
    if (!player.IsObject() || get_string(player, "name") != player_name) {
      continue;
    }
    *out = NowPlaying();
    out->player_id = get_string(player, "player_id");
    out->state = get_string(player, "playback_state");
    if (out->state.empty()) {
      out->state = get_string(player, "state");
    }
    out->elapsed = ElapsedNow(player, out->state == "playing");
    if (player.HasMember("current_media") &&
        player["current_media"].IsObject()) {
      const auto& media = player["current_media"];
      out->title = get_string(media, "title");
      out->artist = get_string(media, "artist");
      if (media.HasMember("duration") && media["duration"].IsNumber()) {
        out->duration = media["duration"].GetDouble();
      }
      // ".../imageproxy/<proxy id>?size=..." - FetchImage wants the id.
      std::string image_url = get_string(media, "image_url");
      size_t id_start = image_url.find("/imageproxy/");
      if (id_start != std::string::npos) {
        id_start += std::strlen("/imageproxy/");
        size_t id_end = image_url.find_first_of("?/", id_start);
        out->image_proxy_id = image_url.substr(
            id_start, id_end == std::string::npos ? std::string::npos
                                                  : id_end - id_start);
      }
    }
    // The player's elapsed_time restarts at 0 whenever Music Assistant
    // starts a new stream to it - resuming from pause, or Xenia reconnecting
    // - even though playback carries on from where it was. The queue's is
    // the real position in the track, so prefer it.
    std::string queue_id = get_string(player, "active_source");
    if (queue_id.empty()) {
      queue_id = out->player_id;
    }
    rapidjson::Document args_doc;
    args_doc.SetObject();
    args_doc.AddMember(
        "queue_id",
        rapidjson::Value(queue_id.c_str(), args_doc.GetAllocator()),
        args_doc.GetAllocator());
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    args_doc.Accept(writer);
    std::string queue_json;
    rapidjson::Document queue;
    if (CallCommand("player_queues/get", buffer.GetString(), &queue_json) &&
        !queue.Parse(queue_json.c_str()).HasParseError() &&
        queue.IsObject() && queue.HasMember("elapsed_time") &&
        queue["elapsed_time"].IsNumber()) {
      out->elapsed = ElapsedNow(queue, out->state == "playing" ||
                                           get_string(queue, "state") ==
                                               "playing");
    }
    return true;
  }
  return false;
}

bool MusicAssistantClient::PlayerCommand(const std::string& player_id,
                                         const std::string& command) {
  rapidjson::Document args_doc;
  args_doc.SetObject();
  auto& allocator = args_doc.GetAllocator();
  args_doc.AddMember(
      "player_id", rapidjson::Value(player_id.c_str(), allocator), allocator);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  args_doc.Accept(writer);
  std::string result_json;
  return CallCommand("players/cmd/" + command, buffer.GetString(),
                     &result_json);
}

bool MusicAssistantClient::QueueCommand(const std::string& queue_id,
                                        const std::string& command) {
  rapidjson::Document args_doc;
  args_doc.SetObject();
  auto& allocator = args_doc.GetAllocator();
  args_doc.AddMember(
      "queue_id", rapidjson::Value(queue_id.c_str(), allocator), allocator);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  args_doc.Accept(writer);
  std::string result_json;
  return CallCommand("player_queues/" + command, buffer.GetString(),
                     &result_json);
}

bool MusicAssistantClient::FetchImage(const std::string& proxy_id,
                                      uint32_t size,
                                      std::vector<uint8_t>* out_image) {
  // Proxy ids are hex digests - reject anything else rather than escaping it
  // into the path.
  if (proxy_id.empty() ||
      proxy_id.find_first_not_of("0123456789abcdefABCDEF") !=
          std::string::npos) {
    return false;
  }
  HttpResponse response;
  if (!HttpGet(host_, port_,
               "/imageproxy/" + proxy_id + "?size=" + std::to_string(size),
               {{"Authorization", "Bearer " + token_}}, &response) ||
      response.status_code != 200 || response.body.empty()) {
    XELOGW("MusicAssistantClient: image {} unavailable (HTTP {})", proxy_id,
           response.status_code);
    return false;
  }
  out_image->assign(response.body.begin(), response.body.end());
  return true;
}

bool MusicAssistantClient::PlayMedia(const std::string& queue_id,
                                     const std::string& media_uri) {
  rapidjson::Document args_doc;
  args_doc.SetObject();
  auto& allocator = args_doc.GetAllocator();
  args_doc.AddMember(
      "queue_id", rapidjson::Value(queue_id.c_str(), allocator), allocator);
  args_doc.AddMember(
      "media", rapidjson::Value(media_uri.c_str(), allocator), allocator);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  args_doc.Accept(writer);

  std::string result_json;
  return CallCommand("player_queues/play_media", buffer.GetString(),
                     &result_json);
}

}  // namespace music_assistant
}  // namespace apu
}  // namespace xe
