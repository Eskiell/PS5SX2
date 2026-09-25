// PS5 port frontend: the console side. The shelf runs on its own Vulkan device (the driver linked
// into the eboot) with a VK_KHR_display swapchain on VideoOut, reads the DualSense, plays its key
// sounds on an audio port of its own, and downloads missing covers over HTTPS with the console's
// own libSceHttp2. Everything is torn down again before PCSX2 opens its device, VideoOut and audio.
//
// Copyright (C) 2026 Spyros
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_ps5.h"

#include "fe_app.h"
#include "fe_covers.h"
#include "fe_games.h"
#include "fe_renderer.h"
#include "fe_sound.h"
#include "fe_text.h"
#include "fe_vk.h"
#include "fe_web.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/time.h>
#include <thread>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

// What sceKernelConvertUtcToLocaltime fills in: 16 bytes (as the open-source KytyPS5 emulator
// implements it), not a struct timezone. vk-285-40 passed an 8-byte struct timezone and an int for
// the DST offset; the call wrote the zone offsets (7200, 3600) over the next stack slot, which held
// the swapchain pointer, and the first vkQueuePresentKHR faulted on it.
struct KernelTimesec
{
	int64_t t;
	uint32_t west_sec; // seconds east of UTC, despite the name
	uint32_t dst_sec;
};

extern "C" {
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName);
int scePadInit(void);
int scePadOpen(int32_t userId, int32_t type, int32_t index, const void* param);
int scePadClose(int32_t handle);
int scePadReadState(int32_t handle, void* data);
int sceUserServiceInitialize(const void* params);
int sceUserServiceGetInitialUser(int32_t* userId);
int sceKernelConvertUtcToLocaltime(time_t utc, time_t* local, KernelTimesec* sec, uint64_t* dst_sec);
int sceSystemServiceParamGetInt(int param, int* value);
int sceSystemServiceHideSplashScreen(void);

// libSceNet, libSceSsl and libSceHttp2 as the payload SDK's http2_get sample declares them. The
// system loads all three into every app already, so linking them adds nothing at start-up.
int sceNetInit(void);
int sceNetPoolCreate(const char* name, int size, int flags);
int sceNetPoolDestroy(int pool);
int sceSslInit(size_t pool_size);
int sceSslTerm(int ctx);
int sceHttp2Init(int net_pool, int ssl_ctx, size_t pool_size, int max_requests);
int sceHttp2Term(int ctx);
int sceHttp2CreateTemplate(int ctx, const char* user_agent, int http_version, int auto_proxy);
int sceHttp2DeleteTemplate(int tmpl);
int sceHttp2CreateRequestWithURL(int tmpl, const char* method, const char* url, uint64_t content_length);
int sceHttp2DeleteRequest(int req);
int sceHttp2SendRequest(int req, const void* data, size_t size);
int sceHttp2GetStatusCode(int req, int* status);
int sceHttp2ReadData(int req, void* data, size_t size);
int sceHttp2SetResolveTimeOut(int id, uint32_t usec);
int sceHttp2SetConnectTimeOut(int id, uint32_t usec);
int sceHttp2SetSendTimeOut(int id, uint32_t usec);
int sceHttp2SetRecvTimeOut(int id, uint32_t usec);
int sceHttp2AbortRequest(int req);
int sceHttp2SetTimeOut(int id, uint32_t usec);
int sceHttp2SetAutoRedirect(int id, int enable);
int sceNetCtlInit(void);
void sceNetCtlTerm(void);
int sceNetCtlGetState(int* state);

// libSceAudioOut, as orbis-shims/ProsperoAudio.cpp declares it for PCSX2's own output.
int sceAudioOutInit(void);
int sceAudioOutOpen(int userId, int type, int index, unsigned int len, unsigned int freq, unsigned int param);
int sceAudioOutOutput(int handle, const void* p);
int sceAudioOutClose(int handle);
}

// The fonts, embedded: PCSX2's Roboto (Apache-2.0) and PromptFont (OFL-1.1), from its resources.
#ifndef FE_FONT_DIR
#error "FE_FONT_DIR must name PCSX2's bin/resources/fonts"
#endif
#define FE_INCBIN(sym, file) \
	__asm__(".section .rodata." #sym ",\"a\",@progbits\n" \
			".balign 16\n" \
			".global " #sym "\n" #sym ":\n" \
			".incbin \"" FE_FONT_DIR "/" file "\"\n" \
			".global " #sym "_end\n" #sym "_end:\n" \
			".byte 0\n" \
			".previous\n")
FE_INCBIN(fe_font_text, "Roboto-Regular.ttf");
FE_INCBIN(fe_font_icons, "promptfont.otf");
extern "C" const uint8_t fe_font_text[], fe_font_text_end[], fe_font_icons[], fe_font_icons_end[];

