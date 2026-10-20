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
#include <optional>
#include <string_view>
#include <vector>

#include "engine/surface.hpp"
#include "levels/gendung_defs.hpp"

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

/** @brief What the weather is doing now (for the F8 weather panel). */
struct WeatherStatus {
	DungeonMood mood;
	bool active;       // weather runs on this level (enabled for this level type, in game)
	bool outdoor;      // outdoor weather turns over time; dungeon moods last the whole visit
	float strength;    // how heavy this roll is, 0.6..1
	float secondsLeft; // until it starts to turn (outdoor only)
	bool changing;     // fading from one weather to the next
	std::optional<DungeonMood> next; // the weather it is changing to, when chosen in the panel
};
[[nodiscard]] WeatherStatus GetWeatherStatus();
/** @brief Display name of a mood, translated. */
[[nodiscard]] std::string_view DungeonMoodTitle(DungeonMood mood);
/** @brief Weathers that suit a level type (what it rolls from), for choosing one in the map editor. */
[[nodiscard]] std::vector<DungeonMood> MoodsForLevelType(dungeon_type type);
/** @brief Whether weather is switched on for a level type in the options. */
[[nodiscard]] bool MoodsEnabledFor(dungeon_type type);
/** @brief Start turning to another weather now (outdoors the next in turn; in dungeons another mood). */
void ChangeWeatherNow();
/** @brief Fade to a chosen weather on this level now (in town, the wilds or a dungeon). */
void SetWeatherNow(DungeonMood mood);
/** @brief The weathers this level can have (Clear first), for choosing one in the weather panel. */
[[nodiscard]] std::vector<DungeonMood> WeatherChoicesHere();

/**
 * @brief Tints the light around torches, candles, fire, spells and portals (full-colour overlay).
 * Call between ColorOverlayBegin and the mood, with the same surface.
 */
void DrawColoredLight(const Surface &out);

/**
 * @brief Day and night outdoors (see day_night.hpp): darkens and tints the scene, with pools of
 * light around heroes, torches, fire and portals. Also moves the clock on, so call it every frame
 * on every level, before DrawColoredLight and with the same surface.
 */
void DrawDayNight(const Surface &out);

} // namespace devilution
