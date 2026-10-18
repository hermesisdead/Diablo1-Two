#include "engine/render/color_overlay.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>
#endif

#include "engine/dx.h"
#include "engine/palette.h"
#include "headless_mode.hpp"
#include "options.h"
#include "utils/display.h"
#include "utils/log.hpp"

namespace devilution {

namespace {

struct Wash {
	OverlayColor color;
	uint8_t alpha;
};

struct Layer {
	SDL_Rect rect {};     // region of the back buffer the layer covers
	bool recording = false;
	bool finished = false; // world snapshot taken: ready to composite
	bool inFrame = false;  // between ColorOverlayBeginFrame and ColorOverlayEndFrame
	bool shown = false;    // composited onto the output during this frame

	Wash pre { { 0, 0, 0 }, 0 };
	Wash post { { 0, 0, 0 }, 0 };

	bool lightUsed = false;
	std::vector<uint32_t> light; // per-pixel multiplier per channel, 128 = 1.0

	OverlayColor fogColor { 0, 0, 0 };
	bool fogUsed = false;
	std::vector<uint8_t> fog; // coverage 0..255

	// Particles, premultiplied: r, g, b, coverage.
	std::vector<uint32_t> particles;
	int pMinX = 0, pMinY = 0, pMaxX = -1, pMaxY = -1; // touched area (inclusive)

	std::vector<uint8_t> snapshot; // the world's 8-bit pixels when it was finished

	// Floor decals and the floor as it was before anything stood on it.
	std::vector<uint32_t> decals; // premultiplied like particles
	int dMinX = 0, dMinY = 0, dMaxX = -1, dMaxY = -1;
	std::vector<uint8_t> floor;
	int floorW = 0, floorH = 0, floorScale = 1, floorOffX = 0;
	int floorX0 = 0, floorY0 = 0; // where the floor surface starts in the back buffer

	// Decal blend modes: multiply (128 = 1.0 per channel) and gloss (added light).
	std::vector<uint32_t> decalMul;
	std::vector<uint32_t> decalGloss;

	// Water tiles, in floor-surface pixels (before zooming).
	std::vector<uint8_t> water;
	int waterW = 0, waterH = 0;
	bool waterUsed = false;
	std::vector<int> shore; // per column: the last row above that was not water

