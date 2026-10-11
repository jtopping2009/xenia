/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_EMULATOR_WINDOW_H_
#define XENIA_APP_EMULATOR_WINDOW_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "xenia/app/game_metadata.h"
#include "xenia/apu/music_assistant/music_assistant_client.h"
#include "xenia/apu/sendspin/sendspin_player_service.h"
#include "xenia/emulator.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/hid/input.h"
#include "xenia/kernel/xam/achievements.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/immediate_drawer.h"
#include "xenia/ui/menu_item.h"
#include "xenia/ui/presenter.h"
#include "xenia/ui/window.h"
#include "xenia/ui/window_listener.h"
#include "xenia/ui/windowed_app_context.h"
#include "xenia/xbox.h"

struct ImDrawList;
struct ImFont;

namespace xe {
namespace app {

// A dashboard/guide row and its picture - see emulator_window.cc.
struct DashboardArt;
struct DashboardRow;

class EmulatorWindow {
 public:
  enum : size_t {
    // The UI is on top of the game and is open in special cases, so
    // lowest-priority.
    kZOrderHidInput,
    kZOrderImGui,
    kZOrderProfiler,
    // Emulator window controls are expected to be always accessible by the
    // user, so highest-priority.
    kZOrderEmulatorWindowInput,
  };

  virtual ~EmulatorWindow();

  static std::unique_ptr<EmulatorWindow> Create(
      Emulator* emulator, ui::WindowedAppContext& app_context);

  Emulator* emulator() const { return emulator_; }
  ui::WindowedAppContext& app_context() const { return app_context_; }
  ui::Window* window() const { return window_.get(); }
  ui::ImGuiDrawer* imgui_drawer() const { return imgui_drawer_.get(); }

  ui::Presenter* GetGraphicsSystemPresenter() const;
  void SetupGraphicsSystemPresenterPainting();
  void ShutdownGraphicsSystemPresenterPainting();

  void OnEmulatorInitialized();

  void UpdateTitle();
  void SetFullscreen(bool fullscreen);
  void ToggleFullscreen();
  void SetInitializingShaderStorage(bool initializing);

  // For a real exit (not RelaunchXenia): stops the music on Xenia's Music
  // Assistant player and clears its queue, per
  // music_assistant_stop_on_exit. Waits at most a couple of seconds, so an
  // unreachable server can't hold up closing.
  void StopMusicForExit();

 private:
  class EmulatorWindowListener final : public ui::WindowListener,
                                       public ui::WindowInputListener {
   public:
    explicit EmulatorWindowListener(EmulatorWindow& emulator_window)
        : emulator_window_(emulator_window) {}

    void OnClosing(ui::UIEvent& e) override;
    void OnFileDrop(ui::FileDropEvent& e) override;

    void OnKeyDown(ui::KeyEvent& e) override;

   private:
    EmulatorWindow& emulator_window_;
  };

  class DisplayConfigGameConfigLoadCallback
      : public Emulator::GameConfigLoadCallback {
   public:
    DisplayConfigGameConfigLoadCallback(Emulator& emulator,
                                        EmulatorWindow& emulator_window)
        : Emulator::GameConfigLoadCallback(emulator),
          emulator_window_(emulator_window) {}

    void PostGameConfigLoad() override;

   private:
    EmulatorWindow& emulator_window_;
  };

  class DisplayConfigDialog final : public ui::ImGuiDialog {
   public:
    DisplayConfigDialog(ui::ImGuiDrawer* imgui_drawer,
                        EmulatorWindow& emulator_window)
        : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {}

   protected:
    void OnDraw(ImGuiIO& io) override;

   private:
    EmulatorWindow& emulator_window_;
  };

  class GpuConfigDialog final : public ui::ImGuiDialog {
   public:
    GpuConfigDialog(ui::ImGuiDrawer* imgui_drawer,
                    EmulatorWindow& emulator_window)
        : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {}

   protected:
    void OnDraw(ImGuiIO& io) override;

   private:
    EmulatorWindow& emulator_window_;
  };

  // Always alive while the window is (not toggled open/closed); draws nothing
  // unless cvars::display_show_fps is set.
  class FpsOverlay final : public ui::ImGuiDialog {
   public:
    FpsOverlay(ui::ImGuiDrawer* imgui_drawer, EmulatorWindow& emulator_window)
        : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {}

