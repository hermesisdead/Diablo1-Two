#include "engine/render/tile_hue.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include "engine/light_tables.hpp"
#include "engine/palette.h"

namespace devilution {

namespace {

std::array<std::unique_ptr<HueLightTableSet>, 2 * MaxTileHueSteps + 1> Cache;

// What the cached tables were built from; when either changes (new level, palette swap) they are rebuilt.
std::array<SDL_Color, 256> BuiltForPalette {};
HueLightTableSet BuiltForLightTables {};

bool SameColors(const std::array<SDL_Color, 256> &a, const std::array<SDL_Color, 256> &b)
{
	for (size_t i = 0; i < a.size(); i++) {
		if (a[i].r != b[i].r || a[i].g != b[i].g || a[i].b != b[i].b)
			return false;
	}
	return true;
}

struct Hsv {
	float h, s, v;
};

Hsv ToHsv(const SDL_Color &c)
{
	const float r = c.r / 255.0f, g = c.g / 255.0f, b = c.b / 255.0f;
	const float max = std::max({ r, g, b }), min = std::min({ r, g, b });
	const float d = max - min;
	float h = 0;
	if (d > 0) {
		if (max == r)
			h = 60.0f * std::fmod((g - b) / d, 6.0f);
		else if (max == g)
			h = 60.0f * ((b - r) / d + 2.0f);
		else
			h = 60.0f * ((r - g) / d + 4.0f);
	}
	if (h < 0)
		h += 360.0f;
	return { h, max > 0 ? d / max : 0.0f, max };
}

void FromHsv(Hsv hsv, int &r, int &g, int &b)
{
	const float c = hsv.v * hsv.s;
	const float x = c * (1.0f - std::fabs(std::fmod(hsv.h / 60.0f, 2.0f) - 1.0f));
	const float m = hsv.v - c;
	float rf = 0, gf = 0, bf = 0;
	if (hsv.h < 60) {
		rf = c, gf = x;
	} else if (hsv.h < 120) {
		rf = x, gf = c;
	} else if (hsv.h < 180) {
		gf = c, bf = x;
	} else if (hsv.h < 240) {
		gf = x, bf = c;
	} else if (hsv.h < 300) {
		rf = x, bf = c;
	} else {
		rf = c, bf = x;
	}
	r = static_cast<int>((rf + m) * 255.0f + 0.5f);
	g = static_cast<int>((gf + m) * 255.0f + 0.5f);
	b = static_cast<int>((bf + m) * 255.0f + 0.5f);
}

uint8_t NearestPaletteColor(int r, int g, int b)
{
	uint8_t best = 0;
	int bestDistance = INT32_MAX;
	for (int i = 0; i < 256; i++) {
		const SDL_Color &p = logical_palette[i];
		const int dr = p.r - r, dg = p.g - g, db = p.b - b;
		// Weighted towards green, the colour the eye is most sensitive to.
		const int distance = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
		if (distance < bestDistance) {
			bestDistance = distance;
			best = static_cast<uint8_t>(i);
		}
	}
	return best;
}

std::unique_ptr<HueLightTableSet> Build(int hueSteps)
{
	std::array<uint8_t, 256> rotate;
	for (int i = 0; i < 256; i++) {
		Hsv hsv = ToHsv(logical_palette[i]);
		if (hsv.s < 0.08f || hsv.v < 0.04f) {
			rotate[i] = static_cast<uint8_t>(i); // greys and black have no hue to turn
			continue;
		}
		hsv.h = std::fmod(hsv.h + hueSteps * TileHueStepDegrees + 360.0f, 360.0f);
		int r, g, b;
		FromHsv(hsv, r, g, b);
		rotate[i] = NearestPaletteColor(r, g, b);
	}
	auto set = std::make_unique<HueLightTableSet>();
	for (size_t level = 0; level < NumLightingLevels; level++) {
		for (int i = 0; i < 256; i++)
			(*set)[level][i] = LightTables[level][rotate[i]];
	}
	return set;
}

} // namespace

void ClearTileHues()
{
	std::memset(dTileHue, 0, sizeof(dTileHue));
}

void RefreshTileHueTables()
{
	if (SameColors(BuiltForPalette, logical_palette) && BuiltForLightTables == LightTables)
		return;
	BuiltForPalette = logical_palette;
	BuiltForLightTables = LightTables;
	for (auto &entry : Cache)
		entry = nullptr;
}

const HueLightTableSet *GetTileHueLightTables(int hueSteps)
{
	hueSteps = std::clamp(hueSteps, -MaxTileHueSteps, MaxTileHueSteps);
	if (hueSteps == 0)
		return nullptr;
	std::unique_ptr<HueLightTableSet> &entry = Cache[hueSteps + MaxTileHueSteps];
	if (entry == nullptr)
		entry = Build(hueSteps);
	return entry.get();
}

const uint8_t *GetTileHueTable(int hueSteps, int lightLevel)
{
	lightLevel = std::clamp(lightLevel, 0, static_cast<int>(NumLightingLevels) - 1);
	const HueLightTableSet *set = GetTileHueLightTables(hueSteps);
	return set != nullptr ? (*set)[lightLevel].data() : LightTables[lightLevel].data();
}

} // namespace devilution