	// Full-colour sprite pixels (premultiplied) and the 8-bit pixel each expects to find.
	std::vector<uint32_t> sprites;
	std::vector<uint8_t> spriteExpect;
	int sMinX = 0, sMinY = 0, sMaxX = -1, sMaxY = -1;
	bool floorValid = false;
};

Layer L;

struct UiImage {
	int x, y, w, h; // back buffer pixels
	const uint8_t *pixels;
	int stride;
};
std::vector<UiImage> UiImages;

constexpr uint32_t Pack(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
	return r | (g << 8) | (b << 16) | (a << 24);
}

uint8_t ToByte(float v)
{
	return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

/** x * a / 255, rounded. */
constexpr uint32_t Mul(uint32_t x, uint32_t a)
{
	const uint32_t t = x * a + 128;
	return (t + (t >> 8)) >> 8;
}

bool InLayer(int x, int y)
{
	return x >= 0 && y >= 0 && x < L.rect.w && y < L.rect.h;
}

void TouchParticles(int x, int y)
{
	if (L.pMaxX < L.pMinX) {
		L.pMinX = L.pMaxX = x;
		L.pMinY = L.pMaxY = y;
		return;
	}
	L.pMinX = std::min(L.pMinX, x);
	L.pMaxX = std::max(L.pMaxX, x);
	L.pMinY = std::min(L.pMinY, y);
	L.pMaxY = std::max(L.pMaxY, y);
}

struct PixelLayout {
	int bytes;
	int rShift, gShift, bShift;
};

bool GetLayout(const SDL_Surface *s, PixelLayout &out)
{
	if (s == nullptr)
		return false;
#ifdef USE_SDL3
	const SDL_PixelFormatDetails *d = SDL_GetPixelFormatDetails(s->format);
	if (d == nullptr)
		return false;
	out = { d->bytes_per_pixel, d->Rshift, d->Gshift, d->Bshift };
	return d->bytes_per_pixel == 4 && d->Rbits == 8 && d->Gbits == 8 && d->Bbits == 8;
#elif defined(USE_SDL1)
	return false;
#else
	const SDL_PixelFormat *f = s->format;
	out = { f->BytesPerPixel, f->Rshift, f->Gshift, f->Bshift };
	return f->BytesPerPixel == 4 && f->Rloss == 0 && f->Gloss == 0 && f->Bloss == 0;
#endif
}

} // namespace

OverlayColor OverlayPaletteColor(uint8_t index)
{
	const SDL_Color &c = system_palette[index];
	return { c.r, c.g, c.b };
}

bool ColorOverlayAvailable()
{
#ifdef USE_SDL1
	return false;
#else
	static int lastReason = -1;
	auto unavailable = [](int reason) {
		if (reason != lastReason)
			LogVerbose("Colour overlay unavailable ({}), effects use palette blending", reason);
		lastReason = reason;
		return false;
	};
	if (HeadlessMode || PalSurface == nullptr)
		return unavailable(0);
	if (!*GetOptions().Graphics.smoothEffects)
		return unavailable(4);
	if (RenderDirectlyToOutputSurface)
		return unavailable(1);
	const SDL_Surface *out = GetOutputSurface();
	PixelLayout layout;
	if (!GetLayout(out, layout))
		return unavailable(2);
	if (out->w < PalSurface->w || out->h < PalSurface->h)
		return unavailable(3);
	if (lastReason != 100)
		LogVerbose("Colour overlay available");
	lastReason = 100;
	return true;
#endif
}

void ColorOverlayBeginFrame()
{
	UiImages.clear();
	L.inFrame = true;
	L.recording = false;
	L.finished = false;
	L.shown = false;
}

void ColorOverlayEndFrame()
{
	L.inFrame = false;
	L.recording = false;
}

bool ColorOverlayBegin(const Surface &region)
{
	L.recording = false;
	L.finished = false;
	if (!L.inFrame || region.surface != PalSurface || !ColorOverlayAvailable())
		return false;
	const SDL_Rect rect { region.region.x, region.region.y, region.w(), region.h() };
	if (rect.w <= 0 || rect.h <= 0)
		return false;
	const size_t size = static_cast<size_t>(rect.w) * static_cast<size_t>(rect.h);
	if (rect.w != L.rect.w || rect.h != L.rect.h || L.particles.size() != size) {
		L.fog.assign(size, 0);
		L.light.assign(size, 0x808080u);
		L.particles.assign(size, 0);
		L.decals.assign(size, 0);
		L.snapshot.assign(size, 0);
		L.sprites.assign(size, 0);
		L.decalMul.assign(size, 0x808080u);
		L.decalGloss.assign(size, 0);
		L.spriteExpect.assign(size, 0);
		L.sMinX = L.sMinY = 0;
		L.sMaxX = L.sMaxY = -1;
		L.dMinX = L.dMinY = 0;
		L.dMaxX = L.dMaxY = -1;
		L.pMinX = L.pMinY = 0;
		L.pMaxX = L.pMaxY = -1;
	} else {
		if (L.fogUsed)
			std::fill(L.fog.begin(), L.fog.end(), 0);
		if (L.lightUsed)
			std::fill(L.light.begin(), L.light.end(), 0x808080u);
		if (L.pMaxX >= L.pMinX) {
			for (int y = L.pMinY; y <= L.pMaxY; y++)
				std::fill_n(&L.particles[static_cast<size_t>(y) * rect.w + L.pMinX], L.pMaxX - L.pMinX + 1, 0u);
		}
		L.pMinX = L.pMinY = 0;
		L.pMaxX = L.pMaxY = -1;
		if (L.dMaxX >= L.dMinX) {
			for (int y = L.dMinY; y <= L.dMaxY; y++) {
				const size_t row = static_cast<size_t>(y) * rect.w + L.dMinX;
				const size_t n = static_cast<size_t>(L.dMaxX - L.dMinX + 1);
				std::fill_n(&L.decals[row], n, 0u);
				std::fill_n(&L.decalMul[row], n, 0x808080u);
				std::fill_n(&L.decalGloss[row], n, 0u);
			}
		}
		L.dMinX = L.dMinY = 0;
		L.dMaxX = L.dMaxY = -1;
		if (L.sMaxX >= L.sMinX) {
			for (int y = L.sMinY; y <= L.sMaxY; y++)
				std::fill_n(&L.sprites[static_cast<size_t>(y) * rect.w + L.sMinX], L.sMaxX - L.sMinX + 1, 0u);
		}
		L.sMinX = L.sMinY = 0;
		L.sMaxX = L.sMaxY = -1;
	}
	L.floorValid = false;
	if (L.waterUsed)
		std::fill(L.water.begin(), L.water.end(), uint8_t { 0 });
	L.waterUsed = false;
	L.rect = rect;
	L.pre = { { 0, 0, 0 }, 0 };
	L.post = { { 0, 0, 0 }, 0 };
	L.fogUsed = false;
	L.lightUsed = false;
	L.recording = true;
	return true;
}

bool ColorOverlayRecording()
{
	return L.recording;
}

void ColorOverlayWash(OverlayColor color, float alpha, bool last)
{
	if (!L.recording)
		return;
	Wash &w = last ? L.post : L.pre;
	const uint8_t a = ToByte(alpha);
	if (a == 0)
		return;
	if (w.alpha == 0) {
		w = { color, a };
		return;
	}
	// Combine two washes into one (the second goes over the first).
	const float a1 = w.alpha / 255.0f, a2 = a / 255.0f;
	const float out = a2 + a1 * (1 - a2);
	auto mix = [&](uint8_t c1, uint8_t c2) {
		return static_cast<uint8_t>(std::clamp((c2 * a2 + c1 * a1 * (1 - a2)) / out, 0.0f, 255.0f));
	};
	w = { { mix(w.color.r, color.r), mix(w.color.g, color.g), mix(w.color.b, color.b) }, ToByte(out) };
}

void ColorOverlayMulLight(int x, int y, float r, float g, float b)
{
	if (!L.recording || !InLayer(x, y))
		return;
	uint32_t &cur = L.light[static_cast<size_t>(y) * L.rect.w + x];
	auto q = [](uint32_t old, float v) { return static_cast<uint32_t>(std::clamp(static_cast<float>(old) * v + 0.5f, 0.0f, 255.0f)); };
	const uint32_t packed = q(cur & 0xFF, r) | (q((cur >> 8) & 0xFF, g) << 8) | (q((cur >> 16) & 0xFF, b) << 16);
	if (packed == cur)
		return;
	L.lightUsed = true;
	cur = packed;
}

void ColorOverlayFloorSnapshot(const Surface &floor, int scale, int offsetX)
{
	if (!L.recording || floor.surface != PalSurface)
		return;
	L.floorW = floor.w();
	L.floorH = floor.h();
	L.floorScale = std::max(1, scale);
	L.floorOffX = offsetX;
	L.floorX0 = floor.region.x;
	L.floorY0 = floor.region.y;
	L.floor.resize(static_cast<size_t>(L.floorW) * L.floorH);
	for (int y = 0; y < L.floorH; y++)
		std::memcpy(&L.floor[static_cast<size_t>(y) * L.floorW], floor.at(0, y), L.floorW);
	L.floorValid = true;
}

void ColorOverlayDecal(int x, int y, OverlayColor color, float alpha)
{
	if (!L.recording || !InLayer(x, y))
		return;
	const uint32_t a = ToByte(alpha);
	if (a == 0)
		return;
	if (L.dMaxX < L.dMinX) {
		L.dMinX = L.dMaxX = x;
		L.dMinY = L.dMaxY = y;
	} else {
		L.dMinX = std::min(L.dMinX, x);
		L.dMaxX = std::max(L.dMaxX, x);
		L.dMinY = std::min(L.dMinY, y);
		L.dMaxY = std::max(L.dMaxY, y);
	}
	uint32_t &p = L.decals[static_cast<size_t>(y) * L.rect.w + x];
	const uint32_t inv = 255 - a;
	const uint32_t r = Mul(color.r, a) + Mul(p & 0xFF, inv);
	const uint32_t g = Mul(color.g, a) + Mul((p >> 8) & 0xFF, inv);
	const uint32_t b = Mul(color.b, a) + Mul((p >> 16) & 0xFF, inv);
	const uint32_t na = a + Mul(p >> 24, inv);
	p = Pack(std::min(r, 255u), std::min(g, 255u), std::min(b, 255u), std::min(na, 255u));
}

namespace {

void TouchDecals(int x, int y)
{
	if (L.dMaxX < L.dMinX) {
		L.dMinX = L.dMaxX = x;
		L.dMinY = L.dMaxY = y;
	} else {
		L.dMinX = std::min(L.dMinX, x);
		L.dMaxX = std::max(L.dMaxX, x);
		L.dMinY = std::min(L.dMinY, y);
		L.dMaxY = std::max(L.dMaxY, y);
	}
}

} // namespace

void ColorOverlayDecalMultiply(int x, int y, float r, float g, float b)
{
	if (!L.recording || !InLayer(x, y))
		return;
	TouchDecals(x, y);
	uint32_t &cur = L.decalMul[static_cast<size_t>(y) * L.rect.w + x];
	auto q = [](uint32_t old, float v) { return static_cast<uint32_t>(std::clamp(static_cast<float>(old) * v + 0.5f, 0.0f, 255.0f)); };
	cur = q(cur & 0xFF, r) | (q((cur >> 8) & 0xFF, g) << 8) | (q((cur >> 16) & 0xFF, b) << 16);
}

void ColorOverlayDecalGloss(int x, int y, OverlayColor color, float amount)
{
	if (!L.recording || !InLayer(x, y))
		return;
	const uint32_t k = ToByte(amount);
	if (k == 0)
		return;
	TouchDecals(x, y);
	uint32_t &p = L.decalGloss[static_cast<size_t>(y) * L.rect.w + x];
	const uint32_t r = std::min(255u, (p & 0xFF) + Mul(color.r, k));
	const uint32_t g = std::min(255u, ((p >> 8) & 0xFF) + Mul(color.g, k));
	const uint32_t b = std::min(255u, ((p >> 16) & 0xFF) + Mul(color.b, k));
	p = Pack(r, g, b, 0);
}

void ColorOverlayWaterTile(const Surface &floor, Point position)
{
	if (!L.recording || floor.surface != PalSurface || !*GetOptions().Graphics.waterEffects)
		return;
	if (L.waterW != floor.w() || L.waterH != floor.h() || L.water.empty()) {
		L.waterW = floor.w();
		L.waterH = floor.h();
		L.water.assign(static_cast<size_t>(L.waterW) * L.waterH, 0);
	}
	// The tile's lowest layers: 64 wide and 32 tall, up from its bottom row (position.y - 1).
	for (int row = 0; row < 32; row++) {
		const int y = position.y - 32 + row;
		if (y < 0 || y >= L.waterH)
			continue;
		const int x0 = std::max(0, position.x), x1 = std::min(L.waterW, position.x + 64);
		if (x0 < x1) {
			std::fill(&L.water[static_cast<size_t>(y) * L.waterW + x0], &L.water[static_cast<size_t>(y) * L.waterW + x1], uint8_t { 1 });
			L.waterUsed = true;
		}
	}
}

void ColorOverlaySpritePixel(const Surface &out, Point p, OverlayColor color, uint8_t alpha, uint8_t expect)
{
	if (!L.recording || alpha == 0 || out.surface != PalSurface)
		return;
	// Layer coordinates; a zoomed view is drawn at half size and doubled afterwards.
	int scale = 1, lx, ly;
	if (L.floorValid && L.floorScale > 1) {
		scale = L.floorScale;
		lx = L.floorOffX + (out.region.x + p.x - L.floorX0) * scale + L.floorX0 - L.rect.x;
		ly = (out.region.y + p.y - L.floorY0) * scale + L.floorY0 - L.rect.y;
	} else {
		lx = out.region.x + p.x - L.rect.x;
		ly = out.region.y + p.y - L.rect.y;
	}
	const uint32_t a = alpha;
	const uint32_t value = Pack(Mul(color.r, a), Mul(color.g, a), Mul(color.b, a), a);
	for (int sy = 0; sy < scale; sy++) {
		for (int sx = 0; sx < scale; sx++) {
			const int x = lx + sx, y = ly + sy;
			if (!InLayer(x, y))
				continue;
			if (L.sMaxX < L.sMinX) {
				L.sMinX = L.sMaxX = x;
				L.sMinY = L.sMaxY = y;
			} else {
				L.sMinX = std::min(L.sMinX, x);
				L.sMaxX = std::max(L.sMaxX, x);
				L.sMinY = std::min(L.sMinY, y);
				L.sMaxY = std::max(L.sMaxY, y);
			}
			const size_t i = static_cast<size_t>(y) * L.rect.w + x;
			L.sprites[i] = value; // the one drawn last is in front
			L.spriteExpect[i] = expect;
		}
	}
}

void ColorOverlayUiImage(const Surface &out, Point topLeft, int width, int height, const uint8_t *pixels, int stride)
{
	if (!L.inFrame || out.surface != PalSurface || pixels == nullptr || !ColorOverlayAvailable())
		return;
	UiImages.push_back({ out.region.x + topLeft.x, out.region.y + topLeft.y, width, height, pixels, stride });
}

void ColorOverlaySetFogColor(OverlayColor color)
{
	L.fogColor = color;
}

void ColorOverlaySetFog(int x, int y, float alpha)
{
	if (!L.recording || !InLayer(x, y))
		return;
	L.fogUsed = true;
	L.fog[static_cast<size_t>(y) * L.rect.w + x] = ToByte(alpha);
}

void ColorOverlayAddFog(int x, int y, float alpha)
{
	if (!L.recording || !InLayer(x, y))
		return;
	L.fogUsed = true;
	uint8_t &f = L.fog[static_cast<size_t>(y) * L.rect.w + x];
	const uint32_t a = ToByte(alpha);
	f = static_cast<uint8_t>(a + Mul(f, 255 - a));
}

void ColorOverlayOver(int x, int y, OverlayColor color, float alpha)
{
	if (!L.recording || !InLayer(x, y))
		return;
	const uint32_t a = ToByte(alpha);
	if (a == 0)
		return;
	TouchParticles(x, y);
	uint32_t &p = L.particles[static_cast<size_t>(y) * L.rect.w + x];
	const uint32_t inv = 255 - a;
	const uint32_t r = Mul(color.r, a) + Mul(p & 0xFF, inv);
	const uint32_t g = Mul(color.g, a) + Mul((p >> 8) & 0xFF, inv);
	const uint32_t b = Mul(color.b, a) + Mul((p >> 16) & 0xFF, inv);
	const uint32_t na = a + Mul(p >> 24, inv);
	p = Pack(std::min(r, 255u), std::min(g, 255u), std::min(b, 255u), std::min(na, 255u));
}

void ColorOverlayAdd(int x, int y, OverlayColor color, float amount)
{
	if (!L.recording || !InLayer(x, y))
		return;
	const uint32_t k = ToByte(amount);
	if (k == 0)
		return;
	TouchParticles(x, y);
	uint32_t &p = L.particles[static_cast<size_t>(y) * L.rect.w + x];
	const uint32_t r = std::min(255u, (p & 0xFF) + Mul(color.r, k));
	const uint32_t g = std::min(255u, ((p >> 8) & 0xFF) + Mul(color.g, k));
	const uint32_t b = std::min(255u, ((p >> 16) & 0xFF) + Mul(color.b, k));
	p = Pack(r, g, b, p >> 24);
}

void ColorOverlayFinish()
{
	if (!L.recording)
		return;
	L.recording = false;
	for (int y = 0; y < L.rect.h; y++) {
		const uint8_t *row = static_cast<const uint8_t *>(PalSurface->pixels) + static_cast<size_t>(L.rect.y + y) * PalSurface->pitch + L.rect.x;
		std::memcpy(&L.snapshot[static_cast<size_t>(y) * L.rect.w], row, L.rect.w);
	}
	L.finished = true;
}

void ColorOverlayComposite(SDL_Surface *src, const SDL_Rect *srcRect, SDL_Surface *dst, const SDL_Rect *dstRect)
{
	if (!L.inFrame || !L.finished || src != PalSurface || dst == nullptr)
		return;
	PixelLayout layout;
	if (!GetLayout(dst, layout))
		return;
	const bool water = L.waterUsed && L.floorValid && L.waterW == L.floorW && L.waterH == L.floorH;
	if (!UiImages.empty()) {
		// Interface images: anywhere on the frame, where their 256-colour pixels are still there.
		const SDL_Rect s = srcRect != nullptr ? *srcRect : SDL_Rect { 0, 0, src->w, src->h };
		const int ox = (dstRect != nullptr ? dstRect->x : 0) - s.x;
		const int oy = (dstRect != nullptr ? dstRect->y : 0) - s.y;
		const int rs = layout.rShift, gs = layout.gShift, bs = layout.bShift;
		const uint32_t keep = ~((0xFFu << rs) | (0xFFu << gs) | (0xFFu << bs));
		for (const UiImage &img : UiImages) {
			const int ix0 = std::max({ img.x, s.x, 0 }), ix1 = std::min({ img.x + img.w, s.x + s.w, src->w });
			const int iy0 = std::max({ img.y, s.y, 0 }), iy1 = std::min({ img.y + img.h, s.y + s.h, src->h });
			for (int y = iy0; y < iy1; y++) {
				const int dy = y + oy;
				if (dy < 0 || dy >= dst->h)
					continue;
				const uint8_t *pal = static_cast<const uint8_t *>(src->pixels) + static_cast<size_t>(y) * src->pitch;
				const uint8_t *px = img.pixels + static_cast<size_t>(y - img.y) * img.stride;
				auto *out = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(dst->pixels) + static_cast<size_t>(dy) * dst->pitch);
				for (int x = ix0; x < ix1; x++) {
					const uint8_t *p = px + static_cast<size_t>(x - img.x) * 5;
					if (p[3] == 0 || pal[x] != p[4])
						continue;
					const int dx = x + ox;
					if (dx < 0 || dx >= dst->w)
						continue;
					uint32_t r = p[0], g = p[1], b = p[2];
					if (p[3] != 255) {
						const uint32_t o = out[dx], a = p[3];
						r = Mul(r, a) + Mul((o >> rs) & 0xFF, 255 - a);
						g = Mul(g, a) + Mul((o >> gs) & 0xFF, 255 - a);
						b = Mul(b, a) + Mul((o >> bs) & 0xFF, 255 - a);
					}
					out[dx] = (out[dx] & keep) | (r << rs) | (g << gs) | (b << bs);
				}
			}
		}
		L.shown = true;
	}
	const bool anything = L.pre.alpha != 0 || L.post.alpha != 0 || L.fogUsed || L.lightUsed || L.pMaxX >= L.pMinX || L.dMaxX >= L.dMinX || L.sMaxX >= L.sMinX || water;
	if (!anything)
		return;