   protected:
    void OnDraw(ImGuiIO& io) override;

   private:
    EmulatorWindow& emulator_window_;
  };

  // Always alive (like FpsOverlay) rather than toggled open/closed via
  // construct/destruct, because it needs to keep polling for the guide-button
  // press that opens it even while it's drawing nothing. A full-screen
  // Xbox 360 dashboard-style overlay, opened via the controller's Home/Guide
  // button (or F12 as a keyboard fallback - see OnKeyDown) without pausing or
  // otherwise disturbing the game running underneath.
  class GuideMenuOverlay final : public ui::ImGuiDialog {
   public:
    GuideMenuOverlay(ui::ImGuiDrawer* imgui_drawer,
                     EmulatorWindow& emulator_window);

    ~GuideMenuOverlay();

    void ToggleOpen() { is_open_ = !is_open_; }

   protected:
    void OnDraw(ImGuiIO& io) override;

   private:
    // Polls all connected controllers, ORing their button state together
    // (single local player is the only case that matters here) and returns
    // true if any is pressed. Also handles the guide-button edge-trigger that
    // opens/closes the overlay, since that must happen regardless of whether
    // the overlay is currently drawing anything.
    uint16_t PollCombinedButtons();

    // One level of the Artists -> Albums -> Tracks browser. `items` always
    // has a synthetic "Play All"/"Play Album" entry prepended for the
    // Albums/Tracks levels (see kPlayActionUri) so it's just another
    // selectable row rather than a separate code path.
    enum class MusicNavLevel { kArtists, kAlbums, kTracks };
    struct MusicNavFrame {
      MusicNavLevel level;
      std::vector<apu::music_assistant::MediaItem> items;
      int selected_index = 0;
      // The artist (for an Albums frame) or album (for a Tracks frame) this
      // list belongs to - unused for the top-level Artists frame.
      apu::music_assistant::MediaItem parent;
      bool load_failed = false;
    };

    // Fetches the artist list and enters the browser (pushes the first
    // frame). Blocking - only ever called from an explicit confirm press.
    void EnterMusicBrowser();
    // Handles a confirm press on the currently-selected row of the top nav
    // frame: either drills into albums/tracks, or triggers the play-action
    // sentinel row (calls PlayMediaOnXeniaPlayer).
    void ConfirmMusicBrowserSelection();
    // Pops one level. Returns true if the browser is now closed (stack
    // empty), in which case the caller should fall back to normal tile-grid
    // input handling.
    bool PopMusicBrowser();

    // A single discovered, launchable game on the Games tab - a flat list
    // (unlike Music's Artists/Albums/Tracks browser) since there's no
    // metadata here to organize by, only whatever games_path contains.
    struct GameEntry {
      // The path relative to games_path when nested more than one level
      // deep, so same-named games in different folders stay distinguishable
      // without a folder drill-down UI - otherwise just the filename.
      std::string display_name;
      std::filesystem::path path;
      // Dashboard presentation, derived from the file name - e.g.
      // "Burnout Revenge (USA).iso" -> title "Burnout Revenge", tags "USA".
      std::string title;
      std::string tags;
      // "ISO", "XEX" or "FOLDER" (extracted disc) - the row badge.
      std::string format;
      // 0 for extracted folders (not worth walking the whole tree for).
      uint64_t size_bytes = 0;
    };

    // Scans games_path (once per overlay session - see games_list_loaded_)
    // for .iso/.xex files and extracted-game folders (a folder containing
    // default.xex directly), filling games_list_. Blocking - only called
    // when the Games tab is first selected.
    void EnterGamesBrowser();
    // Recursive helper for EnterGamesBrowser - see kGamesScanMaxDepth in the
    // .cc for the depth bound.
    void ScanGamesRecursive(const std::filesystem::path& dir,
                            const std::filesystem::path& root, int depth,
                            std::vector<GameEntry>* out_games);
    // In dashboard mode (no title running), launches the selected game
    // in-process; with a title running, asks for confirmation and then
    // relaunches Xenia on it instead (see EmulatorWindow::RelaunchXenia).
    void ConfirmGamesBrowserSelection();