// vk-285-50: the frontend's own files (frontend/assets): Font Awesome Free's brands font (SIL OFL
// 1.1) for the handles' icons, and the settings page with its two icons (Font Awesome, CC BY 4.0).
#ifndef FE_ASSET_DIR
#error "FE_ASSET_DIR must name frontend/assets"
#endif
#define FE_INCBIN_ASSET(sym, file) \
	__asm__(".section .rodata." #sym ",\"a\",@progbits\n" \
			".balign 16\n" \
			".global " #sym "\n" #sym ":\n" \
			".incbin \"" FE_ASSET_DIR "/" file "\"\n" \
			".global " #sym "_end\n" #sym "_end:\n" \
			".byte 0\n" \
			".previous\n")
FE_INCBIN_ASSET(fe_font_brands, "fonts/fa-brands-400.otf");
FE_INCBIN_ASSET(fe_web_page, "web/index.html");
FE_INCBIN_ASSET(fe_web_discord, "web/discord.svg");
FE_INCBIN_ASSET(fe_web_x, "web/x-twitter.svg");
// vk-285-51: the recommended settings the page's Recommended button restores.
FE_INCBIN_ASSET(fe_presets, "presets.ini");
extern "C" const uint8_t fe_font_brands[], fe_font_brands_end[], fe_web_page[], fe_web_page_end[], fe_web_discord[],
	fe_web_discord_end[], fe_web_x[], fe_web_x_end[], fe_presets[], fe_presets_end[];

namespace
{
using namespace fe;

// xlenore/ps2-covers, the default set: <serial>.jpg, 512x736 (the same source as the user's Twiso).
constexpr const char* kCoverUrl = "https://raw.githubusercontent.com/xlenore/ps2-covers/main/covers/default/${serial}.jpg";

// The key sounds' output: a port on the main output, float stereo at 48 kHz in 256-frame grains,
// opened the way PCSX2's own output opens it once the game runs (system user, main port, format 4),
// and a thread that mixes whatever is playing into each grain. sceAudioOutOutput blocks until the
// previous grain has been consumed, which paces the thread.
class AudioOut
{
public:
	bool Start(Mixer* mixer)
	{
		const int init = sceAudioOutInit(); // PCSX2's output calls it again later and ignores the error
		m_handle = sceAudioOutOpen(255, 0, 0, kGrain, SoundBank::kRate, 4);
		std::printf("[frontend] sound: sceAudioOutInit %x, port %x\n", static_cast<unsigned>(init), static_cast<unsigned>(m_handle));
		if (m_handle < 0)
			return false;
		m_mixer = mixer;
		m_thread = std::thread([this]() { Run(); });
		return true;
	}

	void Stop()
	{
		m_quit.store(true);
		if (m_thread.joinable())
			m_thread.join();
		if (m_handle >= 0)
			sceAudioOutClose(m_handle);
		m_handle = -1;
	}

private:
	static constexpr unsigned kGrain = 256;

	void Run()
	{
		alignas(64) float buf[kGrain * 2];
		while (!m_quit.load(std::memory_order_relaxed))
		{
			m_mixer->Mix(buf, static_cast<int>(kGrain));
			sceAudioOutOutput(m_handle, buf);
		}
	}

	Mixer* m_mixer = nullptr;
	int m_handle = -1;
	std::atomic<bool> m_quit{false};
	std::thread m_thread;
};

// libScePad's state, as game_select.cpp and main-boot.cpp read it.
struct PadData
{
	uint32_t buttons;
	uint8_t lx, ly, rx, ry, l2, r2, pad0, pad1;
	uint8_t rest[256];
};
constexpr uint32_t kPadRight = 0x20, kPadLeft = 0x80, kPadCross = 0x4000, kPadOptions = 0x8, kPadL1 = 0x400,
				   kPadR1 = 0x800;

double Now()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<double>(ts.tv_sec) + ts.tv_nsec * 1e-9;
}

