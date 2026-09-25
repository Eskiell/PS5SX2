// PS5 port frontend: covers. A worker thread paints each game's spine and placeholder front, then
// finds its cover -- a file the user put in the covers folder, one downloaded before (the cache),
// or a download -- decodes it and picks the glow colour from it. The main thread polls the results
// and turns them into textures.
//
// Copyright (C) 2026 Spyros
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "fe_games.h"
#include "fe_text.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <pthread.h>
#include <vector>

namespace fe
{
struct CoverImage
{
	enum Kind
	{
		Spine,
		Placeholder,
		Cover
	};
	int game = -1;
	Kind kind = Spine;
	int width = 0, height = 0;
	std::vector<uint8_t> rgba;
	float glow[3] = {0, 0, 0};
	bool has_glow = false;
	const char* source = ""; // "manual", "cache", "download"
};

struct CoverConfig
{
	std::string manual_dir; // covers the user supplies: <serial>.jpg/.png or <file name>.jpg/.png
	std::string cache_dir;  // downloads: <serial>.jpg, and <serial>.missing after a 404
	std::string url_template; // "${serial}" is replaced
	bool allow_download = true;
};

// Fetches `url` into `out`; returns the HTTP status (200 on success), or a negative value when the
// network failed.
using DownloadFn = std::function<int(const std::string& url, std::vector<uint8_t>& out)>;

class CoverService
{
public:
	~CoverService();
	void Start(const std::vector<GameInfo>& games, const Fonts* fonts, const CoverConfig& cfg, DownloadFn download);

	// Asks the worker to stop and waits up to `timeout_ms` (forever when negative); false if it is
	// still busy then (a download that hasn't timed out), in which case the object must stay alive.
	bool Stop(int timeout_ms = -1);

	// Tells the worker to finish after the step it is in; Stop then waits for it.
	void RequestStop() { m_stop = true; }

	// Main thread: the next finished image, if any.
	bool Poll(CoverImage& out);

	// Main thread: games nearer `selected` go first.
	void SetSelected(int selected);

	// A line for the UI: "" when idle, else e.g. "Downloading covers  3 / 11".
	std::string Status() const;

	// Paints a placeholder front / a spine (also used before the worker runs).
	static void PaintSpine(const Fonts& fonts, const GameInfo& g, CoverImage& out);
	static void PaintPlaceholder(const Fonts& fonts, const GameInfo& g, CoverImage& out);

	// Decodes an image file's bytes to RGBA8, at most `max_h` rows tall; false if it isn't one.
	static bool Decode(const std::vector<uint8_t>& bytes, int max_h, CoverImage& out);

	// The games whose cover is none of: the user's own file, a cached download, a 404 in the last
	// two weeks. For fetching them before the HEN jailbreak (fe_ps5.cpp, vk-285-44).
	static std::vector<int> MissingCovers(const std::vector<GameInfo>& games, const CoverConfig& cfg);

	// Downloads those covers into the cache, one after another, until `budget_s` seconds have
	// passed or the network fails. Only images that decode are kept. Returns how many were saved.
	static int Prefetch(const std::vector<GameInfo>& games, const std::vector<int>& which, const CoverConfig& cfg,
		const DownloadFn& download, double budget_s);

private:
	static void* ThreadMain(void* self);
	void Run();
	bool FindCover(int index, CoverImage& out);
	int NextGame(const std::vector<bool>& done) const;

	std::vector<GameInfo> m_games;
	const Fonts* m_fonts = nullptr;
	CoverConfig m_cfg;
	DownloadFn m_download;
	pthread_t m_thread{};
	bool m_thread_started = false;
	std::atomic<bool> m_stop{false};
	std::atomic<bool> m_finished{false};
	std::atomic<int> m_selected{0};
	mutable std::mutex m_mutex;
	std::deque<CoverImage> m_results;
	int m_download_total = 0, m_download_done = 0;
	bool m_offline = false;
	bool m_busy = false;
};
} // namespace fe
