// PS5SX2 (vk-285-73): PS5 notifications from a worker thread, and the notification test.
//
// Two ways to put a toast on the PS5's screen, both public (the ps5-payload-dev SDK declares them and
// ships a sample of each):
//   - the kernel toast, sceKernelSendNotificationRequest(0, request, 0xc30, 0): a 0xc30-byte request whose
//     text starts at byte 0x2d. It is what the port has used since the start ("PS5SX2: starting"). The
//     PS4 layout (PS4-Notify's documentation) also has a "use icon" byte at 0x2c and an icon URI at
//     0x42d; whether the PS5 honours them is one of the things the test below finds out.
//   - the rich toast, libSceNotification's sceNotificationSend(user, logged, json): a JSON payload that
//     the system UI's notification overlay draws (its klog says "[notification] Post7 ... useCaseId=
//     <id> buflen=<bytes>"). The layout here is the SDK's notify sample (LightningMods, GPL-3.0-or-later):
//     rawData.viewTemplateType "InteractiveToastTemplateB", channelType "Downloads", useCaseId "IDC",
//     viewData { icon {type "Url", parameters.url} | {type "Predefined", parameters.icon}, message.body,
//     subMessage.body }, plus createdDateTime and localNotificationId.
// libSceNotification is loaded when the first rich toast is sent (sceKernelLoadStartModule), so a console
// without it only logs that and falls back to the kernel toast.
//
// Callers only queue: the worker thread does the system calls, so the pad thread and the CPU thread
// never wait on the system UI. Every send goes to boot.log ("[notify] ...") with its result.

#include "ProsperoNotify.h"
#include "notify_test_badge.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
int sceKernelSendNotificationRequest(int device, void* request, size_t size, int blocking);
int sceKernelLoadStartModule(const char* path, size_t args, const void* argp, uint32_t flags, void* opt, int* res);
int sceKernelDlsym(int handle, const char* symbol, void** addrp);
int sceUserServiceGetForegroundUser(int32_t* user);
void orbis_event_log(const char* line) __attribute__((weak)); // frontend/fe_ps5.cpp: settings.log
}

namespace
{
	// The kernel toast's request (0xc30 bytes; the PS4 names, as documented by PS4-Notify).
#pragma pack(push, 1)
	struct KernelToast
	{
		int32_t type; // 0: a plain message
		int32_t req_id;
		int32_t priority;
		int32_t msg_id;
		int32_t target_id;
		int32_t user_id;
		int32_t unk1;
		int32_t unk2;
		int32_t app_id;
		int32_t error_num;
		int32_t unk3;
		uint8_t use_icon_image_uri; // 0x2c
		char message[1024]; // 0x2d
		char icon_uri[1024]; // 0x42d
		char unk[1024];
		uint8_t pad[3];
	};
#pragma pack(pop)
	static_assert(sizeof(KernelToast) == 0xc30, "the kernel toast request is 0xc30 bytes");
	static_assert(offsetof(KernelToast, message) == 0x2d, "its text starts at 0x2d");
	static_assert(offsetof(KernelToast, icon_uri) == 0x42d, "its icon URI starts at 0x42d");

	constexpr int32_t USER_SYSTEM = 0xfe; // the SDK sample's SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM

	constexpr const char* BADGE_DIR = "/data/PCSX2/cache/notify";
	constexpr const char* BADGE_DATA = "/data/PCSX2/cache/notify/test_badge.png";
	constexpr const char* BADGE_USER = "/user/data/PCSX2/cache/notify/test_badge.png";
	constexpr const char* APP_ICON = "/user/appmeta/PPSA99203/icon0.png";
	constexpr const char* WEB_ICON = "https://raw.githubusercontent.com/xlenore/ps2-covers/main/covers/default/SCUS-97199.jpg";

	enum class Api : uint8_t
	{
		Kernel,
		Rich,
	};

	struct Request
	{
		Api api = Api::Kernel;
		std::string message; // the kernel toast's whole text, or the rich toast's first line
		std::string sub_message;
		std::string icon; // a path or URL; for a predefined icon, its name
		bool icon_predefined = false;
		bool preview_view = false; // add the sample's platformViews.previewDisabled view
		bool logged = false; // sceNotificationSend's second argument (keep it in the notification list)
		bool foreground_user = false; // send to the foreground user instead of the system's id
		int test = 0; // > 0: the test variant this is
	};