// The time of day in the console's time zone and clock format.
std::string Clock()
{
	const time_t utc = time(nullptr);
	time_t local = utc;
	// Room to spare after both outputs, so a firmware that writes more cannot reach our locals.
	KernelTimesec sec[2] = {};
	uint64_t dst[2] = {};
	if (sceKernelConvertUtcToLocaltime(utc, &local, &sec[0], &dst[0]) != 0)
		local = utc;
	struct tm tm = {};
	gmtime_r(&local, &tm);
	static int s_format = -1;
	if (s_format < 0)
	{
		int v = 1;
		s_format = sceSystemServiceParamGetInt(3 /* time format */, &v) == 0 ? v : 1;
	}
	char buf[16];
	if (s_format == 0) // 12-hour
		std::snprintf(buf, sizeof(buf), "%d:%02d %s", (tm.tm_hour + 11) % 12 + 1, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
	else
		std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
	return buf;
}

// ---- HTTPS through libSceHttp2, on the cover worker's thread only ----
struct Http
{
	bool tried = false, ok = false, netctl = false;
	int pool = -1, ssl = -1, ctx = -1, tmpl = -1;
	std::atomic<int> active{-1}; // the request in flight, for Abort from the main thread

	bool Init()
	{
		if (tried)
			return ok;
		tried = true;
		// Is there a network at all? vk-285-41's first download never came back (100+ s): an
		// offline console must not wait on the resolver. State 3 is "IP address obtained"; a state
		// the call cannot tell leaves the decision to the request itself.
		const int nc = sceNetCtlInit();
		netctl = nc == 0;
		int state[4] = {-1, 0, 0, 0}; // room to spare, as with the time call
		const int gs = sceNetCtlGetState(state);
		std::printf("[frontend] netctl: init %#x, state %d (%#x)\n", static_cast<unsigned>(nc), state[0],
			static_cast<unsigned>(gs));
		std::fflush(stdout);
		if (gs == 0 && state[0] >= 0 && state[0] < 3)
		{
			std::printf("[frontend] the console is not connected; no cover downloads this time\n");
			std::fflush(stdout);
			return false;
		}
		const int net = sceNetInit(); // an error here only means it was up already
		pool = sceNetPoolCreate("pcsx2-frontend", 64 * 1024, 0);
		ssl = pool >= 0 ? sceSslInit(256 * 1024) : -1;
		ctx = ssl >= 0 ? sceHttp2Init(pool, ssl, 256 * 1024, 1) : -1;
		tmpl = ctx >= 0 ? sceHttp2CreateTemplate(ctx, "PS5SX2/1.0", 3, 1) : -1;
		std::printf("[frontend] https: net %#x pool %#x ssl %#x http2 %#x template %#x\n", static_cast<unsigned>(net),
			static_cast<unsigned>(pool), static_cast<unsigned>(ssl), static_cast<unsigned>(ctx), static_cast<unsigned>(tmpl));
		std::fflush(stdout);
		if (tmpl < 0)
			return false;
		ok = true;
		return true;
	}

	// Main thread, at shutdown: fails the request in flight so the worker can finish.
	void Abort()
	{
		const int req = active.load();
		if (req >= 0)
		{
			const int r = sceHttp2AbortRequest(req);
			std::printf("[frontend] aborted request %#x: %#x\n", static_cast<unsigned>(req), static_cast<unsigned>(r));
			std::fflush(stdout);
		}
	}

	// After the worker has stopped: gives the pools back.
	void Term()
	{
		if (tmpl >= 0)
			sceHttp2DeleteTemplate(tmpl);
		if (ctx >= 0)
			sceHttp2Term(ctx);
		if (ssl >= 0)
			sceSslTerm(ssl);
		if (pool >= 0)
			sceNetPoolDestroy(pool);
		if (netctl)
			sceNetCtlTerm();
		tmpl = ctx = ssl = pool = -1;
		tried = ok = netctl = false;
	}

	// The HTTP status, or -1 when the request could not be made at all. Every step is logged with
	// its time: vk-285-41's first request never returned, and the log could not say where.
	int Get(const std::string& url, std::vector<uint8_t>& out)
	{
		if (!Init())
			return -1;
		const double t0 = Now();
		auto ms = [&] { return (Now() - t0) * 1000.0; };
		const int req = sceHttp2CreateRequestWithURL(tmpl, "GET", url.c_str(), 0);
		std::printf("[frontend] get %s: request %#x\n", url.c_str(), static_cast<unsigned>(req));
		std::fflush(stdout);
		if (req < 0)
			return -1;
		// On the request, as Swordpdf/Twiso's working cover fetcher sets them (microseconds): each
		// phase 10 s, the whole request 20 s, redirects followed.
		const int t_resolve = sceHttp2SetResolveTimeOut(req, 10 * 1000 * 1000);
		const int t_connect = sceHttp2SetConnectTimeOut(req, 10 * 1000 * 1000);
		const int t_send = sceHttp2SetSendTimeOut(req, 10 * 1000 * 1000);
		const int t_recv = sceHttp2SetRecvTimeOut(req, 10 * 1000 * 1000);
		const int t_total = sceHttp2SetTimeOut(req, 20 * 1000 * 1000);
		const int redirect = sceHttp2SetAutoRedirect(req, 1);
		std::printf("[frontend] get: timeouts %#x %#x %#x %#x %#x, redirect %#x\n", static_cast<unsigned>(t_resolve),
			static_cast<unsigned>(t_connect), static_cast<unsigned>(t_send), static_cast<unsigned>(t_recv),
			static_cast<unsigned>(t_total), static_cast<unsigned>(redirect));
		std::fflush(stdout);
		active = req;
		int status = -1;
		const int sent = sceHttp2SendRequest(req, nullptr, 0);
		const int got = sent == 0 ? sceHttp2GetStatusCode(req, &status) : -1;
		std::printf("[frontend] get: send %#x, status %d (%#x) after %.0f ms\n", static_cast<unsigned>(sent), status,
			static_cast<unsigned>(got), ms());
		std::fflush(stdout);
		if (sent != 0 || got != 0)
			status = -1;
		else if (status == 200)
		{
			std::vector<uint8_t> buf(64 * 1024);
			for (;;)
			{
				const int n = sceHttp2ReadData(req, buf.data(), buf.size());
				if (n < 0)
				{
					std::printf("[frontend] get: read %#x after %zu bytes\n", static_cast<unsigned>(n), out.size());
					status = -1;
					break;
				}
				if (n == 0)
					break;
				out.insert(out.end(), buf.begin(), buf.begin() + n);
				if (out.size() > (16u << 20))
				{
					status = -1;
					break;
				}
			}
			std::printf("[frontend] get: %zu bytes in %.0f ms\n", out.size(), ms());
			std::fflush(stdout);
		}
		active = -1;
		sceHttp2DeleteRequest(req);
		return status;
	}
};
Http g_http;

// ---- The display ----
struct Display
{
	Vk vk;
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice pd = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	uint32_t qf = 0;
	VkQueue queue = VK_NULL_HANDLE;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	std::vector<VkImage> images;
	VkExtent2D extent = {};
	VkSemaphore acquired[2] = {}, rendered[2] = {};

	bool Fail(const char* what, VkResult r)
	{
		std::printf("[frontend] %s failed: %d\n", what, static_cast<int>(r));
		std::fflush(stdout);
		return false;
	}

	bool Init()
	{
		const char* missing = nullptr;
		if (!vk.LoadGlobal(vk_icdGetInstanceProcAddr, &missing))
		{
			std::printf("[frontend] no %s\n", missing);
			return false;
		}
		const char* inst_ext[2] = {"VK_KHR_surface", "VK_KHR_display"};
		VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
		ai.pApplicationName = "PS5SX2 frontend";
		ai.apiVersion = VK_API_VERSION_1_1;
		VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		ici.pApplicationInfo = &ai;
		ici.enabledExtensionCount = 2;
		ici.ppEnabledExtensionNames = inst_ext;
		VkResult r = vk.vkCreateInstance(&ici, nullptr, &instance);
		if (r != VK_SUCCESS)
			return Fail("vkCreateInstance", r);
		if (!vk.LoadInstance(instance, true, &missing))
		{
			std::printf("[frontend] no %s\n", missing);
			return false;
		}
		uint32_t count = 1;
		r = vk.vkEnumeratePhysicalDevices(instance, &count, &pd);
		if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count == 0)
			return Fail("vkEnumeratePhysicalDevices", r);
		uint32_t qcount = 0;
		vk.vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, nullptr);
		std::vector<VkQueueFamilyProperties> qfs(qcount);
		vk.vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, qfs.data());
		qf = UINT32_MAX;
		for (uint32_t i = 0; i < qcount; i++)
			if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
			{
				qf = i;
				break;
			}
		if (qf == UINT32_MAX)
			return Fail("a graphics queue", VK_ERROR_UNKNOWN);
		const float prio = 1.0f;
		VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		qi.queueFamilyIndex = qf;
		qi.queueCount = 1;
		qi.pQueuePriorities = &prio;
		const char* dev_ext[1] = {"VK_KHR_swapchain"};
		VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		dci.queueCreateInfoCount = 1;
		dci.pQueueCreateInfos = &qi;
		dci.enabledExtensionCount = 1;
		dci.ppEnabledExtensionNames = dev_ext;
		r = vk.vkCreateDevice(pd, &dci, nullptr, &device);
		if (r != VK_SUCCESS)
			return Fail("vkCreateDevice", r);
		if (!vk.LoadDevice(device, true, &missing))
		{
			std::printf("[frontend] no %s\n", missing);
			return false;
		}
		vk.vkGetDeviceQueue(device, qf, 0, &queue);
		return CreateSurface() && CreateSwapchain();
	}

	// As PCSX2's VKSwapChain does on the PS5: the first display with a mode, its largest mode, and
	// a plane that can drive it.
	bool CreateSurface()
	{
		uint32_t display_count = 0;
		if (vk.vkGetPhysicalDeviceDisplayPropertiesKHR(pd, &display_count, nullptr) != VK_SUCCESS || !display_count)
			return Fail("vkGetPhysicalDeviceDisplayPropertiesKHR", VK_ERROR_UNKNOWN);
		std::vector<VkDisplayPropertiesKHR> displays(display_count);
		vk.vkGetPhysicalDeviceDisplayPropertiesKHR(pd, &display_count, displays.data());
		uint32_t plane_count = 0;
		if (vk.vkGetPhysicalDeviceDisplayPlanePropertiesKHR(pd, &plane_count, nullptr) != VK_SUCCESS || !plane_count)
			return Fail("vkGetPhysicalDeviceDisplayPlanePropertiesKHR", VK_ERROR_UNKNOWN);
		std::vector<VkDisplayPlanePropertiesKHR> planes(plane_count);
		vk.vkGetPhysicalDeviceDisplayPlanePropertiesKHR(pd, &plane_count, planes.data());
		VkDisplayKHR display = VK_NULL_HANDLE;
		VkDisplayModePropertiesKHR mode = {};
		for (const VkDisplayPropertiesKHR& d : displays)
		{
			uint32_t mode_count = 0;
			if (vk.vkGetDisplayModePropertiesKHR(pd, d.display, &mode_count, nullptr) != VK_SUCCESS || !mode_count)
				continue;
			std::vector<VkDisplayModePropertiesKHR> modes(mode_count);
			vk.vkGetDisplayModePropertiesKHR(pd, d.display, &mode_count, modes.data());
			for (const VkDisplayModePropertiesKHR& m : modes)
				if (!display || m.parameters.visibleRegion.width * m.parameters.visibleRegion.height >
									mode.parameters.visibleRegion.width * mode.parameters.visibleRegion.height)
				{
					display = d.display;
					mode = m;
				}
			if (display)
				break;
		}
		if (!display)
			return Fail("a display mode", VK_ERROR_UNKNOWN);
		uint32_t plane = UINT32_MAX;
		for (uint32_t i = 0; i < plane_count; i++)
			if (planes[i].currentDisplay == VK_NULL_HANDLE || planes[i].currentDisplay == display)
			{
				plane = i;
				break;
			}
		if (plane == UINT32_MAX)
			return Fail("a display plane", VK_ERROR_UNKNOWN);
		const VkDisplaySurfaceCreateInfoKHR ci = {VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR, nullptr, 0,
			mode.displayMode, plane, planes[plane].currentStackIndex, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, 1.0f,
			VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR, mode.parameters.visibleRegion};
		const VkResult r = vk.vkCreateDisplayPlaneSurfaceKHR(instance, &ci, nullptr, &surface);
		if (r != VK_SUCCESS)
			return Fail("vkCreateDisplayPlaneSurfaceKHR", r);
		extent = mode.parameters.visibleRegion;
		std::printf("[frontend] display %ux%u, plane %u\n", extent.width, extent.height, plane);
		return true;
	}

	bool CreateSwapchain()
	{
		VkSurfaceCapabilitiesKHR caps = {};
		VkResult r = vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);
		if (r != VK_SUCCESS)
			return Fail("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", r);
		if (caps.currentExtent.width != UINT32_MAX)
			extent = caps.currentExtent;
		uint32_t images_wanted = std::max(2u, caps.minImageCount);
		if (caps.maxImageCount)
			images_wanted = std::min(images_wanted, caps.maxImageCount);
		VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
		sci.surface = surface;
		sci.minImageCount = images_wanted;
		sci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
		sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		sci.imageExtent = extent;
		sci.imageArrayLayers = 1;
		sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
		sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
		sci.clipped = VK_TRUE;
		r = vk.vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain);
		if (r != VK_SUCCESS)
			return Fail("vkCreateSwapchainKHR", r);
		uint32_t n = 0;
		vk.vkGetSwapchainImagesKHR(device, swapchain, &n, nullptr);
		images.resize(n);
		vk.vkGetSwapchainImagesKHR(device, swapchain, &n, images.data());
		VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		for (int i = 0; i < 2; i++)
			if (vk.vkCreateSemaphore(device, &si, nullptr, &acquired[i]) != VK_SUCCESS ||
				vk.vkCreateSemaphore(device, &si, nullptr, &rendered[i]) != VK_SUCCESS)
				return Fail("vkCreateSemaphore", VK_ERROR_UNKNOWN);
		std::printf("[frontend] swapchain: %u images %ux%u\n", n, extent.width, extent.height);
		return true;
	}

	void Destroy()
	{
		if (device)
		{
			vk.vkDeviceWaitIdle(device);
			for (int i = 0; i < 2; i++)
			{
				if (acquired[i])
					vk.vkDestroySemaphore(device, acquired[i], nullptr);
				if (rendered[i])
					vk.vkDestroySemaphore(device, rendered[i], nullptr);
				acquired[i] = rendered[i] = VK_NULL_HANDLE;
			}
			if (swapchain)
				vk.vkDestroySwapchainKHR(device, swapchain, nullptr); // closes VideoOut for PCSX2
			swapchain = VK_NULL_HANDLE;
			vk.vkDestroyDevice(device, nullptr);
			device = VK_NULL_HANDLE;
		}
		if (surface)
			vk.vkDestroySurfaceKHR(instance, surface, nullptr);
		surface = VK_NULL_HANDLE;
		if (instance)
			vk.vkDestroyInstance(instance, nullptr);
		instance = VK_NULL_HANDLE;
	}
};

