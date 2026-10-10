/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_MUSIC_ASSISTANT_MUSIC_ASSISTANT_CLIENT_H_
#define XENIA_APU_MUSIC_ASSISTANT_MUSIC_ASSISTANT_CLIENT_H_

#include <cstdint>
#include <string>
#include <vector>

namespace xe {
namespace apu {
namespace music_assistant {

// A browsable playlist or track. `uri` is Music Assistant's own opaque media
// reference (e.g. "library://playlist/4", "library://track/3316") - it's
// exactly what player_queues/play_media expects as a `media` value, so
// there's no need to separately track item_id/provider once we have it.
struct MediaItem {
  std::string name;
  std::string uri;
  // Only meaningful for playlists: the fields playlist_tracks needs to look
  // up this playlist's contents (distinct from `uri`, which play_media wants
  // instead).
  std::string item_id;
  std::string provider_instance_id_or_domain;
  // Music Assistant's id for the item's artwork (artist photo, album cover)
  // behind its /imageproxy - empty if it has none. See FetchImage.
  std::string image_proxy_id;
};

// Talks to a Music Assistant server's plain HTTP JSON-RPC API
// (POST /api {"command": ..., "args": {...}}) - see http_client.h for the
// underlying transport. Every call is synchronous and blocking; callers on a
// UI thread should not call this directly on every frame.
class MusicAssistantClient {
 public:
  MusicAssistantClient(std::string host, uint16_t port, std::string token);

  // Fetches the user's library playlists. Returns false on any transport,
  // HTTP, or JSON error (logged); `out_playlists` is left unchanged on
  // failure.
  bool ListPlaylists(std::vector<MediaItem>* out_playlists);

  // Fetches the tracks belonging to a playlist previously returned by
  // ListPlaylists (uses its item_id/provider_instance_id_or_domain, not its
  // uri).
  bool ListPlaylistTracks(const MediaItem& playlist,
                          std::vector<MediaItem>* out_tracks);

  // Fetches the user's library (album) artists.
  bool ListArtists(std::vector<MediaItem>* out_artists);

  // Fetches the albums belonging to an artist previously returned by
  // ListArtists.
  bool ListArtistAlbums(const MediaItem& artist,
                        std::vector<MediaItem>* out_albums);

  // Fetches the tracks belonging to an album previously returned by
  // ListArtistAlbums.
  bool ListAlbumTracks(const MediaItem& album,
                       std::vector<MediaItem>* out_tracks);

  // Finds the player id Music Assistant assigned to our own Sendspin
  // connection (see sendspin_client.h) by its declared name. Returns false
  // if no player with that name is currently registered (e.g. Xenia hasn't
  // finished connecting yet) or on any transport/HTTP error (logged);
  // *out_player_id is left unchanged on failure.
  bool FindPlayerIdByName(const std::string& name,
                          std::string* out_player_id);

  // Starts playback of `media_uri` (a MediaItem::uri, or any other uri
  // Music Assistant understands) on the given player's queue - every player
  // gets a queue of the same id as the player itself. Returns false on any
  // transport/HTTP error (logged).
  bool PlayMedia(const std::string& queue_id, const std::string& media_uri);

  // Fetches an item's artwork (MediaItem::image_proxy_id) through Music
  // Assistant's image proxy, scaled server-side to about `size` pixels -
  // the encoded JPEG/PNG bytes. The proxy fetches remote art itself, so this
  // works over plain local HTTP even when the original is https.
  bool FetchImage(const std::string& proxy_id, uint32_t size,
                  std::vector<uint8_t>* out_image);

  struct NowPlaying {
    std::string player_id;
    // "playing", "paused" or "idle".
    std::string state;
    std::string title;
    std::string artist;
    // For FetchImage - empty if the track has no art.
    std::string image_proxy_id;
    // Seconds; 0 if unknown. `elapsed` is as of when this was fetched.
    double duration = 0.0;
    double elapsed = 0.0;
  };
  // What the player named `player_name` is playing (its current_media).
  // Returns false only if the player couldn't be found; an idle player
  // succeeds with empty title/artist.
  bool GetNowPlaying(const std::string& player_name, NowPlaying* out);

  // Sends a transport command to a player - players/cmd/<command>, e.g.
  // "play_pause", "next", "previous".
  bool PlayerCommand(const std::string& player_id, const std::string& command);

  // Sends a command to a player queue - player_queues/<command>, e.g.
  // "stop", "clear". Every player's own queue has the player's id.
  bool QueueCommand(const std::string& queue_id, const std::string& command);

 private:
  // Issues one JSON-RPC command and returns the raw "result" JSON (which for
  // every command used here is a JSON array) as text, ready for the caller to
  // parse with rapidjson. Returns false on failure.
  bool CallCommand(const std::string& command, const std::string& args_json,
                   std::string* out_result_json);

  // Shared by ListPlaylistTracks/ListArtistAlbums/ListAlbumTracks, which all
  // take the same {item_id, provider_instance_id_or_domain} args shape keyed
  // off a previously-fetched parent MediaItem.
  bool ListByParent(const std::string& command, const MediaItem& parent,
                    std::vector<MediaItem>* out_items);

  std::string host_;
  uint16_t port_;
  std::string token_;
};

}  // namespace music_assistant
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_MUSIC_ASSISTANT_MUSIC_ASSISTANT_CLIENT_H_