	// The test's variants, in the order D-pad Right steps through them.
	struct Variant
	{
		const char* name;
		Api api;
		const char* icon;
		bool icon_predefined;
		bool preview_view;
		bool logged;
		bool foreground_user;
	};
	constexpr Variant VARIANTS[] = {
		{"kernel toast, text only", Api::Kernel, nullptr, false, false, false, false},
		{"kernel toast + the PS4 trophy icon URI", Api::Kernel, "cxml://psnotification/tex_default_icon_trophy", false, false,
			false, false},
		{"kernel toast + an icon file in /data", Api::Kernel, BADGE_DATA, false, false, false, false},
		{"rich toast + an icon file in /data", Api::Rich, BADGE_DATA, false, false, false, false},
		{"rich toast + the same file as /user/data", Api::Rich, BADGE_USER, false, false, false, false},
		{"rich toast + the app's own icon", Api::Rich, APP_ICON, false, false, false, false},
		{"rich toast + an https image", Api::Rich, WEB_ICON, false, false, false, false},
		{"rich toast, the SDK sample's layout (logged)", Api::Rich, BADGE_DATA, false, true, true, false},
		{"rich toast + a predefined icon named trophy", Api::Rich, "trophy", true, false, false, false},
		{"rich toast to the foreground user", Api::Rich, BADGE_DATA, false, false, false, true},
	};
	constexpr int VARIANT_COUNT = static_cast<int>(sizeof(VARIANTS) / sizeof(VARIANTS[0]));

	constexpr size_t MAX_QUEUED = 8;
	// The queue lives on the heap and is never freed: the worker waits on it for the whole process, so
	// its lock and condition variable must not be destroyed at exit while the worker still waits.
	struct Shared
	{
		std::mutex lock;
		std::condition_variable wake;
		std::deque<Request> queue; // under lock
		bool thread_started = false; // under lock
	};
	Shared& S()
	{
		static Shared* const s = new Shared();
		return *s;
	}
	std::atomic<int> s_test_variant{0};

	using NotificationSend = int (*)(int32_t user, bool logged, const char* payload);

	void Event(const char* line)
	{
		if (orbis_event_log)
			orbis_event_log(line);
	}

	// A system library by name, from the directories the ps5-payload-dev SDK's loader searches
	// (the same list as ProsperoKbdMouse.cpp).
	int LoadModule(const char* name)
	{
		static const char* const dirs[] = {"/system/common/lib/", "/system/priv/lib/", "/system_ex/common_ex/lib/",
			"/system_ex/priv_ex/lib/"};
		for (const char* dir : dirs)
		{
			const std::string path = std::string(dir) + name;
			struct stat st;
			if (stat(path.c_str(), &st) != 0)
				continue;
			int res = 0;
			const int handle = sceKernelLoadStartModule(path.c_str(), 0, nullptr, 0, nullptr, &res);
			printf("[notify] %s: load %#x (start result %d)\n", path.c_str(), static_cast<unsigned>(handle), res);
			if (handle >= 0)
				return handle;
		}
		printf("[notify] %s: not found or not loadable\n", name);
		return -1;
	}

	NotificationSend RichSender()
	{
		static bool s_tried = false;
		static NotificationSend s_send = nullptr;
		if (!s_tried)
		{
			s_tried = true;
			const int module = LoadModule("libSceNotification.sprx");
			if (module >= 0)
			{
				void* address = nullptr;
				const int rc = sceKernelDlsym(module, "sceNotificationSend", &address);
				printf("[notify] sceNotificationSend: dlsym %#x at %p\n", static_cast<unsigned>(rc), address);
				if (rc == 0)
					s_send = reinterpret_cast<NotificationSend>(address);
			}
			fflush(stdout);
		}
		return s_send;
	}

