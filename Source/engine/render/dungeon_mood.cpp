#include "engine/render/dungeon_mood.hpp"

#include <algorithm>
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

DungeonMood currentMood = DungeonMood::Clear;
int64_t moodKey = -1;
std::vector<Particle> particles;
std::vector<DripSource> dripSources;
std::vector<Drop> drops;
std::vector<Splash> splashes;
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
	const float wx = x + t * 0.22f, wy = y + t * 0.09f;
	const float n = ValueNoise(wx / 4.5f, wy / 4.5f) * 0.65f + ValueNoise(wx / 1.7f + 17.0f, wy / 1.7f - 9.0f) * 0.35f;
	return 0.3f + 0.7f * std::clamp((n - 0.40f) / 0.36f, 0.0f, 1.0f);
}

/** How lit a spot is, 0 (dark) .. 1 (fully lit). */
float LightFactor(float x, float y)
{
	const int tx = static_cast<int>(std::floor(x));
	const int ty = static_cast<int>(std::floor(y));
	if (tx < 0 || ty < 0 || tx >= MAXDUNX || ty >= MAXDUNY || dPiece[tx][ty] == 0)
		return 0.0f; // outside the map: the void reads as lit but must stay black
	if (!HasAnyOf(dFlags[tx][ty], DungeonFlag::Visible))
		return 0.0f; // only what the hero can see right now
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

MoodLook LookOf(DungeonMood mood)
{
	switch (mood) {
	case DungeonMood::Mist:
		return { PAL16_GRAY + 7, 0.8f, 50 };
	case DungeonMood::Dust:
		return { PAL16_YELLOW + 7, 0.0f, 280 };
	case DungeonMood::Frost:
		return { PAL16_BLUE + 2, 0.34f, 150 };
	case DungeonMood::Embers:
		return { PAL16_RED + 9, 0.22f, 130 };
	case DungeonMood::Drips:
		return { PAL16_GRAY + 9, 0.18f, 0 };
	default:
		return { 0, 0.0f, 0 };
	}
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

DungeonMood RollMood(uint32_t roll)
{
	struct Weight {
		DungeonMood mood;
		uint32_t weight;
	};
	std::vector<Weight> table;
	switch (leveltype) {
	case DTYPE_CATHEDRAL:
	case DTYPE_CRYPT:
		table = { { DungeonMood::Clear, 20 }, { DungeonMood::Mist, 40 }, { DungeonMood::Dust, 40 } };
		break;
	case DTYPE_CATACOMBS:
		table = { { DungeonMood::Clear, 20 }, { DungeonMood::Mist, 25 }, { DungeonMood::Dust, 20 }, { DungeonMood::Drips, 35 } };
		break;
	case DTYPE_CAVES:
	case DTYPE_NEST:
		table = { { DungeonMood::Clear, 15 }, { DungeonMood::Mist, 20 }, { DungeonMood::Frost, 25 }, { DungeonMood::Embers, 20 }, { DungeonMood::Drips, 20 } };
		break;
	case DTYPE_HELL:
		table = { { DungeonMood::Clear, 20 }, { DungeonMood::Embers, 50 }, { DungeonMood::Mist, 30 } };
		break;
	case DTYPE_FOREST:
		table = { { DungeonMood::Clear, 25 }, { DungeonMood::Mist, 50 }, { DungeonMood::Dust, 25 } };
		break;
	default:
		return DungeonMood::Clear;
	}
	uint32_t total = 0;
	for (const Weight &w : table)
		total += w.weight;
	uint32_t r = roll % total;
	for (const Weight &w : table) {
		if (r < w.weight)
			return w.mood;
		r -= w.weight;
	}
	return DungeonMood::Clear;
}

void SpawnParticle(Particle &p, bool anywhere)
{
	float px, py;
	PlayerWorld(px, py);
	const float angle = Random01() * 6.2832f;
	const float radius = std::sqrt(Random01()) * 10.0f;
	p.x = px + std::cos(angle) * radius;
	p.y = py + std::sin(angle) * radius;
	p.phase = Random01() * 6.2832f;
	switch (currentMood) {
	case DungeonMood::Mist:
	case DungeonMood::Dust:
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
	default:
		break;
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

void InitMood(int64_t key, uint32_t seed)
{
	moodKey = key;
	rngState = (static_cast<uint64_t>(seed) << 32) ^ static_cast<uint64_t>(key) * 0xD1342543DE82EF95ULL ^ 0x5DEECE66DULL;
	currentMood = RollMood(NextRandom());
#ifdef _DEBUG
	if (const char *forced = std::getenv("DEVILUTIONX_DEV_MOOD"); forced != nullptr) {
		for (DungeonMood m : { DungeonMood::Clear, DungeonMood::Mist, DungeonMood::Dust, DungeonMood::Frost, DungeonMood::Embers, DungeonMood::Drips }) {
			if (std::strcmp(forced, DungeonMoodName(m)) == 0)
				currentMood = m;
		}
	}
#endif
	particles.assign(LookOf(currentMood).particles, Particle {});
	for (Particle &p : particles)
		SpawnParticle(p, true);
	drops.clear();
	splashes.clear();
	dripSources.clear();
	if (currentMood == DungeonMood::Drips)
		FindDripSources();
	LogVerbose("Dungeon mood: {}", DungeonMoodName(currentMood));
}

void EnsureMood()
{
	if (leveltype == DTYPE_TOWN || !*GetOptions().Graphics.dungeonMoods) {
		currentMood = DungeonMood::Clear;
		moodKey = -1;
		return;
	}
	// The forest re-rolls its mood with every visit; dungeon levels keep theirs for the game.
	const uint32_t seed = leveltype == DTYPE_FOREST && MyPlayer != nullptr ? MyPlayer->forestSeed : DungeonSeeds[currlevel];
	const int64_t key = (setlevel ? (int64_t { 1 } << 20) + static_cast<int64_t>(setlvlnum) : static_cast<int64_t>(currlevel))
	    + (static_cast<int64_t>(leveltype) << 24) + (static_cast<int64_t>(seed) << 32);
	if (key != moodKey)
		InitMood(key, seed);
}

// ---------------------------------------------------------------------------------------------
// Simulation

void UpdateParticles(float dt)
{
	float px, py;
	PlayerWorld(px, py);
	for (Particle &p : particles) {
		p.life -= dt;
		p.phase += dt;
		switch (currentMood) {
		case DungeonMood::Mist:
		case DungeonMood::Dust:
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
		default:
			break;
		}
		p.x += p.vx * dt;
		p.y += p.vy * dt;
		p.z += p.vz * dt;
		const float dx = p.x - px, dy = p.y - py;
		if (p.life <= 0 || dx * dx + dy * dy > 11.5f * 11.5f)
			SpawnParticle(p, false);
	}
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
	for (const Particle &p : particles) {
		const std::optional<OrbitScreenPoint> sp = pr.Project(p.x, p.y, p.z);
		if (!sp)
			continue;
		// Fade in and out over the particle's life.
		const float age = p.maxLife - p.life;
		const float fade = std::clamp(std::min(age, p.life) / 0.8f, 0.0f, 1.0f);
		const int size = pr.orbit ? std::clamp(static_cast<int>(sp->pixelsPerTile * 0.035f), 1, 4) : 2 * pr.zoom;
		switch (currentMood) {
		case DungeonMood::Mist:
		case DungeonMood::Dust: {
			// Motes only show where the light catches them, and twinkle as they turn.
			const float light = LightFactor(p.x, p.y) * fade;
			const float twinkle = 0.5f + 0.5f * std::sin(p.phase * 2.3f);
			const float glow = light * (0.35f + 0.65f * twinkle);
			if (glow < 0.2f)
				continue;
			const uint8_t core = currentMood == DungeonMood::Dust ? PAL16_YELLOW + (glow > 0.7f ? 0 : 3) : PAL16_GRAY + (glow > 0.7f ? 1 : 4);
			DrawDot(pr, *sp, size, core, core, glow > 0.6f);
			break;
		}
		case DungeonMood::Frost: {
			const float light = LightFactor(p.x, p.y);
			if (light < 0.2f)
				continue;
			const bool sparkle = std::sin(p.phase * 5.0f + p.x) > 0.75f;
			DrawDot(pr, *sp, sparkle ? size + 1 : size, sparkle ? 255 : PAL16_BLUE + 1, PAL16_BLUE + 3, sparkle);
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
		default:
			break;
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
	}
	return "clear";
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
	if (currentMood == DungeonMood::Clear || out.w() <= 0 || out.h() <= 0 || MyPlayer == nullptr)
		return;
	clockSeconds += dt;
	UpdateParticles(dt);
	if (currentMood == DungeonMood::Drips)
		UpdateDrips(dt);

	const Projector pr(out);
	const MoodLook look = LookOf(currentMood);
	if (look.fogDensity > 0) {
		if (pr.orbit)
			DrawFogOrbit(pr, look.fogColor, look.fogDensity);
		else
			DrawFogClassic(pr, look.fogColor, look.fogDensity);
	}
	DrawParticles(pr);
	if (currentMood == DungeonMood::Drips)
		DrawDrips(pr);
}

} // namespace devilution