	// Area copied by this blit, in back buffer coordinates, limited to the layer.
	SDL_Rect s = srcRect != nullptr ? *srcRect : SDL_Rect { 0, 0, src->w, src->h };
	const int offX = (dstRect != nullptr ? dstRect->x : 0) - s.x;
	const int offY = (dstRect != nullptr ? dstRect->y : 0) - s.y;
	const int x0 = std::max(s.x, L.rect.x), x1 = std::min(s.x + s.w, L.rect.x + L.rect.w);
	const int y0 = std::max(s.y, L.rect.y), y1 = std::min(s.y + s.h, L.rect.y + L.rect.h);
	if (x0 >= x1 || y0 >= y1)
		return;

	const uint32_t preA = L.pre.alpha, postA = L.post.alpha;
	const uint32_t preR = Mul(L.pre.color.r, preA), preG = Mul(L.pre.color.g, preA), preB = Mul(L.pre.color.b, preA);
	const uint32_t postR = Mul(L.post.color.r, postA), postG = Mul(L.post.color.g, postA), postB = Mul(L.post.color.b, postA);
	const uint32_t fogR = L.fogColor.r, fogG = L.fogColor.g, fogB = L.fogColor.b;
	const int rs = layout.rShift, gs = layout.gShift, bs = layout.bShift;
	const uint32_t keep = ~((0xFFu << rs) | (0xFFu << gs) | (0xFFu << bs));
	const bool wholeArea = preA != 0 || postA != 0;