	// The test icon, written once per launch if the file is missing or not ours.
	void EnsureBadgeFile()
	{
		static bool s_done = false;
		if (s_done)
			return;
		s_done = true;
		struct stat st;
		if (stat(BADGE_DATA, &st) == 0 && st.st_size == static_cast<off_t>(kNotifyTestBadgePngLen))
		{
			printf("[notify] test icon: %s (present)\n", BADGE_DATA);
			return;
		}
		mkdir("/data/PCSX2/cache", 0777);
		mkdir(BADGE_DIR, 0777);
		FILE* f = fopen(BADGE_DATA, "wb");
		const size_t written = f ? fwrite(kNotifyTestBadgePng, 1, kNotifyTestBadgePngLen, f) : 0;
		if (f)
			fclose(f);
		printf("[notify] test icon: %s (%s, %zu bytes)\n", BADGE_DATA, written == kNotifyTestBadgePngLen ? "written" : "write failed",
			written);
	}

	std::string Quote(const std::string& s)
	{
		std::string out = "\"";
		for (const unsigned char c : s)
		{
			if (c == '"' || c == '\\')
			{
				out += '\\';
				out += static_cast<char>(c);
			}
			else if (c == '\n')
				out += "\\n";
			else if (c < 0x20)
			{
				char esc[8];
				snprintf(esc, sizeof(esc), "\\u%04x", c);
				out += esc;
			}
			else
				out += static_cast<char>(c); // UTF-8 passes through
		}
		return out + "\"";
	}

	std::string IconJson(const Request& r)
	{
		if (r.icon_predefined)
			return "{\"type\":\"Predefined\",\"parameters\":{\"icon\":" + Quote(r.icon) + "}}";
		return "{\"type\":\"Url\",\"parameters\":{\"url\":" + Quote(r.icon) + "}}";
	}

	std::string RichPayload(const Request& r)
	{
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		struct tm tm;
		gmtime_r(&ts.tv_sec, &tm);
		char created[48];
		snprintf(created, sizeof(created), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ts.tv_nsec / 1000000));
		static std::atomic<unsigned> s_count{0};
		char id[16];
		snprintf(id, sizeof(id), "%u",
			static_cast<unsigned>(100000000u + (static_cast<unsigned>(ts.tv_sec) % 800000u) * 1000u + s_count.fetch_add(1) % 1000u));

