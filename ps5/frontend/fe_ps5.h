// PS5 port frontend: the console side (fe_ps5.cpp).
//
// Copyright (C) 2026 Spyros
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

struct OrbisFrontendPaths
{
	std::string games_dir;    // /data/PCSX2/games
	std::string top_dir;      // /data/PCSX2 (older setups keep images here; lastgame.txt lives here)
	std::string settings_dir; // per-game settings, <image name>.ini
	std::string gs_ini;       // the shared settings
	std::string patches_dir;  // *.pnach, for the 16:9 badge
	std::string covers_dir;   // covers the user supplies (PCSX2's covers folder)
	std::string cache_dir;    // downloaded covers
	bool allow_download = true;
	bool sound = true;        // the shelf's key sounds (vk-285-47; off with the nomenusound flag)
	std::string settings_log; // vk-285-51: logs/settings.log, what the settings page changed
};

// Before the HEN jailbreak: downloads the covers the cache lacks, for at most `budget_s` seconds.
// HTTPS works there and failed after the jailbreak (vk-285-41/42). `notify` (may be null) is told
// how many are coming before the first request. Returns how many it saved (0 when none were
// missing, or with downloads off).
int orbis_frontend_prefetch_covers(const OrbisFrontendPaths& paths, double budget_s, void (*notify)(const char*));

// vk-285-50: the settings page's web server (fe_web.cpp), for phones and PCs on the LAN. Start it
// once, after the jailbreak; it runs until the app ends, through the shelf and the game. The shelf
// shows its address as a QR code. The access token is kept in /data/PCSX2/webui_token.txt.
bool orbis_web_start(const OrbisFrontendPaths& paths, const char* build_tag);
// The disc image PCSX2 runs, for the page's "now playing".
void orbis_web_now_playing(const std::string& image_path);

// vk-285-51: the settings log, logs/settings.log. The settings page writes what it changed there
// (fe_web.cpp) and the app what it did around it: starts, games, live applies, crashes and GPU
// hangs. Set its path once at startup; orbis_event_log() then appends "<local time>  <line>" with
// open()/write() only, so the crash handler and the GPU-hang exit can use it too.
void orbis_event_log_init(const std::string& path);

// vk-285-53: hides the system's launch screen (sce_sys/pic1.dds), which covers the app until then.
// The shelf calls it on its first frame; main() calls it again before the plain list or the game
// in case the shelf didn't run. Only the first call does anything; it returns that call's result.
int orbis_hide_splash();
extern "C" void orbis_event_log(const char* line);

// Shows the shelf and returns the picked image's path. *ran is false when the frontend could not
// start (no Vulkan display, say): the caller then shows the plain list instead. With no images the
// result is empty; with one, or the nomenu rule applied by the caller, it returns at once.
std::string orbis_frontend_run(const OrbisFrontendPaths& paths, const char* build_tag, bool* ran);
