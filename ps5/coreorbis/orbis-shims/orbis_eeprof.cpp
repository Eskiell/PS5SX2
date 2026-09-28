// PS5 port, Vulkan build (vk-285-8): a sampling profiler for PCSX2's EE thread.
//
// At a fight's peak the EE thread (PCSX2's CPU thread: the EE, the IOP, VU0 and the event
// loop) is busy about 990 ms of every second, and that is what holds the frame rate at
// 45-49. This profiler finds out where the time goes.
//  - A sampler thread interrupts the EE thread with SIGPROF about every millisecond. It
//    skips the moments when the thread is in one of its accounted waits (OrbisEEWaitScope,
//    pcsx2/OrbisEEProf.h), so the samples are busy time. The handler stores the
//    interrupted RIP.
//  - PCSX2 registers every piece of JIT code it emits (common/Perf.cpp): EE and IOP blocks
//    with their PS2 PC, VU0/VU1 micro programs, VIF unpackers, dispatchers and the SW
//    renderer's code. Those registrations are logged with their host ranges, so a sample in
//    recompiled code maps back to a PS2 block.
//  - A writer thread appends samples and registrations to /data/PCSX2/eeprof.bin every few
//    seconds, so no file I/O runs on the EE thread.
//  - Each [load] line is followed by "[eeprof] n=<samples so far>", which ties each second's
//    samples to its [perf]/[load] figures.
// The analysis is offline (tools/eeprof.py in the port). vk-285-12: opt-in, a file
// /data/PCSX2/eeprof turns it on (main-boot's ITIMER profiler flag prof keeps it off).
//
// The PS5's signal context is FreeBSD's shifted by 0x30 (ProsperoCrash.cpp): RIP at +0xe0,
// RSP at +0xf8. That is checked at start with one signal to this thread.
//
// vk-285-24: the same profiler can sample the GS thread instead, which on the HW renderer
// records every draw (PCSX2's GSRendererHW and the Vulkan driver, both in the eboot) and holds
// the heavy scenes' frame rate. A file /data/PCSX2/gsprof starts it, also while the game runs:
// the GS thread checks for the file once a second (OrbisGSProfStart, from its [load] line), and
// the samples go to /data/PCSX2/gsprof.bin with "[gsprof] n=" marks. Only one thread is
// profiled per run; the EE profiler wins when both files are there at start.
//
// vk-285-29: and the MTVU thread (VU1's micro programs, VIF unpacks and XGKICKs when MTVU is on),
// which is busy the whole second in the crowd fights' dips. A file /data/PCSX2/vuprof, there at
// start, starts it on that thread's first wake (OrbisVUProfStart, MTVU.cpp); its ring waits are
// skipped, the samples go to /data/PCSX2/vuprof.bin with "[vuprof] n=" marks.
//
// Copyright (C) 2026 Spyros
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Memory.h"
#include "OrbisPaths.h" // vk-285-33
#include "OrbisDeferredLog.h" // vk-285-107: OrbisEEProfMark's line

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sched.h>
#include <semaphore.h>
#include <mutex>
#include <new>
#include <pthread.h>
#include <pthread_np.h> // vk-285-36: pthread_attr_get_np, for the profiled thread's stack bounds
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <vector>

extern std::atomic<int> g_orbis_ee_waiting; // pcsx2/MTGS.cpp
extern std::atomic<int> g_orbis_vu_waiting; // pcsx2/MTVU.cpp (vk-285-29)
extern "C" int sceKernelGetCurrentCpu(void);

void OrbisEEProfStart();
void OrbisGSProfStart();
void OrbisVUProfStart();
static void PrintLibraryAddresses();

