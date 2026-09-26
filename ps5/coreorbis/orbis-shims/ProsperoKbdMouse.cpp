// PS5SX2 (vk-285-72): the PS5's USB keyboard and mouse as the PS2's USB keyboard and mouse.
//
// main-boot.cpp puts PCSX2's USB HID keyboard on the PS2's USB port 1 and its HID mouse on port 2
// (USB1/Type=hidkbd, USB2/Type=hidmouse; the flag file nousbkbm or a game's own settings take them off),
// and starts OrbisKbdMouseStart once the game runs. A thread reads the console's keyboard and mouse
// libraries every 4 ms and queues what changed; OrbisKbdMousePump, at each vsync on the CPU thread
// (StubHost.cpp's Host::PumpMessagesOnCPUThread), hands it to the USB devices there, where the
// emulated USB controller reads them. Games that support a USB keyboard or mouse (Half-Life, Unreal
// Tournament, the online games' chat) see a standard HID keyboard and mouse; the others never look.
//
// The libraries are loaded while the game runs (sceKernelLoadStartModule, sceKernelDlsym), so a console
// that has none of them only logs it. The prototypes and data layouts are the public reimplementations'
// (the ps5-payload-dev SDL port's keyboard, and KytyPS5's keyboard and mouse), not Sony's headers:
//   - sceKeyboardReadState fills 96 bytes: "connected" at 16, the modifier byte at 28 (bit 0 left Ctrl ...
//     bit 7 right GUI, as in a USB boot report) and up to 16 HID usages (u16) at 32 (the two agree on
//     these; what the first 16 bytes hold beyond a timestamp they do not, so nothing reads them);
//   - sceMouseRead fills up to num 40-byte records, returns how many: a timestamp, "connected" at 8,
//     buttons at 12 (bit 0 primary, 1 secondary, 2 the middle one), then the x, y and wheel deltas at
//     16, 20 and 24 (s32).
// The buffers are larger than those sizes, and the first records a console returns go to boot.log, so a
// firmware whose layout differs shows it there instead of writing past the buffer.

#include "USB/USB.h"
#include "USB/qemu-usb/hid.h"
#include "Input/InputManager.h"
#include "Config.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern "C" {
int sceKernelLoadStartModule(const char* path, size_t args, const void* argp, uint32_t flags, void* opt, int* res);
int sceKernelDlsym(int handle, const char* symbol, void** addrp);
int sceUserServiceInitialize(const void* params);
int sceUserServiceGetInitialUser(int32_t* user);
int sceUserServiceGetForegroundUser(int32_t* user);
}

namespace
{
	using KeyboardInit = int (*)();
	using KeyboardOpen = int (*)(int32_t user, int32_t type, int32_t index, const void* param);
	using KeyboardReadState = int (*)(int32_t handle, void* state);
	using MouseInit = int (*)();
	using MouseOpen = int (*)(int32_t user, int32_t type, int32_t index, const void* param);
	using MouseRead = int (*)(int32_t handle, void* data, int32_t num);

	constexpr size_t KEYBOARD_STATE_BYTES = 96;
	constexpr size_t MOUSE_RECORD_BYTES = 40;
	// Records asked for at a time, into a buffer with room for this many records of 128 bytes: a
	// firmware whose records are larger than the 40 bytes read here still writes inside it. A mouse
	// reporting at 1000 Hz leaves 4 or 5 in 4 ms.
	constexpr int MOUSE_RECORDS = 8;
	constexpr size_t MOUSE_BUFFER_BYTES = MOUSE_RECORDS * 128;

	enum class Kind : uint8_t
	{
		Key,
		Button,
		Wheel,
	};
	struct Event
	{
		Kind kind;
		uint16_t code; // a HID usage (Key) or an InputButton (Button)
		float value;
	};

	std::mutex s_lock;
	std::vector<Event> s_events; // under s_lock
	long s_dx = 0, s_dy = 0; // under s_lock: the mouse movement since the last pump
	std::atomic<bool> s_running{false};
	constexpr size_t MAX_QUEUED = 512;

	void Queue(Kind kind, uint16_t code, float value)
	{
		std::lock_guard<std::mutex> lock(s_lock);
		if (s_events.size() < MAX_QUEUED)
			s_events.push_back({kind, code, value});
	}

