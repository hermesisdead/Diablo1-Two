/**
 * @file tile_hue.hpp
 *
 * Per-tile hue shifts for painted maps. A tile with a hue shift (dTileHue) is drawn through a
 * copy of the light tables whose colours have been rotated around the colour wheel and matched
 * back to the nearest colour in the current palette, so it still shades with the level's lighting.
 */
#pragma once

#include <array>
#include <cstdint>

#include "engine/lighting_defs.hpp"
#include "levels/dun_tile_data.hpp"

namespace devilution {

using HueLightTableSet = std::array<std::array<uint8_t, LightTableSize>, NumLightingLevels>;

/** @brief Degrees per hue step (what one press of the editor's hue keys changes). */
inline constexpr int TileHueStepDegrees = 15;
/** @brief Largest hue shift in steps either way (12 x 15 = half way round the colour wheel). */
inline constexpr int MaxTileHueSteps = 12;

void ClearTileHues();

/** @brief Drops cached tables if the palette or light tables changed. Call once per frame before drawing. */
void RefreshTileHueTables();

/**
 * @brief Light tables that also rotate the hue by the given number of steps.
 * @return nullptr for 0 steps (use the normal light tables).
 */
[[nodiscard]] const HueLightTableSet *GetTileHueLightTables(int hueSteps);

/** @brief One hue-shifted light table (for sprites and editor previews). */
[[nodiscard]] const uint8_t *GetTileHueTable(int hueSteps, int lightLevel);

} // namespace devilution
