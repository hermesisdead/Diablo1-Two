#include "engine/render/town_weather.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include "engine/palette.h"
#include "utils/palette_blending.hpp"

namespace devilution {

namespace {

struct Particle {
	float x;
	float y;
	float speed;
	float phase;
	uint8_t length;
};

TownWeather currentWeather = TownWeather::Clear;
std::vector<Particle> particles;
int particleAreaW = 0;
int particleAreaH = 0;
uint64_t rngState = 0x2545F4914F6CDD1DULL;
uint32_t lastTicks = 0;
uint32_t nextLightningTicks = 0;
uint32_t lightningStartTicks = 0;
bool lightningActive = false;

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

void Blend(const Surface &out, int x, int y, uint8_t color)
{
	if (x < 0 || y < 0 || x >= out.w() || y >= out.h())
		return;
	uint8_t *pix = out.at(x, y);
	*pix = paletteTransparencyLookup[*pix][color];
}

/** Blends every pixel (or a checkerboard half of them) of the viewport with one palette colour. */
void WashViewport(const Surface &out, uint8_t color, bool checker)
{
	const uint8_t *const lookup = paletteTransparencyLookup[color];
	for (int y = 0; y < out.h(); y++) {
		uint8_t *row = out.at(0, y);
		for (int x = checker ? (y & 1) : 0; x < out.w(); x += checker ? 2 : 1)
			row[x] = lookup[row[x]];
	}
}

/**
 * @brief Blends a share of the viewport's pixels with one palette colour using an ordered dither.
 * @param fractionTop Share of pixels blended at the top row (0..1)
 * @param fractionBottom Share of pixels blended at the bottom row (0..1)
 */
void WashDither(const Surface &out, uint8_t color, float fractionTop, float fractionBottom)
{
	static constexpr uint8_t Bayer[4][4] = {
		{ 0, 8, 2, 10 },
		{ 12, 4, 14, 6 },
		{ 3, 11, 1, 9 },
		{ 15, 7, 13, 5 },
	};
	const uint8_t *const lookup = paletteTransparencyLookup[color];
	for (int y = 0; y < out.h(); y++) {
		const float t = out.h() > 1 ? static_cast<float>(y) / static_cast<float>(out.h() - 1) : 0.0f;
		const int threshold = static_cast<int>((fractionTop + (fractionBottom - fractionTop) * t) * 16.0f);
		uint8_t *row = out.at(0, y);
		const uint8_t *bayerRow = Bayer[y & 3];
		for (int x = 0; x < out.w(); x++) {
			if (bayerRow[x & 3] < threshold)
				row[x] = lookup[row[x]];
		}
	}
}

size_t TargetParticleCount(TownWeather weather, int area)
{
	switch (weather) {
	case TownWeather::Rain:
		return static_cast<size_t>(area / 1100);
	case TownWeather::Storm:
		return static_cast<size_t>(area / 500);
	case TownWeather::Snow:
		return static_cast<size_t>(area / 650);
	case TownWeather::Embers:
		return static_cast<size_t>(area / 2800);
	default:
		return 0;
	}
}

void RespawnParticle(Particle &p, int w, int h, bool anywhere)
{
	p.x = Random01() * static_cast<float>(w + 60) - 30.0f;
	p.phase = Random01() * 6.28f;
	switch (currentWeather) {
	case TownWeather::Rain:
	case TownWeather::Storm:
		p.speed = 380.0f + Random01() * 220.0f;
		p.length = static_cast<uint8_t>(5 + (NextRandom() % 5));
		p.y = anywhere ? Random01() * static_cast<float>(h) : -10.0f;
		break;
	case TownWeather::Snow:
		p.speed = 28.0f + Random01() * 30.0f;
		p.length = 2 + (NextRandom() % 2);
		p.y = anywhere ? Random01() * static_cast<float>(h) : -4.0f;
		break;
	case TownWeather::Embers:
		p.speed = 18.0f + Random01() * 26.0f;
		p.length = 1;
		p.y = anywhere ? Random01() * static_cast<float>(h) : static_cast<float>(h) + 4.0f;
		break;
	default:
		break;
	}
}

void UpdateParticles(int w, int h, float dt)
{
	const size_t target = TargetParticleCount(currentWeather, w * h);
	if (w != particleAreaW || h != particleAreaH || particles.size() != target) {
		particleAreaW = w;
		particleAreaH = h;
		particles.assign(target, Particle {});
		for (Particle &p : particles)
			RespawnParticle(p, w, h, true);
	}
	for (Particle &p : particles) {
		switch (currentWeather) {
		case TownWeather::Rain:
			p.y += p.speed * dt;
			p.x -= p.speed * 0.32f * dt;
			break;
		case TownWeather::Storm:
			p.y += p.speed * dt * 1.15f;
			p.x -= p.speed * 0.55f * dt;
			break;
		case TownWeather::Snow:
			p.y += p.speed * dt;
			p.phase += dt * 1.5f;
			p.x += (std::sin(p.phase) * 14.0f + 6.0f) * dt;
			break;
		case TownWeather::Embers:
			p.y -= p.speed * dt;
			p.phase += dt * 2.2f;
			p.x += std::sin(p.phase) * 18.0f * dt;
			break;
		default:
			break;
		}
		const bool gone = p.y > static_cast<float>(h) + 12.0f || p.y < -12.0f || p.x < -40.0f || p.x > static_cast<float>(w) + 40.0f;
		if (gone)
			RespawnParticle(p, w, h, false);
	}
}

} // namespace

void InitTownWeather()
{
	const uint32_t ticks = SDL_GetTicks();
	rngState ^= static_cast<uint64_t>(ticks) * 0xD1342543DE82EF95ULL;
	particles.clear();
	particleAreaW = particleAreaH = 0;
	lightningActive = false;
	lastTicks = ticks;
	nextLightningTicks = ticks + 3000 + NextRandom() % 6000;

	// Weights out of 100: clear 30, rain 25, fog 15, snow 15, storm 10, embers 5.
	const uint32_t roll = NextRandom() % 100;
	if (roll < 30)
		currentWeather = TownWeather::Clear;
	else if (roll < 55)
		currentWeather = TownWeather::Rain;
	else if (roll < 70)
		currentWeather = TownWeather::Fog;
	else if (roll < 85)
		currentWeather = TownWeather::Snow;
	else if (roll < 95)
		currentWeather = TownWeather::Storm;
	else
		currentWeather = TownWeather::Embers;

#ifdef _DEBUG
	if (const char *forced = std::getenv("DEVILUTIONX_DEV_WEATHER"); forced != nullptr) {
		if (std::strcmp(forced, "clear") == 0) currentWeather = TownWeather::Clear;
		else if (std::strcmp(forced, "rain") == 0) currentWeather = TownWeather::Rain;
		else if (std::strcmp(forced, "storm") == 0) currentWeather = TownWeather::Storm;
		else if (std::strcmp(forced, "snow") == 0) currentWeather = TownWeather::Snow;
		else if (std::strcmp(forced, "fog") == 0) currentWeather = TownWeather::Fog;
		else if (std::strcmp(forced, "embers") == 0) currentWeather = TownWeather::Embers;
	}
#endif
}

TownWeather GetTownWeather()
{
	return currentWeather;
}

void DrawTownWeather(const Surface &out)
{
	if (currentWeather == TownWeather::Clear || out.w() <= 0 || out.h() <= 0)
		return;

	const uint32_t now = SDL_GetTicks();
	const float dt = std::min(0.1f, static_cast<float>(now - lastTicks) / 1000.0f);
	lastTicks = now;
	UpdateParticles(out.w(), out.h(), dt);

	const uint8_t rainColor = PAL16_GRAY + 4;
	const uint8_t snowColor = PAL16_GRAY;
	const uint8_t fogColor = PAL16_GRAY + 7;
	const uint8_t darkColor = 0;

	switch (currentWeather) {
	case TownWeather::Rain:
	case TownWeather::Storm: {
		WashDither(out, darkColor, 0.3f, 0.3f);
		for (const Particle &p : particles) {
			const int px = static_cast<int>(p.x);
			const int py = static_cast<int>(p.y);
			const int steps = p.length;
			const float slope = currentWeather == TownWeather::Storm ? 0.5f : 0.32f;
			for (int i = 0; i < steps; i++)
				Blend(out, px - static_cast<int>(static_cast<float>(i) * slope), py - i, rainColor);
		}
		if (currentWeather == TownWeather::Storm) {
			if (!lightningActive && now >= nextLightningTicks) {
				lightningActive = true;
				lightningStartTicks = now;
			}
			if (lightningActive) {
				const uint32_t t = now - lightningStartTicks;
				if (t < 90)
					WashViewport(out, PAL16_GRAY, false);
				else if (t >= 170 && t < 260)
					WashDither(out, PAL16_GRAY, 0.5f, 0.5f);
				else if (t >= 400) {
					lightningActive = false;
					nextLightningTicks = now + 4000 + NextRandom() % 10000;
				}
			}
		}
		break;
	}
	case TownWeather::Snow:
		WashDither(out, fogColor, 0.12f, 0.2f);
		for (const Particle &p : particles) {
			const int px = static_cast<int>(p.x);
			const int py = static_cast<int>(p.y);
			for (int dx = 0; dx < p.length; dx++)
				for (int dy = 0; dy < p.length; dy++)
					Blend(out, px + dx, py + dy, snowColor);
		}
		break;
	case TownWeather::Fog:
		// Thicker towards the bottom of the viewport, like ground fog.
		WashDither(out, fogColor, 0.18f, 0.55f);
		break;
	case TownWeather::Embers:
		for (const Particle &p : particles) {
			const uint8_t color = (static_cast<int>(p.phase * 4.0f) & 1) ? PAL16_ORANGE + 2 : PAL8_YELLOW;
			const int px = static_cast<int>(p.x);
			const int py = static_cast<int>(p.y);
			// Opaque glowing core with a blended halo.
			for (int dx = -1; dx <= 1; dx++)
				for (int dy = -1; dy <= 1; dy++)
					Blend(out, px + dx, py + dy, color);
			if (px >= 0 && py >= 0 && px < out.w() && py < out.h())
				*out.at(px, py) = color;
		}
		break;
	default:
		break;
	}
}

} // namespace devilution