namespace
{
// vk-285-33: in logs/ (OrbisPaths.h), decided when the profiler starts.
const char* const kPath = "eeprof.bin";
const char* const kGSPath = "gsprof.bin";
const char* const kVUPath = "vuprof.bin"; // vk-285-29: the MTVU (VU1) thread
constexpr uint32_t kMaxSamples = 2u << 20; // 16 MiB: about 35 minutes at 1 kHz
constexpr long kPeriodUs = 1000;
constexpr unsigned kFlushSeconds = 3;

struct FileHeader
{
	char magic[8]; // "EEPROF1"
	uint32_t version; // 1
	uint32_t rip_offset; // bytes into the signal context
	uint64_t code_base; // SysMemory::GetCodePtr(0): the recompilers' code area
	uint64_t code_size; // HostMemoryMap::CodeSize
	uint64_t self_fn; // run-time address of OrbisEEProfStart: the eboot's load base is this minus its ELF value
	uint64_t ee_stack; // an address on the profiled thread's stack
	uint64_t period_us;
	uint64_t thread; // vk-285-24: 0 the EE thread, 1 the GS thread (was reserved, 0); vk-285-29: 2 the VU1 thread
	uint64_t reserved[3];
};
static_assert(sizeof(FileHeader) == 88);

struct RecordHeader
{
	uint32_t type; // 1 samples (u64 RIPs), 2 JIT registrations (JitEntry), 3 library samples' stacks (LibSample)
	uint32_t count;
	uint64_t first; // type 1: the index of the first sample in this record
};
static_assert(sizeof(RecordHeader) == 16);

struct JitEntry
{
	uint64_t ptr;
	uint64_t key; // the PS2 PC for EE/IOP/VU blocks, a key for keyed code, 0 for named code
	uint32_t size;
	uint32_t at; // the sample count when it was registered: it covers samples from here on
	char group[8]; // Perf group: "EE", "IOP", "VU0", "VU1", "VIF", "" (any)
	char name[32]; // the symbol or key prefix, or empty
};
static_assert(sizeof(JitEntry) == 64);

uint64_t* s_samples;
std::atomic<uint32_t> s_count{0};

// vk-285-11: a sample in the system libraries (memcpy was 3.9% in vk-285-10) also keeps the top six
// words of the stack, so the offline tool can find the eboot or JIT return address that called it.
constexpr uint64_t kLibLo = 0x800000000ull, kLibHi = 0x900000000ull; // libkernel and the other modules
constexpr uint32_t kStackWords = 6, kMaxLibSamples = 1u << 16;
struct LibSample
{
	uint64_t index;
	uint64_t words[kStackWords];
};
static_assert(sizeof(LibSample) == 56);
LibSample* s_lib_samples;
std::atomic<uint32_t> s_lib_count{0};
std::atomic<uint32_t> s_dropped{0};
std::atomic<uint32_t> s_sent{0};
std::atomic<uint32_t> s_skipped{0};
std::atomic<int> s_kill_error{0};
std::atomic<bool> s_started{false};
std::atomic<bool> s_running{false};
bool s_gs_mode = false; // vk-285-24: the target is not the EE thread (no EE wait skipping)
int s_mode = 0; // vk-285-29: 0 the EE thread, 1 the GS thread, 2 the VU1 (MTVU) thread
pthread_t s_ee; // the profiled thread
uint32_t s_rip_index = 0xe0 / 8;
uint64_t s_ee_stack;
// vk-285-36: the profiled thread's stack, [s_stack_lo, s_stack_hi) (0 when pthread_attr_get_np did not
// say). A library sample's words are then the first six words within kScanWords above RSP that could
// be eboot return addresses, not the top six words: a wait deep in libkernel (the GS thread's 40% at
// R&C3's slowdown, vk-285-35) had no eboot address among its top six.
uint64_t s_stack_lo, s_stack_hi;
constexpr uint32_t kScanWords = 512; // 4 KiB of stack
constexpr uint64_t kEbootLo = 0x400000ull, kEbootHi = 0x10000000ull; // eeprof.py checks the call site

std::mutex& JitMutex()
{
	static std::mutex m;
	return m;
}

std::vector<JitEntry>& JitPending()
{
	static std::vector<JitEntry> v;
	return v;
}

// On the EE thread only: the only thread SIGPROF is sent to.
void SampleHandler(int, siginfo_t*, void* ctx)
{
	if (!ctx)
		return;
	const uint64_t rip = static_cast<const uint64_t*>(ctx)[s_rip_index];
	const uint32_t i = s_count.load(std::memory_order_relaxed);
	if (i >= kMaxSamples)
	{
		s_dropped.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	s_samples[i] = rip;
	if (rip >= kLibLo && rip < kLibHi)
	{
		const uint32_t j = s_lib_count.load(std::memory_order_relaxed);
		if (j < kMaxLibSamples)
		{
			// The interrupted thread's own stack: RSP is three slots above RIP in the context.
			const uint64_t* sp = reinterpret_cast<const uint64_t*>(static_cast<const uint64_t*>(ctx)[s_rip_index + 3]);
			LibSample& l = s_lib_samples[j];
			l.index = i;
			const uint64_t at = reinterpret_cast<uint64_t>(sp);
			if (s_stack_hi != 0 && at >= s_stack_lo && at + 8 * kStackWords <= s_stack_hi)
			{
				// vk-285-36: the first six possible eboot return addresses above RSP, within the stack.
				const uint64_t end = at + 8ull * kScanWords < s_stack_hi ? at + 8ull * kScanWords : s_stack_hi;
				uint32_t found = 0;
				for (const uint64_t* p = sp; reinterpret_cast<uint64_t>(p) + 8 <= end && found < kStackWords; p++)
				{
					if (*p >= kEbootLo && *p < kEbootHi)
						l.words[found++] = *p;
				}
				for (; found < kStackWords; found++)
					l.words[found] = 0;
			}
			else
			{
				for (uint32_t k = 0; k < kStackWords; k++)
					l.words[k] = sp[k];
			}
			s_lib_count.store(j + 1, std::memory_order_release);
		}
	}
	s_count.store(i + 1, std::memory_order_release);
}

volatile uint64_t s_calibration[0x140 / 8];
volatile int s_calibrated;

void CalibrationHandler(int, siginfo_t*, void* ctx)
{
	if (!ctx)
		return;
	const uint64_t* q = static_cast<const uint64_t*>(ctx);
	for (size_t i = 0; i < sizeof(s_calibration) / sizeof(s_calibration[0]); i++)
		s_calibration[i] = q[i];
	s_calibrated = 1;
}

bool Install(void (*handler)(int, siginfo_t*, void*))
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = handler;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigemptyset(&sa.sa_mask);
	return sigaction(SIGPROF, &sa, nullptr) == 0;
}

// One signal to this thread: which slot of the context holds its stack pointer says which
// layout the kernel uses. The PS5 one (RSP at +0xf8, RIP at +0xe0) is the default.
void Calibrate()
{
	volatile int here = 0;
	const uint64_t local = reinterpret_cast<uint64_t>(&here);
	s_calibrated = 0;
	if (!Install(CalibrationHandler) || pthread_kill(pthread_self(), SIGPROF) != 0 || !s_calibrated)
	{
		std::printf("[eeprof] context check: no signal came back (errno=%d); RIP taken at +0xe0\n", errno);
		return;
	}
	auto on_stack = [local](uint64_t v) { return v > local - (1u << 20) && v < local + (1u << 20); };
	const uint64_t rsp_ps5 = s_calibration[0xf8 / 8], rsp_bsd = s_calibration[0xc8 / 8];
	if (on_stack(rsp_ps5))
		s_rip_index = 0xe0 / 8;
	else if (on_stack(rsp_bsd))
		s_rip_index = 0xb0 / 8;
	std::printf("[eeprof] context check: local %#llx, rsp@+0xf8 %#llx, rsp@+0xc8 %#llx, rip@+0xe0 %#llx, rip@+0xb0 %#llx -> RIP at +%#x%s\n",
		static_cast<unsigned long long>(local), static_cast<unsigned long long>(rsp_ps5),
		static_cast<unsigned long long>(rsp_bsd), static_cast<unsigned long long>(s_calibration[0xe0 / 8]),
		static_cast<unsigned long long>(s_calibration[0xb0 / 8]), s_rip_index * 8,
		(on_stack(rsp_ps5) || on_stack(rsp_bsd)) ? "" : " (neither slot held this stack; kept the default)");
}

// vk-285-88: a profiled thread pinned alone to a CPU (live.ini pin=3) got its SIGPROF only at its next
// system call -- the signal waits for the thread to enter the kernel when nothing interrupts it -- so every
// sample of vk-285-87's GS profile fell on a libkernel syscall stub. The sampler therefore follows the
// profiled thread's affinity: on its CPU, its wake-up preempts the thread, which then takes the signal at
// the instruction it was interrupted at.
extern "C" int scePthreadGetaffinity(pthread_t thread, unsigned long long* mask);
extern "C" int scePthreadSetaffinity(pthread_t thread, unsigned long long mask);
static void FollowAffinity()
{
	static unsigned long long s_last = 0;
	unsigned long long mask = 0;
	if (scePthreadGetaffinity(s_ee, &mask) != 0 || mask == 0 || mask == s_last)
		return;
	s_last = mask;
	const int rc = scePthreadSetaffinity(pthread_self(), mask);
	std::printf("[eeprof] sampler follows the profiled thread's CPUs %#llx (rc=%d)\n", mask, rc);
	std::fflush(stdout);
}

// vk-285-89: and at the highest game priority (256; the PS5's range is 256-767, lower first), since
// vk-285-88's sampler on the EE's CPU hardly ever got it: 475 samples in a minute, taken only when the EE
// blocked. At that priority its wake-up preempts the profiled thread at once, every millisecond.
extern "C" int scePthreadGetprio(pthread_t thread, int* prio);
extern "C" int scePthreadSetprio(pthread_t thread, int prio);
static void RaisePriority()
{
	int target = -1, before = -1;
	scePthreadGetprio(s_ee, &target);
	scePthreadGetprio(pthread_self(), &before);
	const int rc = scePthreadSetprio(pthread_self(), 256);
	int after = -1;
	scePthreadGetprio(pthread_self(), &after);
	std::printf("[eeprof] sampler priority %d -> %d (rc=%d), profiled thread's %d\n", before, after, rc, target);
	std::fflush(stdout);
}

void* SamplerThread(void*)
{
	const timespec period = {0, kPeriodUs * 1000};
	unsigned tick = 0;
	RaisePriority();
	for (;;)
	{
		if ((tick++ & 255u) == 0)
			FollowAffinity();
		nanosleep(&period, nullptr);
		if (s_count.load(std::memory_order_relaxed) >= kMaxSamples)
			continue;
		// vk-285-29: the VU1 thread's ring wait is skipped the same way, so its samples are busy time.
		if ((s_mode == 0 && g_orbis_ee_waiting.load(std::memory_order_relaxed) != 0) ||
			(s_mode == 2 && g_orbis_vu_waiting.load(std::memory_order_relaxed) != 0))
		{
			s_skipped.fetch_add(1, std::memory_order_relaxed);
			continue;
		}
		const int rc = pthread_kill(s_ee, SIGPROF);
		if (rc == 0)
			s_sent.fetch_add(1, std::memory_order_relaxed);
		else
			s_kill_error.store(rc, std::memory_order_relaxed);
	}
	return nullptr;
}

void Flush(FILE* f, uint32_t& written, uint32_t& lib_written)
{
	const uint32_t n = s_count.load(std::memory_order_acquire);
	if (n > written)
	{
		const RecordHeader r = {1, n - written, written};
		std::fwrite(&r, sizeof(r), 1, f);
		std::fwrite(s_samples + written, sizeof(uint64_t), n - written, f);
		written = n;
	}
	const uint32_t nl = s_lib_count.load(std::memory_order_acquire);
	if (nl > lib_written)
	{
		const RecordHeader r = {3, nl - lib_written, lib_written};
		std::fwrite(&r, sizeof(r), 1, f);
		std::fwrite(s_lib_samples + lib_written, sizeof(LibSample), nl - lib_written, f);
		lib_written = nl;
	}
	std::vector<JitEntry> jit;
	{
		std::lock_guard<std::mutex> lock(JitMutex());
		jit.swap(JitPending());
	}
	if (!jit.empty())
	{
		const RecordHeader r = {2, static_cast<uint32_t>(jit.size()), 0};
		std::fwrite(&r, sizeof(r), 1, f);
		std::fwrite(jit.data(), sizeof(JitEntry), jit.size(), f);
	}
	std::fflush(f);
}

void* WriterThread(void*)
{
	const std::string out = OrbisLogPath(s_mode == 2 ? kVUPath : s_gs_mode ? kGSPath : kPath); // vk-285-33
	const char* const path = out.c_str();
	FILE* f = std::fopen(path, "wb");
	if (!f)
	{
		std::printf("[eeprof] cannot write %s (errno=%d); profiler off\n", path, errno);
		std::fflush(stdout);
		s_running.store(false, std::memory_order_relaxed);
		return nullptr;
	}
	FileHeader h = {};
	std::memcpy(h.magic, "EEPROF1", 8);
	h.version = 1;
	h.rip_offset = s_rip_index * 8;
	h.code_base = reinterpret_cast<uint64_t>(SysMemory::GetCodePtr(0));
	h.code_size = HostMemoryMap::CodeSize;
	h.self_fn = reinterpret_cast<uint64_t>(&OrbisEEProfStart);
	h.ee_stack = s_ee_stack;
	h.period_us = kPeriodUs;
	h.thread = static_cast<uint64_t>(s_mode);
	std::fwrite(&h, sizeof(h), 1, f);
	std::fflush(f);
	uint32_t written = 0, lib_written = 0;
	for (;;)
	{
		const timespec wait = {kFlushSeconds, 0};
		nanosleep(&wait, nullptr);
		Flush(f, written, lib_written);
	}
	return nullptr;
}
} // namespace

