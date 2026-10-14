#include "engine/render/dungeon_mood.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include "engine/lighting_defs.hpp"
#include "engine/palette.h"
#include "engine/render/orbit_view.hpp"
#include "engine/render/scrollrt.h"
#include "engine/sound.h"
#include "levels/desert.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "options.h"
#include "player.h"
#include "utils/log.hpp"
#include "utils/palette_blending.hpp"

namespace devilution {

namespace {

constexpr uint8_t Bayer[4][4] = {
	{ 0, 8, 2, 10 },
	{ 12, 4, 14, 6 },
	{ 3, 11, 1, 9 },
	{ 15, 7, 13, 5 },
};

/** Particles live in world tile space: x/y on the map (tile centre = +0.5), z = height in tiles. */
struct Particle {
	float x, y, z;
	float vx, vy, vz;
	float phase;
	float life, maxLife;
	float rate;   // per-particle speed of its own animation (blink, tumble)
	uint8_t kind; // variant within a mood (leaf vs dust streak, glowing ash...)
};

struct DripSource {
	float x, y;
	float period;
	float timer;
};

struct Drop {
	float x, y, z, vz;
};

struct Splash {
	float x, y, age;
};

struct MoodLook {
	uint8_t fogColor;
	float fogDensity;
	size_t particles;
};

/** Palette indices for the weather colours, picked from the current level's palette. */
struct WeatherColors {
	uint8_t white = PAL16_GRAY;
	uint8_t black = 0;
	uint8_t rain = PAL16_GRAY + 4;
	uint8_t rainSplash = PAL16_GRAY + 2;
	uint8_t snow = PAL16_GRAY;
	uint8_t snowHaze = PAL16_GRAY + 2;
	uint8_t leaves[3] = { PAL16_ORANGE + 6, PAL16_YELLOW + 8, PAL16_RED + 8 };
	uint8_t windDust = PAL16_YELLOW + 9;
	uint8_t fireflyCore = PAL8_YELLOW;
	uint8_t fireflyHalo = PAL16_YELLOW + 4;
	uint8_t sporeCore = PAL16_YELLOW + 2;
	uint8_t sporeHalo = PAL16_YELLOW + 6;
	uint8_t sporeHaze = PAL16_YELLOW + 10;
	uint8_t ash = PAL16_GRAY + 6;
	uint8_t ashDark = PAL16_GRAY + 10;
	uint8_t ashHaze = PAL16_GRAY + 11;
	uint8_t blood = PAL16_RED + 6;
	uint8_t bloodMote = PAL16_RED + 2;
	uint8_t nightMist = PAL16_GRAY + 9;
};

DungeonMood currentMood = DungeonMood::Clear;
int64_t moodKey = -1;
bool outdoor = false;
float strength = 1.0f;   // how heavy this roll of the weather is (0.6..1)
float fade = 1.0f;       // 0..1 while outdoor weather changes over
bool fadingOut = false;
float changeTimer = 0;   // seconds until the outdoor weather turns
float windSign = 1.0f;   // which way the wind blows across the screen
float flashTime = -1.0f; // seconds since the last lightning strike (-1: none)
float nextLightning = 0;
float thunderIn = -1.0f;
float thunderLevel = 0;
WeatherColors colors;

std::vector<Particle> particles;
std::vector<DripSource> dripSources;
std::vector<Drop> drops;
std::vector<Splash> splashes;
std::vector<Splash> rainSplashes;
uint64_t rngState = 1;
uint32_t lastTicks = 0;
float clockSeconds = 0;

uint32_t NextRandom()
{
	rngState += 0x9E3779B97F4A7C15ULL;
	uint64_t z = rngState;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return static_cast<uint32_t>((z ^ (z >> 31)) >> 16);
}

float Random01()
{
	return static_cast<float>(NextRandom() & 0xFFFFFF) / static_cast<float>(1 << 24);
}

float RandomRange(float lo, float hi)
{
	return lo + (hi - lo) * Random01();
}

float Hash2(int x, int y)
{
	uint32_t h = static_cast<uint32_t>(x) * 0x8DA6B343u ^ static_cast<uint32_t>(y) * 0xD8163841u;
	h ^= h >> 13;
	h *= 0x85EBCA6Bu;
	h ^= h >> 16;
	return static_cast<float>(h & 0xFFFF) / 65535.0f;
}

float ValueNoise(float x, float y)
{
	const int ix = static_cast<int>(std::floor(x));
	const int iy = static_cast<int>(std::floor(y));
	float fx = x - static_cast<float>(ix);
	float fy = y - static_cast<float>(iy);
	fx = fx * fx * (3.0f - 2.0f * fx);
	fy = fy * fy * (3.0f - 2.0f * fy);
	const float a = Hash2(ix, iy), b = Hash2(ix + 1, iy);
	const float c = Hash2(ix, iy + 1), d = Hash2(ix + 1, iy + 1);
	const float top = a + (b - a) * fx;
	return top + ((c + (d - c) * fx) - top) * fy;
}

/** Fog thickness at a ground position (tile units): two octaves drifting with a slow draught. */
float FogNoise(float x, float y, float t)
{
	const float drift = currentMood == DungeonMood::Wind ? 1.4f : 0.22f;
	const float wx = x + t * drift * windSign, wy = y + t * 0.09f;
	const float n = ValueNoise(wx / 4.5f, wy / 4.5f) * 0.65f + ValueNoise(wx / 1.7f + 17.0f, wy / 1.7f - 9.0f) * 0.35f;
	return 0.3f + 0.7f * std::clamp((n - 0.40f) / 0.36f, 0.0f, 1.0f);
}

bool IsOutdoorLevel()
{
	return leveltype == DTYPE_TOWN || leveltype == DTYPE_FOREST;
}

bool IsVisibleTile(int tx, int ty)
{
	if (tx < 0 || ty < 0 || tx >= MAXDUNX || ty >= MAXDUNY || dPiece[tx][ty] == 0)
		return false;
	// Tristram has no vision bookkeeping: everything is in plain view.
	return leveltype == DTYPE_TOWN || HasAnyOf(dFlags[tx][ty], DungeonFlag::Visible);
}

/** How lit a spot is, 0 (dark) .. 1 (fully lit). */
float LightFactor(float x, float y)
{
	const int tx = static_cast<int>(std::floor(x));
	const int ty = static_cast<int>(std::floor(y));
	if (!IsVisibleTile(tx, ty))
		return 0.0f; // outside the map or out of sight: stays black
	if (leveltype == DTYPE_TOWN)
		return 1.0f;
	return std::clamp(static_cast<float>(LightsMax - dLight[tx][ty]) / static_cast<float>(LightsMax), 0.0f, 1.0f);
}

/** Bilinear LightFactor, so fog fades out softly at the edge of what the hero can see. */
float SmoothLight(float x, float y)
{
	const float fx = x - 0.5f, fy = y - 0.5f;
	const float x0 = std::floor(fx), y0 = std::floor(fy);
	const float tx = fx - x0, ty = fy - y0;
	const float a = LightFactor(x0 + 0.5f, y0 + 0.5f) * (1 - tx) + LightFactor(x0 + 1.5f, y0 + 0.5f) * tx;
	const float b = LightFactor(x0 + 0.5f, y0 + 1.5f) * (1 - tx) + LightFactor(x0 + 1.5f, y0 + 1.5f) * tx;
	return a * (1 - ty) + b * ty;
}

/** Closest colour in the level palette (skipping the cycling water/lava range). */
uint8_t NearestColor(int r, int g, int b)
{
	int best = PAL16_GRAY;
	int bestDistance = 1 << 30;
	for (int i = 32; i < 256; i++) {
		const SDL_Color &c = logical_palette[i];
		const int dr = c.r - r, dg = c.g - g, db = c.b - b;
		const int distance = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
		if (distance < bestDistance) {
			bestDistance = distance;
			best = i;
		}
	}
	return static_cast<uint8_t>(best);
}

void PickColors()
{
	colors.white = NearestColor(255, 255, 255);
	colors.black = NearestColor(0, 0, 0);
	colors.rain = NearestColor(150, 160, 178);
	colors.rainSplash = NearestColor(190, 200, 215);
	colors.snow = NearestColor(240, 240, 246);
	colors.snowHaze = NearestColor(200, 205, 215);
	colors.leaves[0] = NearestColor(150, 88, 30);
	colors.leaves[1] = NearestColor(175, 135, 40);
	colors.leaves[2] = NearestColor(110, 50, 22);
	colors.windDust = NearestColor(150, 130, 95);
	colors.fireflyCore = NearestColor(235, 255, 140);
	colors.fireflyHalo = NearestColor(130, 175, 45);
	colors.sporeCore = NearestColor(170, 230, 110);
	colors.sporeHalo = NearestColor(90, 140, 55);
	colors.sporeHaze = NearestColor(70, 95, 50);
	colors.ash = NearestColor(200, 195, 188);
	colors.ashDark = NearestColor(120, 116, 112);
	colors.ashHaze = NearestColor(75, 60, 52);
	colors.blood = NearestColor(130, 18, 18);
	colors.bloodMote = NearestColor(210, 50, 40);
	colors.nightMist = NearestColor(80, 95, 105);
}

MoodLook LookOf(DungeonMood mood)
{
	switch (mood) {
	case DungeonMood::Mist:
		return { PAL16_GRAY + 7, 0.8f, 50 };
	case DungeonMood::Dust:
		return { PAL16_YELLOW + 7, 0.12f, 300 };
	case DungeonMood::Frost:
		return { PAL16_BLUE + 2, 0.34f, 150 };
	case DungeonMood::Embers:
		return { PAL16_RED + 9, 0.22f, 130 };
	case DungeonMood::Drips:
		return { PAL16_GRAY + 9, 0.18f, 0 };
	case DungeonMood::Rain:
		return { colors.rain, 0.10f, 520 };
	case DungeonMood::Storm:
		return { colors.rain, 0.22f, 900 };
	case DungeonMood::Snow:
		return { colors.snowHaze, 0.30f, 750 };
	case DungeonMood::Wind:
		return { colors.windDust, 0.18f, 300 };
	case DungeonMood::Fireflies:
		return { colors.nightMist, 0.28f, 120 };
	case DungeonMood::Ash:
		return { colors.ashHaze, 0.42f, 600 };
	case DungeonMood::Spores:
		return { colors.sporeHaze, 0.32f, 280 };
	case DungeonMood::BloodMist:
		return { colors.blood, 0.75f, 45 };
	default:
		return { 0, 0.0f, 0 };
	}
}

bool IsPrecipitation(DungeonMood mood)
{
	return IsAnyOf(mood, DungeonMood::Rain, DungeonMood::Storm, DungeonMood::Snow, DungeonMood::Ash, DungeonMood::Wind);
}

void PlayerWorld(float &x, float &y)
{
	x = y = 0;
	if (MyPlayer == nullptr)
		return;
	x = MyPlayer->position.tile.x + 0.5f;
	y = MyPlayer->position.tile.y + 0.5f;
}

// ---------------------------------------------------------------------------------------------
// Mood selection

struct Weight {
	DungeonMood mood;
	uint32_t weight;
};

std::vector<Weight> MoodTable()
{
	if (InDesertRegion() && IsAnyOf(leveltype, DTYPE_TOWN, DTYPE_FOREST)) {
		// Sand Tristram and the wastes: dust, wind-driven sand, the odd storm rolling in.
		return { { DungeonMood::Clear, 30 }, { DungeonMood::Wind, 28 }, { DungeonMood::Dust, 20 }, { DungeonMood::Storm, 8 },
			{ DungeonMood::Ash, 6 }, { DungeonMood::Mist, 4 }, { DungeonMood::Embers, 4 } };
	}
	switch (leveltype) {
	case DTYPE_TOWN:
		return { { DungeonMood::Clear, 22 }, { DungeonMood::Rain, 18 }, { DungeonMood::Storm, 12 }, { DungeonMood::Snow, 14 },
			{ DungeonMood::Mist, 12 }, { DungeonMood::Wind, 14 }, { DungeonMood::Ash, 4 }, { DungeonMood::Embers, 4 } };
	case DTYPE_FOREST:
		return { { DungeonMood::Clear, 8 }, { DungeonMood::Mist, 14 }, { DungeonMood::Rain, 16 }, { DungeonMood::Storm, 12 },
			{ DungeonMood::Snow, 10 }, { DungeonMood::Wind, 14 }, { DungeonMood::Fireflies, 18 }, { DungeonMood::Spores, 8 } };
	case DTYPE_CATHEDRAL:
	case DTYPE_CRYPT:
		return { { DungeonMood::Clear, 12 }, { DungeonMood::Mist, 22 }, { DungeonMood::Dust, 26 }, { DungeonMood::Drips, 16 },
			{ DungeonMood::Frost, 12 }, { DungeonMood::Ash, 6 }, { DungeonMood::BloodMist, 6 } };
	case DTYPE_CATACOMBS:
		return { { DungeonMood::Clear, 12 }, { DungeonMood::Mist, 16 }, { DungeonMood::Dust, 16 }, { DungeonMood::Drips, 24 },
			{ DungeonMood::Frost, 12 }, { DungeonMood::Spores, 12 }, { DungeonMood::BloodMist, 8 } };
	case DTYPE_CAVES:
	case DTYPE_NEST:
		return { { DungeonMood::Clear, 8 }, { DungeonMood::Mist, 10 }, { DungeonMood::Frost, 16 }, { DungeonMood::Embers, 16 },
			{ DungeonMood::Drips, 14 }, { DungeonMood::Spores, 18 }, { DungeonMood::Ash, 18 } };
	case DTYPE_HELL:
		return { { DungeonMood::Clear, 8 }, { DungeonMood::Embers, 32 }, { DungeonMood::Ash, 30 }, { DungeonMood::BloodMist, 30 } };
	default:
		return {};
	}
}

DungeonMood RollMood(DungeonMood exclude)
{
	std::vector<Weight> table = MoodTable();
	std::erase_if(table, [exclude](const Weight &w) { return w.mood == exclude; });
	uint32_t total = 0;
	for (const Weight &w : table)
		total += w.weight;
	if (total == 0)
		return DungeonMood::Clear;
	uint32_t r = NextRandom() % total;
	for (const Weight &w : table) {
		if (r < w.weight)
			return w.mood;
		r -= w.weight;
	}
	return DungeonMood::Clear;
}

/** A random ground point within the screen-shaped area around the hero (tile units). */
void RandomScreenPoint(float &x, float &y, float halfWidth, float top, float bottom)
{
	float px, py;
	PlayerWorld(px, py);
	// Screen-horizontal unit (0.5,-0.5) moves 32 px right; screen-vertical unit (0.5,0.5) 16 px down.
	const float u = RandomRange(-halfWidth, halfWidth);
	const float v = RandomRange(top, bottom);
	x = px + 0.5f * (u + v);
	y = py + 0.5f * (v - u);
}

void SpawnParticle(Particle &p, bool anywhere)
{
	float px, py;
	PlayerWorld(px, py);
	p.phase = Random01() * 6.2832f;
	p.rate = RandomRange(0.7f, 1.4f);
	p.kind = 0;
	switch (currentMood) {
	case DungeonMood::Rain:
	case DungeonMood::Storm: {
		// Ground points extend below the screen: drops high in the air show further up.
		RandomScreenPoint(p.x, p.y, 13.0f, -17.0f, 34.0f);
		const float wind = currentMood == DungeonMood::Storm ? 1.8f : 0.55f;
		p.vx = wind * 0.5f * windSign;
		p.vy = -wind * 0.5f * windSign;
		p.vz = currentMood == DungeonMood::Storm ? -RandomRange(12.0f, 15.0f) : -RandomRange(8.5f, 11.0f);
		p.z = anywhere ? RandomRange(0.0f, 8.5f) : RandomRange(7.5f, 8.5f);
		p.maxLife = 60.0f;
		break;
	}
	case DungeonMood::Snow:
	case DungeonMood::Ash:
		RandomScreenPoint(p.x, p.y, 13.0f, -17.0f, 30.0f);
		p.vx = p.vy = 0;
		p.vz = currentMood == DungeonMood::Snow ? -RandomRange(0.55f, 0.95f) : -RandomRange(0.25f, 0.5f);
		p.z = anywhere ? RandomRange(0.0f, 7.0f) : RandomRange(6.5f, 7.0f);
		p.maxLife = 60.0f;
		p.kind = currentMood == DungeonMood::Ash && Random01() < 0.07f ? 1 : 0; // a few still glowing
		break;
	case DungeonMood::Wind: {
		p.kind = Random01() < 0.55f ? 0 : 1; // 0: leaf, 1: dust streak
		if (anywhere) {
			RandomScreenPoint(p.x, p.y, 13.0f, -17.0f, 17.0f);
		} else {
			// Blow in from the upwind edge of the screen.
			const float u = -windSign * 13.0f;
			const float v = RandomRange(-17.0f, 17.0f);
			p.x = px + 0.5f * (u + v);
			p.y = py + 0.5f * (v - u);
		}
		const float speed = p.kind == 0 ? RandomRange(2.0f, 3.6f) : RandomRange(4.5f, 7.0f);
		p.vx = speed * 0.5f * windSign;
		p.vy = -speed * 0.5f * windSign;
		p.z = p.kind == 0 ? RandomRange(0.2f, 2.2f) : RandomRange(0.0f, 1.2f);
		p.vz = 0;
		p.maxLife = RandomRange(5.0f, 9.0f);
		break;
	}
	default: {
		const float angle = Random01() * 6.2832f;
		const float radius = std::sqrt(Random01()) * 10.0f;
		p.x = px + std::cos(angle) * radius;
		p.y = py + std::sin(angle) * radius;
		switch (currentMood) {
		case DungeonMood::Mist:
		case DungeonMood::Dust:
		case DungeonMood::BloodMist:
			p.z = RandomRange(0.15f, 2.2f);
			p.vx = RandomRange(-0.08f, 0.08f);
			p.vy = RandomRange(-0.08f, 0.08f);
			p.vz = RandomRange(-0.05f, 0.02f);
			p.maxLife = RandomRange(6.0f, 14.0f);
			break;
		case DungeonMood::Frost:
			p.z = anywhere ? RandomRange(0.0f, 2.6f) : 2.6f;
			p.vx = RandomRange(-0.05f, 0.05f);
			p.vy = RandomRange(-0.05f, 0.05f);
			p.vz = -RandomRange(0.22f, 0.42f);
			p.maxLife = 30.0f;
			break;
		case DungeonMood::Embers:
			p.z = anywhere ? RandomRange(0.0f, 2.5f) : RandomRange(0.0f, 0.3f);
			p.vx = RandomRange(-0.1f, 0.1f);
			p.vy = RandomRange(-0.1f, 0.1f);
			p.vz = RandomRange(0.45f, 0.9f);
			p.maxLife = RandomRange(2.5f, 4.5f);
			break;
		case DungeonMood::Fireflies:
			p.z = RandomRange(0.3f, 1.7f);
			p.vx = RandomRange(-0.3f, 0.3f);
			p.vy = RandomRange(-0.3f, 0.3f);
			p.vz = 0;
			p.maxLife = RandomRange(10.0f, 20.0f);
			break;
		case DungeonMood::Spores:
			p.z = anywhere ? RandomRange(0.0f, 2.5f) : RandomRange(0.0f, 0.2f);
			p.vx = RandomRange(-0.06f, 0.06f);
			p.vy = RandomRange(-0.06f, 0.06f);
			p.vz = RandomRange(0.08f, 0.2f);
			p.maxLife = RandomRange(10.0f, 16.0f);
			break;
		default:
			break;
		}
		break;
	}
	}
	p.life = anywhere ? Random01() * p.maxLife : p.maxLife;
}

void FindDripSources()
{
	dripSources.clear();
	std::vector<Point> floor;
	for (int x = 1; x < MAXDUNX - 1; x++)
		for (int y = 1; y < MAXDUNY - 1; y++)
			if (dPiece[x][y] != 0 && !IsTileSolid({ x, y }))
				floor.push_back({ x, y });
	if (floor.empty())
		return;
	const size_t count = std::min<size_t>(floor.size() / 12 + 1, 320);
	for (size_t i = 0; i < count; i++) {
		const Point t = floor[NextRandom() % floor.size()];
		const float period = RandomRange(1.1f, 3.6f);
		dripSources.push_back({ t.x + RandomRange(0.2f, 0.8f), t.y + RandomRange(0.2f, 0.8f), period, Random01() * period });
	}
}

float RandomChangeDelay()
{
#ifdef _DEBUG
	if (const char *change = std::getenv("DEVILUTIONX_DEV_WEATHERCHANGE"); change != nullptr)
		return static_cast<float>(std::atof(change));
#endif
	return RandomRange(120.0f, 260.0f);
}

/** Switches to a mood and sets up its particles. */
void SetMood(DungeonMood mood)
{
	currentMood = mood;
	strength = RandomRange(0.6f, 1.0f);
	windSign = Random01() < 0.5f ? -1.0f : 1.0f;
	const size_t count = static_cast<size_t>(static_cast<float>(LookOf(currentMood).particles) * strength);
	particles.assign(count, Particle {});
	for (Particle &p : particles)
		SpawnParticle(p, true);
	drops.clear();
	splashes.clear();
	rainSplashes.clear();
	dripSources.clear();
	if (currentMood == DungeonMood::Drips)
		FindDripSources();
	flashTime = -1.0f;
	thunderIn = -1.0f;
	nextLightning = RandomRange(3.0f, 9.0f);
	LogVerbose("Weather: {} (strength {:.2f})", DungeonMoodName(currentMood), strength);
}

void InitMood(int64_t key, uint32_t seed)
{
	moodKey = key;
	// A fresh roll on every visit: the level seed plus the clock.
	rngState = (static_cast<uint64_t>(seed) << 32) ^ static_cast<uint64_t>(key) * 0xD1342543DE82EF95ULL
	    ^ static_cast<uint64_t>(SDL_GetTicks()) * 0x9E3779B97F4A7C15ULL ^ 0x5DEECE66DULL;
	outdoor = IsOutdoorLevel();
	PickColors();
	DungeonMood mood = RollMood(DungeonMood::COUNT);
#ifdef _DEBUG
	if (const char *forced = std::getenv("DEVILUTIONX_DEV_MOOD"); forced != nullptr) {
		for (int m = 0; m < static_cast<int>(DungeonMood::COUNT); m++) {
			if (std::strcmp(forced, DungeonMoodName(static_cast<DungeonMood>(m))) == 0)
				mood = static_cast<DungeonMood>(m);
		}
	}
#endif
	SetMood(mood);
	fade = 1.0f;
	fadingOut = false;
	changeTimer = RandomChangeDelay();
}

bool MoodEnabled()
{
	if (leveltype == DTYPE_TOWN)
		return *GetOptions().Graphics.townWeather;
	return *GetOptions().Graphics.dungeonMoods;
}

void EnsureMood()
{
	if (!MoodEnabled() || MyPlayer == nullptr) {
		currentMood = DungeonMood::Clear;
		particles.clear();
		moodKey = -1;
		return;
	}
	const uint32_t seed = leveltype == DTYPE_FOREST ? MyPlayer->forestSeed : (leveltype == DTYPE_TOWN ? MyPlayer->townSeed : DungeonSeeds[currlevel]);
	const int64_t key = (setlevel ? (int64_t { 1 } << 20) + static_cast<int64_t>(setlvlnum) : static_cast<int64_t>(currlevel))
	    + (static_cast<int64_t>(leveltype) << 24) + (static_cast<int64_t>(seed) << 32);
	if (key != moodKey)
		InitMood(key, seed);
}

// ---------------------------------------------------------------------------------------------
// Weather over time

/** Brightness of the lightning flash at a time after the strike: a double flicker, then fade. */
float FlashBrightness(float t)
{
	if (t < 0)
		return 0;
	if (t < 0.07f)
		return 1.0f;
	if (t < 0.14f)
		return 0.15f;
	if (t < 0.24f)
		return 0.75f;
	return 0.75f * std::exp(-(t - 0.24f) / 0.12f);
}

void UpdateWeather(float dt)
{
	if (outdoor) {
		changeTimer -= dt;
		if (!fadingOut && changeTimer <= 0)
			fadingOut = true;
		if (fadingOut) {
			fade -= dt / 6.0f;
			if (fade <= 0) {
				fade = 0;
				SetMood(RollMood(currentMood));
				fadingOut = false;
				changeTimer = RandomChangeDelay();
			}
		} else {
			fade = std::min(1.0f, fade + dt / 6.0f);
		}
	}

	if (currentMood == DungeonMood::Storm) {
		nextLightning -= dt;
		if (nextLightning <= 0 && fade > 0.5f) {
			flashTime = 0;
			thunderIn = RandomRange(0.15f, 2.4f);
			thunderLevel = 1.0f - 0.55f * (thunderIn - 0.15f) / 2.25f; // close strikes are louder
			nextLightning = RandomRange(6.0f, 18.0f);
		}
	}
	if (flashTime >= 0) {
		flashTime += dt;
		if (flashTime > 1.2f)
			flashTime = -1.0f;
	}
	if (thunderIn >= 0) {
		thunderIn -= dt;
		if (thunderIn < 0)
			PlayThunder(thunderLevel * fade);
	}
}

AmbienceTrack TrackFor(DungeonMood mood, float &level)
{
	level = 1.0f;
	switch (mood) {
	case DungeonMood::Clear:
		if (leveltype == DTYPE_TOWN)
			return AmbienceTrack::Breeze;
		if (leveltype == DTYPE_FOREST)
			return InDesertRegion() ? AmbienceTrack::Breeze : AmbienceTrack::NightForest;
		return AmbienceTrack::None;
	case DungeonMood::Mist:
		if (outdoor) {
			level = 0.7f;
			return AmbienceTrack::Breeze;
		}
		return AmbienceTrack::Drone;
	case DungeonMood::Dust:
		level = 0.6f;
		return outdoor ? AmbienceTrack::Breeze : AmbienceTrack::Drone;
	case DungeonMood::Frost:
		level = 0.8f;
		return AmbienceTrack::ColdWind;
	case DungeonMood::Embers:
		return AmbienceTrack::Fire;
	case DungeonMood::Drips:
		return AmbienceTrack::CaveDrips;
	case DungeonMood::Rain:
		return AmbienceTrack::Rain;
	case DungeonMood::Storm:
		return AmbienceTrack::Storm;
	case DungeonMood::Snow:
		level = 0.65f;
		return AmbienceTrack::ColdWind;
	case DungeonMood::Wind:
		return AmbienceTrack::Wind;
	case DungeonMood::Fireflies:
		return AmbienceTrack::NightForest;
	case DungeonMood::Ash:
		level = 0.75f;
		return AmbienceTrack::Fire;
	case DungeonMood::Spores:
		return AmbienceTrack::Spores;
	case DungeonMood::BloodMist:
		return AmbienceTrack::Hell;
	default:
		return AmbienceTrack::None;
	}
}

// ---------------------------------------------------------------------------------------------
// Simulation

void UpdateParticles(float dt)
{
	float px, py;
	PlayerWorld(px, py);
	const float range = IsPrecipitation(currentMood) ? 26.0f : 11.5f;
	for (Particle &p : particles) {
		p.life -= dt;
		p.phase += dt * p.rate;
		bool respawn = false;
		switch (currentMood) {
		case DungeonMood::Mist:
		case DungeonMood::Dust:
		case DungeonMood::BloodMist:
			// Lazy Brownian drift, as dust does in still air.
			p.vx = std::clamp(p.vx + RandomRange(-0.12f, 0.12f) * dt, -0.12f, 0.12f);
			p.vy = std::clamp(p.vy + RandomRange(-0.12f, 0.12f) * dt, -0.12f, 0.12f);
			p.vz = std::clamp(p.vz + RandomRange(-0.06f, 0.05f) * dt, -0.07f, 0.04f);
			if (p.z < 0.1f)
				p.vz = std::fabs(p.vz);
			break;
		case DungeonMood::Frost:
			p.vx = std::sin(p.phase * 1.3f) * 0.12f;
			if (p.z <= 0.0f)
				p.life = 0;
			break;
		case DungeonMood::Embers:
			p.vx = std::sin(p.phase * 2.1f) * 0.22f;
			p.vy = std::cos(p.phase * 1.7f) * 0.12f;
			break;
		case DungeonMood::Rain:
		case DungeonMood::Storm:
			if (p.z <= 0.0f) {
				if (rainSplashes.size() < 500 && LightFactor(p.x, p.y) > 0.15f && !IsTileSolid({ static_cast<int>(p.x), static_cast<int>(p.y) }))
					rainSplashes.push_back({ p.x, p.y, 0.0f });
				respawn = true;
			}
			break;
		case DungeonMood::Snow:
		case DungeonMood::Ash: {
			// Flakes sway across the screen as they fall.
			const float sway = (currentMood == DungeonMood::Snow ? 0.12f : 0.06f) * windSign + std::sin(p.phase * 1.1f) * 0.32f;
			p.vx = sway * 0.5f;
			p.vy = -sway * 0.5f;
			if (p.z <= 0.0f)
				respawn = true;
			break;
		}
		case DungeonMood::Wind:
			if (p.kind == 0) {
				// Leaves tumble and bob in the gusts.
				p.vz = std::sin(p.phase * 2.6f) * 0.9f;
				if (p.z < 0.1f)
					p.vz = std::fabs(p.vz) + 0.3f;
			}
			break;
		case DungeonMood::Fireflies:
			p.vx = std::clamp(p.vx + RandomRange(-0.5f, 0.5f) * dt, -0.35f, 0.35f);
			p.vy = std::clamp(p.vy + RandomRange(-0.5f, 0.5f) * dt, -0.35f, 0.35f);
			p.vz = std::sin(p.phase * 0.9f) * 0.15f;
			break;
		case DungeonMood::Spores:
			p.vx = std::sin(p.phase * 0.8f) * 0.1f;
			p.vy = std::cos(p.phase * 0.7f) * 0.1f;
			break;
		default:
			break;
		}
		p.x += p.vx * dt;
		p.y += p.vy * dt;
		p.z += p.vz * dt;
		const float dx = p.x - px, dy = p.y - py;
		if (respawn || p.life <= 0 || dx * dx + dy * dy > range * range)
			SpawnParticle(p, false);
	}
	for (Splash &s : rainSplashes)
		s.age += dt;
	std::erase_if(rainSplashes, [](const Splash &s) { return s.age > 0.22f; });
}

void UpdateDrips(float dt)
{
	float px, py;
	PlayerWorld(px, py);
	for (DripSource &s : dripSources) {
		const float dx = s.x - px, dy = s.y - py;
		if (dx * dx + dy * dy > 14.0f * 14.0f)
			continue;
		s.timer -= dt;
		if (s.timer <= 0) {
			s.timer += s.period * RandomRange(0.8f, 1.25f);
			drops.push_back({ s.x, s.y, 2.8f, 0.0f });
		}
	}
	for (Drop &d : drops) {
		d.vz -= 9.0f * dt;
		d.z += d.vz * dt;
		if (d.z <= 0)
			splashes.push_back({ d.x, d.y, 0.0f });
	}
	std::erase_if(drops, [](const Drop &d) { return d.z <= 0; });
	for (Splash &s : splashes)
		s.age += dt;
	std::erase_if(splashes, [](const Splash &s) { return s.age > 0.5f; });
}

// ---------------------------------------------------------------------------------------------
// Projection (classic isometric view or 3D view)

struct Projector {
	const Surface &out;
	bool orbit;
	int zoom;
	float refX, refY;   // world position of the reference tile centre
	float refSX, refSY; // its screen position (unzoomed)

