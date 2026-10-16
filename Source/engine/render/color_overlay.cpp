#include "engine/render/color_overlay.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

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

	OverlayColor fogColor { 0, 0, 0 };
	bool fogUsed = false;
	std::vector<uint8_t> fog; // coverage 0..255

	// Particles, premultiplied: r, g, b, coverage.
	std::vector<uint32_t> particles;
	int pMinX = 0, pMinY = 0, pMaxX = -1, pMaxY = -1; // touched area (inclusive)

	std::vector<uint8_t> snapshot; // the world's 8-bit pixels when it was finished
};

Layer L;

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
		L.particles.assign(size, 0);
		L.snapshot.assign(size, 0);
		L.pMinX = L.pMinY = 0;
		L.pMaxX = L.pMaxY = -1;
	} else {
		if (L.fogUsed)
			std::fill(L.fog.begin(), L.fog.end(), 0);
		if (L.pMaxX >= L.pMinX) {
			for (int y = L.pMinY; y <= L.pMaxY; y++)
				std::fill_n(&L.particles[static_cast<size_t>(y) * rect.w + L.pMinX], L.pMaxX - L.pMinX + 1, 0u);
		}
		L.pMinX = L.pMinY = 0;
		L.pMaxX = L.pMaxY = -1;
	}
	L.rect = rect;
	L.pre = { { 0, 0, 0 }, 0 };
	L.post = { { 0, 0, 0 }, 0 };
	L.fogUsed = false;
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
	const bool anything = L.pre.alpha != 0 || L.post.alpha != 0 || L.fogUsed || L.pMaxX >= L.pMinX;
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

	for (int y = y0; y < y1; y++) {
		const int ly = y - L.rect.y;
		const int dy = y + offY;
		if (dy < 0 || dy >= dst->h)
			continue;
		const uint8_t *pal = static_cast<const uint8_t *>(src->pixels) + static_cast<size_t>(y) * src->pitch;
		const uint8_t *snap = &L.snapshot[static_cast<size_t>(ly) * L.rect.w];
		const uint8_t *fog = &L.fog[static_cast<size_t>(ly) * L.rect.w];
		const uint32_t *part = &L.particles[static_cast<size_t>(ly) * L.rect.w];
		auto *out = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(dst->pixels) + static_cast<size_t>(dy) * dst->pitch);
		const bool rowHasParticles = ly >= L.pMinY && ly <= L.pMaxY;
		for (int x = x0; x < x1; x++) {
			const int lx = x - L.rect.x;
			const uint32_t f = L.fogUsed ? fog[lx] : 0;
			const uint32_t p = rowHasParticles ? part[lx] : 0;
			if (!wholeArea && f == 0 && p == 0)
				continue;
			if (pal[x] != snap[lx])
				continue; // the interface drew here after the world
			const int dx = x + offX;
			if (dx < 0 || dx >= dst->w)
				continue;
			uint32_t px = out[dx];
			uint32_t r = (px >> rs) & 0xFF, g = (px >> gs) & 0xFF, b = (px >> bs) & 0xFF;
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
