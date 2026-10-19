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
#include "engine/render/hd_layer.hpp"
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
	HdImage hires;               // the same at the window's resolution
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

/** What the liquid looks like at a point of the orb (from its centre, in game pixels): false above the surface. */
bool LiquidAt(const Orb &orb, const Look &look, float fx, float fy, float t, bool warn, float glassLum, float c[3])
{
	const float rf = static_cast<float>(orb.radius);
	const float d2 = (fx * fx + fy * fy) / (rf * rf);
	if (d2 > 1.0f || orb.level <= 0.002f)
		return false;
	const float calm = std::min(1.0f, orb.level * (1.0f - orb.level) * 8.0f);
	// The surface rocks (a tilt that swings back and forth) with a small ripple on it.
	const float surface = rf - 2.0f * rf * orb.level + orb.slosh * (fx / rf) * std::sin(orb.phase) + 0.7f * calm * std::sin(fx * 0.35f - t * 2.4f);
	if (fy < surface)
		return false;
	// Deeper is darker; the ball's edge is in shadow; the meniscus catches the light.
	const float depth = std::clamp((fy - surface) / (2.0f * rf), 0.0f, 1.0f);
	const float shade = 0.42f + 0.72f * std::sqrt(std::max(0.0f, 1.0f - d2));
	for (int k = 0; k < 3; k++)
		c[k] = (look.top[k] + (look.deep[k] - look.top[k]) * std::min(1.0f, depth * 1.4f)) * shade;
	if (fy - surface < 1.5f) {
		for (int k = 0; k < 3; k++)
			c[k] += 60.0f * (1.5f - (fy - surface)) / 1.5f;
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
			for (int k = 0; k < 3; k++)
				c[k] += 50.0f * (1.0f - dd / 2.2f);
		}
	}
	if (warn) {
		const float pulse = 0.5f + 0.5f * std::sin(t * 6.0f);
		c[0] += 50.0f * pulse;
		c[1] += 8.0f * pulse;
	}
	// The glass over it: its reflections stay on top (a screen blend of the art's highlights).
	const float hl = std::clamp((glassLum - 60.0f) / 150.0f, 0.0f, 1.0f) * 0.95f;
	for (int k = 0; k < 3; k++)
		c[k] = std::clamp(c[k] + (255.0f - c[k]) * hl, 0.0f, 255.0f);
	return true;
}

std::shared_ptr<const HdHiresSprite> PanelHi;
bool PanelHiTried = false;

const HdImage *PanelHiImage()
{
	if (!PanelHiTried) {
		PanelHiTried = true;
		PanelHi = LoadHdHiresSprite("ui_hd\\panel.hdr");
		if (PanelHi != nullptr && (PanelHi->frames.empty() || PanelHi->canvasW != PanelW || PanelHi->canvasH != ArtH))
			PanelHi = nullptr;
	}
	return PanelHi != nullptr ? &PanelHi->frames[0].image : nullptr;
}

/** The high-resolution art at a point of the panel art (game pixels from its top-left, the orbs' tops included). */
void HiArt(float ax, float ay, float out[4])
{
	const HdImage &img = *PanelHiImage();
	const float u = std::clamp(ax * static_cast<float>(img.width) / static_cast<float>(PanelW) - 0.5f, 0.0f, static_cast<float>(img.width - 1));
	const float v = std::clamp(ay * static_cast<float>(img.height) / static_cast<float>(ArtH) - 0.5f, 0.0f, static_cast<float>(img.height - 1));
	const int x0 = static_cast<int>(u), y0 = static_cast<int>(v);
	const int x1 = std::min(x0 + 1, img.width - 1), y1 = std::min(y0 + 1, img.height - 1);
	const float fx = u - static_cast<float>(x0), fy = v - static_cast<float>(y0);
	for (int k = 0; k < 4; k++) {
		const auto at = [&](int x, int y) { return static_cast<float>(img.rgba[(static_cast<size_t>(y) * img.width + x) * 4 + k]); };
		out[k] = at(x0, y0) * (1 - fx) * (1 - fy) + at(x1, y0) * fx * (1 - fy) + at(x0, y1) * (1 - fx) * fy + at(x1, y1) * fx * fy;
	}
}

std::vector<int16_t> PanelMask; // the panel art's 256-colour pixels (what must be there for it to show)