	explicit Projector(const Surface &surface)
	    : out(surface)
	    , orbit(IsOrbitViewActive())
	    , zoom(*GetOptions().Graphics.zoom ? 2 : 1)
	{
		const Point tile = MyPlayer != nullptr ? Point(MyPlayer->position.tile) : Point { 0, 0 };
		refX = tile.x + 0.5f;
		refY = tile.y + 0.5f;
		const Point s = GetScreenPosition(tile);
		refSX = static_cast<float>(s.x + TILE_WIDTH / 2);
		refSY = static_cast<float>(s.y - TILE_HEIGHT / 2);
	}

	/** Screen point, depth and tile scale for a world position. Depth is 0 in the classic view. */
	std::optional<OrbitScreenPoint> Project(float x, float y, float z) const
	{
		if (orbit)
			return OrbitProjectEx(x, y, z, out);
		const float dx = x - refX, dy = y - refY;
		const float sx = (refSX + (dx - dy) * 32.0f) * zoom;
		const float sy = (refSY + (dx + dy) * 16.0f - z * 32.0f) * zoom;
		return OrbitScreenPoint { Point { static_cast<int>(sx), static_cast<int>(sy) }, 0.0f, 45.0f * zoom };
	}

	/** Ground position under a classic-view screen pixel. */
	void Unproject(float sx, float sy, float &x, float &y) const
	{
		const float a = (sx / zoom - refSX) / 32.0f;
		const float b = (sy / zoom - refSY) / 16.0f;
		x = refX + (a + b) * 0.5f;
		y = refY + (b - a) * 0.5f;
	}

