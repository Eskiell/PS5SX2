// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "ImGui/FullscreenUI.h"
#include "ImGui/ImGuiManager.h"
#include "GS/Renderers/Common/GSRenderer.h"
#include "GS/GSCapture.h"
#include "GS/GSDump.h"
#include "GS/GSGL.h"
#include "GS/GSPerfMon.h"
#include "GS/GSUtil.h"
#include "GSDumpReplayer.h"
#include "Host.h"
#include "PerformanceMetrics.h"
#include "pcsx2/Config.h"
#include "VMManager.h"

#include "common/FileSystem.h"
#include "common/Image.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "common/Timer.h"

#include "fmt/format.h"
#include "IconsFontAwesome.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

static void DumpGSPrivRegs(const GSPrivRegSet& r, const std::string& filename);

// Orbis: GL/GS readback dump implemented in GSDeviceOGL.cpp.
void OrbisPresentGLFrame();
void OrbisSampleWindow();

static constexpr std::array<PresentShader, 8> s_tv_shader_indices = {
	PresentShader::COPY, PresentShader::SCANLINE,
	PresentShader::DIAGONAL_FILTER, PresentShader::TRIANGULAR_FILTER,
	PresentShader::COMPLEX_FILTER, PresentShader::LOTTES_FILTER,
	PresentShader::SUPERSAMPLE_4xRGSS, PresentShader::SUPERSAMPLE_AUTO};

// ---- eerec-278 (PS5 port): live present tuning + FPS box for the GL presenter ----
// Modes cycle when L3+R3 are held (the pad thread bumps g_orbis_filter_cycle) or are set from
// /data/PCSX2/live.ini (re-read every 30 vsyncs when its content changes). There is no ImGui
// on Orbis, so the FPS / mode label is a small CPU-drawn texture presented top-right.
extern std::atomic<int> g_orbis_filter_cycle;
extern "C" void orbis_text_rgba(u32* buf, unsigned w, unsigned h, unsigned x, unsigned y, const char* s, unsigned scale, u32 color);
float g_orbis_present_param[4] = {0.5f, 0.0f, 0.0f, 0.0f}; // u_orbis_param (GSDeviceOGL::PresentRect)
int g_orbis_swtex = 2; // eerec-280 default (eerec-279: 1): SW output texture kind (GSRendererSW::GetOutput)
int g_orbis_testpat = 0; // eerec-279: SW output replaced by a test pattern
int g_orbis_upload_mode = 0; // eerec-279: GSTextureOGL::Update path (0 PCSX2, 1 direct DSA, 2 direct bind)
char g_orbis_osd_text[24]; // eerec-282: one-shot OSD label (savestates)
std::atomic<int> g_orbis_osd_text_frames{0};
void OrbisOSDLabel(const char* text)
{
	snprintf(g_orbis_osd_text, sizeof(g_orbis_osd_text), "%s", text);
	g_orbis_osd_text_frames.store(120, std::memory_order_release);
}
int g_orbis_diag = 0; // eerec-280: periodic GL readback diagnostics (live.ini diag=1)
int g_orbis_perf = 0; // eerec-280: perf OSD + [perf] klog line every second (live.ini perf=1)
// ---- eerec-285: live gs.ini reload flags; CPU placement sampling and pinning ----
#include <pthread.h>
std::atomic<int> g_orbis_gsini_reload{0}; // GS thread saw gs.ini change -> the CPU thread applies it
std::atomic<int> g_orbis_live_reapply{0}; // the CPU thread applied gs.ini -> apply live.ini again
std::atomic<int> g_orbis_pin_request{-1}; // live.ini pin= -> the CPU thread (OrbisApplyPinning)
extern "C" int sceKernelGetCurrentCpu(void);
extern "C" int sceKernelGetCpumode(void);
extern "C" int scePthreadGetaffinity(pthread_t thread, unsigned long long* mask);
extern "C" int scePthreadSetaffinity(pthread_t thread, unsigned long long mask);
// slots: 0 EE, 1 GS, 2 VU1, 3..7 SW workers 0..4
static constexpr int ORBIS_CPU_SLOTS = 8;
static pthread_t s_orbis_thr[ORBIS_CPU_SLOTS];
static unsigned s_orbis_cpu_hist[ORBIS_CPU_SLOTS][16];
static unsigned long long s_orbis_cpu_last[ORBIS_CPU_SLOTS];
static int s_orbis_pin_mode = 0;
void OrbisCpuSample(int slot)
{
	if (slot < 0 || slot >= ORBIS_CPU_SLOTS)
		return;
	const unsigned long long t = __builtin_ia32_rdtsc();
	if (t - s_orbis_cpu_last[slot] < 16000000ull) // about 5 ms
		return;
	s_orbis_cpu_last[slot] = t;
	if (s_orbis_thr[slot] == pthread_t{})
	{
		s_orbis_thr[slot] = pthread_self();
		if (s_orbis_pin_mode != 0) // a thread that started after pinning is pinned too
			g_orbis_pin_request.store(s_orbis_pin_mode, std::memory_order_release);
	}
	const int c = sceKernelGetCurrentCpu();
	if (c >= 0 && c < 16)
		s_orbis_cpu_hist[slot][c]++;
}
void OrbisCpuForget(int slot)
{
	if (slot >= 0 && slot < ORBIS_CPU_SLOTS)
		s_orbis_thr[slot] = pthread_t{};
}
// CPU thread (Host::PumpMessagesOnCPUThread). SMT siblings are taken to be CPUs 2k and 2k+1.
void OrbisApplyPinning(int mode)
{
	static unsigned long long s_orig = 0;
	const pthread_t self = pthread_self();
	s_orbis_thr[0] = self;
	if (s_orig == 0)
	{
		unsigned long long m = 0;
		const int rc = scePthreadGetaffinity(self, &m);
		printf("[pin] process mask %#llx (rc=%d) cpumode=%d\n", m, rc, sceKernelGetCpumode());
		if (rc != 0 || m == 0)
		{
			fflush(stdout);
			OrbisOSDLabel("PIN: NO MASK");
			return;
		}
		s_orig = m;
	}
	int pairs[8], np = 0;
	for (int k = 7; k >= 0; k--)
		if (((s_orig >> (2 * k)) & 3ull) == 3ull)
			pairs[np++] = k;
	if (np < 3)
		mode = 0;
	unsigned long long ee = s_orig, gs = s_orig, rest = s_orig;
	if (mode >= 1)
	{
		ee = 1ull << (2 * pairs[0]);
		rest &= ~(3ull << (2 * pairs[0]));
		gs = rest;
	}
	if (mode >= 2 && np >= 4)
	{
		gs = 1ull << (2 * pairs[1]);
		rest &= ~(3ull << (2 * pairs[1]));
	}
	s_orbis_pin_mode = mode;
	int rc[ORBIS_CPU_SLOTS];
	for (int i = 0; i < ORBIS_CPU_SLOTS; i++)
	{
		const unsigned long long m = (i == 0) ? ee : (i == 1) ? gs : rest;
		rc[i] = (s_orbis_thr[i] != pthread_t{}) ? scePthreadSetaffinity(s_orbis_thr[i], m) : 1;
	}
	printf("[pin] mode=%d pairs=%d ee=%#llx gs=%#llx rest=%#llx rc ee=%d gs=%d vu=%d sw=%d/%d/%d/%d/%d\n", mode, np, ee,
		gs, rest, rc[0], rc[1], rc[2], rc[3], rc[4], rc[5], rc[6], rc[7]);
	fflush(stdout);
	char label[24];
	snprintf(label, sizeof(label), "PIN %d", mode);
	OrbisOSDLabel(label);
}
// Appended to the [load] line: each thread's two most frequent CPUs and their share of the samples.
static void OrbisPrintCpu()
{
	static const char* const names[ORBIS_CPU_SLOTS] = {"ee", "gs", "vu", "sw0", "sw1", "sw2", "sw3", "sw4"};
	printf(" | cpu");
	for (int i = 0; i < ORBIS_CPU_SLOTS; i++)
	{
		unsigned h[16];
		memcpy(h, s_orbis_cpu_hist[i], sizeof(h));
		memset(s_orbis_cpu_hist[i], 0, sizeof(h));
		unsigned tot = 0;
		int a = -1, b = -1;
		for (int c = 0; c < 16; c++)
		{
			tot += h[c];
			if (h[c] == 0)
				continue;
			if (a < 0 || h[c] > h[a])
			{
				b = a;
				a = c;
			}
			else if (b < 0 || h[c] > h[b])
				b = c;
		}
		if (tot == 0)
			continue;
		printf(" %s=%d:%u", names[i], a, h[a] * 100 / tot);
		if (b >= 0)
			printf(",%d:%u", b, h[b] * 100 / tot);
	}
}
extern double GetVerticalFrequency();
extern unsigned long long g_orbis_gs_idle_ticks, g_orbis_ee_waitgs_ticks, g_orbis_ee_stall_ticks; // eerec-174
extern unsigned long long g_orbis_ee_vsyncq_ticks, g_orbis_vu_idle_ticks, g_orbis_ee_waitvu_ticks, g_orbis_ee_vuring_ticks,
	g_orbis_ee_throttle_ticks, g_orbis_gs_swsync_ticks, g_orbis_sw_busy_ticks[16]; // eerec-281