std::string ReadLastGame(const std::string& dir)
{
	std::string name;
	if (FILE* f = std::fopen((dir + "/lastgame.txt").c_str(), "r"))
	{
		char buf[512] = {};
		if (std::fgets(buf, sizeof(buf), f))
			name = buf;
		std::fclose(f);
	}
	while (!name.empty() && (name.back() == '\n' || name.back() == '\r'))
		name.pop_back();
	return name;
}

void WriteLastGame(const std::string& dir, const std::string& file)
{
	if (FILE* f = std::fopen((dir + "/lastgame.txt").c_str(), "w"))
	{
		std::fprintf(f, "%s\n", file.c_str());
		std::fclose(f);
	}
}
} // namespace

// vk-285-50: the settings page's server; it lives until the app ends.
static fe::WebServer* g_web = nullptr;

// vk-285-51: the settings log's clock, in the console's time zone (as Clock() above).
static long long SettingsLogLocalTime(long long utc)
{
	time_t local = static_cast<time_t>(utc);
	KernelTimesec sec[2] = {};
	uint64_t dst[2] = {};
	if (sceKernelConvertUtcToLocaltime(static_cast<time_t>(utc), &local, &sec[0], &dst[0]) != 0)
		local = static_cast<time_t>(utc);
	return static_cast<long long>(local);
}

