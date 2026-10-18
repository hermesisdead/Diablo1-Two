/**
 * @file day_night.hpp
 *
 * Day and night in Tristram and the Darkwood: a clock that runs in real time (one full day takes
 * the "Day Length" option), turning to night at the "Night Begins" hour and back to day at the
 * "Day Begins" hour, with a short twilight around each. The scene is darkened and tinted in full
 * colour (see DrawDayNight in dungeon_mood.hpp); lights and the hero's lantern glow at night.
 */
#pragma once

#include <string>

namespace devilution {

/** @brief The cycle is switched on in the options. */
[[nodiscard]] bool DayNightEnabled();

/** @brief The clock, in hours after midnight (0..24). */
[[nodiscard]] float TimeOfDay();
void SetTimeOfDay(float hours);

/** @brief Moves the clock on by the real time since the last call (once per drawn frame). */
void AdvanceDayClock();

/** @brief How much night it is at an hour: 0 full day .. 1 full night (ignores the on/off switch). */
[[nodiscard]] float NightAmountAt(float hours);
/** @brief How much night it is now: 0 day .. 1 night; 0 when the cycle is off. */
[[nodiscard]] float NightAmount();
/** @brief Sunset / sunrise glow now, 0..1 (peaks at the dusk / dawn hour). */
[[nodiscard]] float DuskGlow();
[[nodiscard]] float DawnGlow();
/** @brief More night than day right now (and the cycle is on). */
[[nodiscard]] bool IsNightNow();

/** @brief The clock as "14:05". */
[[nodiscard]] std::string ClockText(float hours);
/** @brief The day / night thresholds in effect (the options, kept at least two hours apart). */
void DayNightThresholds(int &dawn, int &dusk);
/** @brief Jumps the clock to the start of the next day or night. */
void SkipToNextDayPhase();

} // namespace devilution