		std::string j = "{\"rawData\":{\"viewTemplateType\":\"InteractiveToastTemplateB\",\"channelType\":\"Downloads\","
		                "\"useCaseId\":\"IDC\",\"toastOverwriteType\":\"No\",\"isImmediate\":true,\"priority\":100,"
		                "\"viewData\":{";
		if (!r.icon.empty())
			j += "\"icon\":" + IconJson(r) + ",";
		j += "\"message\":{\"body\":" + Quote(r.message) + "}";
		if (!r.sub_message.empty())
			j += ",\"subMessage\":{\"body\":" + Quote(r.sub_message) + "}";
		j += "}";
		if (r.preview_view)
			j += ",\"platformViews\":{\"previewDisabled\":{\"viewData\":{\"icon\":{\"type\":\"Predefined\",\"parameters\":"
			     "{\"icon\":\"download\"}},\"message\":{\"body\":" + Quote(r.message) + "}}}}";
		j += "},\"createdDateTime\":" + Quote(created) + ",\"localNotificationId\":" + Quote(id) + "}";
		return j;
	}

	int SendKernel(const Request& r)
	{
		KernelToast req;
		memset(&req, 0, sizeof(req));
		snprintf(req.message, sizeof(req.message), "%s", r.message.c_str());
		if (!r.icon.empty())
		{
			req.use_icon_image_uri = 1;
			snprintf(req.icon_uri, sizeof(req.icon_uri), "%s", r.icon.c_str());
		}
		return sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
	}

	void Send(Request r)
	{
		const Variant* v = r.test > 0 ? &VARIANTS[r.test - 1] : nullptr;
		if (r.api == Api::Rich || (!r.icon.empty() && r.icon.compare(0, 6, "/data/") == 0))
			EnsureBadgeFile();

		const auto t0 = std::chrono::steady_clock::now();
		int rc = 0;
		const char* how = "kernel";
		std::string payload;
		int32_t user = USER_SYSTEM;
		if (r.api == Api::Rich)
		{
			const NotificationSend send = RichSender();
			if (send)
			{
				if (r.foreground_user)
				{
					int32_t fg = -1;
					const int urc = sceUserServiceGetForegroundUser(&fg);
					if (urc == 0 && fg >= 0)
						user = fg;
					else
						printf("[notify] foreground user: rc %#x, sending to the system id\n", static_cast<unsigned>(urc));
				}
				payload = RichPayload(r);
				rc = send(user, r.logged, payload.c_str());
				how = "rich";
			}
			else
			{
				// No rich toasts on this console: the same text as a kernel toast, so something shows.
				Request k;
				k.message = r.message + (r.sub_message.empty() ? "" : "\n" + r.sub_message);
				rc = SendKernel(k);
				how = "kernel (rich unavailable)";
			}
		}
		else
			rc = SendKernel(r);
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

		char line[512];
		if (v)
			snprintf(line, sizeof(line), "notification test %d/%d (%s): %s, rc %#x, %.1f ms", r.test, VARIANT_COUNT, v->name, how,
				static_cast<unsigned>(rc), ms);
		else
			snprintf(line, sizeof(line), "notification: %s, rc %#x, %.1f ms", how, static_cast<unsigned>(rc), ms);
		printf("[notify] %s\n", line);
		if (!r.icon.empty())
			printf("[notify]   icon %s%s\n", r.icon_predefined ? "predefined " : "", r.icon.c_str());
		if (!payload.empty())
			printf("[notify]   user %#x, logged %d, payload %zu bytes (klog: Post7 ... buflen): %s\n", static_cast<unsigned>(user),
				r.logged ? 1 : 0, payload.size(), payload.c_str());
		fflush(stdout);
		if (v)
			Event(line);
	}

	void* Worker(void*)
	{
		for (;;)
		{
			Request r;
			{
				Shared& s = S();
				std::unique_lock<std::mutex> lock(s.lock);
				s.wake.wait(lock, [&s] { return !s.queue.empty(); });
				r = std::move(s.queue.front());
				s.queue.pop_front();
			}
			Send(std::move(r));
		}
		return nullptr;
	}

	void Queue(Request r)
	{
		Shared& s = S();
		std::lock_guard<std::mutex> lock(s.lock);
		if (!s.thread_started)
		{
			pthread_t t;
			if (pthread_create(&t, nullptr, Worker, nullptr) != 0)
			{
				printf("[notify] worker thread: pthread_create failed, notification dropped\n");
				return;
			}
			pthread_detach(t);
			s.thread_started = true;
		}
		if (s.queue.size() >= MAX_QUEUED)
		{
			printf("[notify] queue full, notification dropped\n");
			return;
		}
		s.queue.push_back(std::move(r));
		s.wake.notify_one();
	}
} // namespace

void OrbisNotifyPlain(const char* text)
{
	Request r;
	r.api = Api::Kernel;
	r.message = text ? text : "";
	Queue(std::move(r));
}

void OrbisNotifyRich(const char* message, const char* sub_message, const char* icon)
{
	Request r;
	r.api = Api::Rich;
	r.message = message ? message : "";
	r.sub_message = sub_message ? sub_message : "";
	r.icon = icon ? icon : "";
	Queue(std::move(r));
}

void OrbisNotifyTest(bool next)
{
	int n = s_test_variant.load(std::memory_order_relaxed);
	if (next || n == 0)
		n = (n % VARIANT_COUNT) + 1;
	s_test_variant.store(n, std::memory_order_relaxed);

	const Variant& v = VARIANTS[n - 1];
	Request r;
	r.api = v.api;
	r.test = n;
	r.icon = v.icon ? v.icon : "";
	r.icon_predefined = v.icon_predefined;
	r.preview_view = v.preview_view;
	r.logged = v.logged;
	r.foreground_user = v.foreground_user;
	char tag[160];
	snprintf(tag, sizeof(tag), "test %d/%d: %s", n, VARIANT_COUNT, v.name);
	if (v.api == Api::Kernel)
		r.message = std::string("PS5SX2 notification ") + tag + "\nAchievement unlocked: First Steps (10 points)";
	else
	{
		r.message = "Achievement unlocked: First Steps";
		r.sub_message = std::string("10 points \xC2\xB7 ") + tag;
	}
	printf("[notify] L3+R3 + D-pad %s: %s\n", next ? "Right" : "Left", tag);
	fflush(stdout);
	Queue(std::move(r));
}
