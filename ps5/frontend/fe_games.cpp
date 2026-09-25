// PS5 port frontend: the game list (see fe_games.h).
//
// Copyright (C) 2026 Spyros
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_games.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fe
{
namespace
{
bool EndsWithIso(const char* name)
{
	const size_t n = std::strlen(name);
	return n > 4 && name[0] != '.' && name[n - 4] == '.' && (name[n - 3] | 0x20) == 'i' && (name[n - 2] | 0x20) == 's' &&
	       (name[n - 1] | 0x20) == 'o';
}

std::string Lower(std::string s)
{
	for (char& c : s)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

std::string Trim(const std::string& s)
{
	size_t a = 0, b = s.size();
	while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
		a++;
	while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
		b--;
	return s.substr(a, b - a);
}

bool ReadAt(int fd, uint64_t offset, void* buf, size_t len)
{
	uint8_t* p = static_cast<uint8_t*>(buf);
	while (len)
	{
		const ssize_t n = pread(fd, p, len, static_cast<off_t>(offset));
		if (n <= 0)
			return false;
		p += n;
		offset += static_cast<uint64_t>(n);
		len -= static_cast<size_t>(n);
	}
	return true;
}

uint32_t Le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

const char* const kRegions[] = {"USA", "Europe", "Japan", "Korea", "Asia", "World", "Australia", "France", "Germany",
	"Italy", "Spain", "UK", "Canada", "Brazil", "Russia", "China", "Taiwan", "Netherlands", "Sweden"};
} // namespace

std::string ReadSerial(const std::string& iso_path)
{
	const int fd = open(iso_path.c_str(), O_RDONLY);
	if (fd < 0)
		return {};
	std::string serial;
	uint8_t pvd[2048];
	if (ReadAt(fd, 16 * 2048ull, pvd, sizeof(pvd)) && pvd[0] == 1 && std::memcmp(pvd + 1, "CD001", 5) == 0)
	{
		const uint8_t* root = pvd + 156;
		const uint32_t root_lba = Le32(root + 2);
		const uint32_t root_len = std::min<uint32_t>(Le32(root + 10), 64 * 2048);
		std::vector<uint8_t> dir(root_len);
		if (root_len && ReadAt(fd, static_cast<uint64_t>(root_lba) * 2048, dir.data(), dir.size()))
		{
			for (size_t off = 0; off < dir.size();)
			{
				const uint8_t len = dir[off];
				if (len == 0)
				{
					off = (off / 2048 + 1) * 2048; // records don't cross sectors
					continue;
				}
				if (off + len > dir.size() || len < 34)
					break;
				const uint8_t name_len = dir[off + 32];
				std::string name(reinterpret_cast<const char*>(&dir[off + 33]), std::min<size_t>(name_len, len - 33));
				if (Lower(name).rfind("system.cnf", 0) == 0)
				{
					const uint32_t lba = Le32(&dir[off + 2]);
					const uint32_t size = std::min<uint32_t>(Le32(&dir[off + 10]), 4096);
					std::string cnf(size, '\0');
					if (ReadAt(fd, static_cast<uint64_t>(lba) * 2048, cnf.data(), size))
					{
						// BOOT2 = cdrom0:\SLUS_213.51;1
						const size_t b = cnf.find("BOOT2");
						const size_t s = cnf.find('\\', b == std::string::npos ? 0 : b);
						if (b != std::string::npos && s != std::string::npos)
						{
							std::string elf;
							for (size_t i = s + 1; i < cnf.size() && cnf[i] != ';' && cnf[i] != '\r' && cnf[i] != '\n'; i++)
								elf += cnf[i];
							// SLUS_213.51 -> SLUS-21351
							std::string out;
							for (char c : elf)
							{
								if (c == '_')
									out += '-';
								else if (c != '.')
									out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
							}
							if (out.size() >= 9 && out.size() <= 12)
								serial = out;
						}
					}
					break;
				}
				off += len;
			}
		}
	}
	close(fd);
	return serial;
}

void MakeTitle(const std::string& stem, std::string& title, std::string& region, std::string& extra)
{
	std::string base = stem;
	std::vector<std::string> groups;
	// Bracketed groups at the end: "(USA) (En,Ja)" or "[!]".
	for (;;)
	{
		const std::string t = Trim(base);
		if (t.empty() || (t.back() != ')' && t.back() != ']'))
		{
			base = t;
			break;
		}
		const char open = t.back() == ')' ? '(' : '[';
		const size_t o = t.rfind(open);
		if (o == std::string::npos || o == 0)
		{
			base = t;
			break;
		}
		groups.insert(groups.begin(), t.substr(o + 1, t.size() - o - 2));
		base = t.substr(0, o);
	}
	region.clear();
	extra.clear();
	for (const std::string& g : groups)
	{
		bool is_region = false;
		for (const char* r : kRegions)
			if (g.find(r) == 0)
				is_region = true;
		if (is_region && region.empty())
			region = g;
		else
			extra += (extra.empty() ? "" : ", ") + g;
	}
	// "Lord of the Rings, The - The Two Towers" -> "The Lord of the Rings - The Two Towers".
	const size_t dash = base.find(" - ");
	std::string first = dash == std::string::npos ? base : base.substr(0, dash);
	std::string rest = dash == std::string::npos ? std::string() : base.substr(dash + 3);
	for (const char* art : {", The", ", A", ", An"})
	{
		const size_t al = std::strlen(art);
		if (first.size() > al && first.compare(first.size() - al, al, art) == 0)
		{
			first = std::string(art + 2) + " " + first.substr(0, first.size() - al);
			break;
		}
	}
	// The first " - " is a subtitle's colon; later ones stay.
	title = rest.empty() ? first : first + ": " + rest;
	if (title.empty())
		title = stem;
}

std::vector<GameInfo> ScanGames(const std::vector<std::string>& dirs)
{
	std::vector<GameInfo> games;
	for (const std::string& dir : dirs)
	{
		DIR* d = opendir(dir.c_str());
		if (!d)
			continue;
		while (const dirent* e = readdir(d))
		{
			if (!EndsWithIso(e->d_name))
				continue;
			const std::string file = e->d_name;
			if (std::any_of(games.begin(), games.end(), [&](const GameInfo& g) { return g.file == file; }))
				continue;
			GameInfo g;
			g.path = dir + "/" + file;
			struct stat st = {};
			if (stat(g.path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
				continue;
			g.file = file;
			g.stem = file.substr(0, file.size() - 4);
			g.bytes = static_cast<uint64_t>(st.st_size);
			MakeTitle(g.stem, g.title, g.region, g.extra);
			games.push_back(g);
		}
		closedir(d);
	}
	std::sort(games.begin(), games.end(), [](const GameInfo& a, const GameInfo& b) {
		const std::string la = Lower(a.title), lb = Lower(b.title);
		return la != lb ? la < lb : a.file < b.file;
	});
	return games;
}

namespace
{
// The value of `key=` in a settings file (the last one wins), or empty.
std::string IniValue(const std::string& path, const char* key)
{
	std::string value;
	FILE* f = std::fopen(path.c_str(), "r");
	if (!f)
		return value;
	char line[512];
	const size_t kl = std::strlen(key);
	while (std::fgets(line, sizeof(line), f))
	{
		std::string l = Trim(line);
		if (l.compare(0, kl, key) == 0 && l.size() > kl && l[kl] == '=')
			value = Trim(l.substr(kl + 1));
	}
	std::fclose(f);
	return value;
}

bool IniHasLine(const std::string& path, const char* line_text)
{
	FILE* f = std::fopen(path.c_str(), "r");
	if (!f)
		return false;
	char line[512];
	bool found = false;
	while (!found && std::fgets(line, sizeof(line), f))
		found = Lower(Trim(line)) == Lower(line_text);
	std::fclose(f);
	return found;
}

bool FileHas(const std::string& path, const char* text)
{
	FILE* f = std::fopen(path.c_str(), "r");
	if (!f)
		return false;
	std::string all;
	char buf[4096];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && all.size() < (1u << 20))
		all.append(buf, n);
	std::fclose(f);
	return all.find(text) != std::string::npos;
}
} // namespace

void ReadBadges(GameInfo& g, const std::string& settings_dir, const std::string& gs_ini, const std::string& patches_dir)
{
	g.badges.clear();
	const std::string ini = settings_dir + "/" + g.stem + ".ini";
	std::string scale = IniValue(ini, "upscale_multiplier");
	if (scale.empty())
		scale = IniValue(gs_ini, "upscale_multiplier");
	if (!scale.empty() && scale != "1")
		g.badges.push_back(scale + "x");
	bool widescreen = false;
	if (!g.serial.empty() && IniValue(gs_ini, "EmuCore/EnableWideScreenPatches") == "true")
	{
		if (DIR* d = opendir(patches_dir.c_str()))
		{
			while (const dirent* e = readdir(d))
				if (std::strncmp(e->d_name, g.serial.c_str(), g.serial.size()) == 0 &&
					FileHas(patches_dir + "/" + e->d_name, "[Widescreen 16:9]"))
					widescreen = true;
			closedir(d);
		}
	}
	if (widescreen)
		g.badges.push_back("16:9");
	if (IniHasLine(ini, "Patches/Enable=60 FPS"))
		g.badges.push_back("60 FPS");
}
} // namespace fe
