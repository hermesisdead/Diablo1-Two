#include "engine/render/hd_sprite.hpp"

#include <cstring>
#include <map>

#include "engine/load_file.hpp"
#include "engine/render/color_overlay.hpp"
#include "engine/render/hd_layer.hpp"
#include "options.h"

namespace devilution {

std::shared_ptr<const HdSprite> LoadHdSprite(const std::string &path)
{
	static std::map<std::string, std::shared_ptr<const HdSprite>> cache;
	if (const auto it = cache.find(path); it != cache.end())
		return it->second;
	std::shared_ptr<const HdSprite> &slot = cache[path];
	size_t size = 0;
	std::expected<std::unique_ptr<std::byte[]>, std::string> file = LoadFileInMemWithStatus<std::byte>(path.c_str(), &size);
	if (!file.has_value() || size < 10)
		return nullptr;
	const auto *d = reinterpret_cast<const uint8_t *>(file->get());
	if (std::memcmp(d, "HDS1", 4) != 0)
		return nullptr;
	auto u16 = [d](size_t at) { return static_cast<int>(d[at] | (d[at + 1] << 8)); };
	auto sprite = std::make_shared<HdSprite>();
	const int count = u16(4);
	sprite->width = u16(6);
	sprite->height = u16(8);
	size_t at = 10;
	for (int i = 0; i < count; i++) {
		if (at + 8 > size)
			return nullptr;
		HdSpriteFrame f;
		f.x0 = u16(at);
		f.y0 = u16(at + 2);
		f.width = u16(at + 4);
		f.height = u16(at + 6);
		at += 8;
		const size_t bytes = static_cast<size_t>(f.width) * f.height * 5;
		if (at + bytes > size)
			return nullptr;
		f.pixels.assign(d + at, d + at + bytes);
		at += bytes;
		sprite->frames.push_back(std::move(f));
	}
	if (sprite->frames.empty())
		return nullptr;
	slot = std::move(sprite);
	return slot;
}

bool NewTexturesActive()
{
	const TextureMode mode = *GetOptions().Graphics.textures;
	return mode == TextureMode::HD || mode == TextureMode::HDRedux;
}

bool HudTexturesActive()
{
	return *GetOptions().Graphics.textures != TextureMode::Vanilla;
}

bool HdTexturesActive()
{
	return *GetOptions().Graphics.textures == TextureMode::HD && ColorOverlayRecording();
}

bool DrawHdSpriteOnLayer(const Surface &out, Point position, const HdSprite &footprint, const HdHiresSprite &hires, int frame)
{
	if (!HdLayerWorldActive() || hires.frames.empty() || footprint.frames.empty())
		return false;
	const HdSpriteFrame &f = footprint.frames[static_cast<size_t>(frame) % footprint.frames.size()];
	const HdHiresSprite::Frame &h = hires.frames[static_cast<size_t>(frame) % hires.frames.size()];
	const int left = position.x, top = position.y - footprint.height + 1;
	std::vector<int16_t> mask(static_cast<size_t>(f.width) * f.height, -1);
	const uint8_t *px = f.pixels.data();
	for (int y = 0; y < f.height; y++) {
		for (int x = 0; x < f.width; x++, px += 5) {
			if (px[3] == 0)
				continue;
			const Point p { left + f.x0 + x, top + f.y0 + y };
			if (out.InBounds(p))
				mask[static_cast<size_t>(y) * f.width + x] = *out.at(p.x, p.y);
		}
	}
	HdLayerAdd(out, &h.image, static_cast<float>(left) + h.x, static_cast<float>(top) + h.y, h.w, h.h,
	    left + f.x0, top + f.y0, f.width, f.height, std::move(mask), /*tint=*/true, /*cache=*/true);
	return true;
}

void DrawHdSprite(const Surface &out, Point position, const HdSprite &sprite, int frame)
{
	const HdSpriteFrame &f = sprite.frames[static_cast<size_t>(frame) % sprite.frames.size()];
	const int left = position.x + f.x0;
	const int top = position.y - sprite.height + 1 + f.y0;
	const uint8_t *px = f.pixels.data();
	for (int y = 0; y < f.height; y++) {
		for (int x = 0; x < f.width; x++, px += 5) {
			if (px[3] == 0)
				continue;
			const Point p { left + x, top + y };
			if (!out.InBounds(p))
				continue;
			uint8_t &dst = *out.at(p.x, p.y);
			if (px[4] != 0)
				dst = px[4]; // the 256-colour pixel (it is what the full colour replaces)
			ColorOverlaySpritePixel(out, p, { px[0], px[1], px[2] }, px[3], dst);
		}
	}
}

} // namespace devilution
