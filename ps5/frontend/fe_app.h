// PS5 port frontend: the cover-flow shelf itself: input, animation and the frame it draws.
// Platform code owns the Vulkan device and the display; it calls Update and Build once a frame.
//
// Copyright (C) 2026 Spyros
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "fe_covers.h"
#include "fe_games.h"
#include "fe_renderer.h"
#include "fe_sound.h"
#include "fe_text.h"

#include <string>
#include <vector>

namespace fe
{
struct Input
{
	bool left = false, right = false, cross = false, options = false, l1 = false, r1 = false;
};

struct AppConfig
{
	std::string build_tag;   // shown small in the corner
	int preselect = 0;       // the game selected at start
	SoundSink* sound = nullptr; // told about steps and the launch (may be null)
	int test_build = 0;          // test build 1 (vk-285-55): > 0 draws TESTING and build_label mid-screen
	std::string build_label;     // "Test build 1 · vk-285-55"
	std::string test_note;       // vk-285-105: a smaller line under build_label (the testers' Discord)
};

class App
{
public:
	bool Init(Renderer* renderer, const Fonts* fonts, std::vector<GameInfo> games, CoverService* covers, const AppConfig& cfg);
	void Shutdown();

	// Advances `dt` seconds with the pad's current buttons.
	void Update(double dt, const Input& in);

	// Fills `f` for this moment; `clock` is the time of day to show ("" for none).
	void Build(FrameDesc& f, const std::string& clock);

	// The settings page's address for the QR tile (vk-285-50): "http://<ip>:<port>/?t=<token>", or
	// empty when there is no network (the tile then says so). Call it again when the address changes.
	void SetWebUrl(const std::string& url, const std::string& shown);

	// True once a game was picked and its launch animation has played.
	bool Done() const { return m_done; }
	int Chosen() const { return m_selected; }

	const std::vector<GameInfo>& games() const { return m_games; }

private:
	struct Slot
	{
		Texture* cover = nullptr;
		Texture* placeholder = nullptr;
		Texture* spine = nullptr;
		VkDescriptorSet set = VK_NULL_HANDLE;
		bool dirty = false;
		float cover_mix = 0;     // animates to 1 once the cover arrives
		bool has_cover = false;
		float glow[3] = {0.55f, 0.42f, 1.0f};
		bool has_glow = false;
	};

	bool Step(int dir); // true when the selection moved
	void Sound(Sfx sfx, float pan);
	void PollCovers();
	void Pose(float d, float t, Mat4& model, float& brightness) const;

	Renderer* m_renderer = nullptr;
	const Fonts* m_fonts = nullptr;
	CoverService* m_covers = nullptr;
	AppConfig m_cfg;
	std::vector<GameInfo> m_games;
	std::vector<Slot> m_slots;

	int m_selected = 0;
	float m_scroll = 0, m_scroll_vel = 0; // the shelf's position (a game index), sprung to m_selected
	double m_time = 0;
	double m_select_time = -10;            // when the selection last changed (for the sheen)
	float m_glow[3] = {0.55f, 0.42f, 1.0f};

	// Held-direction repeat.
	int m_held = 0;
	double m_held_for = 0, m_next_repeat = 0;
	Input m_prev;
	bool m_released = false; // buttons held at start count only once released

	// The QR tile (vk-285-50): the code's modules, row by row, 1 = dark.
	std::string m_web_url, m_web_shown;
	std::vector<uint8_t> m_qr;
	int m_qr_size = 0;
	bool m_web_known = false; // SetWebUrl was called (the host preview may never call it)

	bool m_launching = false;
	double m_launch_time = 0;
	bool m_done = false;
	Texture* m_atlas = nullptr;
};
} // namespace fe