	if (water) {
		// Water: the floor's own pixels wobble with ripples running along the flow, the bank above
		// shows mirrored in it, and a few glints drift by. Done first, on the bare scene.
		std::array<uint32_t, 256> rgb;
		std::array<bool, 256> bluish;
		for (int i = 0; i < 256; i++) {
			const SDL_Color &c = system_palette[i];
			rgb[i] = Pack(c.r, c.g, c.b, 0);
			bluish[i] = c.b > c.r + 4 && c.b + 2 >= c.g && c.b > 12;
		}
		const float t = static_cast<float>(SDL_GetTicks() % 3600000u) / 1000.0f;
		const int scale = L.floorScale;
		auto isWater = [&](int lx, int ly) {
			if (lx < 0 || ly < 0 || lx >= L.rect.w || ly >= L.rect.h)
				return false;
			const int fx = (lx - L.floorOffX) / scale, fy = ly / scale;
			if (lx < L.floorOffX || fx >= L.floorW || fy >= L.floorH)
				return false;
			const size_t fi = static_cast<size_t>(fy) * L.floorW + fx;
			const uint8_t world = L.snapshot[static_cast<size_t>(ly) * L.rect.w + lx];
			// The water is drawn with the tile (not in the floor pass): its colours in a water tile.
			return L.water[fi] != 0 && bluish[world];
		};
		L.shore.assign(static_cast<size_t>(L.rect.w), -100000);
		for (int y = std::max(0, L.rect.y); y < y1; y++) {
			const int ly = y - L.rect.y;
			const int dy = y + offY;
			for (int x = x0; x < x1; x++) {
				const int lx = x - L.rect.x;
				if (!isWater(lx, ly)) {
					L.shore[lx] = ly;
					continue;
				}
				if (y < y0 || dy < 0 || dy >= dst->h)
					continue;
				const int dx = x + offX;
				if (dx < 0 || dx >= dst->w)
					continue;
				const uint8_t *palRow = static_cast<const uint8_t *>(src->pixels) + static_cast<size_t>(y) * src->pitch;
				if (palRow[x] != L.snapshot[static_cast<size_t>(ly) * L.rect.w + lx])
					continue; // the interface is over it
				// Ripples travelling along the stream.
				const float fx = static_cast<float>(lx) / scale, fy = static_cast<float>(ly) / scale;
				const float k1 = fx * 0.09f + fy * 0.19f - t * 1.7f;
				const float k2 = -fx * 0.14f + fy * 0.12f - t * 1.15f;
				const float wobble = std::sin(k1) * 1.2f + std::sin(k2 * 1.3f) * 0.8f;
				const int rx = static_cast<int>(std::lround(wobble)) * scale;
				const int ry = static_cast<int>(std::lround(std::cos(k1 * 0.8f) * 0.6f)) * scale;
				uint32_t base = rgb[L.snapshot[static_cast<size_t>(ly) * L.rect.w + lx]];
				if (isWater(lx + rx, ly + ry))
					base = rgb[L.snapshot[static_cast<size_t>(ly + ry) * L.rect.w + lx + rx]];
				float r = static_cast<float>(base & 0xFF), g = static_cast<float>((base >> 8) & 0xFF), b = static_cast<float>((base >> 16) & 0xFF);
				// The bank above, mirrored at the waterline (a wavering reflection).
				const int depth = ly - L.shore[lx];
				constexpr int Reach = 56;
				if (depth > 0 && depth < Reach * scale) {
					const int sy = L.shore[lx] - depth + ry;
					const int sx = lx + rx * 2;
					if (sy >= 0 && sx >= 0 && sx < L.rect.w) {
						const uint32_t refl = rgb[L.snapshot[static_cast<size_t>(sy) * L.rect.w + sx]];
						const float k = 1.0f - static_cast<float>(depth) / static_cast<float>(Reach * scale);
						const float w = 0.5f * k * k;
						r = r * (1 - w) + static_cast<float>(refl & 0xFF) * w * 0.8f;
						g = g * (1 - w) + static_cast<float>((refl >> 8) & 0xFF) * w * 0.85f;
						b = b * (1 - w) + static_cast<float>((refl >> 16) & 0xFF) * w * 0.95f;
					}
				}
				// Glints: short streaks scattered on the surface, drifting with the current and twinkling.
				{
					const float u = fx + t * 5.0f, v = fy - t * 7.0f; // carried along the stream
					const int cu = static_cast<int>(std::floor(u / 9.0f)), cv = static_cast<int>(std::floor(v / 5.0f));
					uint32_t hsh = static_cast<uint32_t>(cu) * 73856093u ^ static_cast<uint32_t>(cv) * 19349663u;
					hsh ^= hsh >> 13;
					hsh *= 0x5bd1e995u;
					hsh ^= hsh >> 15;
					if ((hsh & 0xFF) < 70) {
						const float gx = static_cast<float>(cu) * 9.0f + 1.5f + static_cast<float>((hsh >> 8) & 7);
						const float gy = static_cast<float>(cv) * 5.0f + 1.0f + static_cast<float>((hsh >> 11) % 3);
						const float len = 1.5f + static_cast<float>((hsh >> 14) & 3);
						const float ddx = std::abs(u - gx), ddy = std::abs(v - gy);
						if (ddy < 0.75f && ddx < len) {
							const float twinkle = std::max(0.0f, std::sin(t * (2.5f + static_cast<float>((hsh >> 17) & 3)) + static_cast<float>((hsh >> 20) & 63)));
							const float lum = (r + g + b) / (3.0f * 255.0f);
							const float glint = (1.0f - ddx / len) * twinkle * twinkle * (30.0f + 150.0f * std::min(1.0f, lum * 2.5f));
							r += glint * 0.85f;
							g += glint * 0.95f;
							b += glint;
						}
					}
				}
				auto* out = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(dst->pixels) + static_cast<size_t>(dy) * dst->pitch);
				const uint32_t px = out[dx];
				out[dx] = (px & keep) | (static_cast<uint32_t>(std::clamp(r, 0.0f, 255.0f)) << rs) | (static_cast<uint32_t>(std::clamp(g, 0.0f, 255.0f)) << gs) | (static_cast<uint32_t>(std::clamp(b, 0.0f, 255.0f)) << bs);
			}
		}
	}