// eerec-281: ms per second each thread spent waiting (TSC, calibrated against steady_clock every print)
static void OrbisPrintLoad()
{
	static unsigned long long s_tsc = 0, s_prev[26] = {};
	static auto s_t = std::chrono::steady_clock::now();
	const unsigned long long tsc = __builtin_ia32_rdtsc();
	const auto now = std::chrono::steady_clock::now();
	const double sec = std::chrono::duration<double>(now - s_t).count();
	const unsigned long long cur[10] = {g_orbis_ee_waitgs_ticks, g_orbis_ee_stall_ticks, g_orbis_ee_vsyncq_ticks,
		g_orbis_ee_waitvu_ticks, g_orbis_ee_vuring_ticks, g_orbis_ee_throttle_ticks, g_orbis_gs_idle_ticks,
		g_orbis_gs_swsync_ticks, g_orbis_vu_idle_ticks, 0};
	const u32 nsw = std::min<u32>(PerformanceMetrics::GetGSSWThreadCount(), 16);
	if (s_tsc != 0 && sec > 0.2)
	{
		const double k = 1000.0 / static_cast<double>(tsc - s_tsc); // ms of each second per TSC tick
		double v[10];
		for (int i = 0; i < 9; i++)
			v[i] = static_cast<double>(cur[i] - s_prev[i]) * k;
		const double ee_wait = v[0] + v[1] + v[2] + v[3] + v[4] + v[5];
		printf("[load] ms/s ee: busy=%.0f waitgs=%.0f ringfull=%.0f vsyncq=%.0f waitvu=%.0f vuring=%.0f throttle=%.0f | gs: busy=%.0f swsync=%.0f | vu: busy=%.0f | sw busy=",
			1000.0 - ee_wait, v[0], v[1], v[2], v[3], v[4], v[5], 1000.0 - v[6], v[7], 1000.0 - v[8]);
		for (u32 i = 0; i < nsw; i++)
			printf("%s%.0f", i ? "/" : "", static_cast<double>(g_orbis_sw_busy_ticks[i] - s_prev[10 + i]) * k);
		OrbisPrintCpu(); // eerec-285
		printf("\n");
	}
	for (int i = 0; i < 9; i++)
		s_prev[i] = cur[i];
	for (u32 i = 0; i < nsw; i++)
		s_prev[10 + i] = g_orbis_sw_busy_ticks[i];
	s_tsc = tsc;
	s_t = now;
}
void OrbisDiagTexture(const char* tag, GSTexture* t);

namespace
{
	struct OrbisPresentMode
	{
		const char* name;
		int tv_shader; // index into s_tv_shader_indices
		int presharp; // native-res unsharp mask (ShadeBoost slot), percent, 0 = off
		int sharp; // FSR sharpening (RCAS) amount, percent, TVShader 6 only
	};
	OrbisPresentMode s_orbis_modes[] = {
		{"FSR", 6, 50, 50},
		{"FSR SOFT", 7, 0, 0},
		{"CLASSIC", 0, 60, 0},
		{"CRT", 5, 0, 0},
	};
	constexpr int ORBIS_NUM_MODES = static_cast<int>(sizeof(s_orbis_modes) / sizeof(s_orbis_modes[0]));
	int s_orbis_mode = 0;
	int s_orbis_label_frames = 0;
	bool s_orbis_fps_box = true;
	bool s_orbis_gl = false;
} // namespace

static void OrbisApplyMode(int m, bool announce)
{
	m = ((m % ORBIS_NUM_MODES) + ORBIS_NUM_MODES) % ORBIS_NUM_MODES;
	s_orbis_mode = m;
	const OrbisPresentMode& pm = s_orbis_modes[m];
	GSConfig.TVShader = pm.tv_shader;
	GSConfig.LinearPresent = GSPostBilinearMode::BilinearSmooth;
	GSConfig.ShadeBoost = (pm.presharp > 0);
	GSConfig.ShadeBoost_Saturation = static_cast<u8>(std::clamp(pm.presharp / 2, 0, 100));
#ifdef ORBIS_VULKAN
	// The port's GL shadeboost.glsl reads the saturation as a pre-sharpen amount.
	// The Vulkan build runs PCSX2's own shadeboost, where 25 means desaturate to
	// half (50 is neutral), so it stays off.
	if (g_gs_device && g_gs_device->GetRenderAPI() == RenderAPI::Vulkan)
		GSConfig.ShadeBoost = false;
#endif
	g_orbis_present_param[0] = static_cast<float>(pm.sharp) / 100.0f;
	if (announce)
		s_orbis_label_frames = 120;
	printf("[present] mode=%d %s tv=%d presharp=%d sharp=%d split=%.0f aspect=%d\n", m, pm.name, pm.tv_shader,
		pm.presharp, pm.sharp, g_orbis_present_param[1], static_cast<int>(EmuConfig.CurrentAspectRatio));
	fflush(stdout);
}

static void OrbisLiveTune()
{
	OrbisCpuSample(1); // eerec-285: GS thread
#ifdef ORBIS_VULKAN
	// PS5 Vulkan build: the same present modes, live.ini tuning, FPS box and
	// [perf]/[load] lines on GSDeviceVK.
	if (!g_gs_device || (g_gs_device->GetRenderAPI() != RenderAPI::OpenGL &&
							g_gs_device->GetRenderAPI() != RenderAPI::Vulkan))
		return;
#else
	if (!g_gs_device || g_gs_device->GetRenderAPI() != RenderAPI::OpenGL)
		return;
#endif
	static bool s_init = false;
	static int s_seen_cycle = 0;
	if (!s_init)
	{
		s_init = true;
		s_orbis_gl = true;
		s_seen_cycle = g_orbis_filter_cycle.load(std::memory_order_relaxed);
		int m = 0;
		for (int i = 0; i < ORBIS_NUM_MODES; i++)
		{
			if (s_orbis_modes[i].tv_shader == static_cast<int>(GSConfig.TVShader))
			{
				m = i;
				break;
			}
		}
		s_orbis_fps_box = (access("/data/PCSX2/nofps", F_OK) != 0);
		OrbisApplyMode(m, false);
	}
	const int cyc = g_orbis_filter_cycle.load(std::memory_order_relaxed);
	if (cyc != s_seen_cycle)
	{
		s_seen_cycle = cyc;
		OrbisApplyMode(s_orbis_mode + 1, true);
	}

	static unsigned s_poll = 0;
	if ((s_poll++ % 30) != 0)
		return;
	{
		// eerec-285: gs.ini applies live once a changed file reads the same twice (a half-written one is skipped)
		static bool s_gsini_init = false;
		static std::string s_gsini_applied, s_gsini_pending;
		std::string g;
		if (FILE* f = fopen("/data/PCSX2/gs.ini", "rb"))
		{
			char b[2048];
			const size_t n = fread(b, 1, sizeof(b), f);
			fclose(f);
			g.assign(b, n);
		}
		if (!s_gsini_init)
		{
			s_gsini_init = true;
			s_gsini_applied = s_gsini_pending = g;
			unsigned long long m = 0;
			const int rc = scePthreadGetaffinity(pthread_self(), &m);
			printf("[cpu] GS thread on cpu %d, affinity %#llx (rc=%d), cpumode=%d\n", sceKernelGetCurrentCpu(), m, rc,
				sceKernelGetCpumode());
			fflush(stdout);
		}
		else if (g != s_gsini_applied)
		{
			if (g == s_gsini_pending)
			{
				s_gsini_applied = g;
				g_orbis_gsini_reload.store(1, std::memory_order_release);
				printf("[gsini] gs.ini changed (%zu bytes): applying at the next vsync\n", g.size());
				fflush(stdout);
			}
			else
				s_gsini_pending = g;
		}
	}
	static std::string s_last;
	std::string cur;
	if (g_orbis_live_reapply.exchange(0, std::memory_order_acq_rel))
		s_last = "\x01"; // eerec-285: gs.ini was applied, so apply live.ini again (present mode, pin)
	if (FILE* f = fopen("/data/PCSX2/live.ini", "rb"))
	{
		char buf[1024];
		const size_t n = fread(buf, 1, sizeof(buf), f);
		fclose(f);
		cur.assign(buf, n);
	}
	if (cur == s_last)
		return;
	s_last = cur;
	int mode = -1;
	size_t pos = 0;
	while (pos < cur.size())
	{
		size_t eol = cur.find('\n', pos);
		if (eol == std::string::npos)
			eol = cur.size();
		const std::string line = cur.substr(pos, eol - pos);
		pos = eol + 1;
		char key[32] = {};
		float v = 0.0f;
		if (sscanf(line.c_str(), " %31[a-z_] = %f", key, &v) != 2)
			continue;
		const std::string k(key);
		if (k == "mode")
			mode = static_cast<int>(v);
		else if (k == "sharp")
			s_orbis_modes[0].sharp = std::clamp(static_cast<int>(v * 100.0f + 0.5f), 0, 100);
		else if (k == "presharp")
			s_orbis_modes[0].presharp = std::clamp(static_cast<int>(v * 100.0f + 0.5f), 0, 200);
		else if (k == "classic_sharp")
			s_orbis_modes[2].presharp = std::clamp(static_cast<int>(v * 100.0f + 0.5f), 0, 200);
		else if (k == "split")
			g_orbis_present_param[1] = (v > 0.0f && v <= 1.0f) ? v * static_cast<float>(g_gs_device->GetWindowWidth()) : std::max(v, 0.0f);
		else if (k == "fps")
			s_orbis_fps_box = (v != 0.0f);
		else if (k == "deinterlace")
			GSConfig.InterlaceMode = static_cast<GSInterlaceMode>(std::clamp(static_cast<int>(v), 0, static_cast<int>(GSInterlaceMode::Count) - 1));
		else if (k == "fxaa")
			GSConfig.FXAA = (v != 0.0f);
		else if (k == "antiblur")
			GSConfig.PCRTCAntiBlur = (v != 0.0f);
		else if (k == "swtex")
			g_orbis_swtex = std::clamp(static_cast<int>(v), 0, 2);
		else if (k == "testpat")
			g_orbis_testpat = (v != 0.0f);
		else if (k == "upload")
			g_orbis_upload_mode = std::clamp(static_cast<int>(v), 0, 2);
		else if (k == "diag")
			g_orbis_diag = (v != 0.0f);
		else if (k == "perf")
			g_orbis_perf = (v != 0.0f);
		else if (k == "pin") // eerec-285
			g_orbis_pin_request.store(std::clamp(static_cast<int>(v), 0, 2), std::memory_order_release);
		else if (k == "aspect")
		{
			const int a = static_cast<int>(v);
			const AspectRatioType t = (a == 0) ? AspectRatioType::Stretch : (a == 4) ? AspectRatioType::R4_3 :
				(a == 16) ? AspectRatioType::R16_9 : AspectRatioType::RAuto4_3_3_2;
			EmuConfig.CurrentAspectRatio = t;
			GSConfig.AspectRatio = t;
		}
	}
	printf("[present] live.ini (%zu bytes) applied: deinterlace=%d fxaa=%d antiblur=%d\n", cur.size(),
		static_cast<int>(GSConfig.InterlaceMode), static_cast<int>(GSConfig.FXAA), static_cast<int>(GSConfig.PCRTCAntiBlur));
	printf("[present] swtex=%d testpat=%d upload=%d diag=%d perf=%d\n", g_orbis_swtex, g_orbis_testpat, g_orbis_upload_mode,
		g_orbis_diag, g_orbis_perf); // eerec-280
	OrbisApplyMode(mode >= 0 ? mode : s_orbis_mode, mode >= 0);
}

