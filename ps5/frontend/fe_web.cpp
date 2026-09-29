// PS5 port frontend: the settings page's web server (see fe_web.h).
//
// Copyright (C) 2026 Spyros
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_web.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

namespace fe
{
namespace
{
double Now()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string Lower(std::string s)
{
	for (char& c : s)
		c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
	return s;
}

std::string Trim(const std::string& s)
{
	size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
		b++;
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
		e--;
	return s.substr(b, e - b);
}

int HexValue(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

std::string UrlDecode(const std::string& s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); i++)
	{
		if (s[i] == '+')
			out += ' ';
		else if (s[i] == '%' && i + 2 < s.size() && HexValue(s[i + 1]) >= 0 && HexValue(s[i + 2]) >= 0)
		{
			out += static_cast<char>(HexValue(s[i + 1]) * 16 + HexValue(s[i + 2]));
			i += 2;
		}
		else
			out += s[i];
	}
	return out;
}

std::string QueryValue(const std::string& query, const char* name)
{
	const std::string want = name;
	size_t pos = 0;
	while (pos <= query.size())
	{
		size_t amp = query.find('&', pos);
		if (amp == std::string::npos)
			amp = query.size();
		const std::string part = query.substr(pos, amp - pos);
		const size_t eq = part.find('=');
		if (UrlDecode(part.substr(0, eq)) == want)
			return eq == std::string::npos ? std::string() : UrlDecode(part.substr(eq + 1));
		pos = amp + 1;
	}
	return {};
}

std::string Json(const std::string& s)
{
	std::string out = "\"";
	for (unsigned char c : s)
	{
		switch (c)
		{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (c < 0x20)
				{
					char buf[8];
					std::snprintf(buf, sizeof(buf), "\\u%04x", c);
					out += buf;
				}
				else
					out += static_cast<char>(c);
		}
	}
	return out + "\"";
}

bool ReadFile(const std::string& path, std::string& out)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	out.clear();
	char buf[16384];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
		out.append(buf, n);
	std::fclose(f);
	return true;
}

// Written beside, then renamed over: the GS thread polls these files and must never read half of one.
bool WriteFileAtomic(const std::string& path, const std::string& data)
{
	const std::string tmp = path + ".tmp";
	FILE* f = std::fopen(tmp.c_str(), "wb");
	if (!f)
		return false;
	const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
	if (std::fclose(f) != 0 || !ok)
	{
		unlink(tmp.c_str());
		return false;
	}
	if (std::rename(tmp.c_str(), path.c_str()) != 0)
	{
		unlink(tmp.c_str());
		return false;
	}
	return true;
}

bool Exists(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0;
}

// ---- settings files --------------------------------------------------------------------------
// The port's ini lines are "key=value": a bare key is in EmuCore/GS, others carry their section
// ("EmuCore/Speedhacks/EECycleRate"). Patches/Enable lines repeat, one group name each
// (main-boot.cpp orbis_apply_ini_file).

struct IniLine
{
	std::string raw;
	std::string key; // canonical: EmuCore/GS/ dropped
	std::string value;
	bool kv = false;
};

std::string CanonKey(std::string key)
{
	key = Trim(key);
	if (key.compare(0, 11, "EmuCore/GS/") == 0)
		key.erase(0, 11);
	return key;
}

std::vector<IniLine> ParseIni(const std::string& text)
{
	std::vector<IniLine> lines;
	size_t pos = 0;
	while (pos < text.size())
	{
		size_t nl = text.find('\n', pos);
		if (nl == std::string::npos)
			nl = text.size();
		IniLine l;
		l.raw = text.substr(pos, nl - pos);
		if (!l.raw.empty() && l.raw.back() == '\r')
			l.raw.pop_back();
		const std::string t = Trim(l.raw);
		const size_t eq = t.find('=');
		if (!t.empty() && t[0] != '#' && t[0] != ';' && eq != std::string::npos)
		{
			l.kv = true;
			l.key = CanonKey(t.substr(0, eq));
			l.value = Trim(t.substr(eq + 1));
		}
		lines.push_back(std::move(l));
		pos = nl + 1;
	}
	return lines;
}

bool IsListKey(const std::string& key)
{
	return key == "Patches/Enable" || key == "Patches/Disable" || key == "Cheats/Enable" || key == "Cheats/Disable";
}

bool SafeKey(const std::string& key)
{
	if (key.empty() || key.size() > 80 || IsListKey(key))
		return false;
	for (char c : key)
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '/'))
			return false;
	return true;
}

bool SafeValue(const std::string& v)
{
	if (v.size() > 200)
		return false;
	for (char c : v)
		if (c == '\n' || c == '\r' || c == '#')
			return false;
	return true;
}

// The groups ("[60 FPS]") of the game's patch files, <serial>_<crc>.pnach.
struct PatchGroup
{
	std::string name, file, description;
};

std::vector<PatchGroup> PatchGroups(const std::string& dir, const std::string& serial)
{
	std::vector<PatchGroup> out;
	if (serial.empty())
		return out;
	DIR* d = opendir(dir.c_str());
	if (!d)
		return out;
	const std::string prefix = Lower(serial) + "_";
	std::vector<std::string> files;
	while (dirent* e = readdir(d))
	{
		const std::string name = e->d_name, low = Lower(name);
		if (low.compare(0, prefix.size(), prefix) == 0 && low.size() > 6 && low.compare(low.size() - 6, 6, ".pnach") == 0)
			files.push_back(name);
	}
	closedir(d);
	std::sort(files.begin(), files.end());
	for (const std::string& file : files)
	{
		std::string text;
		if (!ReadFile(dir + "/" + file, text))
			continue;
		PatchGroup* current = nullptr;
		for (const IniLine& l : ParseIni(text))
		{
			const std::string t = Trim(l.raw);
			if (t.size() > 2 && t.front() == '[' && t.back() == ']')
			{
				const std::string name = Trim(t.substr(1, t.size() - 2));
				current = nullptr;
				if (name.empty())
					continue;
				auto it = std::find_if(out.begin(), out.end(), [&](const PatchGroup& g) { return g.name == name; });
				if (it == out.end())
				{
					out.push_back({name, file, {}});
					current = &out.back();
				}
			}
			else if (current && l.kv && Lower(l.key) == "description" && current->description.empty())
				current->description = l.value;
		}
	}
	return out;
}

