/**
 * @file dungeon_mood.hpp
 *
 * Weather and atmosphere for every area: Tristram and the Darkwood get outdoor weather (rain,
 * thunderstorms, snow, wind-blown leaves, fireflies, fog) that changes every few minutes;
 * dungeon levels roll an atmosphere on each visit (mist, dust, frost, embers, drips, ash,
 * spores, blood mist). Each weather has its own ambient sound loop.
 * Effects only show where the level is lit, so darkness stays dark.
 */
#pragma once

#include <cstdint>

#include "engine/surface.hpp"

namespace devilution {

enum class DungeonMood : uint8_t {
	Clear,
	Mist,      // drifting grey fog with a few motes
	Dust,      // golden dust motes in the light, faint haze
	Frost,     // pale blue haze and slowly falling ice glitter
	Embers,    // rising sparks and a dull red haze
	Drips,     // water dripping from the ceiling onto the floor, with splash rings
	Rain,      // outdoor: rain streaks and splashes
	Storm,     // outdoor: heavy slanting rain, darker sky, lightning and thunder
	Snow,      // outdoor: drifting snowflakes and a white haze
	Wind,      // outdoor: gusts carrying leaves and dust
	Fireflies, // outdoor at night: blinking fireflies over a low mist
	Ash,       // falling ash and a smoky haze
	Spores,    // glowing spores rising through a damp green haze
	BloodMist, // thick red fog
	COUNT
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
