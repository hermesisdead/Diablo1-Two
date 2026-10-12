/**
 * @file town_weather.hpp
 *
 * Randomized weather effects drawn over Tristram.
 */
#pragma once

#include <cstdint>

#include "engine/surface.hpp"

namespace devilution {

enum class TownWeather : uint8_t {
	Clear,
	Rain,
	Storm,
	Snow,
	Fog,
	Embers,
};

/** @brief Picks the weather for this visit to the town. */
void InitTownWeather();

[[nodiscard]] TownWeather GetTownWeather();

/**
 * @brief Draws the current weather over the game viewport.
 * @param out The viewport region of the back buffer.
 */
void DrawTownWeather(const Surface &out);

} // namespace devilution