static void OrbisGLOSD()
{
	if (!s_orbis_gl || !g_gs_device)
		return;
	static u64 s_count = 0;
	static auto s_t0 = std::chrono::steady_clock::now();
	static unsigned s_fps = 0;
	s_count++;
	const auto now = std::chrono::steady_clock::now();
	const double dt = std::chrono::duration<double>(now - s_t0).count();
	if (dt >= 1.0)
	{
		s_fps = static_cast<unsigned>(static_cast<double>(s_count) / dt + 0.5);
		if (g_orbis_perf) // eerec-280
		{
			printf("[perf] fps=%u vfreq=%.2f speed=%.0f ee=%.0f gs=%.0f vu=%.0f ft=%.1f/%.1f/%.1f sw=", s_fps,
				GetVerticalFrequency(), PerformanceMetrics::GetSpeed(), PerformanceMetrics::GetCPUThreadUsage(),
				PerformanceMetrics::GetGSThreadUsage(), PerformanceMetrics::GetVUThreadUsage(),
				PerformanceMetrics::GetMinimumFrameTime(), PerformanceMetrics::GetAverageFrameTime(),
				PerformanceMetrics::GetMaximumFrameTime());
			for (u32 i = 0; i < PerformanceMetrics::GetGSSWThreadCount(); i++)
				printf("%s%.0f", i ? "/" : "", PerformanceMetrics::GetGSSWThreadUsage(i));
			printf("\n");
			OrbisPrintLoad(); // eerec-281
			fflush(stdout);
		}
		s_count = 0;
		s_t0 = now;
	}
	char text[48];
	if (g_orbis_osd_text_frames.load(std::memory_order_acquire) > 0) // eerec-282
	{
		g_orbis_osd_text_frames.fetch_sub(1, std::memory_order_relaxed);
		snprintf(text, sizeof(text), "%s", g_orbis_osd_text);
	}
	else if (s_orbis_label_frames > 0)
	{
		s_orbis_label_frames--;
		snprintf(text, sizeof(text), "%s", s_orbis_modes[s_orbis_mode].name);
	}
	else if (s_orbis_fps_box && g_orbis_perf) // eerec-280
		snprintf(text, sizeof(text), "%u FPS EE%u GS%u VU%u", s_fps,
			static_cast<unsigned>(PerformanceMetrics::GetCPUThreadUsage() + 0.5),
			static_cast<unsigned>(PerformanceMetrics::GetGSThreadUsage() + 0.5f),
			static_cast<unsigned>(PerformanceMetrics::GetVUThreadUsage() + 0.5f));
	else if (s_orbis_fps_box)
		snprintf(text, sizeof(text), "FPS %u", s_fps);
	else
		return;

	constexpr int TW = 480, TH = 50, SCALE = 3; // eerec-280: 320 -> 480 for the perf text
	static GSTexture* s_tex = nullptr;
	static GSDevice* s_dev = nullptr;
	static char s_last[48] = {};
	static int s_box_w = TW;
	if (s_dev != g_gs_device.get())
	{
		s_dev = g_gs_device.get();
		s_tex = s_dev->CreateTexture(TW, TH, 1, GSTexture::Format::Color);
		s_last[0] = 0;
	}
	if (!s_tex)
		return;
	if (strcmp(text, s_last) != 0)
	{
		static u32 s_buf[TW * TH];
		int tw = 0;
		for (const char* c = text; *c; c++)
			tw += ((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z')) ? 6 * SCALE : 2 * SCALE;
		s_box_w = std::min(TW, tw + 30 - SCALE);
		std::fill(std::begin(s_buf), std::end(s_buf), 0xFF202020u);
		orbis_text_rgba(s_buf, TW, TH, 15, 12, text, SCALE, 0xFF00FFFFu);
		s_tex->Update(GSVector4i(0, 0, TW, TH), s_buf, TW * 4);
		snprintf(s_last, sizeof(s_last), "%s", text);
	}
	const float ww = static_cast<float>(g_gs_device->GetWindowWidth());
	const float wh = static_cast<float>(g_gs_device->GetWindowHeight());
	const float x1 = ww - 20.0f, x0 = x1 - static_cast<float>(s_box_w);
#ifdef ORBIS_VULKAN
	// Vulkan's window origin is the top-left corner: 40 px below the top edge.
	(void)wh;
	const float y0 = 40.0f, y1 = y0 + static_cast<float>(TH);
#else
	const float y1 = wh - 40.0f, y0 = y1 - static_cast<float>(TH); // GL lower-left origin: 40 px below the top edge
#endif
	g_gs_device->PresentRect(s_tex, GSVector4(0.0f, 0.0f, static_cast<float>(s_box_w) / TW, 1.0f), nullptr,
		GSVector4(x0, y0, x1, y1), PresentShader::COPY, 0.0f, Nearest);
}
// ---- end eerec-278 ----

static std::deque<std::thread> s_screenshot_threads;
static std::mutex s_screenshot_threads_mutex;

std::unique_ptr<GSRenderer> g_gs_renderer;

// Since we read this on the EE thread, we can't put it in the renderer, because
// we might be switching while the other thread reads it.
static GSVector4 s_last_draw_rect;

// Last time we reset the renderer due to a GPU crash, if any.
static Common::Timer::Value s_last_gpu_reset_time;

// Screen alignment
static GSDisplayAlignment s_display_alignment = GSDisplayAlignment::Center;

GSRenderer::GSRenderer()
	: m_shader_time_start(Common::Timer::GetCurrentValue())
{
	s_last_draw_rect = GSVector4::zero();
}

GSRenderer::~GSRenderer() = default;

void GSRenderer::Reset(bool hardware_reset)
{
	// Clear the current display texture.
	// Orbis: Null/SW renderers have no GSDevice (g_gs_device == nullptr).
	if (hardware_reset && g_gs_device)
		g_gs_device->ClearCurrent();

	GSState::Reset(hardware_reset);
}

void GSRenderer::Destroy()
{
	GSCapture::EndCapture();
}

void GSRenderer::UpdateRenderFixes()
{
}

bool GSRenderer::Merge(int field)
{
	{
		static bool logged = false;
		if (!logged) { logged = true; printf("[dbg] merge called field=%d\n", field); fflush(stdout); }
	}
	GSVector2i fs(0, 0);
	GSTexture* tex[3] = { nullptr, nullptr, nullptr };
	float tex_scale[3] = { 0.0f, 0.0f, 0.0f };
	int y_offset[3] = { 0, 0, 0 };
	const bool feedback_merge = m_regs->EXTWRITE.WRITE == 1;

	// Need to do this here, if the user has Anti-Blur enabled, these offsets can get wiped out/changed.
	const bool game_deinterlacing = (PCRTCDisplays.PCRTCDisplays[0].prevFramebufferOffsets.y != PCRTCDisplays.PCRTCDisplays[0].framebufferOffsets.y) !=
	                                (PCRTCDisplays.PCRTCDisplays[1].prevFramebufferOffsets.y != PCRTCDisplays.PCRTCDisplays[1].framebufferOffsets.y);

	// Only need to check the right/bottom on software renderer, hardware always gets the full texture then cuts a bit out later.
	if (PCRTCDisplays.FrameRectMatch() && !PCRTCDisplays.FrameWrap() && !feedback_merge)
	{
		tex[0] = GetOutput(-1, tex_scale[0], y_offset[0]);
		tex[1] = tex[0]; // saves one texture fetch
		y_offset[1] = y_offset[0];
		tex_scale[1] = tex_scale[0];
	}
	else
	{
		const bool use_rc1 =
			PCRTCDisplays.PCRTCDisplays[0].enabled &&                    // RC1 enabled.
				(!(m_regs->PMODE.MMOD == 1 && m_regs->PMODE.ALP == 0) || // Blend RC1 with non-zero alpha.
				(m_regs->PMODE.AMOD == 0) ||                             // Use alpha of RC1.
				(feedback_merge && m_regs->EXTBUF.FBIN == 0));           // Use RC1 for feedback merge.

		// The following two flags determine if RC1 output completely overwrites RC2 output
		// due to the alpha used for blending and the respective rectangles of the outputs.
		const bool rc1_contains_rc2 =
			PCRTCDisplays.PCRTCDisplays[0].displayRect.rcontains(PCRTCDisplays.PCRTCDisplays[1].displayRect);

		const bool rc1_overwrites_rc2 = use_rc1 && rc1_contains_rc2 && m_regs->PMODE.MMOD == 1 && m_regs->PMODE.ALP == 255;

		const bool use_rc2 =
			PCRTCDisplays.PCRTCDisplays[1].enabled &&                // RC2 enabled.
				((m_regs->PMODE.SLBG == 0 && !rc1_overwrites_rc2) || // Blending RC2 and not overwritten by RC1.
				(m_regs->PMODE.AMOD == 1) ||                         // Use alpha of RC2.
				(feedback_merge && m_regs->EXTBUF.FBIN == 1));       // Use RC2 for feedback merge.

		if (use_rc1)
			tex[0] = GetOutput(0, tex_scale[0], y_offset[0]);
		if (use_rc2)
			tex[1] = GetOutput(1, tex_scale[1], y_offset[1]);
		if (feedback_merge)
			tex[2] = GetFeedbackOutput(tex_scale[2]);
	}

	if (!tex[0] && !tex[1])
	{
		// Clear out the MAD buffer as some remnants of the previously shown frame came be left over, causing a flash for one frame.
		if (GSConfig.InterlaceMode == GSInterlaceMode::Automatic || GSConfig.InterlaceMode >= GSInterlaceMode::AdaptiveTFF)
		{
			GSTexture* mad_tex = g_gs_device->GetMAD();

			if (mad_tex)
			{
				g_gs_device->ClearRenderTarget(mad_tex, 0);
				mad_tex = nullptr;
			}
		}

		// Both circuits off still outputs BGCOLOR on real hardware.
		if (PCRTCDisplays.PCRTCDisplays[0].enabled || PCRTCDisplays.PCRTCDisplays[1].enabled)
		{
			// Orbis: bring-up - why is the frame blank?
			{
				static unsigned long long blanks = 0;
				unsigned long long n = blanks++;
				if (n < 3 || (n % 200) == 0)
				{
					printf("[gsmerge] blank #%llu en0=%d en1=%d fb0=%u fb1=%u\n", n,
						(int)PCRTCDisplays.PCRTCDisplays[0].enabled, (int)PCRTCDisplays.PCRTCDisplays[1].enabled,
						(unsigned)PCRTCDisplays.PCRTCDisplays[0].FBW, (unsigned)PCRTCDisplays.PCRTCDisplays[1].FBW);
					fflush(stdout);
				}
			}
			m_real_size = GSVector2i(0, 0);
			return false;
		}
	}

	s_n++;

	GSVector4 src_gs_read[2] = {};
	GSVector4 dst[3] = {};

	// Use offset for bob deinterlacing always, extra offset added later for FFMD mode.
	const bool scanmask_frame = m_scanmask_used && abs(PCRTCDisplays.PCRTCDisplays[0].displayRect.y - PCRTCDisplays.PCRTCDisplays[1].displayRect.y) != 1;
	int field2 = 0;
	int mode = 3; // If the game is manually deinterlacing then we need to bob (if we want to get away with no deinterlacing).
	bool is_bob = GSConfig.InterlaceMode == GSInterlaceMode::BobTFF || GSConfig.InterlaceMode == GSInterlaceMode::BobBFF;

	// FFMD (half frames) requires blend deinterlacing, so automatically use that. Same when SCANMSK is used but not blended in the merge circuit (Alpine Racer 3).
	if (GSConfig.InterlaceMode != GSInterlaceMode::Automatic || (!game_deinterlacing && !m_regs->SMODE2.FFMD && !scanmask_frame))
	{
		field2 = ((static_cast<int>(GSConfig.InterlaceMode) - 2) & 1);
		mode = ((static_cast<int>(GSConfig.InterlaceMode) - 2) >> 1);
	}

	for (int i = 0; i < 2; i++)
	{
		 const GSPCRTCRegs::PCRTCDisplay& curCircuit = PCRTCDisplays.PCRTCDisplays[i];

		if (!curCircuit.enabled || !tex[i])
			continue;

		const GSVector4 scale = GSVector4(tex_scale[i]);

		// dst is the final destination rect with offset on the screen.
		dst[i] = scale * GSVector4(curCircuit.displayRect);

		// src_gs_read is the size which we're really reading from GS memory.
		src_gs_read[i] = ((GSVector4(curCircuit.framebufferRect) + GSVector4(0, y_offset[i], 0, y_offset[i])) * scale) / GSVector4(tex[i]->GetSize()).xyxy();

		float interlace_offset = 0.0f;
		if (isReallyInterlaced() && m_regs->SMODE2.FFMD && !is_bob && !GSConfig.DisableInterlaceOffset && GSConfig.InterlaceMode != GSInterlaceMode::Off)
		{
			interlace_offset = (scale.y) * static_cast<float>(field ^ field2);
		}
		// Scanmask frame offsets. It's gross, I'm sorry but it sucks.
		if (m_scanmask_used)
		{
			int displayIntOffset = PCRTCDisplays.PCRTCDisplays[i].displayRect.y - PCRTCDisplays.PCRTCDisplays[1 - i].displayRect.y;

			if (displayIntOffset > 0)
			{
				displayIntOffset &= 1;
				dst[i].y -= displayIntOffset * scale.y;
				dst[i].w -= displayIntOffset * scale.y;
				interlace_offset += displayIntOffset;
			}
		}

		dst[i] += GSVector4(0.0f, interlace_offset, 0.0f, interlace_offset);
	}

	if (feedback_merge && tex[2])
	{
		const GSVector4 scale = GSVector4(tex_scale[2]);
		GSVector4i feedback_rect;

		feedback_rect.left = m_regs->EXTBUF.WDX;
		feedback_rect.right = feedback_rect.left + ((m_regs->EXTDATA.WW + 1) / ((m_regs->EXTDATA.SMPH - m_regs->DISP[m_regs->EXTBUF.FBIN].DISPLAY.MAGH) + 1));
		feedback_rect.top = m_regs->EXTBUF.WDY;
		feedback_rect.bottom = ((m_regs->EXTDATA.WH + 1) * (2 - m_regs->EXTBUF.WFFMD)) / ((m_regs->EXTDATA.SMPV - m_regs->DISP[m_regs->EXTBUF.FBIN].DISPLAY.MAGV) + 1);

		dst[2] = GSVector4(scale * GSVector4(feedback_rect.rsize()));
	}

	const GSVector2i resolution = PCRTCDisplays.GetResolution();
	fs = GSVector2i(static_cast<int>(static_cast<float>(resolution.x) * GetUpscaleMultiplier()),
		static_cast<int>(static_cast<float>(resolution.y) * GetUpscaleMultiplier()));
	{
		static unsigned long long mc = 0;
		if ((mc++ % 100) == 0)
		{
			printf("[dbg] merge[%llu]: res=%d,%d fs=%d,%d mult=%.2f\n", mc, resolution.x, resolution.y, fs.x, fs.y, GetUpscaleMultiplier());
			fflush(stdout);
		}
	}

	m_real_size = GSVector2i(fs.x, fs.y);

	if ((tex[0] || tex[1]) && (tex[0] == tex[1]) && (src_gs_read[0] == src_gs_read[1]).alltrue() && (dst[0] == dst[1]).alltrue() &&
		(PCRTCDisplays.PCRTCDisplays[0].displayRect == PCRTCDisplays.PCRTCDisplays[1].displayRect).alltrue() &&
		(PCRTCDisplays.PCRTCDisplays[0].framebufferRect == PCRTCDisplays.PCRTCDisplays[1].framebufferRect).alltrue() &&
		!feedback_merge && !m_regs->PMODE.SLBG)
	{
		// the two outputs are identical, skip drawing one of them (the one that is alpha blended)
		tex[0] = nullptr;
	}

	const u32 c = (m_regs->BGCOLOR.U32[0] & 0x00FFFFFFu) | (m_regs->PMODE.ALP << 24);
	g_gs_device->Merge(tex, src_gs_read, dst, fs, m_regs->PMODE, m_regs->EXTBUF, c);

	if ((tex[0] || tex[1]) && isReallyInterlaced() && GSConfig.InterlaceMode != GSInterlaceMode::Off)
	{
		const float offset = is_bob ? (tex[1] ? tex_scale[1] : tex_scale[0]) : 0.0f;

		g_gs_device->Interlace(fs, field ^ field2, mode, offset);
	}

	if (GSConfig.ShadeBoost)
		g_gs_device->ShadeBoost();

	if (GSConfig.FXAA)
		g_gs_device->FXAA();

	// Sharpens biinear at lower resolutions, almost nearest but with more uniform pixels.
	if (GSConfig.LinearPresent == GSPostBilinearMode::BilinearSharp && (g_gs_device->GetWindowWidth() > fs.x || g_gs_device->GetWindowHeight() > fs.y))
	{
		g_gs_device->Resize(g_gs_device->GetWindowWidth(), g_gs_device->GetWindowHeight());
	}

	if (m_scanmask_used)
		m_scanmask_used--;

	return true;
}

GSVector2i GSRenderer::GetInternalResolution()
{
	return m_real_size;
}

float GSRenderer::GetModXYOffset()
{
	if (GSConfig.UserHacks_HalfPixelOffset == GSHalfPixelOffset::Normal)
	{
		float mod_xy = GetUpscaleMultiplier();
		const int rounded_mod_xy = static_cast<int>(std::round(mod_xy));
		if (rounded_mod_xy > 1)
		{
			if (!(rounded_mod_xy & 1))
				return mod_xy += 0.2f;
			else if (!(rounded_mod_xy & 2))
				return mod_xy += 0.3f;
			else
				return mod_xy += 0.1f;
		}
	}

	return 0.0f;
}

static float GetCurrentAspectRatioFloat(bool is_progressive)
{
	switch (GSConfig.AspectRatio)
	{
		default:
		// We don't know the AR of the display here, nor we care about it
		case AspectRatioType::Stretch:
		case AspectRatioType::RAuto4_3_3_2:
			if (EmuConfig.CurrentCustomAspectRatio > 0.f)
				return EmuConfig.CurrentCustomAspectRatio;
			else if (is_progressive)
				return 3.0f / 2.0f;
			else
				return 4.0f / 3.0f;
		case AspectRatioType::R4_3:
			return 4.0f / 3.0f;
		case AspectRatioType::R16_9:
			return 16.0f / 9.0f;
		case AspectRatioType::R10_7:
			return 10.0f / 7.0f;
	}
}

static GSVector4 CalculateDrawDstRect(s32 window_width, s32 window_height, const GSVector4i& src_rect, const GSVector2i& src_size, GSDisplayAlignment alignment, bool flip_y, bool is_progressive)
{
	const float f_width = static_cast<float>(window_width);
	const float f_height = static_cast<float>(window_height);
	const float clientAr = f_width / f_height;

	float targetAr = clientAr;
	if (EmuConfig.CurrentAspectRatio == AspectRatioType::RAuto4_3_3_2)
	{
		if (is_progressive)
			targetAr = 3.0f / 2.0f;
		else
			targetAr = 4.0f / 3.0f;
		// Fall back on the custom aspect ratio set by patches (e.g. 16:9, 21:9)
		if (EmuConfig.CurrentCustomAspectRatio > 0.f)
			targetAr = EmuConfig.CurrentCustomAspectRatio;
	}
	else if (EmuConfig.CurrentAspectRatio == AspectRatioType::R4_3)
	{
		targetAr = 4.0f / 3.0f;
	}
	else if (EmuConfig.CurrentAspectRatio == AspectRatioType::R16_9)
	{
		targetAr = 16.0f / 9.0f;
	}
	else if (EmuConfig.CurrentAspectRatio == AspectRatioType::R10_7)
	{
		targetAr = 10.0f / 7.0f;
	}

	const float crop_adjust = (static_cast<float>(src_rect.width()) / static_cast<float>(src_size.x)) /
		(static_cast<float>(src_rect.height()) / static_cast<float>(src_size.y));

	const double arr = (targetAr * crop_adjust) / clientAr;
	float target_width = f_width;
	float target_height = f_height;
	if (arr < 1)
		target_width = std::floor(f_width * arr + 0.5f);
	else if (arr > 1)
		target_height = std::floor(f_height / arr + 0.5f);

	target_height *= GSConfig.StretchY / 100.0f;

	if (GSConfig.IntegerScaling)
	{
		// make target width/height an integer multiple of the texture width/height
		float t_width = static_cast<double>(src_rect.width());
		float t_height = static_cast<double>(src_rect.height());

		// If using Bilinear (Shape) the image will be prescaled to larger than the window, so we need to unscale it.
		if (GSConfig.LinearPresent == GSPostBilinearMode::BilinearSharp && src_rect.width() > 0 && src_rect.height() > 0)
		{
			const GSVector2i resolution = g_gs_renderer->PCRTCDisplays.GetResolution();
			const GSVector2i fs = GSVector2i(static_cast<int>(static_cast<float>(resolution.x) * g_gs_renderer->GetUpscaleMultiplier()),
				static_cast<int>(static_cast<float>(resolution.y) * g_gs_renderer->GetUpscaleMultiplier()));

			if (g_gs_device->GetWindowWidth() > fs.x || g_gs_device->GetWindowHeight() > fs.y)
			{
				t_width *= static_cast<float>(fs.x) / src_rect.width();
				t_height *= static_cast<float>(fs.y) / src_rect.height();
			}
		}

		float scale;
		if ((t_width / t_height) >= 1.0)
			scale = target_width / t_width;
		else
			scale = target_height / t_height;

		if (scale > 1.0)
		{
			const float adjust = std::floor(scale) / scale;
			target_width = target_width * adjust;
			target_height = target_height * adjust;
		}
	}

	float target_x, target_y;
	if (target_width >= f_width)
	{
		target_x = -((target_width - f_width) * 0.5f);
	}
	else
	{
		switch (alignment)
		{
			case GSDisplayAlignment::Center:
				target_x = (f_width - target_width) * 0.5f;
				break;
			case GSDisplayAlignment::RightOrBottom:
				target_x = (f_width - target_width);
				break;
			case GSDisplayAlignment::LeftOrTop:
			default:
				target_x = 0.0f;
				break;
		}
	}
	if (target_height >= f_height)
	{
		target_y = -((target_height - f_height) * 0.5f);
	}
	else
	{
		switch (alignment)
		{
			case GSDisplayAlignment::Center:
				target_y = (f_height - target_height) * 0.5f;
				break;
			case GSDisplayAlignment::RightOrBottom:
				target_y = (f_height - target_height);
				break;
			case GSDisplayAlignment::LeftOrTop:
			default:
				target_y = 0.0f;
				break;
		}
	}

	GSVector4 ret(target_x, target_y, target_x + target_width, target_y + target_height);

	if (flip_y)
	{
		const float height = ret.w - ret.y;
		ret.y = static_cast<float>(window_height) - ret.w;
		ret.w = ret.y + height;
	}

	return ret;
}

static GSVector4i CalculateDrawSrcRect(const GSTexture* src, const GSVector2i real_size)
{
	const GSVector2i size(src->GetSize());
	const GSVector2 scale = GSVector2(size.x, size.y) / GSVector2(real_size.x, real_size.y).max(GSVector2(0.1f, 0.1f));
	const float upscale = GSIsHardwareRenderer() ? GSConfig.UpscaleMultiplier : 1;
	const int left = static_cast<int>(static_cast<float>(GSConfig.Crop[0] * scale.x) * upscale);
	const int top = static_cast<int>(static_cast<float>(GSConfig.Crop[1] * scale.y) * upscale);
	const int right =  size.x - static_cast<int>(static_cast<float>(GSConfig.Crop[2] * scale.x) * upscale);
	const int bottom = size.y - static_cast<int>(static_cast<float>(GSConfig.Crop[3] * scale.y) * upscale);
	return GSVector4i(left, top, right, bottom);
}

static const char* GetScreenshotSuffix()
{
	static constexpr const char* suffixes[static_cast<u8>(GSScreenshotFormat::Count)] = {
		"png", "jpg", "webp"};
	return suffixes[static_cast<u8>(GSConfig.ScreenshotFormat)];
}

static void CompressAndWriteScreenshot(std::string filename, u32 width, u32 height, std::vector<u32> pixels)
{
	RGBA8Image image;
	image.SetPixels(width, height, std::move(pixels));

	std::string key(fmt::format("GSScreenshot_{}", filename));

	if (!GSDumpReplayer::IsRunner())
	{
		Host::AddIconOSDMessage(key, ICON_FA_CAMERA,
			fmt::format(TRANSLATE_FS("GS", "Saving screenshot to '{}'."), Path::GetFileName(filename)), 60.0f);
	}

	// maybe std::async would be better here.. but it's definitely worth threading, large screenshots take a while to compress.
	std::unique_lock lock(s_screenshot_threads_mutex);
	s_screenshot_threads.emplace_back([key = std::move(key), filename = std::move(filename), image = std::move(image),
										  quality = GSConfig.ScreenshotQuality]() {
		if (image.SaveToFile(filename.c_str(), quality))
		{
			if (!GSDumpReplayer::IsRunner())
			{
				Host::AddIconOSDMessage(std::move(key), ICON_FA_CAMERA,
					fmt::format(TRANSLATE_FS("GS", "Saved screenshot to '{}'."), Path::GetFileName(filename)),
					Host::OSD_INFO_DURATION);
			}
		}
		else
		{
			Host::AddIconOSDMessage(std::move(key), ICON_FA_CAMERA,
				fmt::format(TRANSLATE_FS("GS", "Failed to save screenshot to '{}'."), Path::GetFileName(filename),
					Host::OSD_ERROR_DURATION));
		}

		// remove ourselves from the list, if the GS thread is waiting for us, we won't be in there
		const auto this_id = std::this_thread::get_id();
		std::unique_lock lock(s_screenshot_threads_mutex);
		for (auto it = s_screenshot_threads.begin(); it != s_screenshot_threads.end(); ++it)
		{
			if (it->get_id() == this_id)
			{
				it->detach();
				s_screenshot_threads.erase(it);
				break;
			}
		}
	});
}

void GSJoinSnapshotThreads()
{
	std::unique_lock lock(s_screenshot_threads_mutex);
	while (!s_screenshot_threads.empty())
	{
		std::thread save_thread(std::move(s_screenshot_threads.front()));
		s_screenshot_threads.pop_front();
		lock.unlock();
		save_thread.join();
		lock.lock();
	}
}

bool GSRenderer::BeginPresentFrame(bool frame_skip)
{
	Host::BeginPresentFrame();

	const GSDevice::PresentResult res = g_gs_device->BeginPresent(frame_skip);
	if (res == GSDevice::PresentResult::FrameSkipped)
	{
		// If we're skipping a frame, we need to reset imgui's state, since
		// we won't be calling EndPresentFrame().
		ImGuiManager::SkipFrame();
		return false;
	}
	else if (res == GSDevice::PresentResult::OK)
	{
		// All good!
		return true;
	}

	// If we're constantly crashing on something in particular, we don't want to end up in an
	// endless reset loop.. that'd probably end up leaking memory and/or crashing us for other
	// reasons. So just abort in such case.
	const Common::Timer::Value current_time = Common::Timer::GetCurrentValue();
	if (s_last_gpu_reset_time != 0 &&
		Common::Timer::ConvertValueToSeconds(current_time - s_last_gpu_reset_time) < 15.0f)
	{
		pxFailRel("Host GPU lost too many times, device is probably completely wedged.");
	}
	s_last_gpu_reset_time = current_time;

	// Device lost, something went really bad.
	// Let's just toss out everything, and try to hobble on.
	if (!GSreopen(true, false, GSGetCurrentRenderer(), std::nullopt))
	{
		pxFailRel("Failed to recreate GS device after loss.");
		return false;
	}

	// First frame after reopening is definitely going to be trash, so skip it.
	Host::AddIconOSDMessage("GSDeviceLost", ICON_FA_TRIANGLE_EXCLAMATION,
		TRANSLATE_SV("GS", "Host GPU device encountered an error and was recovered. This may have broken rendering."),
		Host::OSD_CRITICAL_ERROR_DURATION);
	return false;
}

void GSRenderer::EndPresentFrame()
{
	if (GSDumpReplayer::IsReplayingDump())
		GSDumpReplayer::RenderUI();

	FullscreenUI::Render();
	ImGuiManager::RenderOSD();
	g_gs_device->EndPresent();
	ImGuiManager::NewFrame();
}

void GSRenderer::VSync(u32 field, bool registers_written, bool idle_frame)
{
	// Orbis: Null renderer has no GSDevice; skip presentation/merge entirely.
	if (!g_gs_device)
		return;

	OrbisLiveTune(); // eerec-278
	const auto orbis_vs_t0 = std::chrono::steady_clock::now();
	double orbis_merge_ms = 0.0;
	double orbis_present_ms = 0.0;

	if (GSConfig.ShouldDump(s_n, g_perfmon.GetFrame()))
	{
		if (GSConfig.SaveInfo)
		{
			DumpGSPrivRegs(*m_regs, GetDrawDumpPath("%05lld_f%05lld_vsync_gs_reg.txt", s_n, g_perfmon.GetFrame()));

			DumpDrawInfo(false, false, true);
		}

		if (GSConfig.SaveTransferImages)
			DumpTransferImages();

		if (GSConfig.SaveFrameStats)
		{
			m_perfmon_frame = g_perfmon - m_perfmon_frame;
			m_perfmon_frame.Dump(GetDrawDumpPath("%05lld_f%05lld_frame_stats.txt", s_n, g_perfmon.GetFrame()), GSIsHardwareRenderer());
			m_perfmon_frame = g_perfmon;
		}
	}

	const int fb_sprite_blits = g_perfmon.GetDisplayFramebufferSpriteBlits();
	const bool fb_sprite_frame = (fb_sprite_blits > 0);

	bool skip_frame = false;
	if (GSConfig.SkipDuplicateFrames && !GSCapture::IsCapturingVideo())
	{
		bool is_unique_frame;
		switch (PerformanceMetrics::GetInternalFPSMethod())
		{
		case PerformanceMetrics::InternalFPSMethod::GSPrivilegedRegister:
			is_unique_frame = registers_written;
			break;
		case PerformanceMetrics::InternalFPSMethod::DISPFBBlit:
			is_unique_frame = fb_sprite_frame;
			break;
		default:
			is_unique_frame = true;
			break;
		}

		if (!is_unique_frame && m_skipped_duplicate_frames < MAX_SKIPPED_DUPLICATE_FRAMES)
		{
			m_skipped_duplicate_frames++;
			skip_frame = true;
		}
		else
		{
			m_skipped_duplicate_frames = 0;
		}
	}

	const bool blank_frame = !Merge(field);
	{
		static unsigned s_d = 0;
		if (g_orbis_diag && (s_d++ % 250) == 7 && s_orbis_gl) // eerec-280
			OrbisDiagTexture("current", g_gs_device->GetCurrent());
	} // eerec-279
	orbis_merge_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - orbis_vs_t0).count();

	// Orbis: bring-up - which presentation branch is taken?
	{
		static unsigned long long vs = 0;
		unsigned long long n = vs++;
		if (n < 3 || (n % 100) == 0)
		{
			printf("[gsvs] #%llu skip=%d throttle_skip=%d blank=%d current=%p win=%dx%d\n", n,
				(int)skip_frame, (int)g_gs_device->ShouldSkipPresentingFrame(), (int)blank_frame,
				(void*)g_gs_device->GetCurrent(), (int)g_gs_device->GetWindowWidth(), (int)g_gs_device->GetWindowHeight());
			fflush(stdout);
		}
	}

	m_last_draw_n = s_n;
	m_last_transfer_n = s_transfer_n;

	// Skip presentation when running uncapped while vsync is on.
	if (skip_frame || g_gs_device->ShouldSkipPresentingFrame())
	{
		if (BeginPresentFrame(true))
			EndPresentFrame();

		PerformanceMetrics::Update(registers_written, fb_sprite_frame, skip_frame);
	}
	else
	{
		if (!idle_frame)
			g_gs_device->AgePool();

		g_perfmon.EndFrame(idle_frame);

		if ((g_perfmon.GetFrame() & 0x1f) == 0)
			g_perfmon.Update();

		// Little bit ugly, but we can't do CAS inside the render pass.
		GSVector4i src_rect;
		GSVector4 src_uv, draw_rect;
		GSTexture* current = g_gs_device->GetCurrent();
		if (current && !blank_frame)
		{
			src_rect = CalculateDrawSrcRect(current, m_real_size);
			src_uv = GSVector4(src_rect) / GSVector4(current->GetSize()).xyxy();
			draw_rect = CalculateDrawDstRect(g_gs_device->GetWindowWidth(), g_gs_device->GetWindowHeight(),
				src_rect, current->GetSize(), s_display_alignment, g_gs_device->UsesLowerLeftOrigin(),
				GetVideoMode() == GSVideoMode::SDTV_480P);
			s_last_draw_rect = draw_rect;

			if (GSConfig.CASMode != GSCASMode::Disabled)
			{
				static bool cas_log_once = false;
				if (g_gs_device->Features().cas_sharpening)
				{
					// sharpen only if the IR is higher than the display resolution
					const bool sharpen_only = (GSConfig.CASMode == GSCASMode::SharpenOnly ||
					                           (current->GetWidth() > g_gs_device->GetWindowWidth() &&
					                            current->GetHeight() > g_gs_device->GetWindowHeight()));
					g_gs_device->CAS(current, src_rect, src_uv, draw_rect, sharpen_only);
				}
				else if (!cas_log_once)
				{
					Host::AddIconOSDMessage("CASUnsupported", ICON_FA_TRIANGLE_EXCLAMATION,
						TRANSLATE_SV("GS", "CAS is not available, your graphics driver does not support the required functionality."),
						10.0f);
					cas_log_once = true;
				}
			}
		}

		if (BeginPresentFrame(false))
		{
			if (current && !blank_frame)
			{
				const u64 current_time = Common::Timer::GetCurrentValue();
				const float shader_time = static_cast<float>(Common::Timer::ConvertValueToSeconds(current_time - m_shader_time_start));

				{ static unsigned s_geo = 0; if ((s_geo++ % 600) == 0) { printf("[present] tex=%dx%d src=%.3f,%.3f,%.3f,%.3f dst=%.0f,%.0f,%.0f,%.0f\n", current->GetWidth(), current->GetHeight(), src_uv.x, src_uv.y, src_uv.z, src_uv.w, draw_rect.x, draw_rect.y, draw_rect.z, draw_rect.w); fflush(stdout); } } // eerec-278
				g_gs_device->PresentRect(current, src_uv, nullptr, draw_rect,
					s_tv_shader_indices[GSConfig.TVShader], shader_time, BilnIf(GSConfig.LinearPresent != GSPostBilinearMode::Off));
			}

			OrbisGLOSD(); // eerec-278
			if (s_orbis_gl && g_orbis_diag) // eerec-280
				OrbisSampleWindow(); // eerec-279 (every 120th call)
			EndPresentFrame();

			const float gpu_time = g_gs_device->GetAndResetAccumulatedGPUTime();
			GPUPipelineStatistics gpu_stats = g_gs_device->GetAndResetAccumulatedGPUPipelineStatistics();
			PerformanceMetrics::OnGPUPresent(gpu_time, gpu_stats.vs_invocations, gpu_stats.ps_invocations);
		}

		{
			orbis_present_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - orbis_vs_t0).count() - orbis_merge_ms;
			static unsigned long long orbis_vsn = 0;
			static double orbis_vs_sum = 0, orbis_vs_max = 0, orbis_mg_sum = 0, orbis_pr_sum = 0;
			const double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - orbis_vs_t0).count();
			orbis_vs_sum += total;
			orbis_mg_sum += orbis_merge_ms;
			orbis_pr_sum += orbis_present_ms;
			if (total > orbis_vs_max)
				orbis_vs_max = total;
			if ((++orbis_vsn % 50) == 0)
			{
				printf("[vstime] n=%llu vsync avg=%.1fms max=%.1fms merge_avg=%.1fms present_avg=%.1fms\n",
					orbis_vsn, orbis_vs_sum / 50.0, orbis_vs_max, orbis_mg_sum / 50.0, orbis_pr_sum / 50.0);
				fflush(stdout);
				orbis_vs_sum = orbis_mg_sum = orbis_pr_sum = 0;
				orbis_vs_max = 0;
			}
		}

		PerformanceMetrics::Update(registers_written, fb_sprite_frame, false);
	}

	// snapshot
	if (!m_snapshot.empty())
	{
		u32 screenshot_width, screenshot_height;
		std::vector<u32> screenshot_pixels;

		if (GSConfig.LinearPresent == GSPostBilinearMode::BilinearSharp)
		{
			const GSTexture* current = g_gs_device->GetCurrent();
			const GSVector2i internal_res = GetInternalResolution();

			if (current && (current->GetWidth() > internal_res.x || current->GetHeight() > internal_res.y))
				g_gs_device->Resize(internal_res.x, internal_res.y);
		}

		if (!m_dump && m_dump_frames > 0)
		{
			if (GSConfig.UserHacks_ReadTCOnClose)
				ReadbackTextureCache();

			freezeData fd = {0, nullptr};
			Freeze(&fd, true);
			fd.data = new u8[fd.size];
			Freeze(&fd, false);

			// keep the screenshot relatively small so we don't bloat the dump
			static constexpr u32 DUMP_SCREENSHOT_WIDTH = 640;
			static constexpr u32 DUMP_SCREENSHOT_HEIGHT = 480;
			SaveSnapshotToMemory(DUMP_SCREENSHOT_WIDTH, DUMP_SCREENSHOT_HEIGHT, true, false,
				&screenshot_width, &screenshot_height, &screenshot_pixels);

			std::string_view compression_str;
			if (GSConfig.GSDumpCompression == GSDumpCompressionMethod::Uncompressed)
			{
				m_dump = GSDumpBase::CreateUncompressedDump(m_snapshot, VMManager::GetDiscSerial(),
					VMManager::GetDiscCRC(), screenshot_width, screenshot_height,
					screenshot_pixels.empty() ? nullptr : screenshot_pixels.data(), fd, m_regs);
				compression_str = TRANSLATE_SV("GS", "with no compression");
			}
			else if (GSConfig.GSDumpCompression == GSDumpCompressionMethod::LZMA)
			{
				m_dump = GSDumpBase::CreateXzDump(m_snapshot, VMManager::GetDiscSerial(),
					VMManager::GetDiscCRC(), screenshot_width, screenshot_height,
					screenshot_pixels.empty() ? nullptr : screenshot_pixels.data(), fd, m_regs);
				compression_str = TRANSLATE_SV("GS", "with LZMA compression");
			}
			else
			{
				m_dump = GSDumpBase::CreateZstDump(m_snapshot, VMManager::GetDiscSerial(),
					VMManager::GetDiscCRC(), screenshot_width, screenshot_height,
					screenshot_pixels.empty() ? nullptr : screenshot_pixels.data(), fd, m_regs);
				compression_str = TRANSLATE_SV("GS", "with Zstandard compression");
			}

			delete[] fd.data;

			Host::AddKeyedOSDMessage("GSDump",
				fmt::format(TRANSLATE_FS("GS", "Saving {0} GS dump {1} to '{2}'"),
					(m_dump_frames == 1) ? TRANSLATE_SV("GS", "single frame") : TRANSLATE_SV("GS", "multi-frame"), compression_str,
					Path::GetFileName(m_dump->GetPath())),
				Host::OSD_INFO_DURATION);
		}

		const bool internal_resolution = (GSConfig.ScreenshotSize >= GSScreenshotSize::InternalResolution);
		const bool aspect_correct = (GSConfig.ScreenshotSize != GSScreenshotSize::InternalResolutionUncorrected);

		if (g_gs_device->GetCurrent() && SaveSnapshotToMemory(
			internal_resolution ? 0 : g_gs_device->GetWindowWidth(),
			internal_resolution ? 0 : g_gs_device->GetWindowHeight(),
			aspect_correct, true,
			&screenshot_width, &screenshot_height, &screenshot_pixels))
		{
			CompressAndWriteScreenshot(fmt::format("{}.{}", m_snapshot, GetScreenshotSuffix()),
				screenshot_width, screenshot_height, std::move(screenshot_pixels));
		}
		else
		{
			Host::AddIconOSDMessage("GSScreenshot", ICON_FA_CAMERA,
				TRANSLATE_SV("GS", "Failed to render/download screenshot."), Host::OSD_ERROR_DURATION);
		}

		m_snapshot = {};
	}
	else if (m_dump)
	{
		const bool last = (m_dump_frames == 0);
		if (m_dump->VSync(field, last, m_regs))
		{
			Host::AddKeyedOSDMessage("GSDump",
				fmt::format(TRANSLATE_FS("GS", "Saved GS dump to '{}'."), Path::GetFileName(m_dump->GetPath())),
				Host::OSD_INFO_DURATION);
			m_dump.reset();
		}
		else if (!last)
		{
			m_dump_frames--;
		}
	}

	// capture
	if (GSCapture::IsCapturingVideo())
	{
		const GSVector2i size = GSCapture::GetSize();
		if (GSTexture* current = g_gs_device->GetCurrent())
		{
			// TODO: Maybe avoid this copy in the future? We can use swscale to fix it up on the dumping thread..
			if (current->GetSize() != size)
			{
				GSTexture* temp = g_gs_device->CreateRenderTarget(size.x, size.y, GSTexture::Format::Color, false);
				if (temp)
				{
					g_gs_device->StretchRect(current, temp, GSVector4(0, 0, size.x, size.y), ShaderConvert::COPY, Biln);
					GSCapture::DeliverVideoFrame(temp);
					g_gs_device->Recycle(temp);
				}
			}
			else
			{
				GSCapture::DeliverVideoFrame(current);
			}
		}
		else
		{
			// Bit janky, but unless we want to make variable frame rate files, we need to deliver *a* frame to
			// the video file, so just grab a blank RT.
			GSTexture* temp = g_gs_device->CreateRenderTarget(size.x, size.y, GSTexture::Format::Color, true);
			if (temp)
			{
				GSCapture::DeliverVideoFrame(temp);
				g_gs_device->Recycle(temp);
			}
		}
	}

	if (GSConfig.ShouldDump(s_n, g_perfmon.GetFrame()) && GSConfig.SaveTransferImages)
		DumpTransferImages();
}

