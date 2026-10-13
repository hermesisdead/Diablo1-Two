/**
 * @file dungeon_mood.hpp
 *
 * Randomized atmosphere for dungeon levels: drifting fog, dust motes catching the torchlight,
 * frost sparkles, rising embers and dripping ceilings. Each level rolls its mood from its
 * dungeon seed, so a level keeps its mood for the whole game and a new game re-rolls it.
 * The effects only show where the level is lit, so darkness stays dark.
 */
#pragma once

#include <cstdint>

#include "engine/surface.hpp"

namespace devilution {

enum class DungeonMood : uint8_t {
	Clear,
	Mist,   // drifting grey fog with a few motes
	Dust,   // golden dust motes in the light, faint haze
	Frost,  // pale blue haze and slowly falling ice glitter
	Embers, // rising sparks and a dull red haze
	Drips,  // water dripping from the ceiling onto the floor, with splash rings
};

/** @brief The mood of the current dungeon level (Clear in town or when disabled). */
[[nodiscard]] DungeonMood GetDungeonMood();

/** @brief Display name of a mood, for logs and the debug overlay. */
[[nodiscard]] const char *DungeonMoodName(DungeonMood mood);

/**
 * @brief Draws the current level's mood over the game view (classic or 3D view).
 * @param out The surface the world was drawn to (for the 3D view, the one given to DrawOrbitView).
 */
void DrawDungeonMood(const Surface &out);

} // namespace devilution
