/**
 * @file panels/weather_panel.hpp
 *
 * Weather panel (F8): shows the current weather and when it turns, and sets weather intensity,
 * weather on/off, smooth effects and coloured lighting.
 */
#pragma once

#include "engine/point.hpp"
#include "engine/surface.hpp"

namespace devilution {

extern bool WeatherPanelOpen;

/** @brief F8: open or close the weather panel. */
void ToggleWeatherPanel();

void DrawWeatherPanel(const Surface &out);

/** @brief Left mouse button pressed. Returns true when the click was on the panel. */
bool WeatherPanelMouseDown(Point mousePosition);
/** @brief Left mouse button released (ends a slider drag). */
void WeatherPanelMouseUp();

} // namespace devilution
