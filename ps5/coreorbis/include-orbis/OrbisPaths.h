// PS5 port (vk-285-33): where things live in /data/PCSX2.
//
//   games/       disc images (the game selector also still looks in the top folder)
//   bios/        the BIOS and its .mec/.nvm
//   memcards/    Mcd001.ps2, Mcd002.ps2
//   savestates/  *.p2s
//   settings/    per-game settings (<disc image name>.ini)
//   patches/     *.pnach
//   textures/    replacement textures
//   resources/   GameIndex.yaml, shaders/
//   cache/       PCSX2's and the Vulkan driver's shader caches
//   flags/       the on/off switch files (vk_async, swjit, ...)
//   logs/        boot.log, emulog.txt, stderr.log and diagnostic output
//   gs.ini, live.ini, lastgame.txt, playtime.dat, pid.txt stay in the top folder.
//
// Each folder is used when it exists; without it the top folder is, as in the old flat layout, so
// an older console setup keeps working and a new build can go on before the files move.
//
// Copyright (C) 2026 Spyros
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

// "/data/PCSX2/<sub>" when that folder exists, else "/data/PCSX2".
std::string OrbisDir(const char* sub);
// A switch file: "/data/PCSX2/flags/<name>" when it's there, else "/data/PCSX2/<name>" (the old place).
std::string OrbisFlagPath(const char* name);
// Whether that switch is on (the file exists in either place).
bool OrbisFlag(const char* name);
// Where a log or diagnostic file goes: logs/<name>, or the top folder without logs/.
std::string OrbisLogPath(const char* name);
// pf.log's path for the page-fault handler (no allocation there); main-boot sets it at start.
extern "C" char g_orbis_pf_log[160];