// vk-285-53: sceSystemServiceHideSplashScreen once per process (later calls return the first result).
int orbis_hide_splash()
{
	static std::atomic<int> s_result{1};
	int expected = 1;
	if (s_result.compare_exchange_strong(expected, 2))
		s_result.store(sceSystemServiceHideSplashScreen());
	return s_result.load();
}

static char g_event_log_path[256];

void orbis_event_log_init(const std::string& path)
{
	std::snprintf(g_event_log_path, sizeof(g_event_log_path), "%s", path.c_str());
	fe::g_utc_to_local = &SettingsLogLocalTime;
}

extern "C" void orbis_event_log(const char* line)
{
	if (!g_event_log_path[0] || !line)
		return;
	const time_t local = static_cast<time_t>(SettingsLogLocalTime(static_cast<long long>(time(nullptr))));
	struct tm tm = {};
	gmtime_r(&local, &tm);
	char buf[1280];
	int n = std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d  %s\n", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		tm.tm_hour, tm.tm_min, tm.tm_sec, line);
	if (n <= 0)
		return;
	if (n >= static_cast<int>(sizeof(buf)))
	{
		n = static_cast<int>(sizeof(buf)) - 1;
		buf[n - 1] = '\n';
	}
	const int fd = open(g_event_log_path, O_WRONLY | O_APPEND | O_CREAT, 0666);
	if (fd < 0)
		return;
	(void)!write(fd, buf, static_cast<size_t>(n));
	close(fd);
}

