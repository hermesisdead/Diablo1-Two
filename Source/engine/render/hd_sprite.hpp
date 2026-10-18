/**
 * @file hd_sprite.hpp
 *
 * HD textures: sprites kept in full colour (.hds files next to their 256-colour .clx versions).
 * Drawn, the 8-bit frame gets the 256-colour pixels (so picking, outlines and anything drawn in
 * front behave as usual) and the full-colour layer gets the real colours, soft edges and
 * see-through shadows, shown wherever nothing covered them.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/point.hpp"
#include "engine/surface.hpp"

namespace devilution {

struct HdSpriteFrame {
	int x0 = 0, y0 = 0, width = 0, height = 0;
	std::vector<uint8_t> pixels; // r, g, b, alpha, palette index (0: nothing in the 8-bit frame)
};

struct HdSprite {
	int width = 0, height = 0; // canvas of each frame (as the .clx frames)
	std::vector<HdSpriteFrame> frames;
};

/** @brief Loads (once) an .hds file; nullptr when there is none. */
[[nodiscard]] std::shared_ptr<const HdSprite> LoadHdSprite(const std::string &path);

/** @brief Townspeople and animals are drawn with the HD textures now (the option, and the full-colour layer is on). */
[[nodiscard]] bool HdTexturesActive();
/** @brief The new sprites are shown (HD or HD Redux). */
[[nodiscard]] bool NewTexturesActive();

/** @brief Draws an HD frame, `position` being the bottom-left corner of its canvas (as ClxDraw). */
void DrawHdSprite(const Surface &out, Point position, const HdSprite &sprite, int frame);

} // namespace devilution