// The cover the shelf shows first (vk-285-110: CoverFinder, which also looks on the game's USB drive).
std::string CoverPath(CoverFinder& finder, const GameInfo& g)
{
	return finder.Best(g).path;
}

// ---- recommended settings (vk-285-51) -------------------------------------------------------

// What a settings text sets, as the port's reader applies it: a key set twice counts with its last
// line; Patches/Enable lines add up.
struct IniState
{
	std::vector<std::pair<std::string, std::string>> kv;
	std::vector<std::string> enabled;
};

IniState ReadState(const std::string& text)
{
	IniState st;
	for (const IniLine& l : ParseIni(text))
	{
		if (!l.kv)
			continue;
		if (l.key == "Patches/Enable")
		{
			if (std::find(st.enabled.begin(), st.enabled.end(), l.value) == st.enabled.end())
				st.enabled.push_back(l.value);
			continue;
		}
		if (IsListKey(l.key))
			continue;
		auto it = std::find_if(st.kv.begin(), st.kv.end(), [&](const std::pair<std::string, std::string>& p) { return p.first == l.key; });
		if (it != st.kv.end())
			it->second = l.value;
		else
			st.kv.emplace_back(l.key, l.value);
	}
	return st;
}

std::string KvJson(const IniState& st)
{
	std::string out = "{";
	for (const auto& p : st.kv)
		out += (out.size() > 1 ? "," : "") + Json(p.first) + ":" + Json(p.second);
	return out + "}";
}

std::string ListJson(const std::vector<std::string>& v)
{
	std::string out = "[";
	for (const std::string& s : v)
		out += (out.size() > 1 ? "," : "") + Json(s);
	return out + "]";
}

// vk-285-110: PS5SX2/ keys (the game language) are the player's own choices, not tuning: the Recommended
// button keeps them, and they don't stop a file from counting as the recommended one.
bool IsPlayerKey(const std::string& key)
{
	return key.compare(0, 7, "PS5SX2/") == 0;
}

bool SameState(IniState a, IniState b)
{
	for (IniState* st : {&a, &b})
		st->kv.erase(std::remove_if(st->kv.begin(), st->kv.end(),
						 [](const std::pair<std::string, std::string>& p) { return IsPlayerKey(p.first); }),
			st->kv.end());
	std::sort(a.kv.begin(), a.kv.end());
	std::sort(b.kv.begin(), b.kv.end());
	std::sort(a.enabled.begin(), a.enabled.end());
	std::sort(b.enabled.begin(), b.enabled.end());
	return a.kv == b.kv && a.enabled == b.enabled;
}

// "key=value (was old)"-style notes for the settings log: what `after` sets differently from
// `before`, joined with "; ".
std::string DescribeChanges(const IniState& before, const IniState& after)
{
	std::string out;
	auto add = [&](const std::string& s) { out += (out.empty() ? "" : "; ") + s; };
	auto find = [](const IniState& st, const std::string& k) -> const std::string* {
		for (const auto& p : st.kv)
			if (p.first == k)
				return &p.second;
		return nullptr;
	};
	for (const auto& p : after.kv)
	{
		const std::string* was = find(before, p.first);
		if (!was)
			add(p.first + "=" + p.second + " (was unset)");
		else if (*was != p.second)
			add(p.first + "=" + p.second + " (was " + *was + ")");
	}
	for (const auto& p : before.kv)
		if (!find(after, p.first))
			add("unset " + p.first + " (was " + p.second + ")");
	for (const std::string& e : after.enabled)
		if (std::find(before.enabled.begin(), before.enabled.end(), e) == before.enabled.end())
			add("patch on: " + e);
	for (const std::string& e : before.enabled)
		if (std::find(after.enabled.begin(), after.enabled.end(), e) == after.enabled.end())
			add("patch off: " + e);
	return out;
}

