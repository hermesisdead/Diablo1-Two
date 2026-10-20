#include "panels/weather_panel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

#include "control/control.hpp"
#include "engine/palette.h"
#include "engine/rectangle.hpp"
#include "engine/render/color_overlay.hpp"
#include "engine/render/day_night.hpp"
#include "engine/render/painterly.hpp"
#include "engine/render/dungeon_mood.hpp"
#include "engine/render/primitive_render.hpp"
#include "engine/render/text_render.hpp"
#include "options.h"
#include "towners.h"
#include "utils/format.hpp"
#include "utils/language.h"
#include "utils/str_cat.hpp"
#include "utils/ui_fwd.h"

namespace devilution {

bool WeatherPanelOpen = false;

namespace {

constexpr int PanelWidth = 300;
constexpr int PanelHeight = 338;
/** Top of the day and night section. */
constexpr int DayY = 168;
constexpr int RowHeight = 20;
constexpr int Margin = 14;
constexpr int SliderY = 90;
constexpr int SliderHeight = 10;
constexpr int IntensityMax = 200;
constexpr int IntensityStep = 5;

/** Which slider the mouse is dragging: 0 none, 1 weather intensity, 2 paint smoothing, 3 paint grain. */
int draggingSlider = 0;
/** 0: weather and day/night, 1: the look (textures, painterly smoothing and grain). */
int page = 0;
constexpr int LookHeight = 176;
constexpr int LookSliderY[2] = { 76, 116 };

enum class Button {
	Weather,
	ChangeNow,
	Smooth,
	ColouredLight,
	DayNight,
	SkipPhase,
	Close,
};

/** The day and night settings stepped with - and + buttons. */
enum class DayRow {
	Dawn,
	Dusk,
	Darkness,
	Length,
	COUNT
};

OptionEntryListBase &RowOption(DayRow row)
{
	auto &graphics = GetOptions().Graphics;
	switch (row) {
	case DayRow::Dawn: return graphics.dawnHour;
	case DayRow::Dusk: return graphics.duskHour;
	case DayRow::Darkness: return graphics.nightDarkness;
	default: return graphics.dayLength;
	}
}

Point PanelOrigin()
{
	// The look page sits at the side, so the town stays in view while it is tuned.
	if (page == 1)
		return { 8, 8 };
	return { (gnScreenWidth - PanelWidth) / 2, std::max(8, gnViewportHeight / 2 - PanelHeight / 2 - 60) };
}

int PanelHeightNow()
{
	return page == 1 ? LookHeight : PanelHeight;
}

Rectangle PanelRect()
{
	return { PanelOrigin(), { PanelWidth, PanelHeightNow() } };
}

/** The page tabs at the top of the panel. */
Rectangle TabRect(int tab)
{
	return { PanelOrigin() + Displacement { Margin + tab * 74, 6 }, { 70, 16 } };
}

Rectangle LookSliderRect(int slider)
{
	return { PanelOrigin() + Displacement { Margin, LookSliderY[slider] }, { PanelWidth - 2 * Margin, SliderHeight } };
}

Rectangle LookTexturesRect()
{
	return { PanelOrigin() + Displacement { Margin, 30 }, { PanelWidth - 2 * Margin, 18 } };
}

OptionEntryInt<int> &LookOption(int slider)
{
	return slider == 0 ? GetOptions().Graphics.paintSmoothing : GetOptions().Graphics.paintGrain;
}

void SetLookFromMouse(int slider, int mouseX)
{
	const Rectangle r = LookSliderRect(slider);
	const float t = std::clamp(static_cast<float>(mouseX - r.position.x) / static_cast<float>(r.size.width), 0.0f, 1.0f);
	const int value = std::clamp(static_cast<int>(std::lround(t * 20.0f)) * 5, 0, 100);
	if (value != *LookOption(slider))
		LookOption(slider).SetValue(value);
}

std::string_view TextureModeName(TextureMode mode)
{
	switch (mode) {
	case TextureMode::Vanilla: return _("Vanilla");
	case TextureMode::HD: return _("HD");
	case TextureMode::HDRedux: return _("HD Redux");
	case TextureMode::HudOnly: return _("HUD Only");
	case TextureMode::Painterly: return _("Painterly");
	}
	return "";
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
	case Button::DayNight: return { o + Displacement { Margin, DayY + 36 + 4 * RowHeight + 6 }, { half, 18 } };
	case Button::SkipPhase: return { o + Displacement { Margin + half + 8, DayY + 36 + 4 * RowHeight + 6 }, { half, 18 } };
	case Button::Close: return { o + Displacement { PanelWidth - 22, 6 }, { 16, 14 } };
	}
	return {};
}

/** The - (plus = false) or + button of a day and night row. */
Rectangle StepRect(DayRow row, bool plus)
{
	const Point o = PanelOrigin();
	const int y = DayY + 36 + static_cast<int>(row) * RowHeight;
	const int x = PanelWidth - Margin - (plus ? 18 : 18 + 18 + 64);
	return { o + Displacement { x, y }, { 18, 16 } };
}

std::string RowValue(DayRow row)
{
	auto &graphics = GetOptions().Graphics;
	switch (row) {
	case DayRow::Dawn: return ClockText(static_cast<float>(*graphics.dawnHour));
	case DayRow::Dusk: return ClockText(static_cast<float>(*graphics.duskHour));
	case DayRow::Darkness: return StrCat(*graphics.nightDarkness, "%");
	default: return StrCat(*graphics.dayLength, " ", _("min"));
	}
}

std::string_view RowLabel(DayRow row)
{
	switch (row) {
	case DayRow::Dawn: return _("Day begins");
	case DayRow::Dusk: return _("Night begins");
	case DayRow::Darkness: return _("Night darkness");
	default: return _("Length of a day");
	}
}

void StepRow(DayRow row, bool plus)
{
	OptionEntryListBase &option = RowOption(row);
	const size_t index = option.GetActiveListIndex();
	if (plus && index + 1 < option.GetListSize())
		option.SetActiveListIndex(index + 1);
	else if (!plus && index > 0)
		option.SetActiveListIndex(index - 1);
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
	draggingSlider = 0;
}

namespace {

void DrawSlider(const Surface &out, Rectangle track, int value, int max, bool on, bool dragging)
{
	FillRect(out, track.position.x, track.position.y + 3, track.size.width, 4, PAL16_GRAY + 12);
	const int fillWidth = track.size.width * value / max;
	FillRect(out, track.position.x, track.position.y + 3, fillWidth, 4, on ? PAL16_YELLOW + 6 : PAL16_GRAY + 8);
	const int knobX = track.position.x + fillWidth - 3;
	FillRect(out, knobX, track.position.y, 7, SliderHeight, (dragging || track.contains(MousePosition)) ? PAL16_YELLOW + 2 : PAL16_YELLOW + 4);
}

void DrawTabs(const Surface &out)
{
	for (int tab = 0; tab < 2; tab++) {
		const Rectangle r = TabRect(tab);
		const bool on = page == tab;
		const bool hover = r.contains(MousePosition);
		FillRect(out, r.position.x, r.position.y, r.size.width, r.size.height, on ? PAL16_YELLOW + 12 : (hover ? PAL16_GRAY + 12 : PAL16_GRAY + 14));
		DrawHorizontalLine(out, r.position + Displacement { 0, r.size.height - 1 }, r.size.width, PAL16_YELLOW + 9);
		DrawString(out, tab == 0 ? _("Weather") : _("Look"), { r.position + Displacement { 0, 2 }, { r.size.width, 12 } },
		    { .flags = (on ? UiFlags::ColorWhitegold : UiFlags::ColorWhite) | UiFlags::AlignCenter });
	}
}

/** The look page: texture mode, and the Painterly shader's smoothing and grain, live. */
void DrawLookPage(const Surface &out)
{
	const Rectangle panel = PanelRect();
	const Point o = panel.position;
	const int textWidth = PanelWidth - 2 * Margin;
	const TextureMode mode = *GetOptions().Graphics.textures;
	const bool painterly = mode == TextureMode::Painterly;

	const Rectangle tex = LookTexturesRect();
	const bool hover = tex.contains(MousePosition);
	FillRect(out, tex.position.x, tex.position.y, tex.size.width, tex.size.height, painterly ? PAL16_YELLOW + 12 : (hover ? PAL16_GRAY + 12 : PAL16_GRAY + 14));
	DrawHorizontalLine(out, tex.position, tex.size.width, PAL16_YELLOW + 9);
	DrawHorizontalLine(out, tex.position + Displacement { 0, tex.size.height - 1 }, tex.size.width, PAL16_YELLOW + 9);
	DrawString(out, StrCat(_("Textures: "), TextureModeName(mode), "   (\\)"), { tex.position + Displacement { 0, 3 }, { tex.size.width, 12 } },
	    { .flags = (painterly ? UiFlags::ColorWhitegold : UiFlags::ColorWhite) | UiFlags::AlignCenter });

	for (int slider = 0; slider < 2; slider++) {
		const int y = LookSliderY[slider] - 18;
		const int value = *LookOption(slider);
		DrawString(out, slider == 0 ? _("Smoothing") : _("Grain"), { o + Displacement { Margin, y }, { textWidth, 12 } }, { .flags = UiFlags::ColorWhite });
		DrawString(out, StrCat(value, "%"), { o + Displacement { Margin, y }, { textWidth, 12 } }, { .flags = UiFlags::ColorWhitegold | UiFlags::AlignRight });
		DrawSlider(out, LookSliderRect(slider), value, 100, painterly, draggingSlider == slider + 2);
	}

	std::string status;
	if (!painterly)
		status = _("Choose Painterly above to see these.");
	else
		status = std::string(PainterlyStatus());
	DrawString(out, status, { o + Displacement { Margin, 134 }, { textWidth, 12 } }, { .flags = (painterly && PainterlyRunning()) ? UiFlags::ColorGold : UiFlags::ColorWhite });
	DrawString(out, _("F8: close"), { o + Displacement { 0, LookHeight - 18 }, { PanelWidth, 12 } }, { .flags = UiFlags::ColorWhite | UiFlags::AlignCenter });
}

} // namespace

void DrawWeatherPanel(const Surface &out)
{
#ifdef _DEBUG
	static bool devOpened = false;
	if (const char *dev = std::getenv("DEVILUTIONX_DEV_WEATHERPANEL"); !devOpened && dev != nullptr) {
		devOpened = true;
		WeatherPanelOpen = true;
		page = std::string_view(dev) == "look" ? 1 : 0;
	}
#endif
	if (!WeatherPanelOpen)
		return;
	if (draggingSlider == 1)
		SetIntensityFromMouse(MousePosition.x);
	else if (draggingSlider >= 2)
		SetLookFromMouse(draggingSlider - 2, MousePosition.x);

	const Rectangle panel = PanelRect();
	const Point o = panel.position;
	DrawFrame(out, panel);
	const int textWidth = PanelWidth - 2 * Margin;

	DrawTabs(out);
	DrawString(out, "x", { ButtonRect(Button::Close).position + Displacement { 0, 1 }, ButtonRect(Button::Close).size },
	    { .flags = (ButtonRect(Button::Close).contains(MousePosition) ? UiFlags::ColorWhite : UiFlags::ColorGold) | UiFlags::AlignCenter });
	if (page == 1) {
		DrawLookPage(out);
		return;
	}

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
	FillRect(out, knobX, track.position.y, 7, SliderHeight, (draggingSlider == 1 || track.contains(MousePosition)) ? PAL16_YELLOW + 2 : PAL16_YELLOW + 4);

	// Switches.
	DrawButton(out, Button::Weather, on ? _("Weather: On") : _("Weather: Off"), on, true);
	DrawButton(out, Button::ChangeNow, _("Change weather"), false, ChangeNowAvailable(status));
	const bool smooth = *GetOptions().Graphics.smoothEffects;
	DrawButton(out, Button::Smooth, smooth ? _("Smooth effects: On") : _("Smooth effects: Off"), smooth, true);
	const bool coloured = *GetOptions().Graphics.coloredLighting;
	DrawButton(out, Button::ColouredLight, coloured ? _("Coloured light: On") : _("Coloured light: Off"), coloured, smooth && ColorOverlayAvailable());

	// Day and night.
	const bool cycle = DayNightEnabled();
	DrawHorizontalLine(out, o + Displacement { Margin, DayY }, textWidth, PAL16_YELLOW + 10);
	DrawString(out, _("Day & Night"), { o + Displacement { 0, DayY + 4 }, { PanelWidth, 12 } }, { .flags = UiFlags::ColorWhitegold | UiFlags::AlignCenter });
	const float night = NightAmountAt(TimeOfDay());
	std::string_view phase;
	if (DuskGlow() > 0.25f && night < 0.9f)
		phase = _("Dusk");
	else if (DawnGlow() > 0.25f && night < 0.9f)
		phase = _("Dawn");
	else
		phase = night >= 0.5f ? _("Night") : _("Day");
	DrawString(out, StrCat(_("Time: "), ClockText(TimeOfDay()), " - ", phase, cycle ? "" : StrCat(" (", _("stopped"), ")")),
	    { o + Displacement { Margin, DayY + 20 }, { textWidth, 12 } }, { .flags = UiFlags::ColorWhite });
	for (int r = 0; r < static_cast<int>(DayRow::COUNT); r++) {
		const DayRow row = static_cast<DayRow>(r);
		const Rectangle minus = StepRect(row, false);
		const Rectangle plus = StepRect(row, true);
		DrawString(out, RowLabel(row), { Point { o.x + Margin, minus.position.y + 2 }, { textWidth, 12 } }, { .flags = UiFlags::ColorGold });
		for (const Rectangle &b : { minus, plus }) {
			const bool hover = b.contains(MousePosition);
			FillRect(out, b.position.x, b.position.y, b.size.width, b.size.height, hover ? PAL16_GRAY + 12 : PAL16_GRAY + 14);
			DrawHorizontalLine(out, b.position, b.size.width, PAL16_YELLOW + 9);
			DrawHorizontalLine(out, b.position + Displacement { 0, b.size.height - 1 }, b.size.width, PAL16_YELLOW + 9);
			DrawVerticalLine(out, b.position, b.size.height, PAL16_YELLOW + 9);
			DrawVerticalLine(out, b.position + Displacement { b.size.width - 1, 0 }, b.size.height, PAL16_YELLOW + 9);
		}
		DrawString(out, "-", { minus.position + Displacement { 0, 2 }, minus.size }, { .flags = UiFlags::ColorWhite | UiFlags::AlignCenter });
		DrawString(out, "+", { plus.position + Displacement { 0, 2 }, plus.size }, { .flags = UiFlags::ColorWhite | UiFlags::AlignCenter });
		DrawString(out, RowValue(row), { Point { minus.position.x + minus.size.width, minus.position.y + 2 }, { plus.position.x - minus.position.x - minus.size.width, 12 } },
		    { .flags = UiFlags::ColorWhitegold | UiFlags::AlignCenter });
	}
	DrawButton(out, Button::DayNight, cycle ? _("Day/night: On") : _("Day/night: Off"), cycle, true);
	DrawButton(out, Button::SkipPhase, night >= 0.5f ? _("Skip to day") : _("Skip to night"), false, true);

	DrawString(out, _("F8: close"), { o + Displacement { 0, PanelHeight - 18 }, { PanelWidth, 12 } }, { .flags = UiFlags::ColorWhite | UiFlags::AlignCenter });
}

bool WeatherPanelMouseDown(Point mousePosition)
{
	if (!WeatherPanelOpen || !PanelRect().contains(mousePosition))
		return false;
	for (int tab = 0; tab < 2; tab++) {
		if (TabRect(tab).contains(mousePosition)) {
			page = tab;
			draggingSlider = 0;
			return true;
		}
	}
	if (ButtonRect(Button::Close).contains(mousePosition)) {
		ToggleWeatherPanel();
		return true;
	}
	if (page == 1) {
		for (int slider = 0; slider < 2; slider++) {
			Rectangle g = LookSliderRect(slider);
			g.position.y -= 4;
			g.size.height += 8;
			g.position.x -= 6;
			g.size.width += 12;
			if (g.contains(mousePosition)) {
				draggingSlider = slider + 2;
				SetLookFromMouse(slider, mousePosition.x);
				return true;
			}
		}
		if (LookTexturesRect().contains(mousePosition))
			ToggleNewTextures();
		return true;
	}
	Rectangle grab = SliderRect();
	grab.position.y -= 4;
	grab.size.height += 8;
	grab.position.x -= 6;
	grab.size.width += 12;
	if (grab.contains(mousePosition)) {
		draggingSlider = 1;
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
	} else if (ButtonRect(Button::DayNight).contains(mousePosition)) {
		graphics.dayNightCycle.SetValue(!*graphics.dayNightCycle);
	} else if (ButtonRect(Button::SkipPhase).contains(mousePosition)) {
		SkipToNextDayPhase();
	} else {
		for (int r = 0; r < static_cast<int>(DayRow::COUNT); r++) {
			for (const bool plus : { false, true }) {
				if (StepRect(static_cast<DayRow>(r), plus).contains(mousePosition))
					StepRow(static_cast<DayRow>(r), plus);
			}
		}
	}
	return true; // clicks on the panel never reach the game world
}

void WeatherPanelMouseUp()
{
	draggingSlider = 0;
}

} // namespace devilution
