#include "panels/weather_panel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

#include "control/control.hpp"
#include "engine/palette.h"
#include "engine/rectangle.hpp"
#include "engine/render/color_overlay.hpp"
#include "engine/render/dungeon_mood.hpp"
#include "engine/render/primitive_render.hpp"
#include "engine/render/text_render.hpp"
#include "options.h"
#include "utils/format.hpp"
#include "utils/language.h"
#include "utils/str_cat.hpp"
#include "utils/ui_fwd.h"

namespace devilution {

bool WeatherPanelOpen = false;

namespace {

constexpr int PanelWidth = 300;
constexpr int PanelHeight = 182;
constexpr int Margin = 14;
constexpr int SliderY = 90;
constexpr int SliderHeight = 10;
constexpr int IntensityMax = 200;
constexpr int IntensityStep = 5;

bool draggingSlider = false;

enum class Button {
	Weather,
	ChangeNow,
	Smooth,
	ColouredLight,
	Close,
};

Point PanelOrigin()
{
	return { (gnScreenWidth - PanelWidth) / 2, std::max(8, gnViewportHeight / 2 - PanelHeight / 2 - 60) };
}

Rectangle PanelRect()
{
	return { PanelOrigin(), { PanelWidth, PanelHeight } };
}

Rectangle SliderRect()
{
	return { PanelOrigin() + Displacement { Margin, SliderY }, { PanelWidth - 2 * Margin, SliderHeight } };
}

Rectangle ButtonRect(Button button)
{
	const Point o = PanelOrigin();
	const int half = (PanelWidth - 2 * Margin - 8) / 2;
	switch (button) {
	case Button::Weather: return { o + Displacement { Margin, 116 }, { half, 18 } };
	case Button::ChangeNow: return { o + Displacement { Margin + half + 8, 116 }, { half, 18 } };
	case Button::Smooth: return { o + Displacement { Margin, 140 }, { half, 18 } };
	case Button::ColouredLight: return { o + Displacement { Margin + half + 8, 140 }, { half, 18 } };
	case Button::Close: return { o + Displacement { PanelWidth - 22, 6 }, { 16, 14 } };
	}
	return {};
}

bool ChangeNowAvailable(const WeatherStatus &status)
{
	return status.active && status.outdoor && !status.changing && *GetOptions().Graphics.weatherEffects;
}

void SetIntensityFromMouse(int mouseX)
{
	const Rectangle r = SliderRect();
	const float t = std::clamp(static_cast<float>(mouseX - r.position.x) / static_cast<float>(r.size.width), 0.0f, 1.0f);
	int value = static_cast<int>(std::lround(t * IntensityMax / IntensityStep)) * IntensityStep;
	value = std::clamp(value, 0, IntensityMax);
	if (value != *GetOptions().Graphics.weatherIntensity)
		GetOptions().Graphics.weatherIntensity.SetValue(value);
}

void DrawFrame(const Surface &out, Rectangle r)
{
	// Darkened see-through background with a thin gold edge.
	DrawHalfTransparentRectTo(out, r.position.x, r.position.y, r.size.width, r.size.height);
	DrawHalfTransparentRectTo(out, r.position.x, r.position.y, r.size.width, r.size.height);
	DrawHorizontalLine(out, r.position, r.size.width, PAL16_YELLOW + 8);
	DrawHorizontalLine(out, r.position + Displacement { 0, r.size.height - 1 }, r.size.width, PAL16_YELLOW + 8);
	DrawVerticalLine(out, r.position, r.size.height, PAL16_YELLOW + 8);
	DrawVerticalLine(out, r.position + Displacement { r.size.width - 1, 0 }, r.size.height, PAL16_YELLOW + 8);
}

void DrawButton(const Surface &out, Button button, std::string_view label, bool on, bool enabled)
{
	const Rectangle r = ButtonRect(button);
	const bool hover = enabled && r.contains(MousePosition);
	FillRect(out, r.position.x, r.position.y, r.size.width, r.size.height, on ? PAL16_YELLOW + 12 : (hover ? PAL16_GRAY + 12 : PAL16_GRAY + 14));
	DrawHorizontalLine(out, r.position, r.size.width, PAL16_YELLOW + 9);
	DrawHorizontalLine(out, r.position + Displacement { 0, r.size.height - 1 }, r.size.width, PAL16_YELLOW + 9);
	DrawVerticalLine(out, r.position, r.size.height, PAL16_YELLOW + 9);
	DrawVerticalLine(out, r.position + Displacement { r.size.width - 1, 0 }, r.size.height, PAL16_YELLOW + 9);
	const UiFlags color = !enabled ? UiFlags::ColorBlack : (on ? UiFlags::ColorWhitegold : UiFlags::ColorWhite);
	DrawString(out, label, { r.position + Displacement { 0, 3 }, { r.size.width, 12 } }, { .flags = color | UiFlags::AlignCenter | UiFlags::KerningFitSpacing });
}

std::string FormatTime(float seconds)
{
	const int s = static_cast<int>(std::ceil(seconds));
	return StrCat(s / 60, ":", (s % 60) < 10 ? "0" : "", s % 60);
}

} // namespace

void ToggleWeatherPanel()
{
	WeatherPanelOpen = !WeatherPanelOpen;
	draggingSlider = false;
}

void DrawWeatherPanel(const Surface &out)
{
#ifdef _DEBUG
	static bool devOpened = false;
	if (!devOpened && std::getenv("DEVILUTIONX_DEV_WEATHERPANEL") != nullptr) {
		devOpened = true;
		WeatherPanelOpen = true;
	}
#endif
	if (!WeatherPanelOpen)
		return;
	if (draggingSlider)
		SetIntensityFromMouse(MousePosition.x);

	const Rectangle panel = PanelRect();
	const Point o = panel.position;
	DrawFrame(out, panel);
	const int textWidth = PanelWidth - 2 * Margin;

	DrawString(out, _("Weather"), { o + Displacement { 0, 8 }, { PanelWidth, 12 } }, { .flags = UiFlags::ColorWhitegold | UiFlags::AlignCenter });
	DrawString(out, "x", { ButtonRect(Button::Close).position + Displacement { 0, 1 }, ButtonRect(Button::Close).size },
	    { .flags = (ButtonRect(Button::Close).contains(MousePosition) ? UiFlags::ColorWhite : UiFlags::ColorGold) | UiFlags::AlignCenter });

	const WeatherStatus status = GetWeatherStatus();
	const bool on = *GetOptions().Graphics.weatherEffects;

	// Current weather.
	std::string now;
	if (!status.active) {
		now = _("No weather on this level");
	} else {
		now = StrCat(_("Now: "), DungeonMoodTitle(status.mood));
		if (status.mood != DungeonMood::Clear)
			now += StrCat(" (", status.strength > 0.85f ? _("heavy") : (status.strength > 0.7f ? _("moderate") : _("light")), ")");
		if (!on)
			now += StrCat(" - ", _("hidden"));
	}
	DrawString(out, now, { o + Displacement { Margin, 28 }, { textWidth, 12 } }, { .flags = UiFlags::ColorWhite });

	// When it turns.
	std::string timing;
	if (!status.active)
		timing = _("Enable it under Settings > Graphics.");
	else if (!status.outdoor)
		timing = _("Lasts until you leave this level.");
	else if (status.changing)
		timing = _("Changing now...");
	else
		timing = StrCat(_("Changes in "), FormatTime(status.secondsLeft));
	DrawString(out, timing, { o + Displacement { Margin, 46 }, { textWidth, 12 } }, { .flags = UiFlags::ColorGold });

	// Intensity slider.
	const int intensity = *GetOptions().Graphics.weatherIntensity;
	DrawString(out, _("Intensity"), { o + Displacement { Margin, 70 }, { textWidth, 12 } }, { .flags = UiFlags::ColorWhite });
	DrawString(out, StrCat(intensity, "%"), { o + Displacement { Margin, 70 }, { textWidth, 12 } }, { .flags = UiFlags::ColorWhitegold | UiFlags::AlignRight });
	const Rectangle track = SliderRect();
	FillRect(out, track.position.x, track.position.y + 3, track.size.width, 4, PAL16_GRAY + 12);
	const int fillWidth = track.size.width * intensity / IntensityMax;
	FillRect(out, track.position.x, track.position.y + 3, fillWidth, 4, on ? PAL16_YELLOW + 6 : PAL16_GRAY + 8);
	// Tick at 100% (the normal strength).
	DrawVerticalLine(out, track.position + Displacement { track.size.width / 2, 0 }, SliderHeight, PAL16_GRAY + 6);
	const int knobX = track.position.x + fillWidth - 3;
	FillRect(out, knobX, track.position.y, 7, SliderHeight, (draggingSlider || track.contains(MousePosition)) ? PAL16_YELLOW + 2 : PAL16_YELLOW + 4);

	// Switches.
	DrawButton(out, Button::Weather, on ? _("Weather: On") : _("Weather: Off"), on, true);
	DrawButton(out, Button::ChangeNow, _("Change weather"), false, ChangeNowAvailable(status));
	const bool smooth = *GetOptions().Graphics.smoothEffects;
	DrawButton(out, Button::Smooth, smooth ? _("Smooth effects: On") : _("Smooth effects: Off"), smooth, true);
	const bool coloured = *GetOptions().Graphics.coloredLighting;
	DrawButton(out, Button::ColouredLight, coloured ? _("Coloured light: On") : _("Coloured light: Off"), coloured, smooth && ColorOverlayAvailable());

	DrawString(out, _("F8: close"), { o + Displacement { 0, PanelHeight - 18 }, { PanelWidth, 12 } }, { .flags = UiFlags::ColorWhite | UiFlags::AlignCenter });
}

bool WeatherPanelMouseDown(Point mousePosition)
{
	if (!WeatherPanelOpen || !PanelRect().contains(mousePosition))
		return false;
	Rectangle grab = SliderRect();
	grab.position.y -= 4;
	grab.size.height += 8;
	grab.position.x -= 6;
	grab.size.width += 12;
	if (grab.contains(mousePosition)) {
		draggingSlider = true;
		SetIntensityFromMouse(mousePosition.x);
		return true;
	}
	auto &graphics = GetOptions().Graphics;
	if (ButtonRect(Button::Close).contains(mousePosition)) {
		ToggleWeatherPanel();
	} else if (ButtonRect(Button::Weather).contains(mousePosition)) {
		graphics.weatherEffects.SetValue(!*graphics.weatherEffects);
	} else if (ButtonRect(Button::ChangeNow).contains(mousePosition)) {
		if (ChangeNowAvailable(GetWeatherStatus()))
			ChangeWeatherNow();
	} else if (ButtonRect(Button::Smooth).contains(mousePosition)) {
		graphics.smoothEffects.SetValue(!*graphics.smoothEffects);
	} else if (ButtonRect(Button::ColouredLight).contains(mousePosition)) {
		if (*graphics.smoothEffects && ColorOverlayAvailable())
			graphics.coloredLighting.SetValue(!*graphics.coloredLighting);
	}
	return true; // clicks on the panel never reach the game world
}

void WeatherPanelMouseUp()
{
	draggingSlider = false;
}

} // namespace devilution