namespace
{
// vk-285-12: opt-in. Decided once, before the first JIT registration, so nothing is
// buffered when the profiler is off.
bool ProfilerWanted()
{
	// vk-285-29: the VU1 profile needs the JIT registrations too (its samples land in VU1 programs).
	static const bool wanted = (OrbisFlag("eeprof") || OrbisFlag("vuprof")) && !OrbisFlag("prof"); // vk-285-33: flags/
	return wanted;
}
} // namespace

void OrbisJitRegister(const char* group, const void* ptr, size_t size, u64 key, const char* name)
{
	if (!ProfilerWanted())
		return;
	JitEntry e = {};
	e.ptr = reinterpret_cast<uint64_t>(ptr);
	e.key = key;
	e.size = static_cast<uint32_t>(size);
	e.at = s_count.load(std::memory_order_relaxed);
	if (group)
		std::strncpy(e.group, group, sizeof(e.group) - 1);
	if (name)
		std::strncpy(e.name, name, sizeof(e.name) - 1);
	std::lock_guard<std::mutex> lock(JitMutex());
	JitPending().push_back(e);
}

namespace
{
// Starts sampling the calling thread: mode 0 the EE thread, 1 the GS thread, 2 the VU1 thread.
void StartOnThisThread(int mode)
{
	const bool gs = mode == 1;
	s_mode = mode;
	s_gs_mode = mode != 0;
	s_samples = new (std::nothrow) uint64_t[kMaxSamples];
	s_lib_samples = new (std::nothrow) LibSample[kMaxLibSamples];
	if (!s_samples || !s_lib_samples)
	{
		std::printf("[eeprof] no memory for %u samples; profiler off\n", kMaxSamples);
		std::fflush(stdout);
		return;
	}
	volatile int here = 0;
	s_ee_stack = reinterpret_cast<uint64_t>(&here);
	// vk-285-36: the stack's bounds, for the library samples' deeper scan (SampleHandler).
	{
		pthread_attr_t attr;
		if (pthread_attr_init(&attr) == 0)
		{
			void* stack_addr = nullptr;
			size_t stack_size = 0;
			if (pthread_attr_get_np(pthread_self(), &attr) == 0 &&
				pthread_attr_getstack(&attr, &stack_addr, &stack_size) == 0 && stack_addr != nullptr &&
				s_ee_stack >= reinterpret_cast<uint64_t>(stack_addr) &&
				s_ee_stack < reinterpret_cast<uint64_t>(stack_addr) + stack_size)
			{
				s_stack_lo = reinterpret_cast<uint64_t>(stack_addr);
				s_stack_hi = s_stack_lo + stack_size;
			}
			pthread_attr_destroy(&attr);
		}
		std::printf("[eeprof] stack %#llx-%#llx (here %#llx)%s\n", static_cast<unsigned long long>(s_stack_lo),
			static_cast<unsigned long long>(s_stack_hi), static_cast<unsigned long long>(s_ee_stack),
			s_stack_hi != 0 ? "" : ": unknown, library samples keep their top six words");
	}
	s_ee = pthread_self();
	Calibrate();
	if (!Install(SampleHandler))
	{
		std::printf("[eeprof] sigaction failed (errno=%d); profiler off\n", errno);
		std::fflush(stdout);
		return;
	}
	s_running.store(true, std::memory_order_relaxed);
	pthread_t writer, sampler;
	const int rw = pthread_create(&writer, nullptr, WriterThread, nullptr);
	const int rs = pthread_create(&sampler, nullptr, SamplerThread, nullptr);
	if (rw == 0)
		pthread_detach(writer);
	if (rs == 0)
		pthread_detach(sampler);
	PrintLibraryAddresses();
	std::printf("[eeprof] start: %s thread %#llx, code %p + %#x, eboot fn %p, every %ld us, writer %d sampler %d -> %s\n",
		mode == 2 ? "VU1" : gs ? "GS" : "EE", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(s_ee)),
		SysMemory::GetCodePtr(0), HostMemoryMap::CodeSize, reinterpret_cast<void*>(&OrbisEEProfStart), kPeriodUs, rw, rs,
		OrbisLogPath(mode == 2 ? kVUPath : gs ? kGSPath : kPath).c_str());
	std::fflush(stdout);
}
} // namespace

