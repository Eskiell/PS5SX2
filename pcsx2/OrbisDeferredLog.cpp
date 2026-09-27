// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// PS5 port (vk-285-104): see OrbisDeferredLog.h. Needs proper testing.

#include "OrbisDeferredLog.h"

#include <cstdarg>
#include <mutex>
#include <string>

namespace
{
	std::mutex s_mutex; // held only to append or to swap the buffer out, never during a write
	std::string s_buffer;
	unsigned long long s_dropped = 0; // lines dropped with the buffer full
	constexpr size_t kMaxBytes = 1u << 20;
} // namespace

int OrbisDeferredPrintf(const char* fmt, ...)
{
	char line[2048];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (n <= 0)
		return n;
	const size_t len = static_cast<size_t>(n) < sizeof(line) ? static_cast<size_t>(n) : sizeof(line) - 1;
	std::lock_guard<std::mutex> lock(s_mutex);
	if (s_buffer.size() + len > kMaxBytes)
	{
		s_dropped++;
		return n;
	}
	if (s_buffer.capacity() == 0)
		s_buffer.reserve(64 * 1024);
	s_buffer.append(line, len);
	return n;
}

int OrbisDeferredFlush(FILE* stream)
{
	if (stream == stdout)
		return 0; // the ticker thread writes the buffer and flushes stdout
	return std::fflush(stream);
}

extern "C" void orbis_log_drain()
{
	std::string out;
	unsigned long long dropped = 0;
	{
		// try_lock: the crash printer may run on a thread that faulted while appending.
		std::unique_lock<std::mutex> lock(s_mutex, std::try_to_lock);
		if (!lock.owns_lock())
			return;
		out.swap(s_buffer);
		dropped = s_dropped;
		s_dropped = 0;
	}
	if (!out.empty())
		std::fwrite(out.data(), 1, out.size(), stdout);
	if (dropped)
		std::fprintf(stdout, "[log] %llu GS-thread lines dropped (the deferred buffer was full)\n", dropped);
	std::fflush(stdout);
}
