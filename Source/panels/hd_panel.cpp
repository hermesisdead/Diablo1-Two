#include "panels/hd_panel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include "control/control.hpp"
#include "control/control_panel.hpp"
#include "engine/backbuffer_state.hpp"
#include "engine/load_cel.hpp"
#include "engine/palette.h"
#include "engine/render/clx_render.hpp"
#include "engine/render/color_overlay.hpp"
#include "engine/render/hd_sprite.hpp"
#include "player.h"
#include "utils/sdl_geometry.h"

namespace devilution {

namespace {

constexpr int PanelW = 640, PanelH = 128;
constexpr int DomeRows = 16; // the art starts this far above the panel (the orbs' tops)
constexpr int ArtH = PanelH + DomeRows;

std::shared_ptr<const HdSprite> Panel;
bool PanelTried = false;

const HdSprite *PanelArt()
{
	if (!PanelTried) {
		PanelTried = true;
		Panel = LoadHdSprite("ui_hd\\panel.hds");
		if (Panel != nullptr && (Panel->width != PanelW || Panel->height != ArtH || Panel->frames.empty() || Panel->frames[0].width != PanelW || Panel->frames[0].height != ArtH))
			Panel = nullptr;
	}
	return Panel.get();
}

/** Nearest interface colour (palette entries every level shares: 0 and 128..255), by 15-bit colour. */
uint8_t UiIndex(int r, int g, int b)
{
	static std::vector<uint8_t> lut;
	static std::array<SDL_Color, 256> builtFor {};
	if (lut.empty() || std::memcmp(builtFor.data(), logical_palette.data(), sizeof(builtFor)) != 0) {
		std::memcpy(builtFor.data(), logical_palette.data(), sizeof(builtFor));
		lut.assign(32768, 0);
		for (int i = 0; i < 32768; i++) {
			const int cr = ((i >> 10) & 31) * 255 / 31, cg = ((i >> 5) & 31) * 255 / 31, cb = (i & 31) * 255 / 31;
			int best = 0, bestD = INT32_MAX;
			for (int k = 127; k < 256; k++) {
				const int idx = k == 127 ? 0 : k;
				const SDL_Color &c = logical_palette[idx];
				const int dr = cr - c.r, dg = cg - c.g, db = cb - c.b;
				const int d = dr * dr * 3 + dg * dg * 6 + db * db;
				if (d < bestD) {
					bestD = d;
					best = idx;
				}
			}
			lut[i] = static_cast<uint8_t>(best);
		}
	}
	return lut[((std::clamp(r, 0, 255) >> 3) << 10) | ((std::clamp(g, 0, 255) >> 3) << 5) | (std::clamp(b, 0, 255) >> 3)];
}

/** An orb's liquid: shown level, sloshing, and the picture made from them this frame. */
struct Orb {
	int cx, cy, radius; // liquid, in the panel (cy can be above it: the orbs rise over its top)
	int glass;          // the glass ball's outer radius
	float level = -1;   // shown level (0..1)
	float target = 0;
	float slosh = 0;    // surface wave height, pixels
	float phase = 0;
	std::vector<uint8_t> pixels; // (2r+1)^2 * 5
};

Orb LifeOrb { 139, 25, 33, 44 };
Orb ManaOrb { 503, 26, 35, 45 };

struct Look {
	float top[3], deep[3];
};
const Look LifeLook { { 215, 28, 30 }, { 70, 0, 6 } };
const Look ManaLook { { 60, 95, 245 }, { 8, 10, 80 } };

void Animate(Orb &orb, float target, float dt)
{
	if (orb.level < 0)
		orb.level = target;
	const float change = target - orb.target;
	// A change sets it rocking (more for a bigger change), and it settles again.
	if (std::abs(change) > 0.001f)
		orb.slosh = std::min(orb.slosh + std::abs(change) * static_cast<float>(orb.radius) * 0.9f, static_cast<float>(orb.radius) * 0.16f);
	orb.target = target;
	orb.level += (target - orb.level) * std::min(1.0f, dt * 3.0f);
	orb.slosh *= std::exp(-dt * 1.1f);
	orb.phase += dt * 4.2f;
}

/** The art's pixel (r, g, b, alpha, index) at a point of the panel (y from the panel's top; negative: above it). */
const uint8_t *ArtPixel(int x, int y)
{
	const HdSprite *art = PanelArt();
	const int ay = y + DomeRows;
	if (x < 0 || x >= PanelW || ay < 0 || ay >= ArtH)
		return nullptr;
	return art->frames[0].pixels.data() + (static_cast<size_t>(ay) * PanelW + x) * 5;
}

void RenderOrb(const Surface &out, Orb &orb, const Look &look, bool warn)
{
	const Point panel = GetMainPanel().position;
	const int r = orb.radius, gr = orb.glass, size = 2 * gr + 1;
	orb.pixels.assign(static_cast<size_t>(size) * size * 5, 0);
	const float t = static_cast<float>(SDL_GetTicks() % 3600000u) / 1000.0f;
	const float rf = static_cast<float>(r);
	const float surfaceBase = rf - 2.0f * rf * orb.level; // from the centre, + is down
	const float calm = std::min(1.0f, orb.level * (1.0f - orb.level) * 8.0f);
	const float pulse = warn ? 0.5f + 0.5f * std::sin(t * 6.0f) : 0.0f;
	for (int y = -gr; y <= gr; y++) {
		for (int x = -gr; x <= gr; x++) {
			const uint8_t *g = ArtPixel(orb.cx + x, orb.cy + y);
			if (g == nullptr || g[3] == 0)
				continue; // see-through above the panel
			uint8_t *o = &orb.pixels[(static_cast<size_t>(y + gr) * size + (x + gr)) * 5];
			std::memcpy(o, g, 5); // the glass (and what is around it) as the art has it
			const Point screen = panel + Displacement { orb.cx + x, orb.cy + y };
			const float d2 = static_cast<float>(x * x + y * y) / (rf * rf);
			const float fx = static_cast<float>(x), fy = static_cast<float>(y);
			// The surface rocks (a tilt that swings back and forth) with a small ripple on it.
			const float surface = surfaceBase + orb.slosh * (fx / rf) * std::sin(orb.phase) + 0.7f * calm * std::sin(fx * 0.35f - t * 2.4f);
			const bool liquid = d2 <= 1.0f && fy >= surface && orb.level > 0.002f;
			if (!liquid) {
				if (out.InBounds(screen))
					*out.at(screen.x, screen.y) = g[4];
				continue;
			}
			// Deeper is darker; the ball's edge is in shadow; the meniscus catches the light.
			const float depth = std::clamp((fy - surface) / (2.0f * rf), 0.0f, 1.0f);
			const float shade = 0.42f + 0.72f * std::sqrt(std::max(0.0f, 1.0f - d2));
			float c[3];
			for (int k = 0; k < 3; k++)
				c[k] = (look.top[k] + (look.deep[k] - look.top[k]) * std::min(1.0f, depth * 1.4f)) * shade;
			if (fy - surface < 1.5f) {
				for (float &v : c)
					v += 60.0f * (1.5f - (fy - surface)) / 1.5f;
			}
			// Bubbles rising through it.
			for (int b = 0; b < 7; b++) {
				const float bx = std::sin(static_cast<float>(b) * 12.9898f) * rf * 0.6f + std::sin(t * 1.3f + static_cast<float>(b)) * 1.5f;
				const float speed = 6.0f + static_cast<float>(b % 3) * 4.0f;
				const float by = rf - std::fmod(t * speed + static_cast<float>(b) * 9.7f, rf * 2.0f);
				if (by < surface)
					continue;
				const float dd = (fx - bx) * (fx - bx) + (fy - by) * (fy - by);
				if (dd < 2.2f) {
					for (float &v : c)
						v += 50.0f * (1.0f - dd / 2.2f);
				}
			}
			if (warn) {
				c[0] += 50.0f * pulse;
				c[1] += 8.0f * pulse;
			}
			// The glass over it: its reflections stay on top (a screen blend of the art's highlights).
			const float lum = g[0] * 0.3f + g[1] * 0.59f + g[2] * 0.11f;
			const float hl = std::clamp((lum - 60.0f) / 150.0f, 0.0f, 1.0f) * 0.95f;
			for (float &v : c)
				v = std::clamp(v + (255.0f - v) * hl, 0.0f, 255.0f);
			const int ri = static_cast<int>(c[0]), gi = static_cast<int>(c[1]), bi = static_cast<int>(c[2]);
			uint8_t idx = UiIndex(ri, gi, bi);
			if (idx == 0 && orb.cy + y < 0)
				idx = UiIndex(ri + 8, gi + 8, bi + 8); // above the panel 0 would be see-through
			if (out.InBounds(screen))
				*out.at(screen.x, screen.y) = idx;
			o[0] = static_cast<uint8_t>(ri);
			o[1] = static_cast<uint8_t>(gi);
			o[2] = static_cast<uint8_t>(bi);
			o[3] = 255;
			o[4] = idx;
		}
	}
	ColorOverlayUiImage(out, panel + Displacement { orb.cx - gr, orb.cy - gr }, size, size, orb.pixels.data(), size * 5);
}

} // namespace

bool HdPanelActive()
{
	return NewTexturesActive() && PanelArt() != nullptr;
}

void ApplyPanelTextures()
{
	if (!BottomBuffer)
		return;
	const HdSprite *art = HdPanelActive() ? PanelArt() : nullptr;
	if (art != nullptr) {
		// The art (with the orbs' tops above the panel) over the panel buffer's first panel.
		const uint8_t *px = art->frames[0].pixels.data();
		for (int y = 0; y < ArtH; y++) {
			const int by = PanelPaddingHeight - DomeRows + y;
			if (by < 0 || by >= BottomBuffer->h())
				continue;
			uint8_t *row = BottomBuffer->at(0, by);
			for (int x = 0; x < PanelW; x++)
				row[x] = px[(static_cast<size_t>(y) * PanelW + x) * 5 + 4];
		}
	} else {
		const OwnedClxSpriteList sprite = LoadCel("ctrlpan\\panel8", PanelW);
		ClxDraw(*BottomBuffer, { 0, (PanelH + PanelPaddingHeight) - 1 }, sprite[0]);
	}
	RedrawEverything();
}

void DrawHdPanelColour(const Surface &out)
{
	if (!HdPanelActive())
		return;
	const HdSprite *art = PanelArt();
	ColorOverlayUiImage(out, GetMainPanel().position, PanelW, PanelH, art->frames[0].pixels.data() + static_cast<size_t>(DomeRows) * PanelW * 5, PanelW * 5);
}

bool DrawHdOrb(const Surface &out, bool life)
{
	if (!HdPanelActive() || MyPlayer == nullptr)
		return false;
	const uint32_t now = SDL_GetTicks();
	Orb &orb = life ? LifeOrb : ManaOrb;
	const Player &player = *MyPlayer;
	float target;
	if (life) {
		target = player._pMaxHP > 0 ? static_cast<float>(std::max(0, player._pHitPoints.whole())) / static_cast<float>(std::max(1, player._pMaxHP.whole())) : 0.0f;
	} else {
		const bool noMana = HasAnyOf(player._pIFlags, ItemSpecialEffect::NoMana) || player.hasNoMana();
		target = noMana || player._pMaxMana <= 0 ? 0.0f : static_cast<float>(std::max(0, player._pMana.whole())) / static_cast<float>(std::max(1, player._pMaxMana.whole()));
	}
	target = std::clamp(target, 0.0f, 1.0f);
#ifdef _DEBUG
	// Test hook: DEVILUTIONX_DEV_ORBTEST = the orbs drain and refill in steps (shows levels and sloshing).
	if (std::getenv("DEVILUTIONX_DEV_ORBTEST") != nullptr) {
		const int step = static_cast<int>(now / 1500u) % 8;
		static constexpr float Levels[8] { 0.9f, 0.7f, 0.55f, 0.35f, 0.2f, 0.12f, 0.6f, 1.0f };
		target = life ? Levels[step] : Levels[(step + 3) % 8];
	}
#endif
	static uint32_t lastLife = 0, lastMana = 0;
	uint32_t &last = life ? lastLife : lastMana;
	const float dt = last == 0 ? 0.0f : std::min(0.1f, static_cast<float>(now - last) / 1000.0f);
	last = now;
	Animate(orb, target, dt);
	RenderOrb(out, orb, life ? LifeLook : ManaLook, life && target < 0.25f && target > 0.0f);
	return true;
}

} // namespace devilution