// One section of presets.ini ("[@global]", "[SLUS-20733]"): its lines as they are, comments
// included. False when there is none.
bool PresetSection(const std::string& presets, const std::string& id, std::string& out)
{
	out.clear();
	if (id.empty())
		return false;
	bool found = false, in = false;
	size_t pos = 0;
	while (pos < presets.size())
	{
		size_t nl = presets.find('\n', pos);
		if (nl == std::string::npos)
			nl = presets.size();
		std::string line = presets.substr(pos, nl - pos);
		pos = nl + 1;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		const std::string t = Trim(line);
		if (t.size() > 2 && t.front() == '[' && t.back() == ']')
		{
			in = Lower(Trim(t.substr(1, t.size() - 2))) == Lower(id);
			found = found || in;
			continue;
		}
		if (in)
			out += line + "\n";
	}
	// The blank lines between sections are not part of either.
	while (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n')
		out.pop_back();
	if (out == "\n")
		out.clear();
	return found;
}

std::string Stamp()
{
	const long long utc = static_cast<long long>(std::time(nullptr));
	std::tm tm = {};
	if (g_utc_to_local)
	{
		const std::time_t local = static_cast<std::time_t>(g_utc_to_local(utc));
		gmtime_r(&local, &tm);
	}
	else
	{
		const std::time_t t = static_cast<std::time_t>(utc);
		localtime_r(&t, &tm);
	}
	char buf[32];
	std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
	return buf;
}

bool SendAll(int fd, const void* data, size_t size)
{
	const char* p = static_cast<const char*>(data);
#ifdef MSG_NOSIGNAL
	const int flags = MSG_NOSIGNAL;
#else
	const int flags = 0;
#endif
	while (size > 0)
	{
		const ssize_t n = send(fd, p, size, flags);
		if (n <= 0)
		{
			if (n < 0 && errno == EINTR)
				continue;
			return false;
		}
		p += n;
		size -= static_cast<size_t>(n);
	}
	return true;
}

const char* Reason(int status)
{
	switch (status)
	{
		case 200: return "OK";
		case 400: return "Bad Request";
		case 401: return "Unauthorized";
		case 404: return "Not Found";
		case 405: return "Method Not Allowed";
		case 413: return "Payload Too Large";
		default: return "Internal Server Error";
	}
}

std::string Error(const char* message)
{
	return std::string("{\"error\":") + Json(message) + "}";
}

// ---- Test build 1 (vk-285-55): the logs report.

std::string StampOf(long long utc)
{
	std::tm tm = {};
	if (g_utc_to_local)
	{
		const std::time_t local = static_cast<std::time_t>(g_utc_to_local(utc));
		gmtime_r(&local, &tm);
	}
	else
	{
		const std::time_t t = static_cast<std::time_t>(utc);
		localtime_r(&t, &tm);
	}
	char buf[32];
	std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
	return buf;
}

std::string Human(long long bytes)
{
	char buf[32];
	if (bytes >= 1024 * 1024)
		std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
	else if (bytes >= 1024)
		std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
	else
		std::snprintf(buf, sizeof(buf), "%lld B", bytes);
	return buf;
}

// Up to `cap` bytes of a file: all of it when it fits, else its first `head` bytes and its last
// cap - head, with a line saying how much was cut between them. False when it can't be opened.
bool ReadCapped(const std::string& path, size_t cap, size_t head, std::string& out, long long& size, long long& mtime)
{
	const int fd = open(path.c_str(), O_RDONLY);
	if (fd < 0)
		return false;
	struct stat st = {};
	if (fstat(fd, &st) != 0)
	{
		close(fd);
		return false;
	}
	size = static_cast<long long>(st.st_size);
	mtime = static_cast<long long>(st.st_mtime);
	auto read_at = [&](long long off, size_t n) {
		std::string part(n, '\0');
		size_t got = 0;
		while (got < n)
		{
			const ssize_t r = pread(fd, &part[got], n - got, static_cast<off_t>(off + static_cast<long long>(got)));
			if (r <= 0)
				break;
			got += static_cast<size_t>(r);
		}
		part.resize(got);
		return part;
	};
	if (static_cast<unsigned long long>(size) <= cap)
		out = read_at(0, static_cast<size_t>(size));
	else
	{
		head = std::min(head, cap);
		const size_t tail = cap - head;
		out = read_at(0, head);
		out += "\n[... " + Human(size - static_cast<long long>(cap)) + " cut here ...]\n";
		out += read_at(size - static_cast<long long>(tail), tail);
	}
	close(fd);
	return true;
}

// The names in a folder, sorted, without the dot entries.
std::vector<std::string> ListNames(const std::string& dir)
{
	std::vector<std::string> names;
	if (DIR* d = opendir(dir.c_str()))
	{
		while (const dirent* e = readdir(d))
			if (e->d_name[0] != '.')
				names.push_back(e->d_name);
		closedir(d);
	}
	std::sort(names.begin(), names.end());
	return names;
}
} // namespace

long long (*g_utc_to_local)(long long utc) = nullptr;

void AppendSettingsLog(const std::string& path, const std::string& line)
{
	if (path.empty())
		return;
	static std::mutex s_mutex;
	std::lock_guard<std::mutex> lock(s_mutex);
	struct stat st = {};
	if (stat(path.c_str(), &st) == 0 && st.st_size > 512 * 1024)
	{
		const size_t dot = path.rfind('.');
		const std::string old = dot == std::string::npos ? path + ".1" : path.substr(0, dot) + ".1" + path.substr(dot);
		std::rename(path.c_str(), old.c_str());
	}
	FILE* f = std::fopen(path.c_str(), "a");
	if (!f)
		return;
	std::fprintf(f, "%s  %s\n", Stamp().c_str(), line.c_str());
	std::fclose(f);
}

std::string LocalAddress()
{
	// connect() on a UDP socket sends nothing; it only picks the route, and getsockname() then names
	// the interface address that route leaves from.
	const char* targets[] = {"1.1.1.1", "192.168.1.1", "192.168.0.1", "10.0.0.1", "172.16.0.1"};
	for (const char* t : targets)
	{
		const int s = socket(AF_INET, SOCK_DGRAM, 0);
		if (s < 0)
			return {};
		sockaddr_in to = {};
		to.sin_family = AF_INET;
		to.sin_port = htons(53);
		inet_pton(AF_INET, t, &to.sin_addr);
		std::string out;
		if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0)
		{
			sockaddr_in me = {};
			socklen_t len = sizeof(me);
			char buf[INET_ADDRSTRLEN] = {};
			if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0 && inet_ntop(AF_INET, &me.sin_addr, buf, sizeof(buf)))
				out = buf;
		}
		close(s);
		if (!out.empty() && out != "0.0.0.0" && out.compare(0, 4, "127.") != 0)
			return out;
	}
	return {};
}

WebServer::~WebServer()
{
	Stop();
}

bool WebServer::LoadToken()
{
	std::string text;
	if (ReadFile(m_cfg.token_path, text))
	{
		text = Trim(text);
		bool ok = text.size() >= 12 && text.size() <= 64;
		for (char c : text)
			ok = ok && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'));
		if (ok)
		{
			m_token = text;
			return true;
		}
	}
	// A new one: 16 characters from the kernel's random source, or from the clock if it has none.
	unsigned char raw[16] = {};
	bool random = false;
	const int fd = open("/dev/urandom", O_RDONLY);
	if (fd >= 0)
	{
		random = read(fd, raw, sizeof(raw)) == static_cast<ssize_t>(sizeof(raw));
		close(fd);
	}
	if (!random)
	{
		uint64_t x = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
		             static_cast<uint64_t>(std::time(nullptr)) * 0x9E3779B97F4A7C15ull ^ reinterpret_cast<uintptr_t>(&x);
		for (unsigned char& b : raw)
		{
			x ^= x << 13;
			x ^= x >> 7;
			x ^= x << 17;
			b = static_cast<unsigned char>(x >> 24);
		}
	}
	const char* alphabet = "abcdefghijkmnpqrstuvwxyz23456789"; // no 0/o, 1/l
	m_token.clear();
	for (unsigned char b : raw)
		m_token += alphabet[b & 31];
	if (!WriteFileAtomic(m_cfg.token_path, m_token + "\n"))
		std::printf("[web] could not save the access token to %s (errno %d); it lasts this session\n", m_cfg.token_path.c_str(), errno);
	return true;
}

