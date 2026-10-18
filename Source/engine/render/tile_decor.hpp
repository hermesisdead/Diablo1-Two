/**
 * @file tile_decor.hpp
 *
 * Decor layers for hand-made maps: things painted over the ground in up to DecorLayerCount layers.
 * A decor item is either a town megatile, of which only the parts standing on the ground are
 * drawn (shrubs, rocks, gravestones, walls, trees), or one of the flower patches drawn here.
 */
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "engine/point.hpp"
#include "engine/render/light_render.hpp"
#include "engine/surface.hpp"

namespace devilution {

/** Decor ids: 1..0x7FFF are town megatiles, DecorFlowerBase + n is flower patch n, 0 is nothing. */
inline constexpr uint16_t DecorFlowerBase = 0x8000;
inline constexpr int FlowerPatchCount = 8;

[[nodiscard]] inline bool IsFlowerDecor(uint16_t id)
{
	return id >= DecorFlowerBase && id < DecorFlowerBase + FlowerPatchCount;
}

[[nodiscard]] std::string_view FlowerPatchName(int patch);

/**
 * @brief Draws a flower patch on one world tile's ground.
 * @param position Bottom-left corner of the tile's floor (as for DrawFloorTile).
 * @param seed Varies where the flowers grow, so neighbouring tiles differ.
 * @param tbl Light table (or hue-shifted light table) to colour through.
 */
void DrawFlowerPatch(const Surface &out, Point position, int patch, uint32_t seed, const uint8_t *tbl);

/** @brief Draws the parts of a level piece that stand on the ground (everything but its floor). */
void DrawDecorPiece(const Surface &out, const Lightmap &lightmap, Point position, uint16_t piece, const uint8_t *tbl);

/** @brief Flower patches of every decor layer at a world tile (floor pass). */
void DrawDecorFloor(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightTableIndex);

/** @brief Standing decor of every decor layer at a world tile (drawn with the tile). */
void DrawDecorCell(const Surface &out, const Lightmap &lightmap, Point tilePosition, Point targetBufferPosition, int lightTableIndex);

/**
 * @brief Draws wooden fencing on a world tile: a post, with rails out to the fenced neighbours.
 * @param fence 1 fence, 2 gate.
 * @param links Neighbours that are fenced too: +x, -x, +y, -y.
 */
void DrawFence(const Surface &out, Point position, uint8_t fence, std::array<bool, 4> links, const uint8_t *tbl);

/** @brief Whether a level piece has anything standing on its floor (so it works as decor). */
[[nodiscard]] bool PieceHasStandingParts(uint16_t piece);

} // namespace devilution