	// A system library by name, from the directories the ps5-payload-dev SDK's loader searches.
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
			printf("[kbm] %s: load %#x (start result %d)\n", path.c_str(), static_cast<unsigned>(handle), res);
			if (handle >= 0)
				return handle;
		}
		printf("[kbm] %s: not found or not loadable\n", name);
		return -1;
	}

	template <typename F>
	bool Symbol(int module, const char* name, F& fn)
	{
		void* address = nullptr;
		const int rc = sceKernelDlsym(module, name, &address);
		fn = reinterpret_cast<F>(address);
		if (rc != 0 || !address)
			printf("[kbm] %s: dlsym %#x\n", name, static_cast<unsigned>(rc));
		return rc == 0 && address;
	}

	void HexDump(const char* what, const uint8_t* bytes, size_t count)
	{
		printf("[kbm] %s:", what);
		for (size_t i = 0; i < count; i++)
			printf("%s%02x", (i % 8) == 0 ? " " : "", bytes[i]);
		printf("\n");
	}

	// The keys a keyboard state holds down: the modifiers as usages 0xe0-0xe7, then the key codes.
	int PressedKeys(const uint8_t* state, uint16_t* keys)
	{
		int n = 0;
		if (state[16] == 0) // no keyboard
			return 0;
		uint32_t modifiers;
		std::memcpy(&modifiers, state + 28, sizeof(modifiers));
		for (int bit = 0; bit < 8; bit++)
		{
			if (modifiers & (1u << bit))
				keys[n++] = static_cast<uint16_t>(0xe0 + bit);
		}
		for (int i = 0; i < 16; i++)
		{
			uint16_t code;
			std::memcpy(&code, state + 32 + i * 2, sizeof(code));
			// 1-3 are the report's error codes (roll-over), and the modifiers came from their byte.
			if (code >= 4 && code < 0xe0)
				keys[n++] = code;
		}
		return n;
	}

	bool Contains(const uint16_t* keys, int n, uint16_t key)
	{
		for (int i = 0; i < n; i++)
		{
			if (keys[i] == key)
				return true;
		}
		return false;
	}

	void* Thread(void*)
	{
		int32_t user = -1;
		sceUserServiceInitialize(nullptr);
		if (sceUserServiceGetInitialUser(&user) != 0 || user < 0)
			sceUserServiceGetForegroundUser(&user);

		KeyboardInit keyboard_init = nullptr;
		KeyboardOpen keyboard_open = nullptr;
		KeyboardReadState keyboard_read = nullptr;
		int keyboard = -1;
		const int keyboard_module = LoadModule("libSceKeyboard.sprx");
		if (keyboard_module >= 0 && Symbol(keyboard_module, "sceKeyboardInit", keyboard_init) &&
			Symbol(keyboard_module, "sceKeyboardOpen", keyboard_open) &&
			Symbol(keyboard_module, "sceKeyboardReadState", keyboard_read))
		{
			const int init = keyboard_init();
			uint8_t param[16] = {};
			keyboard = keyboard_open(user, 0, 0, param);
			printf("[kbm] keyboard: init %#x, open for user %d: %#x\n", static_cast<unsigned>(init), user,
				static_cast<unsigned>(keyboard));
		}

		MouseInit mouse_init = nullptr;
		MouseOpen mouse_open = nullptr;
		MouseRead mouse_read = nullptr;
		int mouse = -1;
		const int mouse_module = LoadModule("libSceMouse.sprx");
		if (mouse_module >= 0 && Symbol(mouse_module, "sceMouseInit", mouse_init) &&
			Symbol(mouse_module, "sceMouseOpen", mouse_open) && Symbol(mouse_module, "sceMouseRead", mouse_read))
		{
			const int init = mouse_init();
			// behaviorFlag 1: every mouse through this one handle.
			uint8_t param[8] = {1};
			mouse = mouse_open(user, 0, 0, param);
			if (mouse < 0)
			{
				std::memset(param, 0, sizeof(param));
				mouse = mouse_open(user, 0, 0, param);
			}
			printf("[kbm] mouse: init %#x, open for user %d: %#x\n", static_cast<unsigned>(init), user,
				static_cast<unsigned>(mouse));
		}
		fflush(stdout);
		if (keyboard < 0 && mouse < 0)
			return nullptr;
		s_running.store(true, std::memory_order_release);

		alignas(8) uint8_t state[256] = {};
		alignas(8) uint8_t records[MOUSE_BUFFER_BYTES] = {};
		uint16_t keys[24] = {}, previous[24] = {};
		int key_count = 0, previous_count = 0;
		uint32_t buttons = 0;
		bool keyboard_seen = false, mouse_seen = false;
		unsigned keyboard_errors = 0, mouse_errors = 0;
		for (;;)
		{
			if (keyboard >= 0)
			{
				std::memset(state, 0, sizeof(state));
				const int rc = keyboard_read(keyboard, state);
				if (rc == 0)
				{
					key_count = PressedKeys(state, keys);
					if (!keyboard_seen && state[16] != 0)
					{
						keyboard_seen = true;
						HexDump("first connected keyboard state", state, KEYBOARD_STATE_BYTES + 8);
						fflush(stdout);
					}
					for (int i = 0; i < previous_count; i++)
					{
						if (!Contains(keys, key_count, previous[i]))
							Queue(Kind::Key, previous[i], 0.0f);
					}
					for (int i = 0; i < key_count; i++)
					{
						if (!Contains(previous, previous_count, keys[i]))
							Queue(Kind::Key, keys[i], 1.0f);
					}
					std::memcpy(previous, keys, sizeof(keys));
					previous_count = key_count;
				}
				else if (keyboard_errors++ < 5)
				{
					printf("[kbm] keyboard read %#x\n", static_cast<unsigned>(rc));
					fflush(stdout);
				}
			}
			if (mouse >= 0)
			{
				const int count = mouse_read(mouse, records, MOUSE_RECORDS);
				if (count > 0 && count <= MOUSE_RECORDS)
				{
					long dx = 0, dy = 0;
					for (int i = 0; i < count; i++)
					{
						const uint8_t* const record = records + static_cast<size_t>(i) * MOUSE_RECORD_BYTES;
						if (record[8] == 0) // not connected
							continue;
						if (!mouse_seen)
						{
							mouse_seen = true;
							HexDump("first connected mouse records", records,
								std::min<size_t>(static_cast<size_t>(count) * MOUSE_RECORD_BYTES + 8, 96));
							fflush(stdout);
						}
						uint32_t now;
						int32_t x, y, wheel;
						std::memcpy(&now, record + 12, 4);
						std::memcpy(&x, record + 16, 4);
						std::memcpy(&y, record + 20, 4);
						std::memcpy(&wheel, record + 24, 4);
						// A delta no mouse reports in 4 ms is a record read at the wrong place.
						if (x < -4096 || x > 4096 || y < -4096 || y > 4096)
							continue;
						dx += x;
						dy += y;
						static constexpr struct
						{
							uint32_t mask;
							InputButton button;
						} map[] = {{0x1, INPUT_BUTTON_LEFT}, {0x2, INPUT_BUTTON_RIGHT}, {0x4, INPUT_BUTTON_MIDDLE}};
						for (const auto& m : map)
						{
							if ((now ^ buttons) & m.mask)
								Queue(Kind::Button, static_cast<uint16_t>(m.button), (now & m.mask) ? 1.0f : 0.0f);
						}
						buttons = now;
						for (int notch = 0; notch < (wheel < 0 ? -wheel : wheel) && notch < 16; notch++)
							Queue(Kind::Wheel, 0, wheel > 0 ? 1.0f : -1.0f);
					}
					if (dx != 0 || dy != 0)
					{
						std::lock_guard<std::mutex> lock(s_lock);
						s_dx += dx;
						s_dy += dy;
					}
				}
				else if (count < 0 && mouse_errors++ < 5)
				{
					printf("[kbm] mouse read %#x\n", static_cast<unsigned>(count));
					fflush(stdout);
				}
			}
			usleep(4000);
		}
		return nullptr;
	}
} // namespace