    // True once the emulator is up with no title running - the overlay then
    // acts as the always-open Xbox 360-style dashboard rather than the
    // in-game guide.
    bool IsDashboardMode() const;
    // Draws the Blades-style dashboard (all tab blades visible, a center
    // panel with the list and a details pane) - the in-game guide keeps its
    // own simpler look, drawn inline in OnDraw.
    void DrawDashboard(ImGuiIO& io);
    // Resets browsing state when the overlay closes or switches between
    // guide and dashboard mode.
    void ResetBrowsingState();

    // An in-game action that quits the running title - needs an explicit
    // second A press (B cancels), since it closes the game without saving.
    // kTurnOff only exists inside a shell (IsInShell): quits Xenia, and
    // with it the shell's "console".
    enum class PendingAction {
      kNone,
      kReturnToDashboard,
      kSwitchGame,
      kTurnOff
    };
    PendingAction pending_action_ = PendingAction::kNone;
    std::filesystem::path pending_game_path_;
    std::string pending_game_name_;
    // Performs pending_action_ via EmulatorWindow::RelaunchXenia.
    void RunPendingAction();

    // Official titles and icons for the Games list, read in the background
    // (uncached, each game's whole default.xex is read and decrypted) - see
    // game_metadata.h. Keyed by the game's path; entries are only ever
    // inserted, complete, and never modified or erased, so pointers to them
    // stay valid for drawing without holding the lock.
    struct LoadedGameMetadata {
      bool found = false;
      GameMetadata data;
    };
    // Starts the loader thread for any games in games_list_ with no entry
    // yet, unless it's already running. Cheap - call every frame.
    void EnsureGameMetadataLoading();
    // Null until loaded (or if nothing was found).
    const GameMetadata* FindGameMetadata(const std::filesystem::path& path);
    std::mutex game_metadata_mutex_;
    std::unordered_map<std::string, LoadedGameMetadata> game_metadata_;
    std::thread game_metadata_thread_;
    std::atomic<bool> game_metadata_thread_running_{false};
    std::atomic<bool> game_metadata_stop_{false};

    // Box art, background art and details for the Games blade's coverflow,
    // from the LaunchBox Games Database (see LaunchBoxIndex), loaded in the
    // background nearest the highlighted game first. Like game_metadata_,
    // entries are inserted complete and never changed or erased after.
    struct GameArt {
      bool matched = false;
      LaunchBoxGame info;
      std::vector<uint8_t> cover_rgba;
      uint32_t cover_width = 0;
      uint32_t cover_height = 0;
      std::vector<uint8_t> fanart_rgba;
      uint32_t fanart_width = 0;
      uint32_t fanart_height = 0;
    };
    enum class GameArtStatus { kIdle, kPreparing, kReady, kFailed };
    // Starts the loader for any games with no art entry yet. Cheap - call
    // every frame.
    void EnsureGameArtLoading();
    // Null until loaded.
    const GameArt* FindGameArt(const std::filesystem::path& path);
    std::mutex game_art_mutex_;
    std::unordered_map<std::string, GameArt> game_art_;
    std::thread game_art_thread_;
    std::atomic<bool> game_art_thread_running_{false};
    std::atomic<GameArtStatus> game_art_status_{GameArtStatus::kIdle};
    // The highlighted game, for the loader to work outward from.
    std::atomic<int> game_art_focus_{0};
    // Only touched by the loader thread.
    LaunchBoxIndex launchbox_index_;
    bool launchbox_index_loaded_ = false;

