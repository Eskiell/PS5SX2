// PS5 port frontend: the cover-flow shelf (see fe_app.h).
//
// Copyright (C) 2026 Spyros
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

extern "C" {
#include "third_party/qrcodegen/qrcodegen.h"
}

namespace fe
{
namespace
{
// The camera and the shelf's layout (world units: 1 = 100 mm).
const Vec3 kEye(0.0f, 0.30f, 7.8f);
const Vec3 kTarget(0.0f, -0.16f, 0.0f);
constexpr float kFovY = 30.0f;
constexpr float kFloorY = -kBoxHalfH;
constexpr float kCenterZ = 1.05f;   // the selected case, pulled forward
constexpr float kSideX = 1.86f;     // the first neighbour's centre
constexpr float kStepX = 0.56f;     // each further one
constexpr float kSideZ = -0.55f;
constexpr float kStepZ = 0.10f;
constexpr float kSideTurn = 58.0f;  // degrees the neighbours turn towards the centre
constexpr int kVisible = 7;         // cases drawn on each side

// The author's handles, under the wordmark (vk-285-50, the user's request).
constexpr const char* kDiscordHandle = "sword.pdf";
constexpr const char* kXHandle = "@sword_pdf";

uint32_t Rgba(float r, float g, float b, float a = 1.0f)
{
	auto c = [](float v) { return static_cast<uint32_t>(Clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
	return c(r) | (c(g) << 8) | (c(b) << 16) | (c(a) << 24);
}

std::string SizeText(uint64_t bytes)
{
	char buf[32];
	const double gb = static_cast<double>(bytes) / 1e9;
	if (gb >= 1.0)
		std::snprintf(buf, sizeof(buf), "%.1f GB", gb);
	else
		std::snprintf(buf, sizeof(buf), "%.0f MB", static_cast<double>(bytes) / 1e6);
	return buf;
}

// Screen position (0..1) of a world point.
Vec2 Project(const Mat4& view_proj, const Vec3& p)
{
	const Vec4 c = view_proj * Vec4(p.x, p.y, p.z, 1.0f);
	Vec2 out;
	out.x = (c.x / c.w) * 0.5f + 0.5f;
	out.y = (c.y / c.w) * 0.5f + 0.5f;
	return out;
}
} // namespace

bool App::Init(Renderer* renderer, const Fonts* fonts, std::vector<GameInfo> games, CoverService* covers, const AppConfig& cfg)
{
	m_renderer = renderer;
	m_fonts = fonts;
	m_covers = covers;
	m_cfg = cfg;
	m_games = std::move(games);
	m_slots.assign(m_games.size(), Slot());
	m_selected = m_games.empty() ? 0 : std::max(0, std::min(cfg.preselect, static_cast<int>(m_games.size()) - 1));
	m_scroll = static_cast<float>(m_selected);
	m_atlas = m_renderer->CreateTexture(static_cast<uint32_t>(fonts->AtlasWidth()), static_cast<uint32_t>(fonts->AtlasHeight()),
		VK_FORMAT_R8_UNORM, fonts->AtlasPixels().data());
	if (!m_atlas)
		return false;
	m_renderer->SetAtlas(m_atlas);
	if (m_covers)
		m_covers->SetSelected(m_selected);
	return true;
}

void App::SetWebUrl(const std::string& url, const std::string& shown)
{
	m_web_known = true;
	if (url == m_web_url && shown == m_web_shown)
		return;
	m_web_url = url;
	m_web_shown = shown;
	m_qr.clear();
	m_qr_size = 0;
	if (url.empty())
		return;
	// Byte mode up to version 10 (57 modules) is plenty for "http://255.255.255.255:65535/?t=" and a
	// 16-character token; medium error correction.
	uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(10)], tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
	if (!qrcodegen_encodeText(url.c_str(), tmp, qr, qrcodegen_Ecc_MEDIUM, 1, 10, qrcodegen_Mask_AUTO, true))
	{
		std::printf("[frontend] QR: \"%s\" does not fit\n", url.c_str());
		return;
	}
	m_qr_size = qrcodegen_getSize(qr);
	m_qr.resize(static_cast<size_t>(m_qr_size) * m_qr_size);
	for (int y = 0; y < m_qr_size; y++)
		for (int x = 0; x < m_qr_size; x++)
			m_qr[static_cast<size_t>(y) * m_qr_size + x] = qrcodegen_getModule(qr, x, y) ? 1 : 0;
}

void App::Shutdown()
{
	for (Slot& s : m_slots)
	{
		m_renderer->FreeTextureSet(s.set);
		m_renderer->DestroyTexture(s.cover);
		m_renderer->DestroyTexture(s.placeholder);
		m_renderer->DestroyTexture(s.spine);
		s = Slot();
	}
	m_renderer->SetAtlas(nullptr);
	m_renderer->DestroyTexture(m_atlas);
	m_atlas = nullptr;
}

bool App::Step(int dir)
{
	if (m_games.empty())
		return false;
	const int n = static_cast<int>(m_games.size());
	const int next = std::max(0, std::min(n - 1, m_selected + dir));
	if (next == m_selected)
		return false;
	m_selected = next;
	m_select_time = m_time;
	if (m_covers)
		m_covers->SetSelected(m_selected);
	return true;
}

void App::Sound(Sfx sfx, float pan)
{
	if (m_cfg.sound)
		m_cfg.sound->Play(sfx, pan);
}

void App::PollCovers()
{
	if (!m_covers)
		return;
	CoverImage img;
	int budget = 4; // textures a frame
	while (budget-- > 0 && m_covers->Poll(img))
	{
		if (img.game < 0 || img.game >= static_cast<int>(m_slots.size()))
			continue;
		Slot& s = m_slots[static_cast<size_t>(img.game)];
		Texture* t = m_renderer->CreateTexture(static_cast<uint32_t>(img.width), static_cast<uint32_t>(img.height),
			VK_FORMAT_R8G8B8A8_UNORM, img.rgba.data());
		if (!t)
			continue;
		Texture** dst = img.kind == CoverImage::Spine ? &s.spine : img.kind == CoverImage::Placeholder ? &s.placeholder : &s.cover;
		m_renderer->DestroyTexture(*dst);
		*dst = t;
		if (img.kind == CoverImage::Cover)
		{
			s.has_cover = true;
			if (img.has_glow)
			{
				s.glow[0] = img.glow[0];
				s.glow[1] = img.glow[1];
				s.glow[2] = img.glow[2];
				s.has_glow = true;
			}
			std::printf("[frontend] cover for %s (%s, %dx%d)\n", m_games[static_cast<size_t>(img.game)].title.c_str(),
				img.source, img.width, img.height);
		}
		s.dirty = true;
	}
	for (Slot& s : m_slots)
		if (s.dirty && s.spine && s.placeholder)
		{
			m_renderer->FreeTextureSet(s.set);
			s.set = m_renderer->AllocTextureSet(s.cover ? s.cover : s.placeholder, s.placeholder, s.spine);
			s.dirty = false;
		}
}

void App::Update(double dt, const Input& in)
{
	m_time += dt;
	const float fdt = static_cast<float>(std::min(dt, 0.1));

	if (m_launching)
	{
		if (m_time - m_launch_time > 0.6)
			m_done = true;
	}
	else
	{
		const bool any = in.left || in.right || in.cross || in.options || in.l1 || in.r1;
		if (!m_released)
			m_released = !any;
		else
		{
			const int dir = in.left ? -1 : in.right ? 1 : 0;
			const bool fresh = (in.left && !m_prev.left) || (in.right && !m_prev.right);
			// The sounds lean a little towards the side pressed.
			const float pan = 0.18f * static_cast<float>(dir);
			if (dir != 0 && fresh)
			{
				Sound(Step(dir) ? Sfx::Move : Sfx::Edge, pan);
				m_held = dir;
				m_held_for = 0;
				m_next_repeat = 0.36;
			}
			else if (dir != 0 && dir == m_held)
			{
				m_held_for += dt;
				bool moved = false; // one sound a frame, however many steps a slow frame took
				while (m_held_for >= m_next_repeat)
				{
					moved |= Step(dir);
					m_next_repeat += m_held_for > 1.5 ? 0.055 : 0.11;
				}
				if (moved)
					Sound(Sfx::MoveRepeat, pan);
			}
			else
				m_held = 0;
			if (in.l1 && !m_prev.l1)
				Sound(Step(-5) ? Sfx::JumpLeft : Sfx::Edge, -0.25f);
			if (in.r1 && !m_prev.r1)
				Sound(Step(5) ? Sfx::JumpRight : Sfx::Edge, 0.25f);
			if (((in.cross && !m_prev.cross) || (in.options && !m_prev.options)) && !m_games.empty())
			{
				m_launching = true;
				m_launch_time = m_time;
				Sound(Sfx::Launch, 0.0f);
			}
		}
		m_prev = in;
	}

	// The shelf follows the selection on a critically damped spring.
	const float k = 150.0f, c = 2.0f * std::sqrt(k);
	for (int i = 0; i < 4; i++)
	{
		const float h = fdt / 4;
		const float acc = k * (static_cast<float>(m_selected) - m_scroll) - c * m_scroll_vel;
		m_scroll_vel += acc * h;
		m_scroll += m_scroll_vel * h;
	}

	PollCovers();
	for (Slot& s : m_slots)
		if (s.has_cover && s.cover_mix < 1.0f)
			s.cover_mix = std::min(1.0f, s.cover_mix + fdt / 0.35f);

	const float house[3] = {0.55f, 0.42f, 1.0f};
	float want[3] = {house[0], house[1], house[2]};
	if (!m_slots.empty() && m_slots[static_cast<size_t>(m_selected)].has_glow)
		for (int i = 0; i < 3; i++)
			want[i] = Mix(m_slots[static_cast<size_t>(m_selected)].glow[i], house[i], 0.35f);
	const float a = 1.0f - std::exp(-fdt * 5.0f);
	for (int i = 0; i < 3; i++)
		m_glow[i] += (want[i] - m_glow[i]) * a;
}

void App::Pose(float d, float t, Mat4& model, float& brightness) const
{
	const float ad = std::fabs(d);
	const float s = d < 0 ? -1.0f : 1.0f;
	const float e = Smoothstep(0.0f, 1.0f, std::min(ad, 1.0f));
	const float far_ = std::max(ad - 1.0f, 0.0f);
	const float side_x = kSideX + far_ * kStepX;
	const float side_z = kSideZ - far_ * kStepZ;
	const float sway = (1.0f - e) * 0.10f * std::sin(t * 0.7f);
	// The picked case hovers: it lifts off the floor as it is picked, then bobs between 0.03 and
	// 0.054 above it. Resting on the floor at the bottom of the bob (vk-285-46) still put the
	// halo's bottom line, drawn 0.016 outside the case, into the reflection.
	const float bob = (1.0f - e) * (0.030f + 0.012f * (1.0f + std::sin(t * 1.3f)));
	float x = s * Mix(0.0f, side_x, e);
	float z = Mix(kCenterZ, side_z, e);
	const float turn = Mix(sway, -s * Radians(kSideTurn), e);
	if (m_launching && ad < 0.5f)
	{
		const float l = Smoothstep(0.0f, 0.6f, static_cast<float>(m_time - m_launch_time));
		z += l * 2.2f;
	}
	model = Mat4::Translate(x, bob, z) * Mat4::RotateY(turn);
	brightness = Mix(1.0f, 0.74f, e) - 0.05f * std::min(far_, 5.0f);
}

void App::Build(FrameDesc& f, const std::string& clock)
{
	const float W = static_cast<float>(m_renderer->width()), H = static_cast<float>(m_renderer->height());
	const float k = H / 2160.0f;
	const float t = static_cast<float>(m_time);
	const Mat4 proj = Mat4::Perspective(Radians(kFovY), W / H, 0.1f, 60.0f);
	const Mat4 view = Mat4::LookAt(kEye, kTarget, Vec3(0, 1, 0));
	f.view_proj = proj * view;
	f.cam_pos = kEye;
	f.time = t;
	f.glow[0] = m_glow[0];
	f.glow[1] = m_glow[1];
	f.glow[2] = m_glow[2];
	f.glow[3] = 1.0f;
	f.floor_y = kFloorY;
	f.reflect_strength = 0.24f;
	f.reflect_falloff = 3.6f;
	f.boxes.clear();
	f.reflections.clear();
	f.ui.clear();
	f.halo = HaloDraw();

	// Background: the glow sits behind the selected case, the floor glow under it.
	const Vec2 centre = Project(f.view_proj, Vec3(0, 0.1f, kCenterZ - 0.6f));
	const Vec2 floor_pt = Project(f.view_proj, Vec3(0, kFloorY, kCenterZ));
	BgParams& bg = f.bg;
	bg.glow_color[0] = m_glow[0];
	bg.glow_color[1] = m_glow[1];
	bg.glow_color[2] = m_glow[2];
	bg.glow_color[3] = 0.50f;
	bg.glow_pos[0] = centre.x;
	bg.glow_pos[1] = centre.y;
	bg.glow_pos[2] = 0.58f;
	bg.glow_pos[3] = 0.40f;
	bg.floor_glow[0] = floor_pt.x;
	bg.floor_glow[1] = floor_pt.y;
	bg.floor_glow[2] = 0.30f;
	bg.floor_glow[3] = 0.035f;
	const float top[4] = {0.030f, 0.034f, 0.095f, 1}, mid[4] = {0.055f, 0.055f, 0.155f, 1},
				bottom[4] = {0.010f, 0.010f, 0.026f, 1};
	std::copy(top, top + 4, bg.top);
	std::copy(mid, mid + 4, bg.mid);
	std::copy(bottom, bottom + 4, bg.bottom);
	bg.misc[0] = W / H;
	bg.misc[1] = floor_pt.y;
	bg.misc[2] = 0.55f;
	bg.misc[3] = t;

	// The cases, nearest the selection last so the halo lands on top of its neighbours.
	const int n = static_cast<int>(m_games.size());
	const int first = std::max(0, static_cast<int>(std::floor(m_scroll)) - kVisible);
	const int last = std::min(n - 1, static_cast<int>(std::ceil(m_scroll)) + kVisible);
	const float since = static_cast<float>(m_time - m_select_time);
	for (int i = first; i <= last; i++)
	{
		const Slot& s = m_slots[static_cast<size_t>(i)];
		if (!s.set)
			continue;
		const float d = static_cast<float>(i) - m_scroll;
		BoxDraw b;
		Pose(d, t, b.model, b.brightness);
		b.set = s.set;
		b.cover_mix = s.cover_mix;
		b.selected = std::max(0.0f, 1.0f - std::fabs(d) * 2.0f);
		if (i == m_selected && since < 1.2f)
		{
			b.sheen_pos = -0.3f + since * 1.6f;
			b.sheen_strength = 0.16f * (1.0f - Smoothstep(0.6f, 1.2f, since));
		}
		f.boxes.push_back(b);
		BoxDraw r = b;
		r.model = Mat4::Translate(0, 2 * kFloorY, 0) * Mat4::Scale(1, -1, 1) * b.model;
		r.sheen_strength = 0;
		f.reflections.push_back(r);
	}

	// The outline glow on the selected case once the shelf settles on it.
	const float settle = 1.0f - Clamp(std::fabs(m_scroll - static_cast<float>(m_selected)) * 2.5f, 0.0f, 1.0f);
	if (n > 0 && settle > 0.0f && m_slots[static_cast<size_t>(m_selected)].set && !m_launching)
	{
		float bright;
		Pose(static_cast<float>(m_selected) - m_scroll, t, f.halo.model, bright);
		f.halo.enabled = true;
		f.halo.half_w = kBoxHalfW;
		f.halo.half_h = kBoxHalfH;
		f.halo.z = kBoxHalfD - 0.004f;
		f.halo.margin = 0.22f;
		f.halo.radius = 0.035f;
		f.halo.line = 0.0065f;
		f.halo.glow_width = 0.035f;
		f.halo.color[0] = Mix(m_glow[0], 1.0f, 0.8f);
		f.halo.color[1] = Mix(m_glow[1], 1.0f, 0.8f);
		f.halo.color[2] = Mix(m_glow[2], 1.0f, 0.8f);
		f.halo.color[3] = settle * (0.50f + 0.08f * std::sin(t * 2.2f));
	}

	// Fade in at start, out when launching.
	f.fade = std::min(1.0f, t / 0.35f);
	if (m_launching)
		f.fade *= 1.0f - Smoothstep(0.1f, 0.6f, static_cast<float>(m_time - m_launch_time));

	// ---- UI ----
	const uint32_t white = Rgba(1, 1, 1), dim = Rgba(0.72f, 0.69f, 0.82f), faint = Rgba(0.55f, 0.53f, 0.66f);
	const uint32_t accent = Rgba(Mix(m_glow[0], 1.0f, 0.3f), Mix(m_glow[1], 1.0f, 0.3f), Mix(m_glow[2], 1.0f, 0.3f));
	const float margin = 110.0f * k;
	std::vector<UiVertex>& ui = f.ui;

	// Wordmark (vk-285-50: PS5SX2, "SX2" in the glow's colour) and the author's handles under it.
	float x = margin;
	x += m_fonts->AddText(ui, "PS5", x, 150.0f * k, 60.0f * k, white, 0.8f);
	m_fonts->AddText(ui, "SX2", x + 2.0f * k, 150.0f * k, 60.0f * k, accent, 0.8f);
	{
		const float hpx = 32.0f * k, hy2 = 214.0f * k;
		float hx2 = margin + 2.0f * k;
		const bool icons = m_fonts->Has(icon::Discord) && m_fonts->Has(icon::XTwitter);
		if (icons)
			hx2 += m_fonts->AddText(ui, icon::Discord, hx2, hy2 + 2.0f * k, hpx * 1.05f, faint) + 12.0f * k;
		hx2 += m_fonts->AddText(ui, kDiscordHandle, hx2, hy2, hpx, faint, 0.1f) + 40.0f * k;
		if (icons)
			hx2 += m_fonts->AddText(ui, icon::XTwitter, hx2, hy2 + 2.0f * k, hpx * 0.95f, faint) + 12.0f * k;
		m_fonts->AddText(ui, kXHandle, hx2, hy2, hpx, faint, 0.1f);
	}
	if (!clock.empty())
		m_fonts->AddText(ui, clock.c_str(), W - margin, 150.0f * k, 58.0f * k, white, 0.2f, Fonts::Right);

	// The settings page's QR tile, bottom right (vk-285-50). The code is dark on a light tile, as
	// cameras expect; neighbouring dark modules are merged into runs and grown by a pixel so the
	// rectangles' anti-aliased edges leave no seams between them. vk-285-51: smaller and dimmer (the
	// user's choice: "less"); it still scans from a metre or so, and a phone keeps the page's key
	// once it has opened it, so the code is mostly needed once.
	const bool qr_shown = m_qr_size > 0;
	if (qr_shown)
	{
		const float tile = 220.0f * k;
		const int quiet = 4; // the standard quiet zone
		const float mod = std::floor(tile / static_cast<float>(m_qr_size + 2 * quiet));
		const float side = mod * static_cast<float>(m_qr_size + 2 * quiet);
		const float tx = std::floor(W - margin - side), ty = std::floor(H - 150.0f * k - side);
		Fonts::AddRoundedRect(ui, tx, ty, side, side, 12.0f * k, Rgba(0.60f, 0.59f, 0.66f));
		const uint32_t ink = Rgba(0.07f, 0.06f, 0.16f);
		const float grow = 0.4f; // enough to close the seams, little enough to keep the modules their size
		for (int y = 0; y < m_qr_size; y++)
			for (int x0 = 0; x0 < m_qr_size;)
			{
				if (!m_qr[static_cast<size_t>(y) * m_qr_size + x0])
				{
					x0++;
					continue;
				}
				int x1 = x0;
				while (x1 < m_qr_size && m_qr[static_cast<size_t>(y) * m_qr_size + x1])
					x1++;
				Fonts::AddRoundedRect(ui, tx + (x0 + quiet) * mod - grow, ty + (y + quiet) * mod - grow,
					(x1 - x0) * mod + 2 * grow, mod + 2 * grow, 0.0f, ink);
				x0 = x1;
			}
		// vk-285-52: no caption, the user's call ("qr is enough"); 51 had "Settings on your phone" and
		// the address beside it.
	}
	else if (m_web_known)
		m_fonts->AddText(ui, "Game settings: no network", W - margin, H - 170.0f * k, 32.0f * k, faint, 0.1f, Fonts::Right);

	if (n > 0)
	{
		const GameInfo& g = m_games[static_cast<size_t>(m_selected)];
		// Title, shrunk to fit (narrower beside the QR tile, which sits bottom right).
		const float title_w = W * (qr_shown ? 0.72f : 0.8f);
		float px = 96.0f * k;
		while (px > 64.0f * k && m_fonts->Measure(g.title.c_str(), px) > title_w)
			px -= 4.0f * k;
		m_fonts->AddText(ui, g.title.c_str(), W * 0.5f, H * 0.842f, px, white, 0.55f, Fonts::Center);

		// Details and badges on one centred row.
		std::string info;
		auto add = [&](const std::string& s) {
			if (s.empty())
				return;
			if (!info.empty())
				info += "   \xC2\xB7   ";
			info += s;
		};
		add(g.serial);
		add(g.region);
		add(SizeText(g.bytes));
		const float info_px = 44.0f * k, badge_px = 36.0f * k;
		const float pad = 20.0f * k, gap = 16.0f * k;
		float row_w = m_fonts->Measure(info.c_str(), info_px);
		for (const std::string& b : g.badges)
			row_w += gap + m_fonts->Measure(b.c_str(), badge_px) + 2 * pad;
		float rx = W * 0.5f - row_w * 0.5f;
		const float row_y = H * 0.905f;
		rx += m_fonts->AddText(ui, info.c_str(), rx, row_y, info_px, dim, 0.1f);
		for (const std::string& b : g.badges)
		{
			const float bw = m_fonts->Measure(b.c_str(), badge_px) + 2 * pad;
			rx += gap;
			Fonts::AddRoundedRect(ui, rx, row_y - 40.0f * k, bw, 54.0f * k, 27.0f * k, Rgba(1, 1, 1, 0.12f));
			m_fonts->AddText(ui, b.c_str(), rx + pad, row_y - 2.0f * k, badge_px, white, 0.35f);
			rx += bw;
		}
	}
	else
		m_fonts->AddText(ui, "No games in /data/PCSX2/games", W * 0.5f, H * 0.5f, 64.0f * k, white, 0.4f, Fonts::Center);

	// Button hints.
	const float hy = H - 88.0f * k, ipx = 64.0f * k, tpx = 40.0f * k;
	float hx = margin;
	// A PromptFont glyph, or (names starting with '#') a shoulder button drawn as a small pill.
	auto key = [&](const char* name) {
		if (name[0] != '#')
		{
			hx += m_fonts->AddText(ui, name, hx, hy + 6.0f * k, ipx, white);
			return;
		}
		const float kpx = 30.0f * k, kw = m_fonts->Measure(name + 1, kpx) + 26.0f * k, kh = 44.0f * k;
		Fonts::AddRoundedRect(ui, hx, hy - 36.0f * k, kw, kh, 12.0f * k, Rgba(1, 1, 1, 0.9f));
		m_fonts->AddText(ui, name + 1, hx + kw * 0.5f, hy - 4.0f * k, kpx, Rgba(0.08f, 0.08f, 0.14f), 0.6f, Fonts::Center);
		hx += kw;
	};
	auto hint = [&](const char* key_a, const char* key_b, const char* label) {
		key(key_a);
		if (key_b)
		{
			hx += 8.0f * k;
			key(key_b);
		}
		hx += 16.0f * k;
		hx += m_fonts->AddText(ui, label, hx, hy, tpx, dim, 0.1f);
		hx += 56.0f * k;
	};
	hint(icon::Cross, nullptr, "Play");
	// vk-285-69: the shelf opens with one game too (its QR code leads to the settings page and the
	// logs); browsing needs two.
	if (n > 1)
	{
		hint(icon::DpadLeftRight, nullptr, "Browse");
		hint("#L1", "#R1", "Jump");
	}

	std::string status = m_covers ? m_covers->Status() : std::string();
	if (status.empty() && n > 0)
	{
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%d / %d", m_selected + 1, n);
		status = buf;
		if (!m_cfg.build_tag.empty())
			status = m_cfg.build_tag + "   \xC2\xB7   " + status;
	}
	m_fonts->AddText(ui, status.c_str(), W - margin, hy, 36.0f * k, faint, 0.1f, Fonts::Right);

	// Test build 1 (vk-285-55): testing builds say so across the middle of the shelf, over the
	// cases, with the build under it, so every photo and video of one names the build.
	if (m_cfg.test_build > 0)
	{
		const float px = 300.0f * k, sub_px = 64.0f * k;
		const float base = H * 0.5f + m_fonts->Ascent(px) * 0.36f;
		m_fonts->AddText(ui, "TESTING", W * 0.5f + 5.0f * k, base + 5.0f * k, px, Rgba(0, 0, 0, 0.30f), 0.95f, Fonts::Center);
		m_fonts->AddText(ui, "TESTING", W * 0.5f, base, px, Rgba(1, 1, 1, 0.42f), 0.95f, Fonts::Center);
		if (!m_cfg.build_label.empty())
		{
			const float sub_base = base + m_fonts->Descent(px) + sub_px * 1.1f;
			m_fonts->AddText(ui, m_cfg.build_label.c_str(), W * 0.5f + 3.0f * k, sub_base + 3.0f * k, sub_px, Rgba(0, 0, 0, 0.45f),
				0.5f, Fonts::Center);
			m_fonts->AddText(ui, m_cfg.build_label.c_str(), W * 0.5f, sub_base, sub_px, Rgba(1, 1, 1, 0.75f), 0.5f, Fonts::Center);
		}
	}
}
} // namespace fe
