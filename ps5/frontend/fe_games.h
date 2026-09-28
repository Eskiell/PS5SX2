// PS5 port frontend: the game list: disc images (.iso, .chd), their serials (read from the disc's SYSTEM.CNF),
// display titles made from Redump-style file names, and badges from the game's settings.
//
// Copyright (C) 2026 Spyros
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fe
{
struct GameInfo
{
	std::string path;   // full path of the image
	std::string file;   // its file name
	std::string stem;   // the file name without the extension (the settings file's name)
	std::string title;  // display title
	std::string region; // "USA", "Europe", ... (may be empty)
	std::string extra;  // the other bracketed parts of the name, e.g. "En,Ja"
	std::string serial; // "SLUS-21351" (empty when the disc could not be read)
	uint64_t bytes = 0;
	std::vector<std::string> badges; // "6x", "16:9", "60 FPS"
};

// A disc image's file name: .iso or (vk-285-108) .chd, in any case, not hidden.
bool IsDiscImageName(const char* name);

// Lists the disc images in `dirs` (the first folder wins for a name found twice), sorted by title.
std::vector<GameInfo> ScanGames(const std::vector<std::string>& dirs);

// "SLUS-21351" from the image's SYSTEM.CNF (ISO 9660); empty if unreadable. A .chd is read through libchdr
// (a DVD's 2048-byte units or a CD's raw frames); one that needs a parent CHD reads as empty.
std::string ReadSerial(const std::string& image_path);

// vk-285-108: where ReadSerial keeps the CHD serials it found (opening a CHD costs tens of milliseconds);
// empty: none kept.
void SetSerialCacheFile(const std::string& path);

// Display title and region from a file name stem ("Lord of the Rings, The - The Two Towers (USA)").
void MakeTitle(const std::string& stem, std::string& title, std::string& region, std::string& extra);

// Badges from settings/<stem>.ini, gs.ini and the patches folder.
void ReadBadges(GameInfo& g, const std::string& settings_dir, const std::string& gs_ini, const std::string& patches_dir);
} // namespace fe