    // The Games blade's coverflow (the dashboard's, not the in-game guide's
    // list): where it's scrolled to, easing toward the highlighted game, and
    // the background art fading from one game's to the next.
    float coverflow_position_ = 0.0f;
    std::string coverflow_background_key_;
    std::string coverflow_previous_background_key_;
    float coverflow_background_fade_ = 1.0f;
    // The description's scrolling (when it's too long for its box): which
    // game it's for, and how long that's been shown.
    int overview_scroll_game_ = -1;
    std::chrono::steady_clock::time_point overview_scroll_start_;
    // The backdrops' sizes by texture key, for cropping the one fading out.
    std::unordered_map<std::string, std::pair<uint32_t, uint32_t>>
        coverflow_background_sizes_;
    // The letter picker (Y): jumps to the first game starting with a
    // letter. 0 is "#" (digits and symbols), 1-26 A-Z.
    bool letter_picker_open_ = false;
    int letter_picker_index_ = 0;
    // The letter group (0-26, as above) a game's title files under.
    static int GameLetter(const GameEntry& game);
    // Moves the highlight to the first game of the next (or previous) letter
    // group with games.
    void JumpGameLetter(int direction);
    // The triggers as buttons (LT/RT jump between letters), from the last
    // PollCombinedButtons.
    bool left_trigger_down_ = false;
    bool right_trigger_down_ = false;
    bool left_trigger_was_down_ = false;
    bool right_trigger_was_down_ = false;

    // Artwork for the Music blade (artist photos, album covers), fetched from
    // Music Assistant's image proxy on a background thread and cached on disk
    // under cache_root/dashboard/music. Like game_metadata_, entries are
    // never modified once done nor erased after, so a done entry's pointer
    // stays valid without the lock.
    struct MusicImage {
      bool done = false;
      bool ok = false;
      std::vector<uint8_t> rgba;
      uint32_t width = 0;
      uint32_t height = 0;
    };
    // The image if it's loaded; otherwise queues it (most recent request
    // first, so what's on screen now wins over what was scrolled past) and
    // returns null. Cheap - call every frame for every visible row.
    const MusicImage* RequestMusicImage(const std::string& proxy_id);
    void MusicImageThreadMain(std::filesystem::path cache_dir);
    std::mutex music_image_mutex_;
    std::condition_variable music_image_cv_;
    std::unordered_map<std::string, MusicImage> music_images_;
    std::vector<std::string> music_image_queue_;
    std::thread music_image_thread_;
    bool music_image_stop_ = false;

    bool was_dashboard_mode_ = false;
    // Set on switching between guide and dashboard; presses are ignored until
    // every button has been released.
    bool input_release_wait_ = false;

    // Leaving a game to the dashboard in-process (Emulator::
    // UnloadTitleInPlace, on a background thread) so Xenia - and its music -
    // keeps running, with a restart as the fallback if that fails or stalls.
    void StartLeaveInPlace();
    // Checked every frame while a leave is in progress.
    void PollLeaveInPlace();
    bool leave_in_progress_ = false;
    bool leave_fallback_started_ = false;
    std::chrono::steady_clock::time_point leave_started_at_;
    // 0 while running, then 1 on success, 2 on failure.
    std::atomic<int> leave_result_{0};
    bool was_open_ = false;
    // Whether a title has been running at some point (see IsDashboardMode).
    mutable bool title_seen_ = false;
    // Set while a dashboard launch is in flight, so repeated A presses can't
    // queue up a second LaunchPath before the first title is open.
    bool launch_in_progress_ = false;
    bool y_was_down_ = false;

    // Dispatches to EnterMusicBrowser/EnterGamesBrowser based on
    // selected_tile_ - called whenever the overlay opens or the highlighted
    // tab changes, matching the real Xbox Guide's auto-expand-on-highlight
    // behavior.
    void EnterSelectedTab();

    EmulatorWindow& emulator_window_;
    bool is_open_ = false;
    bool guide_was_down_ = false;
    bool nav_left_was_down_ = false;
    bool nav_right_was_down_ = false;
    bool nav_up_was_down_ = false;
    bool nav_down_was_down_ = false;
    bool confirm_was_down_ = false;
    bool close_was_down_ = false;
    // Starts playback of `uri` on the "Xenia" Sendspin player (see
    // sendspin_player_service.h, which keeps that player connected in the
    // background for the app's whole lifetime) via Music Assistant's
    // player_queues/play_media HTTP command, and reports the outcome via
    // last_action_message_. Blocking - only ever called from an explicit
    // confirm press.
    void PlayMediaOnXeniaPlayer(const std::string& uri,
                                const std::string& display_name);

