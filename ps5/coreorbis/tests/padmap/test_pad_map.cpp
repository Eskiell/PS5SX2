// PS5SX2 (vk-285-116, AI-assisted): a check of the controller remapping (orbis-shims/OrbisPadMap.h) on a PC.
//   g++ -std=c++17 -Wall -I../../orbis-shims -o /tmp/test_pad_map test_pad_map.cpp && /tmp/test_pad_map
#include "OrbisPadMap.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>

using namespace orbis_padmap;

static int s_failures = 0;
#define CHECK(cond)                                                                                  \
	do                                                                                               \
	{                                                                                                \
		if (!(cond))                                                                                 \
		{                                                                                            \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                   \
			s_failures++;                                                                            \
		}                                                                                            \
	} while (0)

// Settings as a file would hold them.
static Config From(std::map<std::string, std::string> kv)
{
	return FromSettings([&](const char* key, std::string& v) {
		const auto it = kv.find(key);
		if (it == kv.end())
			return false;
		v = it->second;
		return true;
	});
}

static State Held(uint32_t buttons)
{
	State s;
	s.buttons = buttons;
	return s;
}

static bool Only(const Out& o, std::initializer_list<Target> on)
{
	for (int t = 0; t < T_COUNT; t++)
	{
		bool want = false;
		for (Target x : on)
			want = want || x == t;
		if ((o.value[t] > 0.0f) != want)
			return false;
	}
	return true;
}