bool orbis_web_start(const OrbisFrontendPaths& paths, const char* build_tag)
{
	if (g_web)
		return true;
	WebConfig cfg;
	cfg.game_dirs = {paths.games_dir, paths.top_dir};
	cfg.settings_dir = paths.settings_dir;
	cfg.gs_ini = paths.gs_ini;
	cfg.patches_dir = paths.patches_dir;
	cfg.covers_dir = paths.covers_dir;
	cfg.cache_dir = paths.cache_dir;
	cfg.token_path = paths.top_dir + "/webui_token.txt";
	cfg.build_tag = build_tag ? build_tag : "";
	cfg.port = 8844;
	cfg.presets.assign(reinterpret_cast<const char*>(fe_presets), static_cast<size_t>(fe_presets_end - fe_presets));
	cfg.change_log = paths.settings_log;
	fe::g_utc_to_local = &SettingsLogLocalTime;
	cfg.assets = {
		{"/", "text/html; charset=utf-8", fe_web_page, static_cast<size_t>(fe_web_page_end - fe_web_page)},
		{"/fonts/roboto.ttf", "font/ttf", fe_font_text, static_cast<size_t>(fe_font_text_end - fe_font_text)},
		{"/icons/discord.svg", "image/svg+xml", fe_web_discord, static_cast<size_t>(fe_web_discord_end - fe_web_discord)},
		{"/icons/x-twitter.svg", "image/svg+xml", fe_web_x, static_cast<size_t>(fe_web_x_end - fe_web_x)},
	};
	fe::WebServer* server = new fe::WebServer();
	if (!server->Start(cfg))
	{
		std::printf("[web] the settings page could not start\n");
		std::fflush(stdout);
		delete server;
		return false;
	}
	g_web = server;
	std::string url, shown;
	std::printf("[web] %s\n", g_web->Address(url, shown) ? ("http://" + shown).c_str() : "no network address yet");
	std::fflush(stdout);
	return true;
}

void orbis_web_now_playing(const std::string& image_path)
{
	if (g_web)
		g_web->SetNowPlaying(image_path);
}