    // Index into tabs_/tile_names_.
    int selected_tile_ = 0;
    // Which tabs exist, in blade order - Games before Music, matching the
    // real dashboard's games-then-media order, minus Games in launcher_mode
    // (the frontend is the game picker there). Set up in the constructor.
    static constexpr int kGamesTab = 0;
    static constexpr int kMusicTab = 1;
    // In-game guide only - Leave Game, Now Playing and so on.
    static constexpr int kHomeTab = 2;
    // Graphics and display settings, like the 360's System Settings blade.
    static constexpr int kSettingsTab = 3;
    // In-game guide only - the running game's achievements.
    static constexpr int kAchievementsTab = 4;
    std::vector<int> tabs_;
    std::vector<std::string> tile_names_;
    int CurrentTab() const { return tabs_[selected_tile_]; }
    // The dashboard has Games | Music; the in-game guide has
    // Games | Home | Music (no Games in launcher_mode).
    void RebuildTabs(bool dashboard_mode);
    // -1 if the current mode has no such tab.
    int TabIndex(int tab) const;

    // The in-game guide's Home tab.
    enum class HomeAction { kLeaveGame, kResumeGame, kMusic, kTurnOff };
    struct HomeRow {
      std::string label;
      // Shown right-aligned, like the real guide's counts.
      std::string value;
      HomeAction action;
    };
    std::vector<HomeRow> BuildHomeRows();

    // The Settings tab: a list of settings, each opening its list of choices.
    enum class SettingId {
      kResolutionScale,
      kAntiAliasing,
      kSharpening,
      kVSync,
      kShowFps,
    };
    struct SettingRow {
      SettingId id;
      std::string label;
      std::string description;
      std::vector<std::string> options;
      // Index into options, or -1 if the current value isn't one of them
      // (set by hand in the config).
      int current;
    };
    // Built from the current settings each time, so it's always up to date.
    std::vector<SettingRow> BuildSettingRows();
    // Applies (on the UI thread, where the presenter and command processor
    // expect it) and saves to the config right away.
    void ApplySetting(SettingId id, int option);
    int settings_selected_index_ = 0;
    // Choosing a value for the selected setting, rather than browsing them.
    bool settings_choosing_ = false;
    int settings_option_index_ = 0;
    // The current Settings level as list rows, shared by the dashboard and
    // the guide; `header` gets the breadcrumb and `selected` the highlighted
    // row.
    void BuildSettingsList(std::vector<DashboardRow>* out_rows, int* selected,
                           std::string* header, std::string* detail_title,
                           std::string* detail_body);
    void RunHomeAction(HomeAction action);
    int home_selected_index_ = 0;

    // What the "Xenia" Music Assistant player is playing, for the guide's
    // Now Playing card - fetched in the background when the guide opens,
    // every few seconds while it's open, and after each transport command.
    // `command` (players/cmd/<command>: "play_pause", "next", "previous"),
    // if not empty, is queued and sent first; commands pressed while a fetch
    // is in flight run right after it, in order.
    void RefreshNowPlaying(const std::string& command);
    // Sends a transport command from the guide, updating the card right
    // away where the outcome is predictable (play/pause).
    void SendTransportCommand(const std::string& command);
    // The playback position now, extrapolated from the last fetch.
    double NowPlayingElapsed();
    std::mutex now_playing_mutex_;
    apu::music_assistant::MusicAssistantClient::NowPlaying now_playing_;
    bool now_playing_valid_ = false;
    std::chrono::steady_clock::time_point now_playing_fetched_at_;
    std::chrono::steady_clock::time_point now_playing_requested_at_;
    std::vector<std::string> now_playing_commands_;
    std::thread now_playing_thread_;
    std::atomic<bool> now_playing_busy_{false};
    bool left_shoulder_was_down_ = false;
    bool right_shoulder_was_down_ = false;
    bool x_was_down_ = false;

    // Draws the in-game guide: a centered panel over the running game,
    // modeled on the real Xbox 360 guide.
    void DrawGuide(ImGuiIO& io);
    // Home's Now Playing card: art, title/artist, progress, and the transport
    // buttons with their controller buttons (LB / X / RB).
    void DrawNowPlayingCard(ImDrawList* dl, ImFont* font, float s, float x0,
                            float y0, float x1, float y1);
    // The running title's icon, shown in the guide header where the real
    // guide shows the gamer picture. Loaded once.
    void LoadTitleIcon();
    std::vector<uint8_t> title_icon_rgba_;
    uint32_t title_icon_width_ = 0;
    uint32_t title_icon_height_ = 0;
    bool title_icon_loaded_ = false;
    // After the guide closes, the game's input stays blocked until every
    // button is released, so the press that closed it doesn't reach the game.
    bool guest_input_release_wait_ = false;