bool WebServer::Start(const WebConfig& cfg)
{
	m_cfg = cfg;
	LoadToken();
	for (int i = 0; i < 8 && m_listen < 0; i++)
	{
		const int s = socket(AF_INET, SOCK_STREAM, 0);
		if (s < 0)
		{
			std::printf("[web] socket: errno %d\n", errno);
			return false;
		}
		const int one = 1;
		setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
		setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
		sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		addr.sin_port = htons(static_cast<uint16_t>(cfg.port + i));
		if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 && listen(s, 16) == 0)
		{
			m_listen = s;
			m_port = static_cast<uint16_t>(cfg.port + i);
		}
		else
		{
			std::printf("[web] port %d: errno %d\n", cfg.port + i, errno);
			close(s);
		}
	}
	if (m_listen < 0)
		return false;
	m_stop = false;
	m_thread = std::thread([this]() { Run(); });
	std::printf("[web] listening on port %u (token %.4s…)\n", static_cast<unsigned>(m_port), m_token.c_str());
	std::fflush(stdout);
	return true;
}

void WebServer::Stop()
{
	if (m_listen < 0)
		return;
	m_stop = true;
	shutdown(m_listen, SHUT_RDWR);
	close(m_listen);
	m_listen = -1;
	if (m_thread.joinable())
		m_thread.join();
}

void WebServer::SetNowPlaying(const std::string& image_path)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_now_playing = image_path.substr(image_path.rfind('/') + 1);
}

bool WebServer::Address(std::string& url, std::string& shown) const
{
	url.clear();
	shown.clear();
	if (m_listen < 0 && m_port == 0)
		return false;
	const std::string ip = LocalAddress();
	if (ip.empty())
		return false;
	shown = ip + ":" + std::to_string(m_port);
	url = "http://" + shown + "/?t=" + m_token;
	return true;
}

void WebServer::Run()
{
	while (!m_stop)
	{
		sockaddr_in peer = {};
		socklen_t len = sizeof(peer);
		const int fd = accept(m_listen, reinterpret_cast<sockaddr*>(&peer), &len);
		if (fd < 0)
		{
			if (m_stop)
				break;
			if (errno != EINTR)
				usleep(100000);
			continue;
		}
		timeval tv = {3, 0};
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
		const int one = 1;
		setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
		char ip[INET_ADDRSTRLEN] = {};
		if (!inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip)))
			ip[0] = 0;
		Serve(fd, ip);
		close(fd);
	}
}

void WebServer::Serve(int fd, const char* peer)
{
	// The request: headers up to 16 KiB, a body up to 64 KiB.
	std::string in;
	size_t header_end = std::string::npos;
	char buf[4096];
	while (header_end == std::string::npos && in.size() < 16384)
	{
		const ssize_t n = recv(fd, buf, sizeof(buf), 0);
		if (n <= 0)
			return;
		in.append(buf, static_cast<size_t>(n));
		header_end = in.find("\r\n\r\n");
	}
	if (header_end == std::string::npos)
		return;
	Request req;
	req.peer = peer ? peer : "";
	size_t content_length = 0;
	{
		const std::string head = in.substr(0, header_end);
		const size_t line_end = head.find("\r\n");
		const std::string first = head.substr(0, line_end);
		const size_t sp1 = first.find(' '), sp2 = first.find(' ', sp1 + 1);
		if (sp1 == std::string::npos || sp2 == std::string::npos)
			return;
		req.method = first.substr(0, sp1);
		const std::string target = first.substr(sp1 + 1, sp2 - sp1 - 1);
		const size_t q = target.find('?');
		req.path = UrlDecode(target.substr(0, q));
		req.query = q == std::string::npos ? std::string() : target.substr(q + 1);
		size_t pos = line_end == std::string::npos ? head.size() : line_end + 2;
		while (pos < head.size())
		{
			size_t e = head.find("\r\n", pos);
			if (e == std::string::npos)
				e = head.size();
			const std::string line = head.substr(pos, e - pos);
			const size_t colon = line.find(':');
			if (colon != std::string::npos)
			{
				const std::string name = Lower(Trim(line.substr(0, colon))), value = Trim(line.substr(colon + 1));
				if (name == "content-length")
					content_length = static_cast<size_t>(std::strtoul(value.c_str(), nullptr, 10));
				else if (name == "x-token")
					req.token = value;
			}
			pos = e + 2;
		}
	}
	Response res;
	if (content_length > 65536)
	{
		res.status = 413;
		res.body = Error("too large");
	}
	else
	{
		req.body = in.substr(header_end + 4);
		while (req.body.size() < content_length)
		{
			const ssize_t n = recv(fd, buf, sizeof(buf), 0);
			if (n <= 0)
				return;
			req.body.append(buf, static_cast<size_t>(n));
		}
		req.body.resize(std::min(req.body.size(), content_length));
		if (req.token.empty())
			req.token = QueryValue(req.query, "t");
		const double t0 = Now();
		Route(req, res);
		if (req.method == "POST" || res.status >= 400)
		{
			std::printf("[web] %s %s -> %d (%.0f ms)\n", req.method.c_str(), req.path.c_str(), res.status, (Now() - t0) * 1000.0);
			std::fflush(stdout);
		}
	}
	const size_t size = res.data ? res.size : res.body.size();
	char head[512];
	const int n = std::snprintf(head, sizeof(head),
		"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: %s\r\n"
		"X-Content-Type-Options: nosniff\r\nConnection: close\r\n",
		res.status, Reason(res.status), res.type.c_str(), size, res.cache.c_str());
	// Test build 1: the logs download is saved as a file, and a few MB over Wi-Fi can take a while.
	std::string header(head, static_cast<size_t>(std::max(n, 0)));
	if (!res.disposition.empty())
		header += "Content-Disposition: " + res.disposition + "\r\n";
	header += "\r\n";
	if (res.send_timeout_s > 0)
	{
		timeval tv = {res.send_timeout_s, 0};
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	}
	if (!SendAll(fd, header.data(), header.size()))
		return;
	if (req.method != "HEAD")
		SendAll(fd, res.data ? static_cast<const void*>(res.data) : static_cast<const void*>(res.body.data()), size);
}