int orbis_frontend_prefetch_covers(const OrbisFrontendPaths& paths, double budget_s, void (*notify)(const char*))
{
	if (!paths.allow_download)
		return 0;
	const double t0 = Now();
	std::vector<GameInfo> games = ScanGames({paths.games_dir, paths.top_dir});
	for (GameInfo& g : games)
		g.serial = ReadSerial(g.path);
	CoverConfig cc;
	cc.manual_dir = paths.covers_dir;
	cc.cache_dir = paths.cache_dir;
	cc.url_template = kCoverUrl;
	const std::vector<int> missing = CoverService::MissingCovers(games, cc);
	std::printf("[frontend] prefetch: %zu of %zu covers to fetch (checked in %.0f ms)\n", missing.size(), games.size(),
		(Now() - t0) * 1000.0);
	std::fflush(stdout);
	if (missing.empty())
		return 0;
	if (notify)
	{
		char buf[96];
		std::snprintf(buf, sizeof(buf), "PS5SX2: downloading %zu cover%s", missing.size(), missing.size() == 1 ? "" : "s");
		notify(buf);
	}
	const int saved = CoverService::Prefetch(games, missing, cc,
		[](const std::string& url, std::vector<uint8_t>& out) { return g_http.Get(url, out); }, budget_s);
	g_http.Term();
	std::printf("[frontend] prefetch: %d saved in %.1f s\n", saved, Now() - t0);
	std::fflush(stdout);
	return saved;
}