    // A texture for `art` (resolving Music Assistant art on first use), or
    // null while unavailable - draw its placeholder then.
    void* ArtTexture(DashboardArt& art);
    // The current Music browser level as list rows (shared by the dashboard
    // and the guide); sets `empty_message` when there's nothing to list.
    void BuildMusicRows(std::vector<DashboardRow>* out_rows,
                        std::string* empty_message);

    std::vector<MusicNavFrame> music_nav_stack_;

    std::vector<GameEntry> games_list_;
    bool games_list_loaded_ = false;
    bool games_load_failed_ = false;
    int games_selected_index_ = 0;

    // Achievements (see kernel/xam/achievements.h): a game's list, each with
    // its picture decoded, for the guide's Achievements tab and the
    // dashboard's achievements panel.
    struct GameAchievements {
      uint32_t title_id = 0;
      std::vector<kernel::xam::AchievementInfo> list;
      struct Icon {
        std::vector<uint8_t> rgba;
        uint32_t width = 0;
        uint32_t height = 0;
      };
      // Parallel to list.
      std::vector<Icon> icons;
    };
    static void ReadGameAchievements(const kernel::util::XdbfGameData& db,
                                     uint32_t title_id, GameAchievements* out);
    // What's been unlocked in a title - read from disk the first time it's
    // asked for after the guide or dashboard opens (ResetBrowsingState
    // forgets it), so it's current without being read every frame.
    const kernel::xam::AchievementProgress& AchievementProgressFor(
        uint32_t title_id);
    std::unordered_map<uint32_t, kernel::xam::AchievementProgress>
        achievement_progress_;
    // Draws a list of achievements in p0-p1 - pictures, names, descriptions
    // (hidden for locked secret ones), gamerscore and when each was unlocked
    // - scrolled to keep `selected` in view. `light` for the guide's white
    // panel, otherwise for the dashboard's dark one.
    void DrawAchievementList(ImDrawList* dl, ImFont* font, float s, float x0,
                             float y0, float x1, float y1,
                              const GameAchievements& achievements,
                             const kernel::xam::AchievementProgress& progress,
                             int selected, bool light);
    // "12 of 50 unlocked  ·  120 / 1000 G".
    static std::string AchievementSummary(
        const GameAchievements& achievements,
        const kernel::xam::AchievementProgress& progress);

    // The guide's Achievements tab: the running game's, read when the tab is
    // first shown for it.
    void LoadTitleAchievements();
    GameAchievements title_achievements_;
    bool title_achievements_loaded_ = false;
    int achievements_selected_index_ = 0;

    // The dashboard's: read in the background (uncached, from the game
    // itself) for the highlighted game. Like game_art_, entries are inserted
    // complete and never changed or erased after.
    void EnsureGameAchievementsLoading();
    // Null until loaded.
    const GameAchievements* FindGameAchievements(
        const std::filesystem::path& path);
    std::mutex game_achievements_mutex_;
    std::unordered_map<std::string, GameAchievements> game_achievements_;
    std::thread game_achievements_thread_;
    std::atomic<bool> game_achievements_thread_running_{false};
    // The highlighted game's achievements over the coverflow (X).
    bool achievements_panel_open_ = false;

    // "Achievement unlocked" notifications, shown one at a time over the
    // game (and the guide) for a few seconds each.
    void DrawAchievementToasts(ImGuiIO& io);
    struct AchievementToast {
      kernel::xam::AchievementNotification notification;
      std::string texture_key;
      std::vector<uint8_t> icon_rgba;
      uint32_t icon_width = 0;
      uint32_t icon_height = 0;
    };
    std::vector<AchievementToast> achievement_toasts_;
    // When the first in achievement_toasts_ started showing.
    std::chrono::steady_clock::time_point achievement_toast_start_;
    uint32_t achievement_toast_count_ = 0;