	[[nodiscard]] bool Visible(Point p, float depth) const
	{
		if (p.x < 0 || p.y < 0 || p.x >= out.w() || p.y >= out.h())
			return false;
		return !orbit || OrbitSceneVisible(p, depth);
	}
};

void Blend(const Surface &out, Point p, uint8_t color)
{
	uint8_t *pix = out.at(p.x, p.y);
	*pix = paletteTransparencyLookup[*pix][color];
}

void Put(const Surface &out, Point p, uint8_t color)
{
	*out.at(p.x, p.y) = color;
}

/** A small glowing dot: solid core, blended halo for larger sizes. */
void DrawDot(const Projector &pr, const OrbitScreenPoint &sp, int size, uint8_t core, uint8_t halo, bool solid)
{
	const int r = size / 2;
	for (int dy = -r; dy <= r; dy++) {
		for (int dx = -r; dx <= r; dx++) {
			const Point p { sp.pos.x + dx, sp.pos.y + dy };
			if (!pr.Visible(p, sp.depth))
				continue;
			const bool centre = dx == 0 && dy == 0;
			if (centre && solid)
				Put(pr.out, p, core);
			else if (centre || std::abs(dx) + std::abs(dy) <= r)
				Blend(pr.out, p, centre ? core : halo);
		}
	}
}

/** A blended line between two projected points (rain streaks, leaves, dust). */
void DrawLine(const Projector &pr, const OrbitScreenPoint &a, const OrbitScreenPoint &b, uint8_t color, bool solidHead)
{
	const int dx = b.pos.x - a.pos.x, dy = b.pos.y - a.pos.y;
	const int steps = std::max({ std::abs(dx), std::abs(dy), 1 });
	if (steps > 200)
		return; // degenerate projection right next to the camera
	for (int i = 0; i <= steps; i++) {
		const Point p { a.pos.x + dx * i / steps, a.pos.y + dy * i / steps };
		if (!pr.Visible(p, a.depth))
			continue;
		if (i == 0 && solidHead)
			Put(pr.out, p, color);
		else
			Blend(pr.out, p, color);
	}
}

/** Blends a share (0..1) of all pixels with a colour using an ordered dither. */
void Wash(const Surface &out, uint8_t color, float fraction)
{
	const int threshold = static_cast<int>(std::clamp(fraction, 0.0f, 1.0f) * 16.0f);
	if (threshold <= 0)
		return;
	const uint8_t *const lookup = paletteTransparencyLookup[color];
	for (int y = 0; y < out.h(); y++) {
		uint8_t *row = out.at(0, y);
		const uint8_t *bayerRow = Bayer[y & 3];
		for (int x = 0; x < out.w(); x++) {
			if (bayerRow[x & 3] < threshold)
				row[x] = lookup[row[x]];
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Fog

void DrawFogClassic(const Projector &pr, uint8_t color, float density)
{
	const Surface &out = pr.out;
	const uint8_t *const lookup = paletteTransparencyLookup[color];
	constexpr int Block = 4;
	// Dither locked to the world so the fog does not shimmer as the camera scrolls.
	const int ox = static_cast<int>(pr.refSX * pr.zoom) & 3;
	const int oy = static_cast<int>(pr.refSY * pr.zoom) & 3;
	for (int by = 0; by < out.h(); by += Block) {
		for (int bx = 0; bx < out.w(); bx += Block) {
			float wx, wy;
			pr.Unproject(bx + Block * 0.5f, by + Block * 0.5f, wx, wy);
			const float light = SmoothLight(wx, wy);
			if (light <= 0.05f)
				continue;
			const float n = FogNoise(wx, wy, clockSeconds) * density * std::pow(light, 1.3f);
			const int threshold = static_cast<int>(n * 16.0f);
			if (threshold <= 0)
				continue;
			for (int y = by; y < std::min(by + Block, out.h()); y++) {
				uint8_t *row = out.at(0, y);
				const uint8_t *bayerRow = Bayer[(y - oy) & 3];
				for (int x = bx; x < std::min(bx + Block, out.w()); x++) {
					if (bayerRow[(x - ox) & 3] < threshold)
						row[x] = lookup[row[x]];
				}
			}
		}
	}
}

/** 3D view: fog as soft, depth-tested puffs hovering just above the floor of every lit tile. */
void DrawFogOrbit(const Projector &pr, uint8_t color, float density)
{
	const Surface &out = pr.out;
	const uint8_t *const lookup = paletteTransparencyLookup[color];
	float px, py;
	PlayerWorld(px, py);
	constexpr int Radius = 12;
	constexpr float Step = 0.5f;
	for (float gy = std::floor(py) - Radius; gy <= std::floor(py) + Radius; gy += Step) {
		for (float gx = std::floor(px) - Radius; gx <= std::floor(px) + Radius; gx += Step) {
			const float wx = gx + Step * 0.5f, wy = gy + Step * 0.5f;
			const Point tile { static_cast<int>(wx), static_cast<int>(wy) };
			if (!InDungeonBounds(tile) || IsTileSolid(tile))
				continue;
			const float light = SmoothLight(wx, wy);
			if (light <= 0.05f)
				continue;
			const float n = FogNoise(wx, wy, clockSeconds) * density * std::pow(light, 1.3f);
			if (n < 0.04f)
				continue;
			// Thin out the fog right around the hero so it never hides them.
			const float pdx = wx - px, pdy = wy - py;
			const float nearHero = std::clamp((pdx * pdx + pdy * pdy - 0.8f) / 2.5f, 0.0f, 1.0f);
			if (nearHero <= 0.0f)
				continue;
			const std::optional<OrbitScreenPoint> sp = pr.Project(wx, wy, 0.15f);
			if (!sp || sp->depth < 1.2f)
				continue;
			const int r = std::min(90, static_cast<int>(sp->pixelsPerTile * 0.55f));
			if (r < 2)
				continue;
			const int x0 = std::max(0, sp->pos.x - r), x1 = std::min(out.w() - 1, sp->pos.x + r);
			const int y0 = std::max(0, sp->pos.y - r / 3), y1 = std::min(out.h() - 1, sp->pos.y + r / 3);
			const float inv = 1.0f / static_cast<float>(r);
			for (int y = y0; y <= y1; y++) {
				uint8_t *row = out.at(0, y);
				const uint8_t *bayerRow = Bayer[y & 3];
				const float fy = (y - sp->pos.y) * 3.0f * inv;
				for (int x = x0; x <= x1; x++) {
					const float fx = (x - sp->pos.x) * inv;
					const float d2 = fx * fx + fy * fy;
					if (d2 >= 1.0f)
						continue;
					// Overlapping puffs: each one contributes a soft-edged share of the dither.
					const int threshold = static_cast<int>(n * nearHero * (1.0f - d2) * 20.0f);
					if (bayerRow[x & 3] >= threshold)
						continue;
					if (!OrbitSceneVisible({ x, y }, sp->depth))
						continue;
					row[x] = lookup[row[x]];
				}
			}
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Particles

void DrawParticles(const Projector &pr)
{
	// While the weather turns over, only part of the particles show.
	const size_t shown = static_cast<size_t>(static_cast<float>(particles.size()) * fade);
	for (size_t i = 0; i < shown; i++) {
		const Particle &p = particles[i];
		const std::optional<OrbitScreenPoint> sp = pr.Project(p.x, p.y, p.z);
		if (!sp)
			continue;
		// Fade in and out over the particle's life.
		const float age = p.maxLife - p.life;
		const float lifeFade = std::clamp(std::min(age, p.life) / 0.8f, 0.0f, 1.0f);
		const int size = pr.orbit ? std::clamp(static_cast<int>(sp->pixelsPerTile * 0.035f), 1, 4) : 2 * pr.zoom;
		switch (currentMood) {
		case DungeonMood::Mist:
		case DungeonMood::Dust:
		case DungeonMood::BloodMist: {
			// Motes only show where the light catches them, and twinkle as they turn.
			const float light = LightFactor(p.x, p.y) * lifeFade;
			const float twinkle = 0.5f + 0.5f * std::sin(p.phase * 2.3f);
			const float glow = light * (0.35f + 0.65f * twinkle);
			if (glow < 0.2f)
				continue;
			uint8_t core;
			if (currentMood == DungeonMood::Dust)
				core = PAL16_YELLOW + (glow > 0.7f ? 0 : 3);
			else if (currentMood == DungeonMood::BloodMist)
				core = colors.bloodMote;
			else
				core = PAL16_GRAY + (glow > 0.7f ? 1 : 4);
			DrawDot(pr, *sp, size, core, core, glow > 0.6f);
			break;
		}
		case DungeonMood::Frost: {
			const float light = LightFactor(p.x, p.y);
			if (light < 0.2f)
				continue;
			const bool sparkle = std::sin(p.phase * 5.0f + p.x) > 0.75f;
			DrawDot(pr, *sp, sparkle ? size + 1 : size, sparkle ? colors.white : PAL16_BLUE + 1, PAL16_BLUE + 3, sparkle);
			break;
		}
		case DungeonMood::Embers: {
			// Embers glow on their own, but stay hidden in unexplored darkness.
			if (LightFactor(p.x, p.y) < 0.05f)
				continue;
			const float heat = p.life / p.maxLife;
			const uint8_t core = heat > 0.6f ? PAL8_YELLOW : (heat > 0.3f ? PAL16_ORANGE + 2 : PAL16_RED + 4);
			DrawDot(pr, *sp, std::max(size, 2) + 1, core, heat > 0.5f ? PAL16_ORANGE + 2 : PAL16_ORANGE + 6, true);
			break;
		}
		case DungeonMood::Rain:
		case DungeonMood::Storm: {
			if (LightFactor(p.x, p.y) < 0.15f)
				continue;
			// The streak trails up and back along the drop's path.
			const float len = currentMood == DungeonMood::Storm ? 0.032f : 0.026f;
			const std::optional<OrbitScreenPoint> tail = pr.Project(p.x - p.vx * len, p.y - p.vy * len, p.z - p.vz * len);
			if (!tail)
				continue;
			DrawLine(pr, *sp, *tail, colors.rain, false);
			break;
		}
		case DungeonMood::Snow: {
			if (LightFactor(p.x, p.y) < 0.15f)
				continue;
			const bool big = (static_cast<int>(p.rate * 10.0f) & 1) != 0;
			DrawDot(pr, *sp, big ? std::max(size, 2) + 1 : std::max(size, 2), colors.snow, colors.snowHaze, true);
			break;
		}
		case DungeonMood::Ash: {
			if (LightFactor(p.x, p.y) < 0.1f)
				continue;
			if (p.kind == 1) {
				const bool hot = std::sin(p.phase * 3.0f) > 0;
				DrawDot(pr, *sp, std::max(size, 2), hot ? PAL16_ORANGE + 2 : PAL16_RED + 4, PAL16_ORANGE + 6, true);
			} else {
				const bool dark = (static_cast<int>(p.rate * 10.0f) & 1) != 0;
				DrawDot(pr, *sp, std::max(size, 2), dark ? colors.ashDark : colors.ash, colors.ashDark, true);
			}
			break;
		}
		case DungeonMood::Wind: {
			if (LightFactor(p.x, p.y) < 0.15f)
				continue;
			if (p.kind == 0) {
				// A leaf: a short stroke that turns over as it tumbles.
				const float angle = p.phase * 4.0f;
				const float r = 0.13f;
				const std::optional<OrbitScreenPoint> a = pr.Project(p.x + std::cos(angle) * r, p.y - std::cos(angle) * r, p.z + std::sin(angle) * r);
				const std::optional<OrbitScreenPoint> b = pr.Project(p.x - std::cos(angle) * r, p.y + std::cos(angle) * r, p.z - std::sin(angle) * r);
				if (a && b)
					DrawLine(pr, *a, *b, colors.leaves[static_cast<int>(p.rate * 100.0f) % 3], true);
			} else {
				const std::optional<OrbitScreenPoint> tail = pr.Project(p.x - p.vx * 0.09f, p.y - p.vy * 0.09f, p.z);
				if (tail)
					DrawLine(pr, *sp, *tail, colors.windDust, false);
			}
			break;
		}
		case DungeonMood::Fireflies: {
			// They glow on their own, so they show anywhere the hero can see.
			const int tx = static_cast<int>(std::floor(p.x)), ty = static_cast<int>(std::floor(p.y));
			if (!IsVisibleTile(tx, ty))
				continue;
			const float s = std::max(0.0f, std::sin(p.phase * 1.7f));
			const float glow = s * s * s * s * lifeFade;
			if (glow < 0.15f)
				continue;
			DrawDot(pr, *sp, glow > 0.6f ? std::max(size, 2) + 2 : std::max(size, 2), colors.fireflyCore, colors.fireflyHalo, glow > 0.4f);
			break;
		}
		case DungeonMood::Spores: {
			// Spores glow faintly on their own, brighter in the light.
			if (!IsVisibleTile(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y))))
				continue;
			const float light = LightFactor(p.x, p.y);
			const float pulse = (0.5f + 0.5f * std::sin(p.phase * 1.9f)) * lifeFade;
			if (pulse < 0.2f)
				continue;
			DrawDot(pr, *sp, pulse > 0.7f ? std::max(size, 2) + 1 : std::max(size, 2), colors.sporeCore, colors.sporeHalo, pulse * (0.5f + 0.5f * light) > 0.45f);
			break;
		}
		default:
			break;
		}
	}
}

void DrawRainSplashes(const Projector &pr)
{
	for (const Splash &s : rainSplashes) {
		const float t = s.age / 0.22f;
		const std::optional<OrbitScreenPoint> c = pr.Project(s.x, s.y, 0.02f);
		if (!c)
			continue;
		const int spread = 1 + static_cast<int>(t * 3.0f);
		const Point pts[4] = { { c->pos.x - spread, c->pos.y }, { c->pos.x + spread, c->pos.y }, { c->pos.x, c->pos.y - 1 - static_cast<int>(t * 2) }, c->pos };
		for (int i = 0; i < (t < 0.4f ? 4 : 2); i++) {
			if (pr.Visible(pts[i], c->depth))
				Blend(pr.out, pts[i], colors.rainSplash);
		}
	}
}

void DrawDrips(const Projector &pr)
{
	for (const Drop &d : drops) {
		const float light = LightFactor(d.x, d.y);
		if (light < 0.2f)
			continue;
		const std::optional<OrbitScreenPoint> a = pr.Project(d.x, d.y, d.z);
		const std::optional<OrbitScreenPoint> b = pr.Project(d.x, d.y, d.z + 0.12f);
		if (!a || !b)
			continue;
		const int steps = std::max(1, std::abs(a->pos.y - b->pos.y));
		for (int i = 0; i <= steps; i++) {
			const Point p { a->pos.x + (b->pos.x - a->pos.x) * i / steps, a->pos.y + (b->pos.y - a->pos.y) * i / steps };
			if (!pr.Visible(p, a->depth))
				continue;
			if (i == 0)
				Put(pr.out, p, PAL16_BLUE);
			else
				Blend(pr.out, p, PAL16_BLUE + 1);
		}
	}
	for (const Splash &s : splashes) {
		const float light = LightFactor(s.x, s.y);
		if (light < 0.2f)
			continue;
		const float t = s.age / 0.5f;
		const float radius = 0.05f + 0.32f * t;
		const uint8_t color = t < 0.5f ? PAL16_BLUE : PAL16_BLUE + 2;
		constexpr int Points = 14;
		for (int i = 0; i < Points; i++) {
			const float ang = i * 6.2832f / Points;
			const std::optional<OrbitScreenPoint> sp = pr.Project(s.x + std::cos(ang) * radius, s.y + std::sin(ang) * radius, 0.02f);
			if (!sp || !pr.Visible(sp->pos, sp->depth))
				continue;
			if ((i + static_cast<int>(t * 6)) % 3 == 0 && t > 0.5f)
				continue; // the ring breaks up as it fades
			Blend(pr.out, sp->pos, color);
		}
		if (t < 0.3f) {
			// A tiny upward crown right at impact.
			if (const std::optional<OrbitScreenPoint> c = pr.Project(s.x, s.y, 0.05f + 0.15f * t); c && pr.Visible(c->pos, c->depth))
				Put(pr.out, c->pos, PAL16_BLUE);
		}
	}
}

} // namespace

DungeonMood GetDungeonMood()
{
	EnsureMood();
	return currentMood;
}

const char *DungeonMoodName(DungeonMood mood)
{
	switch (mood) {
	case DungeonMood::Clear: return "clear";
	case DungeonMood::Mist: return "mist";
	case DungeonMood::Dust: return "dust";
	case DungeonMood::Frost: return "frost";
	case DungeonMood::Embers: return "embers";
	case DungeonMood::Drips: return "drips";
	case DungeonMood::Rain: return "rain";
	case DungeonMood::Storm: return "storm";
	case DungeonMood::Snow: return "snow";
	case DungeonMood::Wind: return "wind";
	case DungeonMood::Fireflies: return "fireflies";
	case DungeonMood::Ash: return "ash";
	case DungeonMood::Spores: return "spores";
	case DungeonMood::BloodMist: return "bloodmist";
	default: return "clear";
	}
}

void DrawDungeonMood(const Surface &out)
{
	EnsureMood();
	const uint32_t now = SDL_GetTicks();
	float dt = std::min(0.1f, static_cast<float>(now - lastTicks) / 1000.0f);
	lastTicks = now;
#ifdef _DEBUG
	// Burst captures run far slower than real time; step the effects at a steady 30 fps instead.
	if (std::getenv("DEVILUTIONX_DEV_CAPTURE_FRAMES") != nullptr)
		dt = 1.0f / 30.0f;
#endif
	if (moodKey == -1 || MyPlayer == nullptr) {
		UpdateAmbience(AmbienceTrack::None, 0.0f);
		return;
	}
	UpdateWeather(dt);
	float trackLevel = 1.0f;
	const AmbienceTrack track = TrackFor(currentMood, trackLevel);
	UpdateAmbience(track, trackLevel * (0.65f + 0.35f * strength) * (outdoor ? fade : 1.0f));

	if (currentMood == DungeonMood::Clear || out.w() <= 0 || out.h() <= 0)
		return;
	clockSeconds += dt;
	UpdateParticles(dt);
	if (currentMood == DungeonMood::Drips)
		UpdateDrips(dt);

	const Projector pr(out);
	// Rain clouds darken the day.
	if (currentMood == DungeonMood::Storm)
		Wash(out, colors.black, 0.30f * fade);
	else if (currentMood == DungeonMood::Rain)
		Wash(out, colors.black, 0.12f * fade);

	const MoodLook look = LookOf(currentMood);
	const float density = look.fogDensity * (0.7f + 0.3f * strength) * fade;
	if (density > 0.01f) {
		if (pr.orbit)
			DrawFogOrbit(pr, look.fogColor, density);
		else
			DrawFogClassic(pr, look.fogColor, density);
	}
	DrawParticles(pr);
	if (currentMood == DungeonMood::Rain || currentMood == DungeonMood::Storm)
		DrawRainSplashes(pr);
	if (currentMood == DungeonMood::Drips)
		DrawDrips(pr);
	if (flashTime >= 0)
		Wash(out, colors.white, FlashBrightness(flashTime) * 0.7f);
}

} // namespace devilution