int main()
{
	// Nothing set: every button presses its own, as orbis_pad_apply did before (touchpad click and Backspace are Select,
	// Options is Start), the triggers are analog, the sticks pass through.
	{
		const Config c = From({});
		CHECK(c.IsDefault());
		CHECK(Describe(c) == "as on the controller");
		CHECK(Only(Apply(c, Held(0)), {}));
		CHECK(Only(Apply(c, Held(0x4000)), {T_CROSS}));
		CHECK(Only(Apply(c, Held(0x2000)), {T_CIRCLE}));
		CHECK(Only(Apply(c, Held(0x8000)), {T_SQUARE}));
		CHECK(Only(Apply(c, Held(0x1000)), {T_TRIANGLE}));
		CHECK(Only(Apply(c, Held(0x0400)), {T_L1}));
		CHECK(Only(Apply(c, Held(0x0800)), {T_R1}));
		CHECK(Only(Apply(c, Held(0x0002)), {T_L3}));
		CHECK(Only(Apply(c, Held(0x0004)), {T_R3}));
		CHECK(Only(Apply(c, Held(0x0008)), {T_START}));
		CHECK(Only(Apply(c, Held(0x00100000)), {T_SELECT}));
		CHECK(Only(Apply(c, Held(0x00000001)), {T_SELECT}));
		CHECK(Only(Apply(c, Held(0x0010)), {T_UP}));
		CHECK(Only(Apply(c, Held(0x0040)), {T_DOWN}));
		CHECK(Only(Apply(c, Held(0x0080)), {T_LEFT}));
		CHECK(Only(Apply(c, Held(0x0020)), {T_RIGHT}));
		// The L2/R2 bits alone press nothing (the triggers' values do), as before.
		CHECK(Only(Apply(c, Held(0x0100 | 0x0200)), {}));
		State s;
		s.l2 = 100;
		s.r2 = 255;
		s.lx = 0;
		s.ly = 255;
		s.rx = 40;
		s.ry = 200;
		const Out o = Apply(c, s);
		CHECK(std::fabs(o.value[T_L2] - 100 / 255.0f) < 1e-6f && o.value[T_R2] == 1.0f);
		CHECK(o.lx == 0 && o.ly == 255 && o.rx == 40 && o.ry == 200);
		// A light pull of a trigger still reaches the PS2's trigger (it is analog there too).
		s.l2 = 5;
		CHECK(Apply(c, s).value[T_L2] > 0.0f);
		// Everything at once.
		CHECK(Only(Apply(c, Held(0x00104EFE | 0xF000)), {T_CROSS, T_CIRCLE, T_SQUARE, T_TRIANGLE, T_L1, T_R1, T_L3, T_R3, T_START,
														T_SELECT, T_UP, T_DOWN, T_LEFT, T_RIGHT}));
	}

	// Cross and Circle swapped (Japanese games), Square off, Triangle on Select; the file's spelling may vary.
	{
		const Config c = From({{"ButtonCross", "Circle"}, {"ButtonCircle", " cross "}, {"ButtonSquare", "None"}, {"ButtonTriangle", "SELECT"}});
		CHECK(!c.IsDefault());
		CHECK(Only(Apply(c, Held(0x4000)), {T_CIRCLE}));
		CHECK(Only(Apply(c, Held(0x2000)), {T_CROSS}));
		CHECK(Only(Apply(c, Held(0x8000)), {}));
		CHECK(Only(Apply(c, Held(0x1000)), {T_SELECT}));
		CHECK(Only(Apply(c, Held(0x1000 | 0x00100000)), {T_SELECT}));
		CHECK(Describe(c) == "Cross presses Circle, Circle presses Cross, Square presses nothing, Triangle presses Select");
	}

	// Unknown values and settings are left at their defaults.
	{
		const Config c = From({{"ButtonCross", "Jump"}, {"ButtonR1", ""}, {"LeftStickDpad", "7"}, {"InvertLeft", "x"}, {"SwapSticks", "maybe"}});
		CHECK(c.IsDefault());
	}

	// Two buttons on one: the stronger press counts, and releasing one keeps the other's.
	{
		const Config c = From({{"ButtonL1", "Cross"}});
		CHECK(Only(Apply(c, Held(0x0400)), {T_CROSS}));
		CHECK(Only(Apply(c, Held(0x0400 | 0x4000)), {T_CROSS}));
		CHECK(Only(Apply(c, Held(0x4000)), {T_CROSS}));
	}

	// A trigger on a face button: pressure follows the pull, past a light touch.
	{
		const Config c = From({{"ButtonR2", "Square"}, {"ButtonL2", "R2"}});
		State s;
		s.r2 = kTriggerPress - 1;
		CHECK(Apply(c, s).value[T_SQUARE] == 0.0f);
		s.r2 = 128;
		CHECK(std::fabs(Apply(c, s).value[T_SQUARE] - 128 / 255.0f) < 1e-6f);
		CHECK(Apply(c, s).value[T_R2] == 0.0f); // R2 no longer presses R2
		s.r2 = 0;
		s.l2 = 3; // L2 on R2: trigger to trigger, analog all the way
		CHECK(std::fabs(Apply(c, s).value[T_R2] - 3 / 255.0f) < 1e-6f && Apply(c, s).value[T_L2] == 0.0f);
	}

	// A button on a trigger is a full pull.
	{
		const Config c = From({{"ButtonL1", "L2"}, {"ButtonR1", "R2"}, {"ButtonL2", "L1"}, {"ButtonR2", "R1"}});
		CHECK(Only(Apply(c, Held(0x0400)), {T_L2}) && Apply(c, Held(0x0400)).value[T_L2] == 1.0f);
		State s;
		s.l2 = 255;
		s.r2 = 200;
		CHECK(Only(Apply(c, s), {T_L1, T_R1}));
	}

	// The analog button and the pressure modifier are on or off, even from a trigger.
	{
		const Config c = From({{"ButtonTouchpad", "Analog"}, {"ButtonL2", "Pressure"}});
		CHECK(Only(Apply(c, Held(0x00100000)), {T_ANALOG}) && Apply(c, Held(0x00100000)).value[T_ANALOG] == 1.0f);
		State s;
		s.l2 = 90;
		CHECK(Apply(c, s).value[T_PRESSURE] == 1.0f);
		s.l2 = 20;
		CHECK(Apply(c, s).value[T_PRESSURE] == 0.0f);
	}

	// The D-pad elsewhere and Options on Select.
	{
		const Config c = From({{"ButtonUp", "Triangle"}, {"ButtonDown", "Cross"}, {"ButtonOptions", "Select"}, {"ButtonTouchpad", "Start"}});
		CHECK(Only(Apply(c, Held(0x0010)), {T_TRIANGLE}));
		CHECK(Only(Apply(c, Held(0x0040)), {T_CROSS}));
		CHECK(Only(Apply(c, Held(0x0008)), {T_SELECT}));
		CHECK(Only(Apply(c, Held(0x00100000)), {T_START}));
		CHECK(Only(Apply(c, Held(0x00000001)), {T_START})); // the keyboard's Backspace follows the touchpad's click
	}

	// Sticks: swapped, then inverted (the PS2's sticks), then the left one on the D-pad.
	{
		State s;
		s.lx = 10;
		s.ly = 20;
		s.rx = 30;
		s.ry = 250;
		Out o = Apply(From({{"SwapSticks", "true"}}), s);
		CHECK(o.lx == 30 && o.ly == 250 && o.rx == 10 && o.ry == 20);
		o = Apply(From({{"InvertRight", "1"}}), s);
		CHECK(o.rx == 30 && o.ry == 6 && o.lx == 10 && o.ly == 20);
		o = Apply(From({{"InvertRight", "3"}, {"InvertLeft", "2"}}), s);
		CHECK(o.rx == 226 && o.ry == 6 && o.lx == 246 && o.ly == 20);
		// The middle stays the middle; the ends map onto the ends.
		State mid;
		o = Apply(From({{"InvertLeft", "3"}, {"InvertRight", "3"}}), mid);
		CHECK(o.lx == 128 && o.ly == 128 && o.rx == 128 && o.ry == 128);
		CHECK(Invert(0) == 255 && Invert(255) == 1 && Invert(1) == 255);
		// Swap and invert: the inversion is the PS2's right stick, the controller's left.
		o = Apply(From({{"SwapSticks", "1"}, {"InvertRight", "1"}}), s);
		CHECK(o.rx == 10 && o.ry == 236 && o.lx == 30 && o.ly == 250);
		CHECK(Describe(From({{"SwapSticks", "1"}, {"InvertRight", "1"}})) == "sticks swapped, right stick up-down inverted");
	}
	{
		// The left stick on the D-pad too: up-left pressed, the stick still moves.
		State s;
		s.lx = 20;
		s.ly = 30;
		Out o = Apply(From({{"LeftStickDpad", "1"}}), s);
		CHECK(Only(o, {T_LEFT, T_UP}) && o.lx == 20 && o.ly == 30);
		// Only the D-pad: the PS2's left stick stays in the middle.
		o = Apply(From({{"LeftStickDpad", "2"}}), s);
		CHECK(Only(o, {T_LEFT, T_UP}) && o.lx == 128 && o.ly == 128);
		// A small push presses nothing.
		s.lx = 128 - kStickDpad + 1;
		s.ly = 128 + kStickDpad - 1;
		CHECK(Only(Apply(From({{"LeftStickDpad", "2"}}), s), {}));
		s.lx = 255;
		s.ly = 255;
		CHECK(Only(Apply(From({{"LeftStickDpad", "2"}}), s), {T_RIGHT, T_DOWN}));
		// With the sticks swapped it is the controller's right stick that drives the D-pad.
		State r;
		r.rx = 0;
		CHECK(Only(Apply(From({{"LeftStickDpad", "1"}, {"SwapSticks", "true"}}), r), {T_LEFT}));
		CHECK(Only(Apply(From({{"LeftStickDpad", "1"}}), r), {}));
		// The D-pad from the stick and from a button add up.
		CHECK(Only(Apply(From({{"LeftStickDpad", "1"}, {"ButtonCross", "Right"}}), [] {
			State x;
			x.buttons = 0x4000;
			x.lx = 0;
			return x;
		}()),
			{T_LEFT, T_RIGHT}));
	}

	// Every target's name reads back as itself, and only those names do.
	for (int t = 0; t <= T_NONE; t++)
	{
		Target back = T_NONE;
		CHECK(ParseTarget(TargetName(t), back) && back == t);
	}
	{
		Target x;
		CHECK(!ParseTarget("Crosss", x) && !ParseTarget("Cros", x) && !ParseTarget("", x));
	}

	if (s_failures == 0)
		printf("test_pad_map: all checks passed\n");
	return s_failures == 0 ? 0 : 1;
}
