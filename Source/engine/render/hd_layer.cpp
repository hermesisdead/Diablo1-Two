#include "engine/render/hd_layer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>

#include "engine/dx.h"
#include "engine/load_file.hpp"
#include "engine/palette.h"
#include "options.h"
#include "utils/display.h"
#include "utils/log.hpp"

namespace devilution {

namespace {

struct LayerItem {
	const HdImage *image;
	float x, y, w, h; // back buffer pixels
	int maskX, maskY, maskW, maskH;
	std::vector<int16_t> mask;
	bool tint, cache;
};

std::vector<LayerItem> LayerItems;
bool InFrame = false;

#ifndef USE_SDL1
SDL_Texture *Texture = nullptr;
int TexW = 0, TexH = 0;
bool Premultiplied = false;
#endif
std::vector<uint32_t> Buffer; // premultiplied ARGB, TexW x TexH
SDL_Rect LastDirty { 0, 0, 0, 0 };

/** Scaled copies of unchanging pictures (premultiplied ARGB), by picture and size. */
std::map<std::tuple<const HdImage *, int, int>, std::vector<uint32_t>> Cache;

uint32_t PremulSample(const HdImage &img, float u, float v)
{
	// Bilinear, premultiplied (so see-through edges do not darken).
	u = std::clamp(u, 0.0f, static_cast<float>(img.width - 1));
	v = std::clamp(v, 0.0f, static_cast<float>(img.height - 1));
	const int x0 = static_cast<int>(u), y0 = static_cast<int>(v);
	const int x1 = std::min(x0 + 1, img.width - 1), y1 = std::min(y0 + 1, img.height - 1);
	const float fx = u - static_cast<float>(x0), fy = v - static_cast<float>(y0);
	float acc[4] = { 0, 0, 0, 0 };
	const auto add = [&](int x, int y, float wgt) {
		const uint8_t *p = &img.rgba[(static_cast<size_t>(y) * img.width + x) * 4];
		const float a = p[3] * wgt;
		acc[0] += p[0] * a;
		acc[1] += p[1] * a;
		acc[2] += p[2] * a;
		acc[3] += a;
	};
	add(x0, y0, (1 - fx) * (1 - fy));
	add(x1, y0, fx * (1 - fy));
	add(x0, y1, (1 - fx) * fy);
	add(x1, y1, fx * fy);
	const uint32_t a = static_cast<uint32_t>(std::clamp(acc[3], 0.0f, 255.0f));
	if (a == 0)
		return 0;
	const auto c = [](float v) { return static_cast<uint32_t>(std::clamp(v / 255.0f, 0.0f, 255.0f)); };
	return (a << 24) | (c(acc[0]) << 16) | (c(acc[1]) << 8) | c(acc[2]);
}

void UnionRect(SDL_Rect &r, const SDL_Rect &o)
{
	if (o.w <= 0 || o.h <= 0)
		return;
	if (r.w <= 0 || r.h <= 0) {
		r = o;
		return;
	}
	const int x0 = std::min(r.x, o.x), y0 = std::min(r.y, o.y);
	const int x1 = std::max(r.x + r.w, o.x + o.w), y1 = std::max(r.y + r.h, o.y + o.h);
	r = { x0, y0, x1 - x0, y1 - y0 };
}

bool LayerSize(SDL_Renderer *rend, int &w, int &h)
{
#ifdef USE_SDL1
	(void)rend;
	return false;
#elif defined(USE_SDL3)
	SDL_FRect rect;
	if (!SDL_GetRenderLogicalPresentationRect(rend, &rect) || rect.w < 1 || rect.h < 1)
		return false;
	w = static_cast<int>(std::lround(rect.w));
	h = static_cast<int>(std::lround(rect.h));
	return true;
#else
	float sx = 1, sy = 1;
	SDL_RenderGetScale(rend, &sx, &sy);
	if (sx <= 0 || sy <= 0)
		return false;
	w = static_cast<int>(std::lround(static_cast<float>(gnScreenWidth) * sx));
	h = static_cast<int>(std::lround(static_cast<float>(gnScreenHeight) * sy));
	return w > 0 && h > 0;
#endif
}

float ScaleX = 1, ScaleY = 1;

} // namespace

std::shared_ptr<const HdHiresSprite> LoadHdHiresSprite(const std::string &path)
{
	static std::map<std::string, std::shared_ptr<const HdHiresSprite>> cache;
	if (const auto it = cache.find(path); it != cache.end())
		return it->second;
	std::shared_ptr<const HdHiresSprite> &slot = cache[path];
	size_t size = 0;
	std::expected<std::unique_ptr<std::byte[]>, std::string> file = LoadFileInMemWithStatus<std::byte>(path.c_str(), &size);
	if (!file.has_value() || size < 10)
		return nullptr;
	const auto *d = reinterpret_cast<const uint8_t *>(file->get());
	if (std::memcmp(d, "HDR1", 4) != 0)
		return nullptr;
	auto u16 = [d](size_t at) { return static_cast<int>(d[at] | (d[at + 1] << 8)); };
	auto f32 = [d](size_t at) { float f; std::memcpy(&f, d + at, 4); return f; };
	auto sprite = std::make_shared<HdHiresSprite>();
	const int count = u16(4);
	sprite->canvasW = u16(6);
	sprite->canvasH = u16(8);
	size_t at = 10;
	for (int i = 0; i < count; i++) {
		if (at + 20 > size)
			return nullptr;
		HdHiresSprite::Frame f { f32(at), f32(at + 4), f32(at + 8), f32(at + 12), {} };
		f.image.width = u16(at + 16);
		f.image.height = u16(at + 18);
		at += 20;
		const size_t bytes = static_cast<size_t>(f.image.width) * f.image.height * 4;
		if (at + bytes > size)
			return nullptr;
		f.image.rgba.assign(d + at, d + at + bytes);
		at += bytes;
		sprite->frames.push_back(std::move(f));
	}
	slot = std::move(sprite);
	return slot;
}

bool HdLayerActive()
{
#ifdef USE_SDL1
	return false;
#else
	return renderer != nullptr && *GetOptions().Graphics.textures == TextureMode::HD && !RenderDirectlyToOutputSurface;
#endif
}

bool HdLayerWorldActive()
{
	return HdLayerActive() && !*GetOptions().Graphics.zoom;
}

void HdLayerBeginFrame()
{
	LayerItems.clear();
	InFrame = true;
}

void HdLayerScale(float &sx, float &sy)
{
	sx = ScaleX;
	sy = ScaleY;
}

void HdLayerAdd(const Surface &out, const HdImage *image, float x, float y, float w, float h,
    int maskX, int maskY, int maskW, int maskH, std::vector<int16_t> mask, bool tint, bool cache)
{
	if (!InFrame || image == nullptr || image->width <= 0 || w <= 0 || h <= 0 || !HdLayerActive())
		return;
	const float ox = static_cast<float>(out.region.x), oy = static_cast<float>(out.region.y);
	LayerItems.push_back({ image, x + ox, y + oy, w, h, maskX + out.region.x, maskY + out.region.y, maskW, maskH, std::move(mask), tint, cache });
}

void HdLayerPresent(SDL_Renderer *rend, const SDL_Surface *frame8, const SDL_Surface *frame32)
{
#ifdef USE_SDL1
	(void)rend;
	(void)frame8;
	(void)frame32;
#else
	const bool active = InFrame && HdLayerActive();
	InFrame = false;
	if (rend == nullptr)
		return;
	int w, h;
	if (!active || !LayerSize(rend, w, h)) {
		LayerItems.clear();
		return;
	}
	if (Texture == nullptr || TexW != w || TexH != h) {
		if (Texture != nullptr)
			SDL_DestroyTexture(Texture);
		Texture = SDL_CreateTexture(rend, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
		if (Texture == nullptr) {
			LogError("HD layer: {}", SDL_GetError());
			return;
		}
#ifdef USE_SDL3
		Premultiplied = SDL_SetTextureBlendMode(Texture, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
		if (!Premultiplied)
			SDL_SetTextureBlendMode(Texture, SDL_BLENDMODE_BLEND);
		SDL_SetTextureScaleMode(Texture, SDL_SCALEMODE_LINEAR);
#else
		const SDL_BlendMode premul = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD,
		    SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD);
		Premultiplied = SDL_SetTextureBlendMode(Texture, premul) == 0;
		if (!Premultiplied)
			SDL_SetTextureBlendMode(Texture, SDL_BLENDMODE_BLEND);
#if SDL_VERSION_ATLEAST(2, 0, 12)
		SDL_SetTextureScaleMode(Texture, SDL_ScaleModeLinear);
#endif
#endif
		TexW = w;
		TexH = h;
		Buffer.assign(static_cast<size_t>(w) * h, 0);
		LastDirty = { 0, 0, w, h };
		Cache.clear();
	}
	ScaleX = static_cast<float>(w) / static_cast<float>(gnScreenWidth);
	ScaleY = static_cast<float>(h) / static_cast<float>(gnScreenHeight);
	if (Cache.size() > 400)
		Cache.clear();

	// Clear what was drawn last time.
	SDL_Rect dirty = LastDirty;
	for (int y = LastDirty.y; y < LastDirty.y + LastDirty.h; y++)
		std::fill_n(&Buffer[static_cast<size_t>(y) * w + LastDirty.x], LastDirty.w, 0u);
	SDL_Rect drawn { 0, 0, 0, 0 };

	const uint8_t *pal8 = static_cast<const uint8_t *>(frame8->pixels);
	const int pitch8 = frame8->pitch;
	int rShift = 16, gShift = 8, bShift = 0;
	bool have32 = frame32 != nullptr && frame32->w == frame8->w && frame32->h == frame8->h;
	if (have32) {
#ifdef USE_SDL3
		const SDL_PixelFormatDetails *fmt = SDL_GetPixelFormatDetails(frame32->format);
		have32 = fmt != nullptr && fmt->bytes_per_pixel == 4;
		if (have32) {
			rShift = fmt->Rshift;
			gShift = fmt->Gshift;
			bShift = fmt->Bshift;
		}
#else
		have32 = frame32->format != nullptr && frame32->format->BytesPerPixel == 4;
		if (have32) {
			rShift = frame32->format->Rshift;
			gShift = frame32->format->Gshift;
			bShift = frame32->format->Bshift;
		}
#endif
	}
	for (const LayerItem &item : LayerItems) {
		const int X0 = std::max(0, static_cast<int>(std::floor(item.x * ScaleX)));
		const int Y0 = std::max(0, static_cast<int>(std::floor(item.y * ScaleY)));
		const int X1 = std::min(w, static_cast<int>(std::ceil((item.x + item.w) * ScaleX)));
		const int Y1 = std::min(h, static_cast<int>(std::ceil((item.y + item.h) * ScaleY)));
		if (X0 >= X1 || Y0 >= Y1)
			continue;
		// The picture at this size: whole pixels of the layer from the item's corner.
		const int fullX = static_cast<int>(std::floor(item.x * ScaleX)), fullY = static_cast<int>(std::floor(item.y * ScaleY));
		const int pw = static_cast<int>(std::ceil((item.x + item.w) * ScaleX)) - fullX;
		const int ph = static_cast<int>(std::ceil((item.y + item.h) * ScaleY)) - fullY;
		const std::vector<uint32_t> *scaled = nullptr;
		std::vector<uint32_t> local;
		const auto key = std::make_tuple(item.image, pw, ph);
		if (item.cache) {
			if (const auto it = Cache.find(key); it != Cache.end())
				scaled = &it->second;
		}
		if (scaled == nullptr) {
			local.resize(static_cast<size_t>(pw) * ph);
			const float ku = static_cast<float>(item.image->width) / static_cast<float>(pw);
			const float kv = static_cast<float>(item.image->height) / static_cast<float>(ph);
			for (int y = 0; y < ph; y++)
				for (int x = 0; x < pw; x++)
					local[static_cast<size_t>(y) * pw + x] = PremulSample(*item.image, (static_cast<float>(x) + 0.5f) * ku - 0.5f, (static_cast<float>(y) + 0.5f) * kv - 0.5f);
			if (item.cache)
				scaled = &(Cache[key] = std::move(local));
			else
				scaled = &local;
		}
		for (int py = Y0; py < Y1; py++) {
			const int gy = std::min(static_cast<int>(static_cast<float>(py) / ScaleY), frame8->h - 1);
			const int my = gy - item.maskY;
			for (int px = X0; px < X1; px++) {
				const uint32_t s = (*scaled)[static_cast<size_t>(py - fullY) * pw + (px - fullX)];
				if ((s >> 24) == 0)
					continue;
				const int gx = std::min(static_cast<int>(static_cast<float>(px) / ScaleX), frame8->w - 1);
				const int mx = gx - item.maskX;
				if (mx < 0 || my < 0 || mx >= item.maskW || my >= item.maskH)
					continue;
				const int16_t expect = item.mask[static_cast<size_t>(my) * item.maskW + mx];
				const uint8_t here = pal8[static_cast<size_t>(gy) * pitch8 + gx];
				if (expect < 0 || here != expect)
					continue; // something was drawn over it
				uint32_t a = s >> 24, r = (s >> 16) & 0xFF, g = (s >> 8) & 0xFF, b = s & 0xFF;
				if (item.tint && have32) {
					// The scene's light and weather here: how the frame changed the colour of what is under it.
					const SDL_Color &base = system_palette[here];
					const uint32_t o = *reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(frame32->pixels) + static_cast<size_t>(gy) * frame32->pitch + static_cast<size_t>(gx) * 4);
					const uint32_t orr = (o >> rShift) & 0xFF, og = (o >> gShift) & 0xFF, ob = (o >> bShift) & 0xFF;
					const auto k = [](uint32_t now, uint8_t was) { return std::clamp((static_cast<float>(now) + 4.0f) / (static_cast<float>(was) + 4.0f), 0.0f, 1.6f); };
					r = std::min(a, static_cast<uint32_t>(static_cast<float>(r) * k(orr, base.r)));
					g = std::min(a, static_cast<uint32_t>(static_cast<float>(g) * k(og, base.g)));
					b = std::min(a, static_cast<uint32_t>(static_cast<float>(b) * k(ob, base.b)));
				}
				uint32_t &d = Buffer[static_cast<size_t>(py) * w + px];
				const uint32_t inv = 255 - a;
				const uint32_t da = d >> 24, dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
				d = ((a + da * inv / 255) << 24) | ((r + dr * inv / 255) << 16) | ((g + dg * inv / 255) << 8) | (b + db * inv / 255);
			}
		}
		UnionRect(drawn, { X0, Y0, X1 - X0, Y1 - Y0 });
	}
	LayerItems.clear();
	UnionRect(dirty, drawn);
	LastDirty = drawn;
	if (dirty.w > 0 && dirty.h > 0) {
		if (Premultiplied) {
			SDL_UpdateTexture(Texture, &dirty, &Buffer[static_cast<size_t>(dirty.y) * w + dirty.x], w * 4);
		} else {
			// Straight alpha for the plain blend.
			std::vector<uint32_t> straight(static_cast<size_t>(dirty.w) * dirty.h);
			for (int y = 0; y < dirty.h; y++) {
				for (int x = 0; x < dirty.w; x++) {
					const uint32_t p = Buffer[static_cast<size_t>(dirty.y + y) * w + dirty.x + x];
					const uint32_t a = p >> 24;
					if (a == 0) {
						straight[static_cast<size_t>(y) * dirty.w + x] = 0;
						continue;
					}
					const auto un = [a](uint32_t c) { return std::min(255u, c * 255 / a); };
					straight[static_cast<size_t>(y) * dirty.w + x] = (a << 24) | (un((p >> 16) & 0xFF) << 16) | (un((p >> 8) & 0xFF) << 8) | un(p & 0xFF);
				}
			}
			SDL_UpdateTexture(Texture, &dirty, straight.data(), dirty.w * 4);
		}
	}
	if (drawn.w > 0 && drawn.h > 0) {
#ifdef USE_SDL3
		SDL_RenderTexture(rend, Texture, nullptr, nullptr);
#else
		SDL_RenderCopy(rend, Texture, nullptr, nullptr);
#endif
	}
#endif
}

} // namespace devilution