std::string orbis_frontend_run(const OrbisFrontendPaths& paths, const char* build_tag, bool* ran)
{
	*ran = false;
	const double t0 = Now();
	std::vector<GameInfo> games = ScanGames({paths.games_dir, paths.top_dir});
	for (GameInfo& g : games)
	{
		g.serial = ReadSerial(g.path);
		ReadBadges(g, paths.settings_dir, paths.gs_ini, paths.patches_dir);
		std::printf("[frontend] %s | %s | %s\n", g.file.c_str(), g.serial.empty() ? "no serial" : g.serial.c_str(),
			g.title.c_str());
	}
	std::printf("[frontend] %zu disc image(s), scanned in %.0f ms\n", games.size(), (Now() - t0) * 1000.0);
	std::fflush(stdout);
	if (games.empty())
	{
		*ran = true;
		return {};
	}
	const std::string last = ReadLastGame(paths.top_dir);
	int preselect = 0;
	for (size_t i = 0; i < games.size(); i++)
		if (games[i].file == last)
			preselect = static_cast<int>(i);
	if (games.size() == 1)
	{
		*ran = true;
		WriteLastGame(paths.top_dir, games[0].file);
		return games[0].path;
	}

	int32_t user = -1;
	(void)sceUserServiceInitialize(nullptr);
	(void)sceUserServiceGetInitialUser(&user);
	(void)scePadInit();
	const int pad = user >= 0 ? scePadOpen(user, 0, 0, nullptr) : -1;
	if (pad < 0)
	{
		std::printf("[frontend] no controller (%d); the plain list takes over\n", pad);
		return {};
	}

	// Heap objects, so a download still running at the end can be left to finish on its own.
	Display* display = new Display();
	if (!display->Init())
	{
		display->Destroy();
		delete display;
		scePadClose(pad);
		return {};
	}
	Fonts* fonts = new Fonts();
	Renderer renderer;
	if (!fonts->Init(fe_font_text, static_cast<size_t>(fe_font_text_end - fe_font_text), fe_font_icons,
			static_cast<size_t>(fe_font_icons_end - fe_font_icons), fe_font_brands,
			static_cast<size_t>(fe_font_brands_end - fe_font_brands)) ||
		!renderer.Init(&display->vk, display->pd, display->device, display->qf, display->queue, display->extent.width,
			display->extent.height, VK_FORMAT_B8G8R8A8_UNORM, display->images, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR))
	{
		std::printf("[frontend] renderer: %s\n", renderer.error().c_str());
		renderer.Shutdown();
		display->Destroy();
		delete display;
		delete fonts;
		scePadClose(pad);
		return {};
	}

	CoverService* covers = new CoverService();
	CoverConfig cc;
	cc.manual_dir = paths.covers_dir;
	cc.cache_dir = paths.cache_dir;
	cc.url_template = kCoverUrl;
	cc.allow_download = paths.allow_download;
	covers->Start(games, fonts, cc, [](const std::string& url, std::vector<uint8_t>& out) { return g_http.Get(url, out); });

	// The key sounds (vk-285-47). Without an audio port the shelf is simply quiet.
	Mixer* mixer = nullptr;
	AudioOut* audio = nullptr;
	if (paths.sound)
	{
		const double s0 = Now();
		mixer = new Mixer();
		mixer->Build();
		audio = new AudioOut();
		if (!audio->Start(mixer))
		{
			delete audio;
			audio = nullptr;
			delete mixer;
			mixer = nullptr;
		}
		std::printf("[frontend] sound %s (%.0f ms)\n", audio ? "on" : "off", (Now() - s0) * 1000.0);
	}
	else
		std::printf("[frontend] sound off (nomenusound)\n");

	App app;
	AppConfig acfg;
	acfg.build_tag = build_tag ? build_tag : "";
	acfg.preselect = preselect;
	acfg.sound = mixer;
	bool ok = app.Init(&renderer, fonts, games, covers, acfg);
	std::printf("[frontend] up in %.0f ms (%s)\n", (Now() - t0) * 1000.0, ok ? "ok" : renderer.error().c_str());
	std::fflush(stdout);
	// vk-285-50: the QR tile's address, looked up again every few seconds in case the IP changes.
	double web_checked = -1e9;
	auto refresh_web = [&](double now) {
		if (!g_web || now - web_checked < 3.0)
			return;
		web_checked = now;
		std::string url, shown;
		g_web->Address(url, shown);
		app.SetWebUrl(url, shown);
	};

	FrameDesc frame;
	double last_t = Now(), report_t = last_t;
	unsigned frames = 0, slot = 0;
	double worst = 0;
	bool first_shown = false;
	while (ok && !app.Done())
	{
		const double now = Now();
		refresh_web(now);
		const double dt = now - last_t;
		last_t = now;
		worst = std::max(worst, dt);
		PadData pd;
		std::memset(&pd, 0, sizeof(pd));
		pd.lx = pd.ly = 128;
		Input in;
		if (scePadReadState(pad, &pd) == 0)
		{
			in.left = (pd.buttons & kPadLeft) || pd.lx < 48;
			in.right = (pd.buttons & kPadRight) || pd.lx > 208;
			in.cross = pd.buttons & kPadCross;
			in.options = pd.buttons & kPadOptions;
			in.l1 = pd.buttons & kPadL1;
			in.r1 = pd.buttons & kPadR1;
		}
		app.Update(dt, in);
		app.Build(frame, Clock());
		if (!renderer.WaitForSlot())
		{
			ok = false;
			break;
		}
		uint32_t index = 0;
		VkResult r = display->vk.vkAcquireNextImageKHR(display->device, display->swapchain, UINT64_MAX,
			display->acquired[slot], VK_NULL_HANDLE, &index);
		if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
		{
			std::printf("[frontend] vkAcquireNextImageKHR: %d\n", static_cast<int>(r));
			ok = false;
			break;
		}
		if (!renderer.Render(frame, index, display->acquired[slot], display->rendered[slot]))
		{
			std::printf("[frontend] render: %s\n", renderer.error().c_str());
			ok = false;
			break;
		}
		VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
		pi.waitSemaphoreCount = 1;
		pi.pWaitSemaphores = &display->rendered[slot];
		pi.swapchainCount = 1;
		pi.pSwapchains = &display->swapchain;
		pi.pImageIndices = &index;
		r = display->vk.vkQueuePresentKHR(display->queue, &pi);
		if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
		{
			std::printf("[frontend] vkQueuePresentKHR: %d\n", static_cast<int>(r));
			ok = false;
			break;
		}
		slot ^= 1;
		frames++;
		if (!first_shown)
		{
			first_shown = true;
			// vk-285-53: the launch screen (sce_sys/pic1.dds) covers the app until it is hidden; with
			// it, 52 stayed on the launch screen with the shelf running behind it. Hidden once the
			// shelf's first frame is up, so the screen goes from one straight to the other.
			const int hide = orbis_hide_splash();
			std::printf("[frontend] first frame on screen %.0f ms after start; launch screen hidden (%d)\n", (Now() - t0) * 1000.0,
				hide);
			std::fflush(stdout);
		}
		if (now - report_t >= 5.0)
		{
			std::printf("[frontend] %u frames in %.1f s, worst %.1f ms\n", frames, now - report_t, worst * 1000.0);
			std::fflush(stdout);
			frames = 0;
			worst = 0;
			report_t = now;
		}
	}

	const int chosen = app.Chosen();
	const bool picked = ok && app.Done();
	renderer.WaitIdle();
	app.Shutdown();
	// The worker may be inside a download: stop it from starting another, fail the one in flight,
	// give it a moment, then leave it behind if need be.
	covers->RequestStop();
	g_http.Abort();
	const bool stopped = covers->Stop(1500);
	renderer.Shutdown();
	display->Destroy();
	delete display;
	if (stopped)
	{
		delete covers;
		delete fonts;
		g_http.Term();
	}
	else
		std::printf("[frontend] a cover download is still running; leaving it to finish\n");
	scePadClose(pad);
	if (audio)
	{
		// Let the launch sound ring out: it lasts about 0.8 s, the shelf closes 0.6 s after the press.
		const double s0 = Now();
		while (!mixer->Idle() && Now() - s0 < 0.4)
			usleep(5000);
		audio->Stop();
		delete audio;
		delete mixer;
	}

	if (!picked)
	{
		std::printf("[frontend] stopped without a pick; the plain list takes over\n");
		std::fflush(stdout);
		return {};
	}
	*ran = true;
	const GameInfo& g = games[static_cast<size_t>(chosen)];
	WriteLastGame(paths.top_dir, g.file);
	std::printf("[frontend] picked %s (%s)\n", g.file.c_str(), g.serial.c_str());
	std::fflush(stdout);
	return g.path;
}