	for (int y = y0; y < y1; y++) {
		const int ly = y - L.rect.y;
		const int dy = y + offY;
		if (dy < 0 || dy >= dst->h)
			continue;
		const uint8_t *pal = static_cast<const uint8_t *>(src->pixels) + static_cast<size_t>(y) * src->pitch;
		const uint8_t *snap = &L.snapshot[static_cast<size_t>(ly) * L.rect.w];
		const uint8_t *fog = &L.fog[static_cast<size_t>(ly) * L.rect.w];
		const uint32_t *part = &L.particles[static_cast<size_t>(ly) * L.rect.w];
		const uint32_t *lit = &L.light[static_cast<size_t>(ly) * L.rect.w];
		const uint32_t *decal = &L.decals[static_cast<size_t>(ly) * L.rect.w];
		const uint32_t *decalMul = &L.decalMul[static_cast<size_t>(ly) * L.rect.w];
		const uint32_t *decalGloss = &L.decalGloss[static_cast<size_t>(ly) * L.rect.w];
		const uint32_t *spr = &L.sprites[static_cast<size_t>(ly) * L.rect.w];
		const uint8_t *sprExpect = &L.spriteExpect[static_cast<size_t>(ly) * L.rect.w];
		const bool rowHasSprites = ly >= L.sMinY && ly <= L.sMaxY;
		const bool rowHasDecals = ly >= L.dMinY && ly <= L.dMaxY;
		const uint8_t *floorRow = nullptr;
		if (rowHasDecals && L.floorValid) {
			const int fy = ly / L.floorScale;
			floorRow = fy < L.floorH ? &L.floor[static_cast<size_t>(fy) * L.floorW] : nullptr;
		}
		auto *out = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(dst->pixels) + static_cast<size_t>(dy) * dst->pitch);
		const bool rowHasParticles = ly >= L.pMinY && ly <= L.pMaxY;
		for (int x = x0; x < x1; x++) {
			const int lx = x - L.rect.x;
			const uint32_t f = L.fogUsed ? fog[lx] : 0;
			const uint32_t p = rowHasParticles ? part[lx] : 0;
			const uint32_t m = L.lightUsed ? lit[lx] : 0x808080u;
			uint32_t dc = rowHasDecals ? decal[lx] : 0;
			uint32_t dm = rowHasDecals ? decalMul[lx] : 0x808080u;
			uint32_t dg = rowHasDecals ? decalGloss[lx] : 0;
			uint32_t sp = rowHasSprites ? spr[lx] : 0;
			if (!wholeArea && f == 0 && p == 0 && m == 0x808080u && dc == 0 && dm == 0x808080u && dg == 0 && sp == 0)
				continue;
			if (pal[x] != snap[lx])
				continue; // the interface drew here after the world
			const int dx = x + offX;
			if (dx < 0 || dx >= dst->w)
				continue;
			uint32_t px = out[dx];
			uint32_t r = (px >> rs) & 0xFF, g = (px >> gs) & 0xFF, b = (px >> bs) & 0xFF;
			if ((dc != 0 || dm != 0x808080u || dg != 0) && L.floorValid) {
				// Only on floor nothing stands on: the pixel is still what the floor pass drew.
				const int fx = (lx - L.floorOffX) / L.floorScale;
				if (floorRow == nullptr || lx < L.floorOffX || fx >= L.floorW || floorRow[fx] != pal[x]) {
					dc = 0;
					dm = 0x808080u;
					dg = 0;
				}
			}
			if (dm != 0x808080u) {
				// Stains soak in: the floor's own texture stays, darkened and tinted.
				r = std::min(255u, (r * (dm & 0xFF) + 64) >> 7);
				g = std::min(255u, (g * ((dm >> 8) & 0xFF) + 64) >> 7);
				b = std::min(255u, (b * ((dm >> 16) & 0xFF) + 64) >> 7);
			}
			if (dc != 0) {
				const uint32_t inv = 255 - (dc >> 24);
				r = std::min(255u, (dc & 0xFF) + Mul(r, inv));
				g = std::min(255u, ((dc >> 8) & 0xFF) + Mul(g, inv));
				b = std::min(255u, ((dc >> 16) & 0xFF) + Mul(b, inv));
			}
			if (dg != 0) {
				r = std::min(255u, r + (dg & 0xFF));
				g = std::min(255u, g + ((dg >> 8) & 0xFF));
				b = std::min(255u, b + ((dg >> 16) & 0xFF));
			}
			if (sp != 0 && sprExpect[lx] == snap[lx]) {
				// A full-colour sprite pixel nothing was drawn over.
				const uint32_t inv = 255 - (sp >> 24);
				r = std::min(255u, (sp & 0xFF) + Mul(r, inv));
				g = std::min(255u, ((sp >> 8) & 0xFF) + Mul(g, inv));
				b = std::min(255u, ((sp >> 16) & 0xFF) + Mul(b, inv));
			}
			if (m != 0x808080u) {
				r = std::min(255u, (r * (m & 0xFF) + 64) >> 7);
				g = std::min(255u, (g * ((m >> 8) & 0xFF) + 64) >> 7);
				b = std::min(255u, (b * ((m >> 16) & 0xFF) + 64) >> 7);
			}
			if (preA != 0) {
				r = preR + Mul(r, 255 - preA);
				g = preG + Mul(g, 255 - preA);
				b = preB + Mul(b, 255 - preA);
			}
			if (f != 0) {
				r = Mul(fogR, f) + Mul(r, 255 - f);
				g = Mul(fogG, f) + Mul(g, 255 - f);
				b = Mul(fogB, f) + Mul(b, 255 - f);
			}
			if (p != 0) {
				const uint32_t inv = 255 - (p >> 24);
				r = std::min(255u, (p & 0xFF) + Mul(r, inv));
				g = std::min(255u, ((p >> 8) & 0xFF) + Mul(g, inv));
				b = std::min(255u, ((p >> 16) & 0xFF) + Mul(b, inv));
			}
			if (postA != 0) {
				r = postR + Mul(r, 255 - postA);
				g = postG + Mul(g, 255 - postA);
				b = postB + Mul(b, 255 - postA);
			}
			out[dx] = (px & keep) | (r << rs) | (g << gs) | (b << bs);
		}
	}
	L.shown = true;
}

SDL_Surface *ColorOverlayShownSurface()
{
	return L.shown ? GetOutputSurface() : nullptr;
}

} // namespace devilution
