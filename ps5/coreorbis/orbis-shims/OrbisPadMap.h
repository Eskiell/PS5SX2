// PS5SX2 (vk-285-116, AI-assisted): the controller remapping, as functions of what is held: no console library, no emulator,
// so tests/padmap/test_pad_map.cpp checks it on a PC. main-boot.cpp's orbis_pad_apply uses it for both PS2 ports.
//
// The settings (the Controls tab of the settings page and of the shelf's sheet; gs.ini or the game's own file):
//   PS5SX2/Button<Cross|Circle|Square|Triangle|L1|R1|L2|R2|L3|R3|Options|Touchpad|Up|Down|Left|Right> = the PS2 input that
//     controller button presses: Cross Circle Square Triangle L1 R1 L2 R2 L3 R3 Start Select Up Down Left Right Analog
//     (the DualShock 2's analog mode button) Pressure (PCSX2's pressure modifier: while held, buttons press at half
//     strength) or None. Unset: the same button (Options: Start, the touchpad's click: Select).
//   PS5SX2/SwapSticks = true: the left stick moves the PS2's right stick and the right stick its left one.
//   PS5SX2/InvertLeft, InvertRight = 0 no, 1 up-down, 2 left-right, 3 both: the PS2 stick's axes (after the swap).
//   PS5SX2/LeftStickDpad = 0 no, 1 the PS2's left stick also presses the D-pad, 2 it presses only the D-pad (the stick
//     itself stays in the middle).
// PS5SX2's own button combos (save states, the menu, the settings page) read the controller's real buttons before this.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace orbis_padmap
{
// What a controller button can press on the PS2 controller.
enum Target : uint8_t
{
	T_CROSS,
	T_CIRCLE,
	T_SQUARE,
	T_TRIANGLE,
	T_L1,
	T_R1,
	T_L2,
	T_R2,
	T_L3,
	T_R3,
	T_START,
	T_SELECT,
	T_UP,
	T_DOWN,
	T_LEFT,
	T_RIGHT,
	T_ANALOG,
	T_PRESSURE,
	T_NONE, // nothing
	T_COUNT = T_NONE, // the PS2 inputs (T_NONE is not one)
};

// The controller's buttons that can be remapped.
enum Source : uint8_t
{
	S_CROSS,
	S_CIRCLE,
	S_SQUARE,
	S_TRIANGLE,
	S_L1,
	S_R1,
	S_L2,
	S_R2,
	S_L3,
	S_R3,
	S_OPTIONS,
	S_TOUCHPAD,
	S_UP,
	S_DOWN,
	S_LEFT,
	S_RIGHT,
	S_COUNT,
};

struct SourceInfo
{
	const char* key;  // the setting, in [PS5SX2]
	const char* name; // for the log
	uint32_t bits;    // ScePad's button bits (L2 and R2 are read from their triggers)
	Target def;       // what it presses when the setting is unset
};

// ScePad's bits, as orbis_pad_apply always read them. The touchpad's click is 0x00100000; 0x1 is the keyboard's Select
// (Backspace, orbis-shims/ProsperoKbdMap.h PAD_SELECT).
inline const SourceInfo& SourceAt(int s)
{
	static const SourceInfo k[S_COUNT] = {
		{"ButtonCross", "Cross", 0x00004000u, T_CROSS},
		{"ButtonCircle", "Circle", 0x00002000u, T_CIRCLE},
		{"ButtonSquare", "Square", 0x00008000u, T_SQUARE},
		{"ButtonTriangle", "Triangle", 0x00001000u, T_TRIANGLE},
		{"ButtonL1", "L1", 0x00000400u, T_L1},
		{"ButtonR1", "R1", 0x00000800u, T_R1},
		{"ButtonL2", "L2", 0x00000100u, T_L2},
		{"ButtonR2", "R2", 0x00000200u, T_R2},
		{"ButtonL3", "L3", 0x00000002u, T_L3},
		{"ButtonR3", "R3", 0x00000004u, T_R3},
		{"ButtonOptions", "Options", 0x00000008u, T_START},
		{"ButtonTouchpad", "the touchpad's click", 0x00100001u, T_SELECT},
		{"ButtonUp", "D-pad up", 0x00000010u, T_UP},
		{"ButtonDown", "D-pad down", 0x00000040u, T_DOWN},
		{"ButtonLeft", "D-pad left", 0x00000080u, T_LEFT},
		{"ButtonRight", "D-pad right", 0x00000020u, T_RIGHT},
	};
	return k[s];
}

// The setting's values, in Target order (T_NONE last).
inline const char* TargetName(int t)
{
	static const char* const k[T_NONE + 1] = {"Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "L3", "R3", "Start",
		"Select", "Up", "Down", "Left", "Right", "Analog", "Pressure", "None"};
	return (t >= 0 && t <= T_NONE) ? k[t] : "None";
}

inline bool SameText(const std::string& a, const char* b)
{
	size_t i = 0;
	for (; i < a.size() && b[i]; i++)
	{
		const char x = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
		const char y = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
		if (x != y)
			return false;
	}
	return i == a.size() && b[i] == '\0';
}

// "Circle" (any case, spaces around it allowed) -> T_CIRCLE. False for anything else.
inline bool ParseTarget(std::string v, Target& out)
{
	while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r'))
		v.pop_back();
	size_t start = 0;
	while (start < v.size() && (v[start] == ' ' || v[start] == '\t'))
		start++;
	v.erase(0, start);
	for (int t = 0; t <= T_NONE; t++)
		if (SameText(v, TargetName(t)))
		{
			out = static_cast<Target>(t);
			return true;
		}
	return false;
}

struct Config
{
	Target target[S_COUNT];
	bool swap_sticks = false;
	uint8_t left_dpad = 0;    // 0 no, 1 the D-pad too, 2 the D-pad only
	uint8_t invert_left = 0;  // bit 0 up-down, bit 1 left-right
	uint8_t invert_right = 0;

	Config()
	{
		for (int s = 0; s < S_COUNT; s++)
			target[s] = SourceAt(s).def;
	}

	bool operator==(const Config& o) const
	{
		for (int s = 0; s < S_COUNT; s++)
			if (target[s] != o.target[s])
				return false;
		return swap_sticks == o.swap_sticks && left_dpad == o.left_dpad && invert_left == o.invert_left && invert_right == o.invert_right;
	}
	bool operator!=(const Config& o) const { return !(*this == o); }
	bool IsDefault() const { return *this == Config(); }
};

inline bool Truthy(const std::string& v)
{
	return SameText(v, "true") || v == "1" || SameText(v, "on") || SameText(v, "yes");
}

// A small number setting (0..max); anything else is 0.
inline uint8_t SmallInt(const std::string& v, int max)
{
	char* end = nullptr;
	const long n = std::strtol(v.c_str(), &end, 10);
	if (v.empty() || end == v.c_str() || n < 0 || n > max)
		return 0;
	return static_cast<uint8_t>(n);
}

// The settings, read with `get(key, value)` (the key within [PS5SX2]; true when it is set). An unknown value leaves that
// button or stick option as it is by default.
template <typename Get>
inline Config FromSettings(Get get)
{
	Config c;
	std::string v;
	for (int s = 0; s < S_COUNT; s++)
	{
		v.clear();
		Target t;
		if (get(SourceAt(s).key, v) && ParseTarget(v, t))
			c.target[s] = t;
	}
	v.clear();
	if (get("SwapSticks", v))
		c.swap_sticks = Truthy(v);
	v.clear();
	if (get("LeftStickDpad", v))
		c.left_dpad = SmallInt(v, 2);
	v.clear();
	if (get("InvertLeft", v))
		c.invert_left = SmallInt(v, 3);
	v.clear();
	if (get("InvertRight", v))
		c.invert_right = SmallInt(v, 3);
	return c;
}

// What the controller holds now (OrbisPadData's fields; the keyboard and mouse already added in).
struct State
{
	uint32_t buttons = 0;
	uint8_t l2 = 0, r2 = 0;
	uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
};

// What the PS2 controller gets: each input's value (0 released .. 1 fully pressed; the face buttons, the D-pad and the
// shoulders are pressure sensitive on a DualShock 2) and the sticks (0..255, 128 the middle).
struct Out
{
	float value[T_COUNT] = {};
	uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
};

// A trigger on anything but a trigger presses once it is pulled past an eighth (then as hard as it is pulled), so a
// finger resting on it presses nothing.
constexpr uint8_t kTriggerPress = 32;
// How far a stick is pushed (from the middle, of 128) before it presses the D-pad (Config::left_dpad).
constexpr int kStickDpad = 64;

inline uint8_t Invert(uint8_t v)
{
	return static_cast<uint8_t>(std::min(255, 256 - static_cast<int>(v)));
}

inline Out Apply(const Config& c, const State& s)
{
	Out o;
	for (int src = 0; src < S_COUNT; src++)
	{
		const Target t = c.target[src];
		if (t >= T_NONE)
			continue;
		float v;
		if (src == S_L2 || src == S_R2)
		{
			const uint8_t raw = src == S_L2 ? s.l2 : s.r2;
			const bool to_trigger = t == T_L2 || t == T_R2;
			v = (to_trigger || raw >= kTriggerPress) ? raw / 255.0f : 0.0f;
		}
		else
			v = (s.buttons & SourceAt(src).bits) ? 1.0f : 0.0f;
		// PCSX2 reads the analog button and the pressure modifier as on or off (it acts when they change).
		if ((t == T_ANALOG || t == T_PRESSURE) && v > 0.0f)
			v = 1.0f;
		o.value[t] = std::max(o.value[t], v);
	}

	uint8_t lx = s.lx, ly = s.ly, rx = s.rx, ry = s.ry;
	if (c.swap_sticks)
	{
		std::swap(lx, rx);
		std::swap(ly, ry);
	}
	if (c.invert_left & 1)
		ly = Invert(ly);
	if (c.invert_left & 2)
		lx = Invert(lx);
	if (c.invert_right & 1)
		ry = Invert(ry);
	if (c.invert_right & 2)
		rx = Invert(rx);
	if (c.left_dpad != 0)
	{
		if (lx <= 128 - kStickDpad)
			o.value[T_LEFT] = 1.0f;
		if (lx >= 128 + kStickDpad)
			o.value[T_RIGHT] = 1.0f;
		if (ly <= 128 - kStickDpad)
			o.value[T_UP] = 1.0f;
		if (ly >= 128 + kStickDpad)
			o.value[T_DOWN] = 1.0f;
		if (c.left_dpad == 2)
			lx = ly = 128;
	}
	o.lx = lx;
	o.ly = ly;
	o.rx = rx;
	o.ry = ry;
	return o;
}

// For the log: "Cross presses Circle, Circle presses Cross; sticks swapped" or "as on the controller".
inline std::string Describe(const Config& c)
{
	std::string buttons, sticks;
	for (int s = 0; s < S_COUNT; s++)
	{
		const Target t = c.target[s];
		if (t == SourceAt(s).def)
			continue;
		if (!buttons.empty())
			buttons += ", ";
		buttons += SourceAt(s).name;
		buttons += t == T_NONE ? std::string(" presses nothing") : std::string(" presses ") + TargetName(t);
	}
	auto add = [&](const std::string& what) { sticks += (sticks.empty() ? "" : ", ") + what; };
	static const char* const axes[4] = {"", "up-down", "left-right", "up-down and left-right"};
	if (c.swap_sticks)
		add("sticks swapped");
	if (c.invert_left)
		add(std::string("left stick ") + axes[c.invert_left & 3] + " inverted");
	if (c.invert_right)
		add(std::string("right stick ") + axes[c.invert_right & 3] + " inverted");
	if (c.left_dpad == 1)
		add("left stick also the D-pad");
	if (c.left_dpad == 2)
		add("left stick as the D-pad only");
	if (buttons.empty() && sticks.empty())
		return "as on the controller";
	if (buttons.empty())
		return sticks;
	return sticks.empty() ? buttons : buttons + "; " + sticks;
}
} // namespace orbis_padmap
