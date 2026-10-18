#include "engine/render/day_night.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include "diablo.h"
#include "options.h"
#include "utils/str_cat.hpp"

namespace devilution {

namespace {

/** Half the length of a twilight, in hours: the change takes 1.5 hours, centred on the threshold. */
constexpr float Twilight = 0.75f;

float clockHours = -1.0f; // -1: not yet read from the options
uint32_t lastTicks = 0;
int savedMinutes = -1;
bool frozen = false; // debug: a fixed time for captures

float Wrap(float hours)
{
	hours = std::fmod(hours, 24.0f);
	return hours < 0 ? hours + 24.0f : hours;
}

/** Signed distance from `from` to `to` around the clock, -12..12. */
float ClockDelta(float from, float to)
{
	float d = Wrap(to - from);
	return d > 12.0f ? d - 24.0f : d;
}

float SmoothStep(float x)
{
	x = std::clamp(x, 0.0f, 1.0f);
	return x * x * (3 - 2 * x);
}

void EnsureClock()
{
	if (clockHours >= 0)
		return;
	clockHours = Wrap(static_cast<float>(*GetOptions().Graphics.timeOfDay) / 60.0f);
	savedMinutes = *GetOptions().Graphics.timeOfDay;
#ifdef _DEBUG
	if (const char *fixed = std::getenv("DEVILUTIONX_DEV_TIMEOFDAY"); fixed != nullptr) {
		clockHours = Wrap(static_cast<float>(std::atof(fixed)));
		frozen = true;
	}
#endif
}

void Remember()
{
	const int minutes = static_cast<int>(clockHours * 60.0f);
	// Kept in the options, so the next session starts where this one left off.
	if (std::abs(minutes - savedMinutes) >= 5) {
		savedMinutes = minutes;
		GetOptions().Graphics.timeOfDay.SetValue(minutes);
	}
}

} // namespace

bool DayNightEnabled()
{
	return *GetOptions().Graphics.dayNightCycle;
}

void DayNightThresholds(int &dawn, int &dusk)
{
	dawn = std::clamp(*GetOptions().Graphics.dawnHour, 0, 23);
	dusk = std::clamp(*GetOptions().Graphics.duskHour, 0, 23);
	// At least two hours of day and of night, so the twilights never overlap.
	const int day = (dusk - dawn + 24) % 24;
	if (day < 2)
		dusk = (dawn + 2) % 24;
	else if (day > 22)
		dusk = (dawn + 22) % 24;
}

float TimeOfDay()
{
	EnsureClock();
	return clockHours;
}

void SetTimeOfDay(float hours)
{
	EnsureClock();
	clockHours = Wrap(hours);
	Remember();
}

void AdvanceDayClock()
{
	EnsureClock();
	const uint32_t now = SDL_GetTicks();
	const float dt = lastTicks == 0 ? 0.0f : std::min(0.25f, static_cast<float>(now - lastTicks) / 1000.0f);
	lastTicks = now;
	if (frozen || PauseMode != 0 || !DayNightEnabled())
		return;
	const float minutesPerDay = static_cast<float>(std::max(1, *GetOptions().Graphics.dayLength));
	clockHours = Wrap(clockHours + dt * 24.0f / (minutesPerDay * 60.0f));
	Remember();
}

float NightAmountAt(float hours)
{
	int dawn, dusk;
	DayNightThresholds(dawn, dusk);
	// Day rises through the dawn twilight and falls through the dusk twilight.
	const float sinceDawn = ClockDelta(static_cast<float>(dawn), hours);
	const float sinceDusk = ClockDelta(static_cast<float>(dusk), hours);
	const float dayLength = static_cast<float>((dusk - dawn + 24) % 24);
	const float intoDay = Wrap(hours - static_cast<float>(dawn));
	if (std::abs(sinceDawn) < Twilight)
		return 1.0f - SmoothStep((sinceDawn + Twilight) / (2 * Twilight));
	if (std::abs(sinceDusk) < Twilight)
		return SmoothStep((sinceDusk + Twilight) / (2 * Twilight));
	return intoDay < dayLength ? 0.0f : 1.0f;
}

float NightAmount()
{
	if (!DayNightEnabled())
		return 0.0f;
	return NightAmountAt(TimeOfDay());
}

namespace {
float GlowAround(int threshold)
{
	if (!DayNightEnabled())
		return 0.0f;
	const float d = std::abs(ClockDelta(static_cast<float>(threshold), TimeOfDay()));
	const float t = 1.0f - d / (Twilight * 1.6f);
	return t > 0 ? SmoothStep(t) : 0.0f;
}
} // namespace

float DuskGlow()
{
	int dawn, dusk;
	DayNightThresholds(dawn, dusk);
	return GlowAround(dusk);
}

float DawnGlow()
{
	int dawn, dusk;
	DayNightThresholds(dawn, dusk);
	return GlowAround(dawn);
}

bool IsNightNow()
{
	return NightAmount() >= 0.5f;
}

std::string ClockText(float hours)
{
	const int minutes = static_cast<int>(Wrap(hours) * 60.0f);
	const int h = minutes / 60, m = minutes % 60;
	return StrCat(h < 10 ? "0" : "", h, ":", m < 10 ? "0" : "", m);
}

void SkipToNextDayPhase()
{
	int dawn, dusk;
	DayNightThresholds(dawn, dusk);
	// Land just past the twilight, so the change is complete.
	const float target = static_cast<float>(NightAmountAt(TimeOfDay()) >= 0.5f ? dawn : dusk) + Twilight;
	SetTimeOfDay(target);
}

} // namespace devilution
