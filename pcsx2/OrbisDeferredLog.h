// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// PS5 port (vk-285-104): the GS thread's log lines, kept in memory and written out by the ticker thread
// (main-boot.cpp) once a second, so the GS thread never writes to /data or waits for stdout's lock. A write
// there costs ~0.7 ms and sometimes 30+ ms: vk-285-101/102's GS thread stalled ~34 ms every few seconds in a
// merge or present printf/fflush (the 85-88% seconds in Shadow of the Colossus's heavy view). vk-285-102/103
// tried to move the file writes to a logger thread behind a pipe (apps may not call pipe()) and then a loopback
// connection (no line arrived); this leaves stdout as it is and only defers what the GS-side files print.
//
// A file opts in after its includes:
//   #include "OrbisDeferredLog.h"
//   #define printf OrbisDeferredPrintf
//   #define fflush OrbisDeferredFlush
// OrbisDeferredFlush(stdout) does nothing (the ticker flushes); other streams are flushed as before. The crash
// printer writes the buffer out first (orbis_log_drain). At most 1 MB waits; beyond that lines are dropped and
// counted. Needs proper testing.

#include <cstdio>

int OrbisDeferredPrintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
int OrbisDeferredFlush(FILE* stream);
extern "C" void orbis_log_drain(); // the buffer to stdout, and stdout flushed (the ticker; the crash printer)