void WebServer::Route(const Request& req, Response& res)
{
	// The page and what it loads need no token: they hold no data.
	if (req.method == "GET" || req.method == "HEAD")
		for (const WebAsset& a : m_cfg.assets)
			if (a.path == req.path)
			{
				res.type = a.type;
				res.data = a.data;
				res.size = a.size;
				res.cache = a.path == "/" ? "no-cache" : "max-age=86400";
				return;
			}
	if (req.path.compare(0, 5, "/api/") != 0)
	{
		res.status = 404;
		res.body = Error("not found");
		return;
	}
	// Compared without an early exit, so the time taken says nothing about the token.
	bool ok = req.token.size() == m_token.size() && !m_token.empty();
	unsigned diff = 0;
	for (size_t i = 0; ok && i < m_token.size(); i++)
		diff |= static_cast<unsigned char>(req.token[i] ^ m_token[i]);
	if (!ok || diff != 0)
	{
		res.status = 401;
		res.body = Error("scan the QR code on the TV");
		return;
	}
	if (req.path == "/api/state" && req.method == "GET")
		ApiState(res);
	else if (req.path == "/api/games" && req.method == "GET")
		ApiGames(res);
	else if (req.path == "/api/cover" && req.method == "GET")
		ApiCover(req, res);
	else if (req.path == "/api/settings" && req.method == "GET")
		ApiSettings(req, res);
	else if (req.path == "/api/settings" && req.method == "POST")
		ApiSave(req, res);
	else if (req.path == "/api/report" && (req.method == "GET" || req.method == "HEAD"))
		ApiReport(req, res);
	else if (req.path == "/api/note" && req.method == "POST")
		ApiNote(req, res);
	else
	{
		res.status = req.method == "GET" || req.method == "POST" ? 404 : 405;
		res.body = Error("not found");
	}
}

std::vector<GameInfo> WebServer::Games()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (Now() - m_games_time > 10.0) // a game copied in shows up within seconds
	{
		m_games = ScanGames(m_cfg.game_dirs);
		for (GameInfo& g : m_games)
		{
			// Test build 1: an image's serial is read once (per size), not at every rescan.
			const auto it = m_serials.find(g.path);
			if (it != m_serials.end() && it->second.first == g.bytes)
				g.serial = it->second.second;
			else
			{
				g.serial = ReadSerial(g.path);
				m_serials[g.path] = {g.bytes, g.serial};
			}
			ReadBadges(g, m_cfg.settings_dir, m_cfg.gs_ini, m_cfg.patches_dir);
		}
		m_games_time = Now();
	}
	return m_games;
}

CoverFinder& WebServer::Covers()
{
	if (!m_covers || Now() - m_covers_time > 10.0) // a picture copied in shows up within seconds
	{
		m_covers = std::make_unique<CoverFinder>(m_cfg.covers_dir, m_cfg.cache_dir);
		m_covers_time = Now();
	}
	return *m_covers;
}

const GameInfo* WebServer::FindGame(const std::string& id, std::vector<GameInfo>& games)
{
	games = Games();
	for (const GameInfo& g : games)
		if (g.file == id)
			return &g;
	return nullptr;
}

void WebServer::ApiState(Response& res)
{
	std::string playing;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		playing = m_now_playing;
	}
	std::string game = "null";
	if (!playing.empty())
	{
		std::vector<GameInfo> games;
		if (const GameInfo* g = FindGame(playing, games))
			game = "{\"id\":" + Json(g->file) + ",\"title\":" + Json(g->title) + ",\"serial\":" + Json(g->serial) + "}";
		else
			game = "{\"id\":" + Json(playing) + ",\"title\":" + Json(playing) + ",\"serial\":\"\"}";
	}
	res.body = "{\"app\":\"PS5SX2\",\"build\":" + Json(m_cfg.build_tag) + ",\"test\":" + std::to_string(m_cfg.test_build) +
	           ",\"mode\":" + Json(playing.empty() ? "menu" : "game") + ",\"playing\":" + game + "}";
}

void WebServer::ApiGames(Response& res)
{
	std::string playing;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		playing = m_now_playing;
	}
	std::string out = "[";
	CoverFinder& finder = Covers();
	for (const GameInfo& g : Games())
	{
		if (out.size() > 1)
			out += ",";
		std::string badges = "[";
		for (const std::string& b : g.badges)
			badges += (badges.size() > 1 ? "," : "") + Json(b);
		badges += "]";
		out += "{\"id\":" + Json(g.file) + ",\"title\":" + Json(g.title) + ",\"region\":" + Json(g.region) +
		       ",\"serial\":" + Json(g.serial) + ",\"size\":" + std::to_string(g.bytes) + ",\"badges\":" + badges +
		       ",\"cover\":" + (CoverPath(finder, g).empty() ? "false" : "true") +
		       ",\"settings\":" + (Exists(m_cfg.settings_dir + "/" + g.stem + ".ini") ? "true" : "false") +
		       ",\"playing\":" + (g.file == playing ? "true" : "false") + "}";
	}
	res.body = out + "]";
}

void WebServer::ApiCover(const Request& req, Response& res)
{
	std::vector<GameInfo> games;
	const GameInfo* g = FindGame(QueryValue(req.query, "id"), games);
	const std::string path = g ? CoverPath(Covers(), *g) : std::string();
	if (path.empty() || !ReadFile(path, res.body))
	{
		res.status = 404;
		res.body = Error("no cover");
		return;
	}
	const std::string low = Lower(path);
	res.type = low.size() > 4 && low.compare(low.size() - 4, 4, ".png") == 0 ? "image/png" : "image/jpeg";
	res.cache = "private, max-age=600";
}

