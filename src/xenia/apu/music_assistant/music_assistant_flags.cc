/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/music_assistant/music_assistant_flags.h"

DEFINE_string(music_assistant_host, "",
              "Music Assistant server hostname/IP (e.g. 192.168.1.50). "
              "Leave empty to disable the guide menu's Music tile.",
              "MusicAssistant");
DEFINE_int32(music_assistant_port, 8095,
            "Music Assistant server HTTP API port.", "MusicAssistant");
DEFINE_string(music_assistant_token, "",
              "Music Assistant long-lived access token (Settings -> Profile "
              "in the Music Assistant web UI).",
              "MusicAssistant");
DEFINE_bool(music_assistant_stop_on_exit, true,
            "When Xenia is closed, stop the music on its Music Assistant "
            "player and clear that player's queue - like switching a console "
            "off - instead of leaving it paused where it was. Switching games "
            "or returning to the dashboard doesn't count as closing.",
            "MusicAssistant");