void GSRenderer::QueueSnapshot(const std::string& path, const u32 gsdump_frames)
{
	if (!m_snapshot.empty())
		return;

	// Allows for providing a complete path
	if (path.size() > 4 && StringUtil::EndsWithNoCase(path, ".png"))
		m_snapshot = path.substr(0, path.size() - 4);
	else
		m_snapshot = GSGetBaseSnapshotFilename();

	// this is really gross, but wx we get the snapshot request after shift...
	m_dump_frames = gsdump_frames;
}

static std::string GSGetBaseFilename()
{
	std::string filename;

	// append the game serial and title
	if (std::string name(VMManager::GetTitle(true)); !name.empty())
	{
		Path::SanitizeFileName(&name);
		if (name.length() > 219)
			name.resize(219);
		filename += name;
	}
	if (std::string serial = VMManager::GetDiscSerial(); !serial.empty())
	{
		Path::SanitizeFileName(&serial);
		filename += '_';
		filename += serial;
	}

	const time_t cur_time = time(nullptr);
	char local_time[16];

	if (strftime(local_time, sizeof(local_time), "%Y%m%d%H%M%S", localtime(&cur_time)))
	{
		static time_t prev_snap;
		// The variable 'n' is used for labelling the screenshots when multiple screenshots are taken in
		// a single second, we'll start using this variable for naming when a second screenshot request is detected
		// at the same time as the first one. Hence, we're initially setting this counter to 2 to imply that
		// the captured image is the 2nd image captured at this specific time.
		static int n = 2;

		filename += '_';

		if (cur_time == prev_snap)
			filename += fmt::format("{0}_({1})", local_time, n++);
		else
		{
			n = 2;
			filename += fmt::format("{}", local_time);
		}
		prev_snap = cur_time;
	}

	return filename;
}