    // Status line shown under the tab strip regardless of which tab is
    // active - "Playing: X", a play_media failure, a games_path scan
    // failure, and so on.
    std::string last_action_message_;
  };

  explicit EmulatorWindow(Emulator* emulator,
                          ui::WindowedAppContext& app_context);

  bool Initialize();

  // For comparisons, use GetSwapPostEffectForCvarValue instead as the default
  // fallback may be used for multiple values.
  static const char* GetCvarValueForSwapPostEffect(
      gpu::CommandProcessor::SwapPostEffect effect);
  static gpu::CommandProcessor::SwapPostEffect GetSwapPostEffectForCvarValue(
      const std::string& cvar_value);
  // For comparisons, use GetGuestOutputPaintEffectForCvarValue instead as the
  // default fallback may be used for multiple values.
  static const char* GetCvarValueForGuestOutputPaintEffect(
      ui::Presenter::GuestOutputPaintConfig::Effect effect);
  static ui::Presenter::GuestOutputPaintConfig::Effect
  GetGuestOutputPaintEffectForCvarValue(const std::string& cvar_value);
  static ui::Presenter::GuestOutputPaintConfig
  GetGuestOutputPaintConfigForCvars();
  void ApplyDisplayConfigForCvars();

  void OnKeyDown(ui::KeyEvent& e);
  void FileDrop(const std::filesystem::path& filename);
  void FileOpen();
  void FileClose();
  void ShowContentDirectory();
  void CpuTimeScalarReset();
  void CpuTimeScalarSetHalf();
  void CpuTimeScalarSetDouble();
  void CpuBreakIntoDebugger();
  void CpuBreakIntoHostDebugger();
  void GpuTraceFrame();
  void GpuClearCaches();
  void ToggleDisplayConfigDialog();
  void ToggleGpuConfigDialog();
  // Quits the running title by starting a fresh Xenia process (on `target`,
  // or on the dashboard if empty) and hard-exiting this one, the same way a
  // normal exit already does. Stopping a title in-process
  // (Emulator::TerminateTitle) can deadlock on games with several active
  // guest threads, and a new process sidesteps that entirely. UI thread only;
  // returns only on failure.
  void RelaunchXenia(const std::filesystem::path& target);
  // Quits Xenia the same way (settings saved, then a hard exit that never
  // stops the title in-process) - for launcher_mode, where the frontend that
  // started Xenia takes over once this process exits. Doesn't return.
  void QuitXenia();
  // Running inside a frontend shell's window (--embed_parent_window) rather
  // than as a window of its own: no menu bar or fullscreen (the shell owns
  // those), Turn Off returns to the shell, and restarts stay inside it.
  static bool IsInShell();
  // Exit code for RelaunchXenia, telling a shell that another Xenia is taking
  // over its window rather than the console being turned off.
  static constexpr int kRelaunchExitCode = 3;
  void ShowCompatibility();
  void ShowFAQ();
  void ShowBuildCommit();

  Emulator* emulator_;
  ui::WindowedAppContext& app_context_;
  EmulatorWindowListener window_listener_;
  std::unique_ptr<ui::Window> window_;
  std::unique_ptr<ui::ImGuiDrawer> imgui_drawer_;
  std::unique_ptr<DisplayConfigGameConfigLoadCallback>
      display_config_game_config_load_callback_;
  // Creation may fail, in this case immediate drawer UI must not be drawn.
  std::unique_ptr<ui::ImmediateDrawer> immediate_drawer_;

  bool emulator_initialized_ = false;

  std::string base_title_;
  bool initializing_shader_storage_ = false;

  std::unique_ptr<DisplayConfigDialog> display_config_dialog_;
  std::unique_ptr<GpuConfigDialog> gpu_config_dialog_;
  std::unique_ptr<FpsOverlay> fps_overlay_;
  std::unique_ptr<GuideMenuOverlay> guide_menu_overlay_;
  // Keeps Xenia connected to Music Assistant as a real player in the
  // background - started once audio_system() exists (OnEmulatorInitialized),
  // not in the constructor (see sendspin_player_service.h).
  std::unique_ptr<apu::sendspin::SendspinPlayerService> sendspin_player_service_;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_EMULATOR_WINDOW_H_