// {"id", "title", "serial", "file", "exists", "values": {key: value}, "global": {key: value},
//  "patches": [{"name", "file", "description", "enabled"}],
//  "recommended": {"found", "values": {key: value}, "patches": [name]}, "is_recommended"}.
// id "@global" is gs.ini itself. "recommended" is the game's presets.ini section (vk-285-51); a game
// without one is recommended to follow gs.ini, so its "values" are empty.
void WebServer::ApiSettings(const Request& req, Response& res)
{
	const std::string id = QueryValue(req.query, "id");
	std::vector<GameInfo> games;
	const GameInfo* g = nullptr;
	std::string path = m_cfg.gs_ini;
	if (id != "@global")
	{
		g = FindGame(id, games);
		if (!g)
		{
			res.status = 404;
			res.body = Error("no such game");
			return;
		}
		path = m_cfg.settings_dir + "/" + g->stem + ".ini";
	}
	std::string text, global_text, preset;
	ReadFile(path, text);
	const IniState own = ReadState(text);
	if (g)
		ReadFile(m_cfg.gs_ini, global_text);
	const bool has_preset = PresetSection(m_cfg.presets, g ? g->serial : std::string("@global"), preset);
	const IniState rec = ReadState(preset);
	const std::vector<std::string>& enabled = own.enabled;
	std::string out = "{\"id\":" + Json(id) + ",\"title\":" + Json(g ? g->title : "All games") + ",\"serial\":" +
	                  Json(g ? g->serial : "") + ",\"file\":" + Json(g ? "settings/" + g->stem + ".ini" : "gs.ini") +
	                  ",\"exists\":" + (Exists(path) ? "true" : "false") + ",\"values\":" + KvJson(own);
	out += ",\"global\":" + (g ? KvJson(ReadState(global_text)) : std::string("{}"));
	out += std::string(",\"recommended\":{\"found\":") + (has_preset ? "true" : "false") + ",\"values\":" + KvJson(rec) +
	       ",\"patches\":" + ListJson(rec.enabled) + "}";
	out += std::string(",\"is_recommended\":") + ((has_preset || g) && SameState(own, rec) ? "true" : "false");
	out += ",\"patches\":[";
	if (g)
	{
		bool first = true;
		const std::vector<PatchGroup> groups = PatchGroups(m_cfg.patches_dir, g->serial);
		for (const PatchGroup& p : groups)
		{
			const bool on = std::find(enabled.begin(), enabled.end(), p.name) != enabled.end();
			out += std::string(first ? "" : ",") + "{\"name\":" + Json(p.name) + ",\"file\":" + Json(p.file) +
			       ",\"description\":" + Json(p.description) + ",\"enabled\":" + (on ? "true" : "false") + "}";
			first = false;
		}
		// An enabled group whose patch file is gone still shows, so it can be turned off.
		for (const std::string& e : enabled)
		{
			bool listed = false;
			for (const PatchGroup& p : groups)
				listed = listed || p.name == e;
			if (!listed)
			{
				out += std::string(first ? "" : ",") + "{\"name\":" + Json(e) + ",\"file\":\"\",\"description\":" +
				       Json("no patch file has this group") + ",\"enabled\":true}";
				first = false;
			}
		}
	}
	res.body = out + "]}";
}

// The body is one change a line: "set <key>=<value>", "unset <key>", "patch+ <group>" or
// "patch- <group>". Comments and other lines in the file stay as they are.
void WebServer::ApiSave(const Request& req, Response& res)
{
	const std::string id = QueryValue(req.query, "id");
	std::vector<GameInfo> games;
	const GameInfo* g = nullptr;
	std::string path = m_cfg.gs_ini;
	if (id != "@global")
	{
		g = FindGame(id, games);
		if (!g)
		{
			res.status = 404;
			res.body = Error("no such game");
			return;
		}
		path = m_cfg.settings_dir + "/" + g->stem + ".ini";
	}
	if (Trim(req.body) == "recommended")
	{
		ApiRecommended(req, g, path, res);
		return;
	}
	std::string text;
	const bool existed = ReadFile(path, text);
	const IniState before = ReadState(text);
	std::vector<IniLine> lines = ParseIni(text);
	if (!existed && g)
	{
		IniLine header;
		header.raw = "# " + g->title + (g->serial.empty() ? std::string() : " (" + g->serial + ")") + ": written by the PS5SX2 settings page";
		lines.push_back(header);
	}
	int changes = 0;
	size_t pos = 0;
	while (pos < req.body.size())
	{
		size_t nl = req.body.find('\n', pos);
		if (nl == std::string::npos)
			nl = req.body.size();
		const std::string cmd = Trim(req.body.substr(pos, nl - pos));
		pos = nl + 1;
		if (cmd.empty())
			continue;
		const size_t sp = cmd.find(' ');
		const std::string verb = cmd.substr(0, sp), arg = sp == std::string::npos ? std::string() : Trim(cmd.substr(sp + 1));
		if (verb == "set")
		{
			const size_t eq = arg.find('=');
			const std::string key = CanonKey(arg.substr(0, eq)), value = eq == std::string::npos ? "" : Trim(arg.substr(eq + 1));
			if (eq == std::string::npos || !SafeKey(key) || !SafeValue(value))
			{
				res.status = 400;
				res.body = Error(("bad setting: " + arg).c_str());
				return;
			}
			bool placed = false;
			for (size_t i = 0; i < lines.size();)
			{
				if (lines[i].kv && lines[i].key == key)
				{
					if (!placed)
					{
						lines[i].raw = key + "=" + value;
						lines[i].value = value;
						placed = true;
						i++;
					}
					else
						lines.erase(lines.begin() + static_cast<long>(i)); // one line a key
				}
				else
					i++;
			}
			if (!placed)
			{
				IniLine l;
				l.raw = key + "=" + value;
				l.key = key;
				l.value = value;
				l.kv = true;
				lines.push_back(l);
			}
			changes++;
		}
		else if (verb == "unset")
		{
			const std::string key = CanonKey(arg);
			if (!SafeKey(key))
			{
				res.status = 400;
				res.body = Error(("bad setting: " + arg).c_str());
				return;
			}
			lines.erase(std::remove_if(lines.begin(), lines.end(), [&](const IniLine& l) { return l.kv && l.key == key; }), lines.end());
			changes++;
		}
		else if ((verb == "patch+" || verb == "patch-") && g)
		{
			if (arg.empty() || arg.size() > 100 || !SafeValue(arg))
			{
				res.status = 400;
				res.body = Error("bad patch name");
				return;
			}
			const bool on = verb == "patch+";
			bool present = false;
			for (const IniLine& l : lines)
				present = present || (l.kv && l.key == "Patches/Enable" && l.value == arg);
			if (on && !present)
			{
				IniLine l;
				l.raw = "Patches/Enable=" + arg;
				l.key = "Patches/Enable";
				l.value = arg;
				l.kv = true;
				lines.push_back(l);
			}
			else if (!on)
				lines.erase(std::remove_if(lines.begin(), lines.end(),
								[&](const IniLine& l) { return l.kv && l.key == "Patches/Enable" && l.value == arg; }),
					lines.end());
			changes++;
		}
		else
		{
			res.status = 400;
			res.body = Error(("unknown change: " + cmd).c_str());
			return;
		}
	}
	std::string out;
	for (const IniLine& l : lines)
		out += l.raw + "\n";
	if (!WriteFileAtomic(path, out))
	{
		res.status = 500;
		res.body = Error("could not write the settings file");
		std::printf("[web] writing %s failed (errno %d)\n", path.c_str(), errno);
		return;
	}
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_games_time = -1e9; // the badges may have changed
	}
	std::printf("[web] %s: %d change(s)\n", path.c_str(), changes);
	std::fflush(stdout);
	const std::string what = DescribeChanges(before, ReadState(out));
	Log(req, (g ? "settings/" + g->stem + ".ini" : std::string("gs.ini")) + ": " + (what.empty() ? std::string("no change") : what));
	Request again = req;
	ApiSettings(again, res);
}