std::string GSGetBaseSnapshotFilename()
{
	// If organize by game is enabled, use or create a game-specific folder.
	if (GSConfig.OrganizeSnapshotsByGame)
	{
		const bool prefer_english = Host::GetBaseBoolSettingValue("UI", "PreferEnglishGameList", false);
		std::string game_name = VMManager::GetTitle(prefer_english);
		if (!game_name.empty())
		{
			Path::SanitizeFileName(&game_name);
			const std::string game_dir = Path::Combine(EmuFolders::Snapshots, game_name);

			// Make sure the per-game directory exists or that we can successfully create it.
			if (FileSystem::DirectoryExists(game_dir.c_str()) || FileSystem::CreateDirectoryPath(game_dir.c_str(), false))
				return Path::Combine(game_dir, GSGetBaseFilename());
		}
	}

	return Path::Combine(EmuFolders::Snapshots, GSGetBaseFilename());
}

std::string GSGetBaseVideoFilename()
{
	// If organize by game is enabled, use or create a game-specific folder.
	if (GSConfig.OrganizeVideoCaptureByGame)
	{
		const bool prefer_english = Host::GetBaseBoolSettingValue("UI", "PreferEnglishGameList", false);
		std::string game_name = VMManager::GetTitle(prefer_english);
		if (!game_name.empty())
		{
			Path::SanitizeFileName(&game_name);
			const std::string game_dir = Path::Combine(EmuFolders::Videos, game_name);

			// Make sure the per-game directory exists or that we can successfully create it.
			if (FileSystem::DirectoryExists(game_dir.c_str()) || FileSystem::CreateDirectoryPath(game_dir.c_str(), false))
				return Path::Combine(game_dir, GSGetBaseFilename());
		}
	}
	// prepend video directory
	return Path::Combine(EmuFolders::Videos, GSGetBaseFilename());
}

