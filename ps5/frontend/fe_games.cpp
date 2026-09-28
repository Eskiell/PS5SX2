// PS5 port frontend: the game list (see fe_games.h).
//
// Copyright (C) 2026 Spyros
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_games.h"

#include "libchdr/chd.h" // vk-285-108: CHD images (3rdparty/libchdr)

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>

namespace fe
{
namespace
{
// `name` ends in `ext` (".iso", lower case), in any case, and isn't hidden.
bool HasExtension(const char* name, const char* ext)
{
	const size_t n = std::strlen(name), e = std::strlen(ext);
	if (n <= e || name[0] == '.')
		return false;
	for (size_t i = 0; i < e; i++)
		if (std::tolower(static_cast<unsigned char>(name[n - e + i])) != ext[i])
			return false;
	return true;
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

namespace
{
// A disc image's 2048-byte data sectors, read by number.
class SectorReader
{
public:
	virtual ~SectorReader() = default;
	virtual bool Read(uint32_t lba, void* buf, size_t len) = 0; // `len` bytes from the start of sector `lba`
};

class IsoSectors final : public SectorReader
{
public:
	explicit IsoSectors(int fd)
		: m_fd(fd)
	{
	}
	bool Read(uint32_t lba, void* buf, size_t len) override { return ReadAt(m_fd, static_cast<uint64_t>(lba) * 2048, buf, len); }

private:
	int m_fd;
};

// vk-285-108: a CHD through libchdr. A DVD image (chdman createdvd) holds 2048-byte units; a CD image
// (createcd) 2448-byte frames (2352 of sector, 96 of subcode). In a frame the data starts 24 bytes in for a
// raw mode 2 track (the PS2's CDs from a .cue/.bin), 16 for raw mode 1, and at 0 for a track chdman got as
// 2048-byte sectors (vk-285-109: createcd from an .iso, which people do with DVD games too), 8 for 2336-byte
// mode 2. Read() stays inside one sector, so a CD sector never spans two frames.
class ChdSectors final : public SectorReader
{
public:
	~ChdSectors() override
	{
		if (m_chd)
			chd_close(m_chd);
	}

	bool Open(const std::string& path)
	{
		const chd_error err = chd_open(path.c_str(), CHD_OPEN_READ, nullptr, &m_chd);
		if (err != CHDERR_NONE)
		{
			m_chd = nullptr; // a CHD that needs its parent reads as no serial (PCSX2 finds the parent itself)
			m_what = std::string("libchdr can't open it: ") + chd_error_string(err);
			return false;
		}
		const chd_header* const h = chd_get_header(m_chd);
		if (!h || h->hunkbytes == 0 || h->unitbytes == 0)
		{
			m_what = "no hunk or unit size in its header";
			return false;
		}
		m_unit = h->unitbytes;
		m_hunk_bytes = h->hunkbytes;
		m_buf.resize(m_hunk_bytes);
		char meta[256] = {};
		uint32_t len = 0;
		if (chd_get_metadata(m_chd, CDROM_TRACK_METADATA2_TAG, 0, meta, sizeof(meta) - 1, &len, nullptr, nullptr) == CHDERR_NONE ||
			chd_get_metadata(m_chd, CDROM_TRACK_METADATA_TAG, 0, meta, sizeof(meta) - 1, &len, nullptr, nullptr) == CHDERR_NONE)
			m_track = meta;
		char codecs[64] = {};
		for (int i = 0; i < 4; i++)
		{
			const uint32_t c = h->compression[i];
			if (c == 0)
				continue;
			// v5: a four-letter code (zlib, lzma, cdlz...); v1-v4: a number (1 zlib, 2 zlib+, 3 A/V).
			char tag[16];
			const bool fourcc = ((c >> 24) & 0xff) >= 0x20 && ((c >> 16) & 0xff) >= 0x20 && ((c >> 8) & 0xff) >= 0x20 && (c & 0xff) >= 0x20;
			if (fourcc)
				std::snprintf(tag, sizeof(tag), "%c%c%c%c", static_cast<char>(c >> 24), static_cast<char>(c >> 16), static_cast<char>(c >> 8),
					static_cast<char>(c));
			else
				std::snprintf(tag, sizeof(tag), "#%u", c);
			std::snprintf(codecs + std::strlen(codecs), sizeof(codecs) - std::strlen(codecs), "%s%s", codecs[0] ? "," : "", tag);
		}
		char what[512];
		std::snprintf(what, sizeof(what), "v%u, %u-byte units, %u-byte hunks, %llu MB of data, codecs %s%s%s", h->version,
			h->unitbytes, h->hunkbytes, static_cast<unsigned long long>(h->logicalbytes >> 20), codecs[0] ? codecs : "none",
			m_track.empty() ? "" : ", track ", m_track.c_str());
		m_what = what;
		if (m_unit != 2352 && m_unit != 2448)
			return true; // 2048-byte sectors back to back
		// A CD: the data offset that puts the volume descriptor ("\1CD001") at sector 16, the track type's first.
		// The TYPE field ("TRACK:1 TYPE:MODE2_RAW SUBTYPE:NONE ..."; not PGTYPE, the pregap's type).
		std::string type;
		const size_t t = m_track.find(" TYPE:");
		if (t != std::string::npos)
			type = m_track.substr(t + 6, m_track.find(' ', t + 6) - (t + 6));
		uint32_t first = 24;
		if (type == "MODE1_RAW")
			first = 16;
		else if (type == "MODE1" || type == "MODE2_FORM1")
			first = 0;
		else if (type == "MODE2" || type == "MODE2_FORM_MIX")
			first = 8;
		for (const uint32_t offset : {first, 24u, 16u, 0u, 8u})
		{
			m_offset = offset;
			uint8_t pvd[6];
			if (Read(16, pvd, sizeof(pvd)) && pvd[0] == 1 && std::memcmp(pvd + 1, "CD001", 5) == 0)
			{
				m_what += ", data " + std::to_string(offset) + " bytes into each frame";
				return true;
			}
		}
		m_what += ", no ISO 9660 volume descriptor at sector 16 at any data offset";
		m_offset = first;
		return true;
	}

	// What the image is, for the log (vk-285-109): the header, the codecs, the first track, the data offset.
	const std::string& What() const { return m_what; }

	bool Read(uint32_t lba, void* buf, size_t len) override
	{
		if (len > 2048)
		{
			// Sector by sector (a CD's sectors aren't contiguous).
			uint8_t* p = static_cast<uint8_t*>(buf);
			for (; len; lba++)
			{
				const size_t n = std::min<size_t>(len, 2048);
				if (!Read(lba, p, n))
					return false;
				p += n;
				len -= n;
			}
			return true;
		}
		const bool raw = m_unit == 2352 || m_unit == 2448;
		uint64_t pos = raw ? static_cast<uint64_t>(lba) * m_unit + m_offset : static_cast<uint64_t>(lba) * 2048;
		uint8_t* p = static_cast<uint8_t*>(buf);
		while (len)
		{
			const uint64_t hunk = pos / m_hunk_bytes;
			const size_t in = static_cast<size_t>(pos % m_hunk_bytes);
			if (hunk > 0xffffffffull)
				return false;
			if (hunk != m_cached)
			{
				if (chd_read(m_chd, static_cast<uint32_t>(hunk), m_buf.data()) != CHDERR_NONE)
					return false;
				m_cached = hunk;
			}
			const size_t n = std::min<size_t>(len, m_hunk_bytes - in);
			std::memcpy(p, m_buf.data() + in, n);
			p += n;
			pos += n;
			len -= n;
		}
		return true;
	}

private:
	chd_file* m_chd = nullptr;
	uint32_t m_unit = 0;
	uint32_t m_hunk_bytes = 0;
	uint32_t m_offset = 0;
	uint64_t m_cached = ~0ull; // the hunk in m_buf
	std::vector<uint8_t> m_buf;
	std::string m_track; // the first track's metadata (CD images)
	std::string m_what;
};

// "SLUS-21351" from SYSTEM.CNF's BOOT2 line, found in the root directory of the ISO 9660 file system.
std::string SerialFromDisc(SectorReader& disc)
{
	std::string serial;
	uint8_t pvd[2048];
	if (!disc.Read(16, pvd, sizeof(pvd)) || pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0)
		return serial;
	const uint8_t* root = pvd + 156;
	const uint32_t root_lba = Le32(root + 2);
	const uint32_t root_len = std::min<uint32_t>(Le32(root + 10), 64 * 2048);
	std::vector<uint8_t> dir(root_len);
	if (!root_len || !disc.Read(root_lba, dir.data(), dir.size()))
		return serial;
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
			if (disc.Read(lba, cnf.data(), size))
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
	return serial;
}

// vk-285-108: a CHD's serial costs its map's decompression (tens of milliseconds for a DVD), so the ones
// found are kept in a file, one "<path>\t<size>\t<mtime>\t<serial>" line each (a later line wins).
std::mutex s_serial_mutex;
std::string s_serial_file;
bool s_serial_loaded = false;
std::unordered_map<std::string, std::string> s_serials; // "<path>\t<size>\t<mtime>" -> serial

std::string SerialKey(const std::string& path)
{
	struct stat st = {};
	if (stat(path.c_str(), &st) != 0)
		return {};
	return path + "\t" + std::to_string(static_cast<long long>(st.st_size)) + "\t" +
	       std::to_string(static_cast<long long>(st.st_mtime));
}

void LoadSerialsLocked()
{
	if (s_serial_loaded || s_serial_file.empty())
		return;
	s_serial_loaded = true;
	FILE* f = std::fopen(s_serial_file.c_str(), "rb");
	if (!f)
		return;
	char line[1024];
	while (std::fgets(line, sizeof(line), f))
	{
		std::string l = line;
		while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
			l.pop_back();
		const size_t tab = l.rfind('\t');
		if (tab != std::string::npos && tab > 0 && tab + 1 < l.size())
			s_serials[l.substr(0, tab)] = l.substr(tab + 1);
	}
	std::fclose(f);
}

std::string ChdSerial(const std::string& path)
{
	const std::string key = SerialKey(path);
	if (!key.empty())
	{
		std::lock_guard<std::mutex> lock(s_serial_mutex);
		LoadSerialsLocked();
		const auto it = s_serials.find(key);
		if (it != s_serials.end())
			return it->second;
	}
	ChdSectors disc;
	const std::string serial = disc.Open(path) ? SerialFromDisc(disc) : std::string();
	if (!serial.empty() && !key.empty())
	{
		std::lock_guard<std::mutex> lock(s_serial_mutex);
		s_serials[key] = serial;
		if (!s_serial_file.empty())
		{
			const size_t slash = s_serial_file.rfind('/');
			if (slash != std::string::npos)
				mkdir(s_serial_file.substr(0, slash).c_str(), 0777);
			if (FILE* f = std::fopen(s_serial_file.c_str(), "ab"))
			{
				const std::string line = key + "\t" + serial + "\n";
				std::fwrite(line.data(), 1, line.size(), f);
				std::fclose(f);
			}
		}
	}
	return serial;
}
} // namespace

std::string DescribeImage(const std::string& image_path)
{
	if (!HasExtension(image_path.c_str(), ".chd"))
		return {};
	ChdSectors disc;
	disc.Open(image_path);
	return disc.What();
}

bool IsDiscImageName(const char* name)
{
	return HasExtension(name, ".iso") || HasExtension(name, ".chd");
}

void SetSerialCacheFile(const std::string& path)
{
	std::lock_guard<std::mutex> lock(s_serial_mutex);
	if (path == s_serial_file)
		return;
	s_serial_file = path;
	s_serial_loaded = false;
	s_serials.clear();
}

std::string ReadSerial(const std::string& image_path)
{
	if (HasExtension(image_path.c_str(), ".chd"))
		return ChdSerial(image_path);
	const int fd = open(image_path.c_str(), O_RDONLY);
	if (fd < 0)
		return {};
	IsoSectors disc(fd);
	const std::string serial = SerialFromDisc(disc);
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
			if (!IsDiscImageName(e->d_name))
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
			g.stem = file.substr(0, file.size() - 4); // ".iso" and ".chd"
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
