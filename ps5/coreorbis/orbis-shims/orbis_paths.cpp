// PS5 port (vk-285-33): the /data/PCSX2 folder layout (include-orbis/OrbisPaths.h).
//
// Copyright (C) 2026 Spyros
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OrbisPaths.h"

#include <sys/stat.h>
#include <unistd.h>

namespace
{
constexpr const char* kRoot = "/data/PCSX2";

bool IsDir(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
} // namespace

std::string OrbisDir(const char* sub)
{
	std::string dir = std::string(kRoot) + "/" + sub;
	return IsDir(dir) ? dir : std::string(kRoot);
}

// vk-285-46: stat(), not access(). Before the HEN jailbreak the app's sandbox answers access() on a
// file that exists with EPERM (vk-285-45's log: the flags folder stats fine, mode 777, and access
// fails with errno 1), so every flag read as absent there -- in vk-285-43/44 that set the driver's
// environment without vk_renderer. stat() works on both sides of the jailbreak.
namespace
{
bool Exists(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0;
}
} // namespace

std::string OrbisFlagPath(const char* name)
{
	std::string in_flags = std::string(kRoot) + "/flags/" + name;
	if (Exists(in_flags))
		return in_flags;
	return std::string(kRoot) + "/" + name;
}

bool OrbisFlag(const char* name)
{
	return Exists(OrbisFlagPath(name));
}

extern "C" {
char g_orbis_pf_log[160] = "/data/PCSX2/pf.log";
}

std::string OrbisLogPath(const char* name)
{
	return OrbisDir("logs") + "/" + name;
}