// vk-285-51: the Recommended button. The file becomes the game's presets.ini section (gs.ini gets
// [@global]); a game without a section gets a file with no values, so it follows gs.ini. What was
// there before is kept in <file>.before-recommended.
void WebServer::ApiRecommended(const Request& req, const GameInfo* g, const std::string& path, Response& res)
{
	std::string preset;
	const bool has = PresetSection(m_cfg.presets, g ? g->serial : std::string("@global"), preset);
	if (!g && !has)
	{
		res.status = 404;
		res.body = Error("this build has no recommended settings for all games");
		return;
	}
	std::string header = g ? "# " + g->title + (g->serial.empty() ? std::string() : " (" + g->serial + ")") : std::string("# All games");
	header += has ? ": PS5SX2's recommended settings, restored by the settings page" :
	                ": no settings of its own, so it follows the settings for all games (restored by the settings page)";
	std::string old;
	const bool existed = ReadFile(path, old);
	if (existed && !WriteFileAtomic(path + ".before-recommended", old))
		std::printf("[web] could not keep a copy of %s (errno %d)\n", path.c_str(), errno);
	std::string text = header + "\n" + preset;
	// vk-285-110: the player's own choices (PS5SX2/GameLanguage) stay.
	if (existed)
	{
		std::string keep;
		for (const auto& kv : ReadState(old).kv)
			if (IsPlayerKey(kv.first))
				keep += kv.first + "=" + kv.second + "\n";
		if (!keep.empty())
			text += (text.empty() || text.back() == '\n' ? "" : "\n") + keep;
	}
	if (!WriteFileAtomic(path, text))
	{
		res.status = 500;
		res.body = Error("could not write the settings file");
		std::printf("[web] writing %s failed (errno %d)\n", path.c_str(), errno);
		return;
	}
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_games_time = -1e9;
	}
	std::printf("[web] %s: recommended settings\n", path.c_str());
	std::fflush(stdout);
	const std::string what = DescribeChanges(ReadState(old), ReadState(text));
	const std::string name = g ? "settings/" + g->stem + ".ini" : std::string("gs.ini");
	Log(req, name + ": recommended settings" + (has ? "" : " (follows all games)") + (what.empty() ? std::string(", no change") : ": " + what) +
	             (existed ? "; the old file is " + name + ".before-recommended" : std::string()));
	Request again = req;
	ApiSettings(again, res);
}