void RenderOrb(const Surface &out, Orb &orb, const Look &look, bool warn)
{
	const Point panel = GetMainPanel().position;
	const int gr = orb.glass, size = 2 * gr + 1;
	orb.pixels.assign(static_cast<size_t>(size) * size * 5, 0);
	std::vector<int16_t> mask(static_cast<size_t>(size) * size, -1);
	const float t = static_cast<float>(SDL_GetTicks() % 3600000u) / 1000.0f;
	for (int y = -gr; y <= gr; y++) {
		for (int x = -gr; x <= gr; x++) {
			const uint8_t *g = ArtPixel(orb.cx + x, orb.cy + y);
			if (g == nullptr || g[3] == 0)
				continue; // see-through above the panel
			uint8_t *o = &orb.pixels[(static_cast<size_t>(y + gr) * size + (x + gr)) * 5];
			std::memcpy(o, g, 5); // the glass (and what is around it) as the art has it
			const Point screen = panel + Displacement { orb.cx + x, orb.cy + y };
			float c[3];
			const float lum = g[0] * 0.3f + g[1] * 0.59f + g[2] * 0.11f;
			uint8_t idx = g[4];
			if (LiquidAt(orb, look, static_cast<float>(x), static_cast<float>(y), t, warn, lum, c)) {
				const int ri = static_cast<int>(c[0]), gi = static_cast<int>(c[1]), bi = static_cast<int>(c[2]);
				idx = UiIndex(ri, gi, bi);
				if (idx == 0 && orb.cy + y < 0)
					idx = UiIndex(ri + 8, gi + 8, bi + 8); // above the panel 0 would be see-through
				o[0] = static_cast<uint8_t>(ri);
				o[1] = static_cast<uint8_t>(gi);
				o[2] = static_cast<uint8_t>(bi);
				o[3] = 255;
				o[4] = idx;
			}
			if (out.InBounds(screen))
				*out.at(screen.x, screen.y) = idx;
			mask[static_cast<size_t>(y + gr) * size + (x + gr)] = idx;
		}
	}
	ColorOverlayUiImage(out, panel + Displacement { orb.cx - gr, orb.cy - gr }, size, size, orb.pixels.data(), size * 5);

	// The same at the window's resolution, for the HD layer.
	if (!HdLayerActive() || PanelHiImage() == nullptr)
		return;
	float sx, sy;
	HdLayerScale(sx, sy);
	const int hw = std::max(1, static_cast<int>(std::ceil(static_cast<float>(size) * sx)));
	const int hh = std::max(1, static_cast<int>(std::ceil(static_cast<float>(size) * sy)));
	orb.hires.width = hw;
	orb.hires.height = hh;
	orb.hires.rgba.assign(static_cast<size_t>(hw) * hh * 4, 0);
	for (int py = 0; py < hh; py++) {
		for (int px = 0; px < hw; px++) {
			// From the orb's centre, in game pixels.
			const float fx = (static_cast<float>(px) + 0.5f) / sx - static_cast<float>(gr) - 0.5f;
			const float fy = (static_cast<float>(py) + 0.5f) / sy - static_cast<float>(gr) - 0.5f;
			float art[4];
			HiArt(static_cast<float>(orb.cx) + fx + 0.5f, static_cast<float>(orb.cy + DomeRows) + fy + 0.5f, art);
			if (art[3] < 1.0f)
				continue;
			uint8_t *o = &orb.hires.rgba[(static_cast<size_t>(py) * hw + px) * 4];
			float c[3];
			const float lum = art[0] * 0.3f + art[1] * 0.59f + art[2] * 0.11f;
			if (LiquidAt(orb, look, fx, fy, t, warn, lum, c)) {
				o[0] = static_cast<uint8_t>(c[0]);
				o[1] = static_cast<uint8_t>(c[1]);
				o[2] = static_cast<uint8_t>(c[2]);
			} else {
				o[0] = static_cast<uint8_t>(art[0]);
				o[1] = static_cast<uint8_t>(art[1]);
				o[2] = static_cast<uint8_t>(art[2]);
			}
			o[3] = static_cast<uint8_t>(art[3]);
		}
	}
	const Point corner = panel + Displacement { orb.cx - gr, orb.cy - gr };
	HdLayerAdd(out, &orb.hires, static_cast<float>(corner.x), static_cast<float>(corner.y), static_cast<float>(size), static_cast<float>(size),
	    corner.x, corner.y, size, size, std::move(mask), /*tint=*/false, /*cache=*/false);
}

} // namespace

bool HdPanelActive()
{
	return HudTexturesActive() && PanelArt() != nullptr;
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
	if (HdLayerActive() && PanelHiImage() != nullptr) {
		if (PanelMask.empty()) {
			PanelMask.assign(static_cast<size_t>(PanelW) * ArtH, -1);
			const uint8_t *px = art->frames[0].pixels.data();
			for (size_t i = 0; i < PanelMask.size(); i++) {
				if (px[i * 5 + 3] != 0)
					PanelMask[i] = px[i * 5 + 4];
			}
		}
		// The whole panel at the window's resolution (the orbs' tops come with the orbs).
		const Point at = GetMainPanel().position + Displacement { 0, -DomeRows };
		HdLayerAdd(out, PanelHiImage(), static_cast<float>(at.x), static_cast<float>(at.y), static_cast<float>(PanelW), static_cast<float>(ArtH),
		    at.x, at.y, PanelW, ArtH, PanelMask, /*tint=*/false, /*cache=*/true);
	}
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