void GSRenderer::StopGSDump()
{
	m_snapshot = {};
	m_dump_frames = 0;
}

void GSRenderer::PresentCurrentFrame()
{
	if (BeginPresentFrame(false))
	{
		GSTexture* current = g_gs_device->GetCurrent();
		if (current)
		{
			const GSVector4i src_rect(CalculateDrawSrcRect(current, m_real_size));
			const GSVector4 src_uv(GSVector4(src_rect) / GSVector4(current->GetSize()).xyxy());
			const GSVector4 draw_rect(CalculateDrawDstRect(g_gs_device->GetWindowWidth(), g_gs_device->GetWindowHeight(),
				src_rect, current->GetSize(), s_display_alignment, g_gs_device->UsesLowerLeftOrigin(),
				GetVideoMode() == GSVideoMode::SDTV_480P));
			s_last_draw_rect = draw_rect;

			const u64 current_time = Common::Timer::GetCurrentValue();
			const float shader_time = static_cast<float>(Common::Timer::ConvertValueToSeconds(current_time - m_shader_time_start));

			g_gs_device->PresentRect(current, src_uv, nullptr, draw_rect,
				s_tv_shader_indices[GSConfig.TVShader], shader_time, BilnIf(GSConfig.LinearPresent != GSPostBilinearMode::Off));
		}
			EndPresentFrame();

			// Orbis: periodic GL/GS readback dump (bring-up diagnosis).
			{
				static unsigned long long orbis_rb = 0;
				if (g_orbis_diag && (orbis_rb++ % 120) == 0) // eerec-280
				{
					OrbisPresentGLFrame();
					OrbisSampleWindow();
				}
			}

	}
}