// Test build 1 (vk-285-55): everything a tester's report needs, as one text file: the settings log
// (the timeline), this session's logs and the two sessions' before it (after a crash the app is
// started again, so the crashed session is the one before), the GPU hang dump, the settings files,
// the switches and the game list. Long logs keep their start and their end; the whole stays under
// about 8 MB, which a Discord upload takes.
void WebServer::ApiReport(const Request& req, Response& res)
{
	const double t0 = Now();
	std::string playing;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		playing = m_now_playing;
	}
	std::string out;
	out.reserve(1 << 20);
	out += "PS5SX2 logs report\n";
	out += m_cfg.report_header;
	out += "Made: " + Stamp() + " (the console's clock)\n";
	out += "Now: " + (playing.empty() ? std::string("on the shelf") : "playing " + playing) + "\n";
	out += "Sections: settings.log (the timeline), then boot.log, emulog.txt and stderr.log for this session and the\n"
	       "two before it, the GPU hang dump, the settings files, the switches and the game list. Long logs keep\n"
	       "their start and their end.\n";

	auto section = [&](const std::string& path, const std::string& what, size_t cap, size_t head) {
		std::string text;
		long long size = 0, mtime = 0;
		std::string name = path;
		if (!m_cfg.top_dir.empty() && name.compare(0, m_cfg.top_dir.size() + 1, m_cfg.top_dir + "/") == 0)
			name.erase(0, m_cfg.top_dir.size() + 1);
		if (!ReadCapped(path, cap, head, text, size, mtime))
		{
			out += "\n===== " + name + " (" + what + "): not there =====\n";
			return;
		}
		out += "\n===== " + name + " (" + what + "): " + Human(size) + ", last written " + StampOf(mtime) + " =====\n";
		out += text;
		if (!text.empty() && text.back() != '\n')
			out += "\n";
		out += "===== end of " + name + " =====\n";
	};

	const std::string& logs = m_cfg.logs_dir.empty() ? m_cfg.top_dir : m_cfg.logs_dir;
	section(m_cfg.change_log, "the timeline: starts, games, settings, perf each minute, crashes, notes", 384 * 1024, 0);
	struct Session
	{
		const char* suffix;
		const char* what;
		size_t boot, emu, err;
	};
	const Session sessions[] = {
		{"", "this session", 512 * 1024, 2048 * 1024, 512 * 1024},
		{".1", "the session before; after a crash, the crashed one", 512 * 1024, 2048 * 1024, 512 * 1024},
		{".2", "two sessions back", 256 * 1024, 1024 * 1024, 256 * 1024},
	};
	for (const Session& ss : sessions)
	{
		section(logs + "/boot" + ss.suffix + ".log", ss.what, ss.boot, 64 * 1024);
		section(logs + "/emulog" + ss.suffix + ".txt", ss.what, ss.emu, 160 * 1024);
		section(logs + "/stderr" + ss.suffix + ".log", ss.what, ss.err, 32 * 1024);
	}
	section(logs + "/ps5vk-hang.txt", "the last GPU hang the driver caught", 64 * 1024, 32 * 1024);
	// vk-285-109: PCSX2's own dump of a GPU hang (GSDeviceVK.cpp), which settings.log points to; the reports
	// of Driv3r's hangs on vk-285-94 came without it.
	section(logs + "/vkhang.txt", "the last GPU hang PCSX2 caught", 64 * 1024, 32 * 1024);
	// vk-285-113: the hang before that one (PCSX2 renames its dump at each hang; the driver's note has a .1 too since the driver patch).
	// vk-285-112's Hitman hung five times in a row and only the last dump reached the report.
	section(logs + "/vkhang.1.txt", "the GPU hang before that, PCSX2's dump", 64 * 1024, 16 * 1024);
	section(logs + "/ps5vk-hang.1.txt", "the GPU hang before that, the driver's note", 64 * 1024, 16 * 1024);
	// vk-285-113: the hung session's own stderr.log and emulog.txt (GSDeviceVK.cpp OrbisKeepLogTail), which the three-session window above loses
	// when a tester plays on: vk-285-112's Zatch Bell hangs came in a report where two later sessions had pushed them out.
	section(logs + "/hang-stderr.txt", "stderr.log's end when the last GPU hang closed the app", 128 * 1024, 32 * 1024);
	section(logs + "/hang-emulog.txt", "emulog.txt's end when the last GPU hang closed the app", 128 * 1024, 32 * 1024);
	section(logs + "/hang-stderr.1.txt", "the same for the GPU hang before that", 64 * 1024, 16 * 1024);
	section(logs + "/hang-emulog.1.txt", "the same for the GPU hang before that", 64 * 1024, 16 * 1024);
	section(logs + "/pf.log", "page faults this session", 64 * 1024, 16 * 1024);

	// The settings, the switches and what is on the console.
	if (!m_cfg.top_dir.empty())
	{
		section(m_cfg.top_dir + "/gs.ini", "the settings every game starts from", 64 * 1024, 32 * 1024);
		section(m_cfg.top_dir + "/live.ini", "display and overlay switches", 16 * 1024, 8 * 1024);
		size_t total = 0;
		for (const std::string& n : ListNames(m_cfg.settings_dir))
		{
			if (total > 256 * 1024)
				break;
			if (n.size() < 5 || n.compare(n.size() - 4, 4, ".ini") != 0)
				continue; // not the .before-recommended copies
			const size_t before = out.size();
			section(m_cfg.settings_dir + "/" + n, "a game's own settings", 32 * 1024, 16 * 1024);
			total += out.size() - before;
		}
		auto names = [&](const std::string& dir, const char* what) {
			const std::vector<std::string> list = ListNames(dir);
			out += "\n===== " + std::string(what) + ": " + std::to_string(list.size()) + " =====\n";
			for (const std::string& n : list)
				out += n + "\n";
		};
		names(m_cfg.top_dir + "/flags", "switch files in flags/");
		names(m_cfg.patches_dir, "patch files");
		names(m_cfg.top_dir + "/cheats", "cheat files");
	}
	{
		const std::vector<GameInfo> games = Games();
		out += "\n===== games: " + std::to_string(games.size()) + " (file | serial | size | folder) =====\n";
		for (const GameInfo& g : games)
			out += g.file + " | " + (g.serial.empty() ? std::string("no serial") : g.serial) + " | " +
			       std::to_string(static_cast<unsigned long long>(g.bytes)) + " | " + g.path.substr(0, g.path.rfind('/')) + "\n";
	}
	out += "\n===== end of the report =====\n";

	// The file's name: the build and the console's time, e.g. PS5SX2-test1-2026-09-27_1403.txt.
	std::string when = Stamp(); // "2026-09-27 14:03:22"
	when = when.substr(0, 10) + "_" + when.substr(11, 2) + when.substr(14, 2);
	char name[96];
	if (m_cfg.test_build > 0)
		std::snprintf(name, sizeof(name), "PS5SX2-test%d-%s.txt", m_cfg.test_build, when.c_str());
	else
		std::snprintf(name, sizeof(name), "PS5SX2-logs-%s.txt", when.c_str());
	res.type = "text/plain; charset=utf-8";
	res.disposition = std::string("attachment; filename=\"") + name + "\"";
	res.send_timeout_s = 60;
	std::printf("[web] logs report: %zu bytes in %.0f ms\n", out.size(), (Now() - t0) * 1000.0);
	std::fflush(stdout);
	if (req.method == "GET")
		Log(req, "logs downloaded (" + Human(static_cast<long long>(out.size())) + ")");
	res.body = std::move(out);
}

// Test build 1 (vk-285-55): a tester's own words about what happened, on one line of the settings
// log, beside the app's own lines from the same moment.
void WebServer::ApiNote(const Request& req, Response& res)
{
	std::string text;
	for (const char c : req.body)
	{
		if (c == '\r')
			continue;
		if (c == '\n' || c == '\t')
			text += (c == '\n' && !text.empty() && text.back() != ' ') ? " / " : " ";
		else if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f)
			text += c;
	}
	text = Trim(text);
	if (text.size() > 1500)
	{
		size_t cut = 1500;
		while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) // not inside a UTF-8 character
			cut--;
		text.resize(cut);
	}
	if (text.empty())
	{
		res.status = 400;
		res.body = Error("empty note");
		return;
	}
	Log(req, "tester note: " + text);
	std::printf("[note] %s\n", text.c_str());
	std::fflush(stdout);
	res.body = "{\"ok\":true}";
}

void WebServer::Log(const Request& req, const std::string& what)
{
	std::string playing;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		playing = m_now_playing;
	}
	AppendSettingsLog(m_cfg.change_log, "web " + (req.peer.empty() ? std::string("?") : req.peer) + ": " + what +
	                                        (playing.empty() ? std::string(" [on the shelf]") : " [playing " + playing + "]"));
}
} // namespace fe