// Host::PumpMessagesOnCPUThread (StubHost.cpp) calls this at every vsync; the first call, on
// the EE thread, starts the profiler.
void OrbisEEProfStart()
{
	static std::atomic<bool> s_ee_checked{false};
	if (s_ee_checked.load(std::memory_order_relaxed) || s_ee_checked.exchange(true))
		return;
	if (!ProfilerWanted())
	{
		std::printf("[eeprof] off (opt-in: create /data/PCSX2/eeprof, or /data/PCSX2/gsprof for the GS thread)\n");
		std::fflush(stdout);
		return;
	}
	if (!OrbisFlag("eeprof"))
		return; // vk-285-29: vuprof alone asks for the VU1 thread (OrbisVUProfStart)
	if (s_started.exchange(true))
		return;
	StartOnThisThread(0);
}

// vk-285-24: GSRenderer.cpp's OrbisPrintLoad calls this once a second on the GS thread; it starts
// the profiler on that thread once /data/PCSX2/gsprof exists (and no profiler runs yet).
void OrbisGSProfStart()
{
	if (s_started.load(std::memory_order_relaxed))
		return;
	if (!OrbisFlag("gsprof"))
		return;
	if (s_started.exchange(true))
		return;
	StartOnThisThread(1);
}

// vk-285-29: the MTVU thread calls this each time its ring runs dry; it starts the profiler on
// that thread when /data/PCSX2/vuprof was there at start (ProfilerWanted, which also keeps the
// JIT registrations the VU1 samples are named by) and no profiler runs yet.
void OrbisVUProfStart()
{
	if (s_started.load(std::memory_order_relaxed))
		return;
	static const bool s_vu_wanted = ProfilerWanted() && OrbisFlag("vuprof");
	if (!s_vu_wanted || s_started.exchange(true))
		return;
	StartOnThisThread(2);
}