void GSTranslateWindowToDisplayCoordinates(float window_x, float window_y, float* display_x, float* display_y)
{
	const float draw_width = s_last_draw_rect.z - s_last_draw_rect.x;
	const float draw_height = s_last_draw_rect.w - s_last_draw_rect.y;
	const float rel_x = window_x - s_last_draw_rect.x;
	const float rel_y = window_y - s_last_draw_rect.y;
	if (rel_x < 0 || rel_x > draw_width || rel_y < 0 || rel_y > draw_height)
	{
		*display_x = -1.0f;
		*display_y = -1.0f;
		return;
	}

	*display_x = rel_x / draw_width;
	*display_y = rel_y / draw_height;
}

void GSSetDisplayAlignment(GSDisplayAlignment alignment)
{
	s_display_alignment = alignment;
}

bool GSRenderer::BeginCapture(std::string filename, const GSVector2i& size)
{
	const GSVector2i capture_resolution = (size.x != 0 && size.y != 0) ?
											  size :
											  (GSConfig.VideoCaptureAutoResolution ?
													  GetInternalResolution() :
													  GSVector2i(GSConfig.VideoCaptureWidth, GSConfig.VideoCaptureHeight));

	return GSCapture::BeginCapture(GetTvRefreshRate(), capture_resolution,
		GetCurrentAspectRatioFloat(GetVideoMode() == GSVideoMode::SDTV_480P),
		std::move(filename));
}

