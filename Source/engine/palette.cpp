/**
 * @file palette.cpp
 *
 * Implementation of functions for handling the engines color palette.
 */
#include "engine/palette.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>

#ifdef USE_SDL3
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include "engine/backbuffer_state.hpp"
#include "engine/demomode.h"
#include "engine/dx.h"
#include "engine/load_file.hpp"
#include "engine/random.hpp"
#include "headless_mode.hpp"
#include "hwcursor.hpp"
#include "options.h"
#include "utils/display.h"
#include "utils/palette_blending.hpp"
#include "utils/sdl_compat.h"
#include "utils/str_cat.hpp"

namespace devilution {

std::array<SDL_Color, 256> logical_palette;
std::array<SDL_Color, 256> system_palette;

namespace {

/** Specifies whether the palette has max brightness. */
bool sgbFadedIn = true;

void LoadBrightness()
{
	int brightnessValue = *GetOptions().Graphics.brightness;
	brightnessValue = std::clamp(brightnessValue, 0, 100);
	GetOptions().Graphics.brightness.SetValue(brightnessValue - (brightnessValue % 5));
}

/**
 * @brief Cycle the given range of colors in the palette
 * @param from First color index of the range
 * @param to First color index of the range
 */
void CycleColors(int from, int to)
{
	std::rotate(logical_palette.begin() + from, logical_palette.begin() + from + 1, logical_palette.begin() + to + 1);
	std::rotate(system_palette.begin() + from, system_palette.begin() + from + 1, system_palette.begin() + to + 1);

	for (auto &palette : paletteTransparencyLookup) {
		std::rotate(std::begin(palette) + from, std::begin(palette) + from + 1, std::begin(palette) + to + 1);
	}

	std::rotate(&paletteTransparencyLookup[from][0], &paletteTransparencyLookup[from + 1][0], &paletteTransparencyLookup[to + 1][0]);

#if DEVILUTIONX_PALETTE_TRANSPARENCY_BLACK_16_LUT
	UpdateTransparencyLookupBlack16(from, to);
#endif
}

/**
 * @brief Cycle the given range of colors in the palette in reverse direction
 * @param from First color index of the range
 * @param to Last color index of the range
 */
void CycleColorsReverse(int from, int to)
{
	std::rotate(logical_palette.begin() + from, logical_palette.begin() + to, logical_palette.begin() + to + 1);
	std::rotate(system_palette.begin() + from, system_palette.begin() + to, system_palette.begin() + to + 1);

	for (auto &palette : paletteTransparencyLookup) {
		std::rotate(std::begin(palette) + from, std::begin(palette) + to, std::begin(palette) + to + 1);
	}

	std::rotate(&paletteTransparencyLookup[from][0], &paletteTransparencyLookup[to][0], &paletteTransparencyLookup[to + 1][0]);

#if DEVILUTIONX_PALETTE_TRANSPARENCY_BLACK_16_LUT
	UpdateTransparencyLookupBlack16(from, to);
#endif
}

// When brightness==0, then a==0 (identity mapping)
// When brightness==100, then a==-MaxAdjustment (maximum brightening)
constexpr float CalculateToneMappingParameter(int brightness)
{
	// Maximum adjustment factor (tweak this constant to change the effect strength)
	constexpr float MaxAdjustment = 2.0F;
	return -(brightness / 100.0F) * MaxAdjustment;
}

constexpr uint8_t MapTone(float a, uint8_t color)
{
	const auto x = static_cast<float>(color / 255.0F);
	// Our quadratic tone mapping: f(x) = a*x^2 + (1-a)*x.
	const float y = std::clamp((a * x * x) + ((1.0F - a) * x), 0.0F, 1.0F);
	return static_cast<uint8_t>((y * 255.0F) + 0.5F);
}

void ApplyGlobalBrightnessSingleColor(SDL_Color &dst, const SDL_Color &src)
{
	const float a = CalculateToneMappingParameter(*GetOptions().Graphics.brightness);
	dst.r = MapTone(a, src.r);
	dst.g = MapTone(a, src.g);
	dst.b = MapTone(a, src.b);
}

} // namespace

void ApplyGlobalBrightness(SDL_Color *dst, const SDL_Color *src)
{
	// Get the brightness slider value (0 = neutral, 100 = max brightening)
	const int brightnessSlider = *GetOptions().Graphics.brightness;

	// Precompute a lookup table for speed.
	const float a = CalculateToneMappingParameter(brightnessSlider);
	uint8_t toneMap[256];
	for (int i = 0; i < 256; i++) {
		toneMap[i] = MapTone(a, i);
	}

	// Apply the lookup table to each color channel in the palette.
	for (int i = 0; i < 256; i++) {
		dst[i].r = toneMap[src[i].r];
		dst[i].g = toneMap[src[i].g];
		dst[i].b = toneMap[src[i].b];
	}
}

void ApplyFadeLevel(unsigned fadeval, SDL_Color *dst, const SDL_Color *src)
{
	for (int i = 0; i < 256; i++) {
		dst[i].r = (fadeval * src[i].r) / 256;
		dst[i].g = (fadeval * src[i].g) / 256;
		dst[i].b = (fadeval * src[i].b) / 256;
	}
}

// Applies a tone mapping curve based on the brightness slider value.
// The brightness value is in the range [0, 100] where 0 is neutral (no change)
// and 100 produces maximum brightening.
void UpdateSystemPalette(std::span<const SDL_Color, 256> src)
{
	ApplyGlobalBrightness(system_palette.data(), src.data());
	SystemPaletteUpdated();
	RedrawEverything();
}

void SystemPaletteUpdated(int first, int ncolor)
{
	if (HeadlessMode)
		return;

	assert(Palette);
	if (!SDLC_SetSurfaceAndPaletteColors(PalSurface, Palette.get(), system_palette.data() + first, first, ncolor)) {
		ErrSdl();
	}
}

void palette_init()
{
	LoadBrightness();
}

void LoadPalette(const char *path)
{
	assert(path != nullptr);
	if (HeadlessMode) return;

	LogVerbose("Loading palette from {}", path);
	std::array<Color, 256> palData;
	LoadFileInMem(path, palData);
	for (unsigned i = 0; i < palData.size(); i++) {
		logical_palette[i] = palData[i].toSDL();
	}
}

namespace {

/** Small deterministic generator so palette variation never touches the game's random state. */
struct PaletteRng {
	uint64_t state;
	float next01()
	{
		state += 0x9E3779B97F4A7C15ULL;
		uint64_t z = state;
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
		z ^= z >> 31;
		return static_cast<float>(z >> 40) / static_cast<float>(1 << 24);
	}
	float range(float lo, float hi) { return lo + (hi - lo) * next01(); }
};

void RgbToHsv(float r, float g, float b, float &h, float &s, float &v)
{
	const float mx = std::max({ r, g, b });
	const float mn = std::min({ r, g, b });
	const float d = mx - mn;
	v = mx;
	s = mx > 0.0f ? d / mx : 0.0f;
	if (d <= 0.0f) {
		h = 0.0f;
	} else if (mx == r) {
		h = std::fmod((g - b) / d, 6.0f);
	} else if (mx == g) {
		h = (b - r) / d + 2.0f;
	} else {
		h = (r - g) / d + 4.0f;
	}
	h *= 60.0f;
	if (h < 0.0f) h += 360.0f;
}

void HsvToRgb(float h, float s, float v, float &r, float &g, float &b)
{
	h = std::fmod(h, 360.0f);
	if (h < 0.0f) h += 360.0f;
	const float c = v * s;
	const float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
	const float m = v - c;
	float rp = 0, gp = 0, bp = 0;
	if (h < 60) { rp = c; gp = x; }
	else if (h < 120) { rp = x; gp = c; }
	else if (h < 180) { gp = c; bp = x; }
	else if (h < 240) { gp = x; bp = c; }
	else if (h < 300) { rp = x; bp = c; }
	else { rp = c; bp = x; }
	r = rp + m;
	g = gp + m;
	b = bp + m;
}

/**
 * @brief Gives a dungeon level its own colour mood: rotates hues, rescales saturation and tints the greys.
 *
 * Only the level-specific half of the palette (1-127) changes, so monsters, items and the UI keep their colours.
 */
void ApplyPaletteVariation(std::array<SDL_Color, 256> &palette, uint32_t seed)
{
	PaletteRng rng { static_cast<uint64_t>(seed) * 0x2545F4914F6CDD1DULL + 0x1234567ULL };
	const float hueShift = rng.range(-70.0f, 70.0f);
	const float satMul = rng.range(0.85f, 1.35f);
	const float tintHue = rng.range(0.0f, 360.0f);
	const float tintStrength = rng.range(0.12f, 0.32f);
	float tr, tg, tb;
	HsvToRgb(tintHue, 0.75f, 1.0f, tr, tg, tb);

	for (size_t i = 1; i < 128; i++) {
		const float r = palette[i].r / 255.0f;
		const float g = palette[i].g / 255.0f;
		const float b = palette[i].b / 255.0f;
		const float luma = 0.299f * r + 0.587f * g + 0.114f * b;

		float h, s, v;
		RgbToHsv(r, g, b, h, s, v);
		float nr, ng, nb;
		HsvToRgb(h + hueShift, std::min(1.0f, s * satMul), v, nr, ng, nb);

		// Tint towards a luminance-scaled colour so grey stone picks up the mood too.
		nr = nr * (1.0f - tintStrength) + tr * luma * tintStrength * 1.6f;
		ng = ng * (1.0f - tintStrength) + tg * luma * tintStrength * 1.6f;
		nb = nb * (1.0f - tintStrength) + tb * luma * tintStrength * 1.6f;

		// Keep the original brightness so lighting and contrast stay readable.
		const float newLuma = 0.299f * nr + 0.587f * ng + 0.114f * nb;
		const float scale = newLuma > 0.001f ? luma / newLuma : 1.0f;
		palette[i].r = static_cast<uint8_t>(std::clamp(nr * scale, 0.0f, 1.0f) * 255.0f + 0.5f);
		palette[i].g = static_cast<uint8_t>(std::clamp(ng * scale, 0.0f, 1.0f) * 255.0f + 0.5f);
		palette[i].b = static_cast<uint8_t>(std::clamp(nb * scale, 0.0f, 1.0f) * 255.0f + 0.5f);
	}
}

} // namespace

void LoadPaletteAndInitBlending(const char *path, uint32_t variationSeed)
{
	assert(path != nullptr);
	if (HeadlessMode) return;
	LoadPalette(path);
	if (variationSeed != 0)
		ApplyPaletteVariation(logical_palette, variationSeed);
	if (leveltype == DTYPE_CAVES || leveltype == DTYPE_CRYPT) {
		GenerateBlendedLookupTable(logical_palette.data(), /*skipFrom=*/1, /*skipTo=*/31);
	} else if (leveltype == DTYPE_NEST) {
		GenerateBlendedLookupTable(logical_palette.data(), /*skipFrom=*/1, /*skipTo=*/15);
	} else {
		GenerateBlendedLookupTable(logical_palette.data());
	}
}

void LoadRndLvlPal(dungeon_type l, uint32_t variationSeed)
{
	if (HeadlessMode)
		return;

	if (l == DTYPE_TOWN) {
		LoadPaletteAndInitBlending("levels\\towndata\\town.pal");
		return;
	}

	if (l == DTYPE_CRYPT) {
		LoadPaletteAndInitBlending("nlevels\\l5data\\l5base.pal", variationSeed);
		return;
	}

	int rv = RandomIntBetween(1, 4);
	char szFileName[27];
	if (l == DTYPE_NEST) {
		if (!*GetOptions().Graphics.alternateNestArt) {
			rv++;
		}
		*BufCopy(szFileName, R"(nlevels\l6data\l6base)", rv, ".pal") = '\0';
	} else {
		char nbuf[3];
		const char *end = BufCopy(nbuf, static_cast<int>(l));
		const std::string_view n = std::string_view(nbuf, end - nbuf);
		*BufCopy(szFileName, "levels\\l", n, "data\\l", n, "_", rv, ".pal") = '\0';
	}
	LoadPaletteAndInitBlending(szFileName, variationSeed);
}

void IncreaseBrightness()
{
	const int brightnessValue = *GetOptions().Graphics.brightness;
	if (brightnessValue < 100) {
		const int newBrightness = std::min(brightnessValue + 5, 100);
		GetOptions().Graphics.brightness.SetValue(newBrightness);
		UpdateSystemPalette(logical_palette);
	}
}

void DecreaseBrightness()
{
	const int brightnessValue = *GetOptions().Graphics.brightness;
	if (brightnessValue > 0) {
		const int newBrightness = std::max(brightnessValue - 5, 0);
		GetOptions().Graphics.brightness.SetValue(newBrightness);
		UpdateSystemPalette(logical_palette);
	}
}

int UpdateBrightness(int brightness)
{
	if (brightness >= 0) {
		GetOptions().Graphics.brightness.SetValue(brightness);
		UpdateSystemPalette(logical_palette);
	}

	return *GetOptions().Graphics.brightness;
}

void BlackPalette()
{
	for (SDL_Color &c : system_palette) {
		c.r = c.g = c.b = 0;
	}
	SystemPaletteUpdated();
}

void PaletteFadeIn(int fr, const std::array<SDL_Color, 256> &srcPalette)
{
	if (HeadlessMode)
		return;
	if (demo::IsRunning())
		fr = 0;

	std::array<SDL_Color, 256> palette;

#ifndef USE_SDL1
	for (SDL_Color &color : palette) {
		color.a = SDL_ALPHA_OPAQUE;
	}
#endif

	ApplyGlobalBrightness(palette.data(), srcPalette.data());

	if (fr > 0) {
		const uint32_t tc = SDL_GetTicks();
		fr *= 3;
		uint32_t prevFadeValue = 255;
		for (uint32_t i = 0; i < 256; i = fr * (SDL_GetTicks() - tc) / 50) {
			if (i == prevFadeValue) {
				SDL_Delay(1);
				continue;
			}
			ApplyFadeLevel(i, system_palette.data(), palette.data());
			SystemPaletteUpdated();

			// We can skip hardware cursor update for fade level 0 (everything is black).
			if (i != 0 && IsHardwareCursor()) {
				ReinitializeHardwareCursor();
			}

			prevFadeValue = i;

			BltFast(nullptr, nullptr);
			RenderPresent();
		}
	}
	system_palette = palette;
	SystemPaletteUpdated();
	RedrawEverything();
	if (IsHardwareCursor()) ReinitializeHardwareCursor();

	if (fr <= 0) {
		BltFast(nullptr, nullptr);
		RenderPresent();
	}

	sgbFadedIn = true;
}

void PaletteFadeOut(int fr, const std::array<SDL_Color, 256> &srcPalette)
{
	if (!sgbFadedIn || HeadlessMode)
		return;
	if (demo::IsRunning())
		fr = 0;

	if (fr > 0) {
		SDL_Color palette[256];
		ApplyGlobalBrightness(palette, srcPalette.data());

		const uint32_t tc = SDL_GetTicks();
		fr *= 3;
		uint32_t prevFadeValue = 0;
		for (uint32_t i = 0; i < 256; i = fr * (SDL_GetTicks() - tc) / 50) {
			if (i == prevFadeValue) {
				SDL_Delay(1);
				continue;
			}
			ApplyFadeLevel(256 - i, system_palette.data(), palette);
			SystemPaletteUpdated();
			prevFadeValue = i;

			BltFast(nullptr, nullptr);
			RenderPresent();
		}
	}

	BlackPalette();
	if (IsHardwareCursor()) ReinitializeHardwareCursor();

	if (fr <= 0) {
		BltFast(nullptr, nullptr);
		RenderPresent();
	}

	sgbFadedIn = false;
}

void palette_update_caves()
{
	CycleColors(1, 31);
	SystemPaletteUpdated(1, 31);
}

/**
 * @brief Cycle the lava every other frame, and glow every frame
 * Lava has 15 colors and the glow 16, so the full animation has 240 frames before it loops
 */
void palette_update_crypt()
{
	static bool delayLava = false;

	if (!delayLava) {
		CycleColorsReverse(1, 15);
		delayLava = false;
	}

	CycleColorsReverse(16, 31);
	SystemPaletteUpdated(1, 31);
	delayLava = !delayLava;
}

/**
 * @brief Cycle the pond waves and bubles colors every 3rd frame
 * Bubles have 8 colors and waves 7, so the full animation has 56 frames before it loops
 */
void palette_update_hive()
{
	static uint8_t delay = 0;

	if (delay != 2) {
		delay++;
		return;
	}

	CycleColorsReverse(1, 8);
	CycleColorsReverse(9, 15);
	SystemPaletteUpdated(1, 15);
	delay = 0;
}

void SetLogicalPaletteColor(unsigned i, const SDL_Color &color)
{
	logical_palette[i] = color;
	ApplyGlobalBrightnessSingleColor(system_palette[i], logical_palette[i]);
	SystemPaletteUpdated(i, 1);
	UpdateBlendedLookupTableSingleColor(logical_palette.data(), i);
}

} // namespace devilution