void OrbisKbdMouseStart()
{
	static bool started = false;
	if (started)
		return;
	started = true;
	pthread_t thread;
	if (pthread_create(&thread, nullptr, Thread, nullptr) == 0)
		pthread_detach(thread);
}

// At vsync on the CPU thread: what the thread queued, into the ports whose device takes it.
void OrbisKbdMousePump()
{
	if (!s_running.load(std::memory_order_acquire))
		return;
	std::vector<Event> events;
	long dx, dy;
	{
		std::lock_guard<std::mutex> lock(s_lock);
		events.swap(s_events);
		dx = s_dx;
		dy = s_dy;
		s_dx = s_dy = 0;
	}
	static const s32 keyboard_type = USB::DeviceTypeNameToIndex("hidkbd");
	static const s32 mouse_type = USB::DeviceTypeNameToIndex("hidmouse");
	for (u32 port = 0; port < USB::NUM_PORTS; port++)
	{
		const s32 type = EmuConfig.USB.Ports[port].DeviceType;
		if (type == keyboard_type)
		{
			for (const Event& e : events)
			{
				if (e.kind == Kind::Key)
					USB::SetDeviceBindValue(port, e.code, e.value);
			}
		}
		else if (type == mouse_type)
		{
			for (const Event& e : events)
			{
				if (e.kind == Kind::Button)
					USB::SetDeviceBindValue(port, e.code, e.value);
				else if (e.kind == Kind::Wheel)
					USB::SetDeviceBindValue(port, INPUT_BUTTON__MAX + static_cast<u32>(InputPointerAxis::WheelY), e.value);
			}
			if (dx != 0)
				USB::SetDeviceBindValue(port, INPUT_BUTTON__MAX + static_cast<u32>(InputPointerAxis::X), static_cast<float>(dx));
			if (dy != 0)
				USB::SetDeviceBindValue(port, INPUT_BUTTON__MAX + static_cast<u32>(InputPointerAxis::Y), static_cast<float>(dy));
		}
	}
}