void GSRenderer::EndCapture()
{
	GSCapture::EndCapture();
}

GSTexture* GSRenderer::LookupPaletteSource(u32 CBP, u32 CPSM, u32 CBW, GSVector2i& offset, float* scale, const GSVector2i& size)
{
	return nullptr;
}

bool GSRenderer::IsIdleFrame() const
{
	return (m_last_draw_n == s_n && m_last_transfer_n == s_transfer_n);
}

bool GSRenderer::SaveSnapshotToMemory(u32 window_width, u32 window_height, bool apply_aspect, bool crop_borders,
	u32* width, u32* height, std::vector<u32>* pixels)
{
	GSTexture* const current = g_gs_device->GetCurrent();
	if (!current)
	{
		*width = 0;
		*height = 0;
		pixels->clear();
		return false;
	}

	const GSVector4i src_rect(CalculateDrawSrcRect(current, m_real_size));
	const GSVector4 src_uv(GSVector4(src_rect) / GSVector4(current->GetSize()).xyxy());

	const bool is_progressive = (GetVideoMode() == GSVideoMode::SDTV_480P);
	GSVector4 draw_rect;
	if (window_width == 0 || window_height == 0)
	{
		if (apply_aspect)
		{
			// use internal resolution of the texture
			const float aspect = GetCurrentAspectRatioFloat(is_progressive);
			const int tex_width = current->GetWidth();
			const int tex_height = current->GetHeight();

			// expand to the larger dimension
			const float tex_aspect = static_cast<float>(tex_width) / static_cast<float>(tex_height);
			if (tex_aspect >= aspect)
				draw_rect = GSVector4(0.0f, 0.0f, static_cast<float>(tex_width), static_cast<float>(tex_width) / aspect);
			else
				draw_rect = GSVector4(0.0f, 0.0f, static_cast<float>(tex_height) * aspect, static_cast<float>(tex_height));
		}
		else
		{
			// uncorrected aspect is only available at internal resolution
			draw_rect = GSVector4(0.0f, 0.0f, static_cast<float>(current->GetWidth()), static_cast<float>(current->GetHeight()));
		}
	}
	else
	{
		draw_rect = CalculateDrawDstRect(window_width, window_height, src_rect, current->GetSize(),
			GSDisplayAlignment::LeftOrTop, false, is_progressive);
	}
	const u32 draw_width = static_cast<u32>(draw_rect.z - draw_rect.x);
	const u32 draw_height = static_cast<u32>(draw_rect.w - draw_rect.y);
	const u32 image_width = crop_borders ? draw_width : std::max(draw_width, window_width);
	const u32 image_height = crop_borders ? draw_height : std::max(draw_height, window_height);

	// We're not expecting screenshots to be fast, so just allocate a download texture on demand.
	GSTexture* rt = g_gs_device->CreateRenderTarget(draw_width, draw_height, GSTexture::Format::Color, false);
	if (rt)
	{
		std::unique_ptr<GSDownloadTexture> dl(g_gs_device->CreateDownloadTexture(draw_width, draw_height, GSTexture::Format::Color));
		if (dl)
		{
			const GSVector4i rc(0, 0, draw_width, draw_height);
			g_gs_device->StretchRect(current, src_uv, rt, GSVector4(rc), ShaderConvert::TRANSPARENCY_FILTER, Biln);
			dl->CopyFromTexture(rc, rt, rc, 0);
			dl->Flush();

			if (dl->Map(rc))
			{
				const u32 pad_x = (image_width - draw_width) / 2;
				const u32 pad_y = (image_height - draw_height) / 2;
				pixels->clear();
				pixels->resize(image_width * image_height, 0);
				*width = image_width;
				*height = image_height;
				StringUtil::StrideMemCpy(pixels->data() + pad_y * image_width + pad_x, image_width * sizeof(u32), dl->GetMapPointer(),
					dl->GetMapPitch(), draw_width * sizeof(u32), draw_height);

				g_gs_device->Recycle(rt);
				return true;
			}
		}

		g_gs_device->Recycle(rt);
	}

	*width = 0;
	*height = 0;
	pixels->clear();
	return false;
}

void DumpGSPrivRegs(const GSPrivRegSet& r, const std::string& filename)
{
	auto fp = FileSystem::OpenManagedCFile(filename.c_str(), "wt");
	if (!fp)
		return;

	for (int i = 0; i < 2; i++)
	{
		if (i == 0 && !r.PMODE.EN1)
			continue;
		if (i == 1 && !r.PMODE.EN2)
			continue;

		std::fprintf(fp.get(), "DISPFB%d: { BP: 0x%05x, BW: %u, PSM: %u, DBX: %u, DBY: %u }\n",
			i,
			r.DISP[i].DISPFB.Block(),
			r.DISP[i].DISPFB.FBW,
			r.DISP[i].DISPFB.PSM,
			r.DISP[i].DISPFB.DBX,
			r.DISP[i].DISPFB.DBY);

		std::fprintf(fp.get(), "DISPLAY%d: { DX: %u, DY: %u, DW: %u, DH: %u, MAGH: %u, MAGV: %u }\n",
			i,
			r.DISP[i].DISPLAY.DX,
			r.DISP[i].DISPLAY.DY,
			r.DISP[i].DISPLAY.DW,
			r.DISP[i].DISPLAY.DH,
			r.DISP[i].DISPLAY.MAGH,
			r.DISP[i].DISPLAY.MAGV);
	}

	std::fprintf(fp.get(), "PMODE: { EN1: %u, EN2: %u, CRTMD: %u, MMOD: %u, AMOD: %u, SLBG: %u, ALP: %u }\n",
		r.PMODE.EN1,
		r.PMODE.EN2,
		r.PMODE.CRTMD,
		r.PMODE.MMOD,
		r.PMODE.AMOD,
		r.PMODE.SLBG,
		r.PMODE.ALP);

	std::fprintf(fp.get(),
		"SMODE1: { CLKSEL: %u, CMOD: %u, EX: %u, GCONT: %u, LC: %u, NVCK: %u, PCK2: %u, PEHS: %u, PEVS: %u, PHS: %u, PRST: %u, PVS: %u, RC: %u, SINT: %u, SLCK: %u, SLCK2: %u, SPML: %u, T1248: %u, VCKSEL: %u, VHP: %u, XPCK: %u }\n",
		r.SMODE1.CLKSEL,
		r.SMODE1.CMOD,
		r.SMODE1.EX,
		r.SMODE1.GCONT,
		r.SMODE1.LC,
		r.SMODE1.NVCK,
		r.SMODE1.PCK2,
		r.SMODE1.PEHS,
		r.SMODE1.PEVS,
		r.SMODE1.PHS,
		r.SMODE1.PRST,
		r.SMODE1.PVS,
		r.SMODE1.RC,
		r.SMODE1.SINT,
		r.SMODE1.SLCK,
		r.SMODE1.SLCK2,
		r.SMODE1.SPML,
		r.SMODE1.T1248,
		r.SMODE1.VCKSEL,
		r.SMODE1.VHP,
		r.SMODE1.XPCK);

	std::fprintf(fp.get(), "SMODE2: { INT: %u, FFMD: %u, DPMS: %u }\n",
		r.SMODE2.INT,
		r.SMODE2.FFMD,
		r.SMODE2.DPMS);

	std::fprintf(fp.get(), "SRFSH: { U32_0: 0x%08x, U32_1: 0x%08x }\n",
		r.SRFSH.U32[0],
		r.SRFSH.U32[1]);

	std::fprintf(fp.get(), "SYNCH1: { U32_0: 0x%08x, U32_1: 0x%08x }\n",
		r.SYNCH1.U32[0],
		r.SYNCH1.U32[1]);

	std::fprintf(fp.get(), "SYNCH2: { U32_0: 0x%08x, U32_1: 0x%08x }\n",
		r.SYNCH2.U32[0],
		r.SYNCH2.U32[1]);

	std::fprintf(fp.get(), "SYNCV: { VBP: %u, VBPE: %u, VDP: %u, VFP: %u, VFPE: %u, VS: %u }\n",
		r.SYNCV.VBP,
		r.SYNCV.VBPE,
		r.SYNCV.VDP,
		r.SYNCV.VFP,
		r.SYNCV.VFPE,
		r.SYNCV.VS);

	std::fprintf(fp.get(), "CSR: { U32_0: 0x%08x, U32_1: 0x%08x }\n",
		r.CSR.U32[0],
		r.CSR.U32[1]);

	std::fprintf(fp.get(), "BGCOLOR: { B: %u, G: %u, R: %u }\n",
		r.BGCOLOR.B,
		r.BGCOLOR.G,
		r.BGCOLOR.R);

	std::fprintf(fp.get(), "EXTBUF: { BP: 0x%05x, BW: %u, FBIN: %u, WFFMD: %u, EMODA: %u, EMODC: %u, WDX: %u, WDY: %u }\n",
		r.EXTBUF.EXBP, r.EXTBUF.EXBW, r.EXTBUF.FBIN, r.EXTBUF.WFFMD,
		r.EXTBUF.EMODA, r.EXTBUF.EMODC, r.EXTBUF.WDX, r.EXTBUF.WDY);

	std::fprintf(fp.get(), "EXTDATA: { SX: %u, SY: %u, SMPH: %u, SMPV: %u, WW: %u, WH: %u }\n",
		r.EXTDATA.SX, r.EXTDATA.SY, r.EXTDATA.SMPH, r.EXTDATA.SMPV, r.EXTDATA.WW, r.EXTDATA.WH);

	std::fprintf(fp.get(), "EXTWRITE: { EN: %u }\n", r.EXTWRITE.WRITE);
}