// Where the system libraries' hot functions are, so samples outside the eboot and the JIT can be
// named offline (vk-285-8 had two hot spots in the first MB of libkernel).
static void PrintLibraryAddresses()
{
	struct Named
	{
		const char* name;
		const void* address;
	};
	const Named named[] = {
		{"pthread_getspecific", reinterpret_cast<const void*>(&pthread_getspecific)},
		{"pthread_self", reinterpret_cast<const void*>(&pthread_self)},
		{"sched_yield", reinterpret_cast<const void*>(&sched_yield)},
		{"clock_gettime", reinterpret_cast<const void*>(&clock_gettime)},
		{"nanosleep", reinterpret_cast<const void*>(&nanosleep)},
		{"memcpy", reinterpret_cast<const void*>(&memcpy)},
		{"memmove", reinterpret_cast<const void*>(&memmove)},
		{"memset", reinterpret_cast<const void*>(&memset)},
		{"memcmp", reinterpret_cast<const void*>(&memcmp)},
		{"malloc", reinterpret_cast<const void*>(&malloc)},
		{"free", reinterpret_cast<const void*>(&free)},
		{"sem_post", reinterpret_cast<const void*>(&sem_post)},
		{"sem_wait", reinterpret_cast<const void*>(&sem_wait)},
		{"pthread_mutex_lock", reinterpret_cast<const void*>(&pthread_mutex_lock)},
		{"pthread_mutex_unlock", reinterpret_cast<const void*>(&pthread_mutex_unlock)},
		{"pthread_cond_signal", reinterpret_cast<const void*>(&pthread_cond_signal)},
		{"pthread_cond_broadcast", reinterpret_cast<const void*>(&pthread_cond_broadcast)},
		{"pthread_kill", reinterpret_cast<const void*>(&pthread_kill)},
		{"sceKernelGetCurrentCpu", reinterpret_cast<const void*>(&sceKernelGetCurrentCpu)},
	};
	std::printf("[eeprof] libs:");
	for (const Named& n : named)
		std::printf(" %s=%p", n.name, n.address);
	std::printf("\n");
}

// GSRenderer.cpp's OrbisPrintLoad calls this after each [load] line (GS thread).
void OrbisEEProfMark()
{
	if (!s_running.load(std::memory_order_relaxed))
		return;
	const int err = s_kill_error.load(std::memory_order_relaxed);
	// vk-285-107: through the deferred log, as one line. This runs on the GS thread once a second, and a direct
	// printf there waited on stdout's lock while the ticker thread wrote to /data (vk-285-106's [vsslow], ~33 ms).
	char kill[32] = "";
	if (err)
		std::snprintf(kill, sizeof(kill), " kill_error=%d", err);
	OrbisDeferredPrintf("[%s] n=%u sent=%u skipped=%u dropped=%u%s\n", s_mode == 2 ? "vuprof" : s_gs_mode ? "gsprof" : "eeprof",
		s_count.load(std::memory_order_relaxed), s_sent.load(std::memory_order_relaxed),
		s_skipped.load(std::memory_order_relaxed), s_dropped.load(std::memory_order_relaxed), kill);
}
