#include "engine/render/gore_fx.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include "diablo.h"
#include "engine/lighting_defs.hpp"
#include "engine/render/color_overlay.hpp"
#include "engine/render/world_projector.hpp"
#include "free_move.hpp"
#include "items.h"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "monster.h"
#include "options.h"
#include "player.h"
#include "utils/log.hpp"

namespace devilution {

namespace {

enum class GoreKind : uint8_t {
	Blood,  // beasts and men: bright red
	Rot,    // zombies and ghouls: dark, brownish
	Ichor,  // demons: deep crimson, almost black
	Bone,   // skeletons: bone chips and dust
	Rubble, // stone and mud creatures: grit
};

struct GoreLook {
	OverlayColor drop;   // flying droplets
	OverlayColor chunk;  // flying chunks
	OverlayColor splat;  // on the floor
	float splatAlpha;    // how solid the stain is
	float splatSize;     // stain size multiplier
	bool liquid;         // pools and long-lived stains
};

const GoreLook &LookOf(GoreKind kind)
{
	static const GoreLook blood { { 175, 12, 10 }, { 120, 18, 14 }, { 105, 6, 6 }, 0.85f, 1.0f, true };
	static const GoreLook rot { { 110, 40, 18 }, { 80, 45, 25 }, { 62, 26, 12 }, 0.8f, 1.0f, true };
	static const GoreLook ichor { { 130, 0, 24 }, { 70, 10, 20 }, { 48, 0, 12 }, 0.9f, 1.1f, true };
	static const GoreLook bone { { 210, 200, 180 }, { 230, 222, 200 }, { 90, 84, 72 }, 0.45f, 0.6f, false };
	static const GoreLook rubble { { 130, 122, 110 }, { 105, 98, 88 }, { 70, 66, 60 }, 0.45f, 0.6f, false };
	switch (kind) {
	case GoreKind::Rot: return rot;
	case GoreKind::Ichor: return ichor;
	case GoreKind::Bone: return bone;
	case GoreKind::Rubble: return rubble;
	default: return blood;
	}
}

bool Contains(std::string_view text, std::string_view part)
{
	return text.find(part) != std::string_view::npos;
}

GoreKind KindOf(const Monster &monster)
{
	const MonsterData &data = monster.data();
	const std::string_view name = data.name;
	if (Contains(name, "Stone") || Contains(name, "Mud") || Contains(name, "Golem") || Contains(name, "Gargoyle"))
		return GoreKind::Rubble;
	switch (data.monsterClass) {
	case MonsterClass::Undead:
		if (Contains(name, "Skeleton") || Contains(name, "Corpse") || Contains(name, "Burning Dead") || Contains(name, "Horror") || Contains(name, "bone"))
			return GoreKind::Bone;
		return GoreKind::Rot;
	case MonsterClass::Demon:
		return GoreKind::Ichor;
	default:
		return GoreKind::Blood;
	}
}

struct Drop {
	float x, y, z;
	float vx, vy, vz;
	float size; // tiles
	float life;
	GoreKind kind;
	bool chunk;
	uint8_t bounces;
};

struct Blob {
	float dx, dy, r;
};

struct Splat {
	float x, y;
	float age, maxAge;
	float grow; // seconds to spread to full size (pools)
	float size;
	GoreKind kind;
	uint8_t blobs;
	std::array<Blob, 6> blob;
};

struct Flash {
	float x, y;
	float age, duration;
};

struct Mote {
	float x, y, z, vz, life, maxLife;
};

constexpr size_t MaxDrops = 3000;
constexpr size_t MaxSplats = 700;

std::vector<Drop> drops;
std::vector<Splat> splats;
std::vector<Flash> flashes;
std::vector<Mote> motes;
std::array<int, MaxMonsters> lastHp {};
std::array<bool, MaxMonsters> wasDead {};
std::array<bool, MaxMonsters> known {};
int lastPlayerHp = -1;
uint64_t rng = 0xA0761D6478BD642FULL;
uint32_t lastTicks = 0;
int levelKey = -1;
float beamEmit = 0;

float Rand01()
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return static_cast<float>(rng & 0xFFFFFF) / static_cast<float>(1 << 24);
}

float RandRange(float lo, float hi)
{
	return lo + (hi - lo) * Rand01();
}

float GoreAmount()
{
	return 1.0f;
}

void MonsterWorldPosition(const Monster &monster, float &x, float &y)
{
	if (IsOrbitViewActive() && OrbitMonsterRenderPosition(static_cast<int>(monster.getId()), x, y))
		return;
	const Displacement off = GetMonsterDrawOffset(monster);
	x = monster.position.tile.x + 0.5f + off.deltaX / 64.0f + off.deltaY / 32.0f;
	y = monster.position.tile.y + 0.5f + off.deltaY / 32.0f - off.deltaX / 64.0f;
}

void PlayerWorldPosition(const Player &player, float &x, float &y)
{
	const Displacement off = GetPlayerDrawOffset(player);
	x = player.position.tile.x + 0.5f + off.deltaX / 64.0f + off.deltaY / 32.0f;
	y = player.position.tile.y + 0.5f + off.deltaY / 32.0f - off.deltaX / 64.0f;
}

void AddSplat(float x, float y, float size, GoreKind kind, bool pool)
{
	if (splats.size() >= MaxSplats)
		splats.erase(splats.begin()); // the oldest stain goes
	Splat s {};
	s.x = x;
	s.y = y;
	s.kind = kind;
	s.size = size * LookOf(kind).splatSize;
	s.maxAge = LookOf(kind).liquid ? RandRange(90.0f, 150.0f) : RandRange(25.0f, 45.0f);
	s.grow = pool ? 1.6f : 0.0f;
	// An irregular stain: a main blob and a few smaller ones around it.
	s.blobs = static_cast<uint8_t>(pool ? 6 : 3 + static_cast<int>(Rand01() * 3));
	for (int i = 0; i < s.blobs; i++) {
		const float a = RandRange(0.0f, 6.2832f);
		const float d = i == 0 ? 0.0f : RandRange(0.4f, 1.1f) * s.size;
		s.blob[i] = { std::cos(a) * d, std::sin(a) * d, s.size * (i == 0 ? 1.0f : RandRange(0.3f, 0.6f)) };
	}
	splats.push_back(s);
}

void Spray(float x, float y, float z, float dirX, float dirY, GoreKind kind, int count, float speedMin, float speedMax, int chunks, float spread)
{
	for (int i = 0; i < count + chunks && drops.size() < MaxDrops; i++) {
		const bool chunk = i >= count;
		Drop d {};
		d.kind = kind;
		d.chunk = chunk;
		d.x = x + RandRange(-0.1f, 0.1f);
		d.y = y + RandRange(-0.1f, 0.1f);
		d.z = z + RandRange(-0.15f, 0.15f);
		const float a = std::atan2(dirY, dirX) + RandRange(-spread, spread);
		const float speed = RandRange(speedMin, speedMax) * (chunk ? 0.7f : 1.0f);
		const bool any = dirX != 0 || dirY != 0;
		const float ang = any ? a : RandRange(0.0f, 6.2832f);
		d.vx = std::cos(ang) * speed;
		d.vy = std::sin(ang) * speed;
		d.vz = RandRange(0.8f, 3.2f) * (chunk ? 1.2f : 1.0f);
		d.size = chunk ? RandRange(0.05f, 0.09f) : RandRange(0.018f, 0.04f);
		d.life = 4.0f;
		drops.push_back(d);
	}
}

void TrackMonsters()
{
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		const size_t id = monster.getId();
		if (id >= MaxMonsters)
			continue;
		const int hp = static_cast<int>(monster.hitPoints.whole());
		const bool dead = monster.mode == MonsterMode::Death || hp <= 0;
		if (!known[id]) {
			known[id] = true;
			lastHp[id] = hp;
			wasDead[id] = dead;
			continue;
		}
		if (monster.isPlayerMinion() || (monster.flags & MFLAG_HIDDEN) != 0) {
			lastHp[id] = hp;
			wasDead[id] = dead;
			continue;
		}
		float x, y;
		MonsterWorldPosition(monster, x, y);
		// Spray away from the nearest hero (who most likely struck).
		float dirX = 0, dirY = 0;
		if (MyPlayer != nullptr) {
			float px, py;
			PlayerWorldPosition(*MyPlayer, px, py);
			const float dx = x - px, dy = y - py;
			const float len = std::sqrt(dx * dx + dy * dy);
			if (len > 0.01f) {
				dirX = dx / len;
				dirY = dy / len;
			}
		}
		const GoreKind kind = KindOf(monster);
		const float amount = GoreAmount();
		if (dead && !wasDead[id]) {
			// Death: a burst, chunks and a spreading pool.
			Spray(x, y, 0.6f, dirX, dirY, kind, static_cast<int>(70 * amount), 1.2f, 3.6f, static_cast<int>(9 * amount), 1.3f);
			Spray(x, y, 0.5f, 0, 0, kind, static_cast<int>(25 * amount), 0.4f, 1.6f, 0, 3.14f);
			if (LookOf(kind).liquid) {
				// A pool that spreads out from under the body, and a few heavier splashes.
				AddSplat(x, y, RandRange(0.6f, 0.8f), kind, true);
				for (int i = 0; i < 3; i++)
					AddSplat(x + dirX * RandRange(0.5f, 1.2f) + RandRange(-0.3f, 0.3f), y + dirY * RandRange(0.5f, 1.2f) + RandRange(-0.3f, 0.3f), RandRange(0.12f, 0.2f), kind, false);
			}
		} else if (hp < lastHp[id] && !dead) {
			const int maxHp = std::max(1, static_cast<int>(monster.maxHitPoints.whole()));
			const float share = std::clamp(static_cast<float>(lastHp[id] - hp) / static_cast<float>(maxHp), 0.0f, 1.0f);
			const int count = static_cast<int>((14.0f + 70.0f * share) * amount);
			Spray(x, y, 0.65f, dirX, dirY, kind, count, 1.0f, 3.0f, share > 0.3f ? 2 : 0, 0.7f);
		}
		lastHp[id] = hp;
		wasDead[id] = dead;
	}
	if (MyPlayer != nullptr) {
		const int hp = static_cast<int>(MyPlayer->_pHitPoints.whole());
		if (lastPlayerHp >= 0 && hp < lastPlayerHp && hp > 0) {
			float x, y;
			PlayerWorldPosition(*MyPlayer, x, y);
			Spray(x, y, 0.65f, 0, 0, GoreKind::Blood, static_cast<int>(12 * GoreAmount()), 0.6f, 2.0f, 0, 3.14f);
		}
		lastPlayerHp = hp;
	}
}

void Simulate(float dt)
{
	for (Drop &d : drops) {
		d.vz -= 9.0f * dt;
		d.x += d.vx * dt;
		d.y += d.vy * dt;
		d.z += d.vz * dt;
		d.life -= dt;
		if (d.z > 0)
			continue;
		const Point tile { static_cast<int>(std::floor(d.x)), static_cast<int>(std::floor(d.y)) };
		const bool onFloor = InDungeonBounds(tile) && !IsTileSolid(tile);
		if (d.chunk && d.bounces < 2) {
			// Chunks bounce and smear.
			d.z = 0.01f;
			d.vz = -d.vz * 0.35f;
			d.vx *= 0.5f;
			d.vy *= 0.5f;
			d.bounces++;
			if (onFloor)
				AddSplat(d.x, d.y, RandRange(0.06f, 0.1f), d.kind, false);
			continue;
		}
		if (onFloor && (d.chunk || Rand01() < 0.55f))
			AddSplat(d.x, d.y, d.chunk ? RandRange(0.08f, 0.12f) : RandRange(0.035f, 0.07f), d.kind, false);
		d.life = 0;
	}
	std::erase_if(drops, [](const Drop &d) { return d.life <= 0; });
	for (Splat &s : splats)
		s.age += dt;
	std::erase_if(splats, [](const Splat &s) { return s.age >= s.maxAge; });
	for (Flash &f : flashes)
		f.age += dt;
	std::erase_if(flashes, [](const Flash &f) { return f.age >= f.duration; });
	for (Mote &m : motes) {
		m.z += m.vz * dt;
		m.life -= dt;
	}
	std::erase_if(motes, [](const Mote &m) { return m.life <= 0; });
}

float LightAtTile(float x, float y)
{
	const int tx = static_cast<int>(std::floor(x)), ty = static_cast<int>(std::floor(y));
	if (tx < 0 || ty < 0 || tx >= MAXDUNX || ty >= MAXDUNY)
		return 0;
	if (leveltype == DTYPE_TOWN)
		return 1;
	if (!HasAnyOf(dFlags[tx][ty], DungeonFlag::Visible))
		return 0;
	return std::clamp(static_cast<float>(LightsMax - dLight[tx][ty]) / static_cast<float>(LightsMax), 0.0f, 1.0f);
}

OverlayColor Dim(OverlayColor c, float k)
{
	return { static_cast<uint8_t>(c.r * k), static_cast<uint8_t>(c.g * k), static_cast<uint8_t>(c.b * k) };
}

/** How a liquid stain tints the floor it soaks into (multiply), fresh and dried. */
struct StainTint {
	float fresh[3];
	float dried[3];
};

const StainTint &TintOf(GoreKind kind)
{
	static const StainTint blood { { 0.66f, 0.09f, 0.07f }, { 0.40f, 0.10f, 0.08f } };
	static const StainTint rot { { 0.52f, 0.30f, 0.14f }, { 0.36f, 0.24f, 0.13f } };
	static const StainTint ichor { { 0.38f, 0.04f, 0.10f }, { 0.24f, 0.05f, 0.09f } };
	static const StainTint dust { { 0.85f, 0.82f, 0.76f }, { 0.85f, 0.82f, 0.76f } };
	switch (kind) {
	case GoreKind::Rot: return rot;
	case GoreKind::Ichor: return ichor;
	case GoreKind::Bone:
	case GoreKind::Rubble: return dust;
	default: return blood;
	}
}

void DrawSplats(const WorldProjector &pr)
{
	std::vector<float> cover, rim, shine;
	for (const Splat &s : splats) {
		const float light = LightAtTile(s.x, s.y);
		if (light <= 0.03f)
			continue;
		const GoreLook &look = LookOf(s.kind);
		const StainTint &tint = TintOf(s.kind);
		const float grow = s.grow > 0 ? std::clamp(s.age / s.grow, 0.25f, 1.0f) : 1.0f;
		const float fade = std::clamp((s.maxAge - s.age) / 12.0f, 0.0f, 1.0f);
		// Fresh blood is wet and shiny; it darkens and goes matte as it dries.
		const float dry = std::clamp(s.age / 30.0f, 0.0f, 1.0f);
		const float wet = look.liquid ? std::clamp(1.0f - s.age / 18.0f, 0.0f, 1.0f) : 0.0f;
		const float alpha = look.splatAlpha * fade;

		// The stain's blobs on screen, and the box around them.
		struct ScreenBlob {
			Point c;
			float rx, ry, depth, phase;
		};
		std::array<ScreenBlob, 6> sb {};
		int count = 0;
		int minX = INT32_MAX, minY = INT32_MAX, maxX = INT32_MIN, maxY = INT32_MIN;
		for (int b = 0; b < s.blobs; b++) {
			const Blob &blob = s.blob[b];
			const std::optional<OrbitScreenPoint> c = pr.Project(s.x + blob.dx * grow, s.y + blob.dy * grow, 0.0f);
			if (!c)
				continue;
			const float rx = std::max(0.8f, blob.r * grow * c->pixelsPerTile * 0.9f);
			const float ry = rx * (pr.orbit ? 0.6f : 0.5f);
			sb[count++] = { c->pos, rx, ry, c->depth, (s.x * 12.9898f + s.y * 78.233f + static_cast<float>(b) * 3.7f) };
			minX = std::min(minX, c->pos.x - static_cast<int>(rx * 1.3f) - 1);
			maxX = std::max(maxX, c->pos.x + static_cast<int>(rx * 1.3f) + 1);
			minY = std::min(minY, c->pos.y - static_cast<int>(ry * 1.3f) - 1);
			maxY = std::max(maxY, c->pos.y + static_cast<int>(ry * 1.3f) + 1);
		}
		if (count == 0 || maxX - minX > 400 || maxY - minY > 300)
			continue;
		const int w = maxX - minX + 1, h = maxY - minY + 1;
		cover.assign(static_cast<size_t>(w) * h, 0.0f);
		rim.assign(static_cast<size_t>(w) * h, 0.0f);
		shine.assign(static_cast<size_t>(w) * h, 0.0f);
		float depth = sb[0].depth;
		// One coverage for the whole stain (blobs that overlap do not stain twice), with ragged edges.
		for (int i = 0; i < count; i++) {
			const ScreenBlob &bl = sb[i];
			depth = std::min(depth, bl.depth);
			for (int y = std::max(minY, bl.c.y - static_cast<int>(bl.ry * 1.3f) - 1); y <= std::min(maxY, bl.c.y + static_cast<int>(bl.ry * 1.3f) + 1); y++) {
				for (int x = std::max(minX, bl.c.x - static_cast<int>(bl.rx * 1.3f) - 1); x <= std::min(maxX, bl.c.x + static_cast<int>(bl.rx * 1.3f) + 1); x++) {
					const float fx = static_cast<float>(x - bl.c.x) / bl.rx, fy = static_cast<float>(y - bl.c.y) / bl.ry;
					const float ang = std::atan2(fy, fx);
					const float ragged = 1.0f + 0.16f * std::sin(3.0f * ang + bl.phase) + 0.09f * std::sin(7.0f * ang + bl.phase * 1.7f) + 0.05f * std::sin(13.0f * ang + bl.phase * 2.3f);
					const float d = std::sqrt(fx * fx + fy * fy) / ragged;
					if (d >= 1.0f)
						continue;
					const size_t k = static_cast<size_t>(y - minY) * w + (x - minX);
					// Liquid has a clean edge; dust thins out.
					const float c = look.liquid ? std::clamp((1.0f - d) * 5.0f, 0.0f, 1.0f) : std::clamp((1.0f - d) * 2.0f, 0.0f, 1.0f);
					if (c > cover[k]) {
						cover[k] = c;
						// Drying blood pulls to its edge: a darker ring.
						rim[k] = look.liquid ? std::clamp((d - 0.7f) / 0.3f, 0.0f, 1.0f) * (0.4f + 0.6f * dry) : 0.0f;
					}
					if (i == 0 && look.liquid) {
						// A soft highlight up and to the left, where the light catches the wet surface.
						const float hx = fx + 0.32f, hy = fy + 0.42f;
						shine[k] = std::max(shine[k], std::clamp(1.0f - (hx * hx / 0.10f + hy * hy / 0.05f), 0.0f, 1.0f));
					}
				}
			}
		}
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				const size_t k = static_cast<size_t>(y) * w + x;
				const float c = cover[k] * alpha;
				if (c <= 0.01f)
					continue;
				const Point p { minX + x, minY + y };
				if (!pr.Visible(p, depth - 0.2f))
					continue;
				// Soak in: darken and tint the floor (its texture shows through), deeper at the rim.
				float m[3];
				for (int ch = 0; ch < 3; ch++) {
					const float t = tint.fresh[ch] + (tint.dried[ch] - tint.fresh[ch]) * dry;
					m[ch] = 1.0f - c * (1.0f - t * (1.0f - 0.35f * rim[k]));
				}
				ColorOverlayDecalMultiply(p.x, p.y, m[0], m[1], m[2]);
				// A little colour on top so stains still read on dark stone.
				ColorOverlayDecal(p.x, p.y, Dim(look.splat, light * (1.1f - 0.4f * dry)), c * (look.liquid ? 0.22f : 0.55f));
				if (wet > 0 && shine[k] > 0)
					ColorOverlayDecalGloss(p.x, p.y, { 255, 225, 215 }, shine[k] * wet * c * light * 0.55f);
			}
		}
	}
}

void DrawDrops(const WorldProjector &pr)
{
	for (const Drop &d : drops) {
		const std::optional<OrbitScreenPoint> sp = pr.Project(d.x, d.y, d.z);
		if (!sp)
			continue;
		const GoreLook &look = LookOf(d.kind);
		const float light = std::max(0.35f, LightAtTile(d.x, d.y));
		const OverlayColor color = Dim(d.chunk ? look.chunk : look.drop, light);
		// Capped so droplets right in front of the 3D camera do not become big discs.
		const float r = std::clamp(d.size * sp->pixelsPerTile, 0.7f, d.chunk ? 4.0f : 2.2f);
		const int ir = static_cast<int>(std::ceil(r));
		for (int dy = -ir; dy <= ir; dy++) {
			for (int dx = -ir; dx <= ir; dx++) {
				const float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy));
				if (dist > r + 0.5f)
					continue;
				const Point p { sp->pos.x + dx, sp->pos.y + dy };
				if (!pr.Visible(p, sp->depth))
					continue;
				ColorOverlayOver(p.x, p.y, color, std::clamp(r + 0.5f - dist, 0.0f, 1.0f) * 0.95f);
			}
		}
		// A short trail behind fast droplets.
		if (!d.chunk) {
			const std::optional<OrbitScreenPoint> tail = pr.Project(d.x - d.vx * 0.02f, d.y - d.vy * 0.02f, d.z - d.vz * 0.02f);
			if (tail && pr.Visible(tail->pos, tail->depth))
				ColorOverlayOver(tail->pos.x, tail->pos.y, color, 0.5f);
		}
	}
}

void AddGlow(const WorldProjector &pr, Point c, float depth, float radius, OverlayColor color, float amount)
{
	const int r = static_cast<int>(std::ceil(radius));
	for (int dy = -r; dy <= r; dy++) {
		for (int dx = -r; dx <= r; dx++) {
			const float d = std::sqrt(static_cast<float>(dx * dx + dy * dy)) / radius;
			if (d >= 1.0f)
				continue;
			const Point p { c.x + dx, c.y + dy };
			if (pr.Visible(p, depth))
				ColorOverlayAdd(p.x, p.y, color, amount * (1.0f - d) * (1.0f - d));
		}
	}
}

void LightUp(const WorldProjector &pr, Point c, float radius, float r, float g, float b, float strength)
{
	const int ir = static_cast<int>(std::ceil(radius));
	const float squash = pr.orbit ? 1.0f : 2.0f;
	const int iy = static_cast<int>(std::ceil(radius / squash));
	for (int dy = -iy; dy <= iy; dy++) {
		for (int dx = -ir; dx <= ir; dx++) {
			const float fy = dy * squash;
			const float d = std::sqrt(static_cast<float>(dx * dx) + fy * fy) / radius;
			if (d >= 1.0f)
				continue;
			const Point p { c.x + dx, c.y + dy };
			if (p.x < 0 || p.y < 0 || p.x >= pr.out.w() || p.y >= pr.out.h())
				continue;
			const float k = strength * (1.0f - d) * (1.0f - d);
			ColorOverlayMulLight(p.x, p.y, 1.0f + k * (r - 1.0f), 1.0f + k * (g - 1.0f), 1.0f + k * (b - 1.0f));
		}
	}
}

/** A pillar of golden light over every legendary item lying on the ground. */
void DrawLegendaryBeams(const WorldProjector &pr, float dt, uint32_t now)
{
	const OverlayColor gold { 255, 200, 90 };
	const OverlayColor core { 255, 245, 200 };
	for (int i = 0; i < ActiveItemCount; i++) {
		const Item &item = Items[ActiveItems[i]];
		if (item._iMagical != ITEM_QUALITY_UNIQUE)
			continue;
		const float x = item.position.x + 0.5f, y = item.position.y + 0.5f;
		if (LightAtTile(x, y) <= 0.0f && leveltype != DTYPE_TOWN)
			continue;
		const std::optional<OrbitScreenPoint> base = pr.Project(x, y, 0.0f);
		const std::optional<OrbitScreenPoint> top = pr.Project(x, y, 4.5f);
		if (!base || !top)
			continue;
		const float pulse = 0.8f + 0.2f * std::sin(static_cast<float>(now) * 0.006f + i);
		const float halfWidth = std::max(3.0f, base->pixelsPerTile * 0.24f);
		const int steps = std::max(1, std::abs(base->pos.y - top->pos.y));
		for (int s = 0; s <= steps; s++) {
			const float t = static_cast<float>(s) / static_cast<float>(steps);
			const int cx = base->pos.x + static_cast<int>((top->pos.x - base->pos.x) * t);
			const int cy = base->pos.y + static_cast<int>((top->pos.y - base->pos.y) * t);
			const float fadeUp = std::pow(1.0f - t, 1.4f);
			const float w = halfWidth * (1.0f - 0.55f * t);
			const int iw = static_cast<int>(std::ceil(w)) + 2;
			for (int dx = -iw; dx <= iw; dx++) {
				const float d = std::fabs(static_cast<float>(dx)) / (w + 1.5f);
				if (d >= 1.0f)
					continue;
				const Point p { cx + dx, cy };
				if (!pr.Visible(p, base->depth - 0.3f))
					continue;
				const float fall = (1.0f - d) * (1.0f - d);
				ColorOverlayAdd(p.x, p.y, d < 0.25f ? core : gold, 0.85f * fall * fadeUp * pulse);
			}
		}
		LightUp(pr, base->pos, base->pixelsPerTile * 1.4f, 1.5f, 1.3f, 0.85f, 0.7f * pulse);
		AddGlow(pr, base->pos, base->depth - 0.3f, base->pixelsPerTile * 0.35f, gold, 0.5f * pulse);
		// Sparkles rising up the beam.
		beamEmit += 10.0f * dt;
		while (beamEmit >= 1.0f && motes.size() < 400) {
			beamEmit -= 1.0f;
			motes.push_back({ x + RandRange(-0.12f, 0.12f), y + RandRange(-0.12f, 0.12f), RandRange(0.0f, 0.5f), RandRange(0.6f, 1.3f), 0, 0 });
			motes.back().maxLife = motes.back().life = RandRange(1.0f, 2.0f);
		}
	}
	for (const Mote &m : motes) {
		const std::optional<OrbitScreenPoint> sp = pr.Project(m.x, m.y, m.z);
		if (!sp)
			continue;
		const float k = std::min(1.0f, m.life / (m.maxLife * 0.4f));
		AddGlow(pr, sp->pos, sp->depth, std::max(1.5f, sp->pixelsPerTile * 0.05f), core, 0.8f * k);
	}
}

} // namespace

void ClearGore()
{
	drops.clear();
	splats.clear();
	flashes.clear();
	motes.clear();
	known.fill(false);
	lastPlayerHp = -1;
}

namespace {

void LegendaryBurst(float x, float y, GoreKind kind)
{
	// The whole body goes: a fountain in every direction, lots of chunks, a big pool.
	Spray(x, y, 0.7f, 0, 0, kind, 220, 1.5f, 5.5f, 30, 3.14f);
	Spray(x, y, 0.4f, 0, 0, kind, 60, 0.3f, 1.4f, 0, 3.14f);
	if (LookOf(kind).liquid)
		AddSplat(x, y, 1.0f, kind, true);
	for (int i = 0; i < 10; i++) {
		const float a = RandRange(0.0f, 6.2832f), d = RandRange(0.5f, 1.6f);
		AddSplat(x + std::cos(a) * d, y + std::sin(a) * d, RandRange(0.1f, 0.22f), kind, false);
	}
	flashes.push_back({ x, y, 0.0f, 0.7f });
}

} // namespace

void GoreLegendaryDrop(const Monster &monster)
{
	if (!*GetOptions().Graphics.gore)
		return;
	float x, y;
	MonsterWorldPosition(monster, x, y);
	LegendaryBurst(x, y, KindOf(monster));
}

void DrawGore(const Surface &out)
{
	const uint32_t now = SDL_GetTicks();
	float dt = std::min(0.1f, static_cast<float>(now - lastTicks) / 1000.0f);
	lastTicks = now;
#ifdef _DEBUG
	if (std::getenv("DEVILUTIONX_DEV_CAPTURE_FRAMES") != nullptr)
		dt = 1.0f / 30.0f;
#endif
	if (!ColorOverlayRecording() || MyPlayer == nullptr) {
		return;
	}
	const int key = static_cast<int>(currlevel) + (static_cast<int>(leveltype) << 8) + (setlevel ? 1 << 16 : 0);
	if (key != levelKey) {
		levelKey = key;
		ClearGore();
	}
	if (PauseMode != 0)
		dt = 0;
#ifdef _DEBUG
	// Test hook: DEVILUTIONX_DEV_LEGENDARY=<frame> drops a unique next to the nearest monster then.
	if (const char *at = std::getenv("DEVILUTIONX_DEV_LEGENDARY"); at != nullptr) {
		static int frame = 0;
		if (++frame == std::atoi(at)) {
			const Point at2 = Point(MyPlayer->position.tile) + Displacement { 2, 1 };
			SpawnUnique(static_cast<_unique_items>(0), at2, std::nullopt, false);
			LegendaryBurst(at2.x + 0.5f, at2.y + 0.5f, GoreKind::Blood);
			LogInfo("LEGENDARY test at {},{}", at2.x, at2.y);
		}
	}
#endif
	const bool gore = *GetOptions().Graphics.gore;
	if (gore)
		TrackMonsters();
	else
		ClearGore();
	Simulate(dt);
	const WorldProjector pr(out);
	if (gore) {
		DrawSplats(pr);
		DrawDrops(pr);
	}
	for (const Flash &f : flashes) {
		const std::optional<OrbitScreenPoint> c = pr.Project(f.x, f.y, 0.6f);
		const std::optional<OrbitScreenPoint> g = pr.Project(f.x, f.y, 0.0f);
		if (!c || !g)
			continue;
		const float k = 1.0f - f.age / f.duration;
		LightUp(pr, g->pos, g->pixelsPerTile * 3.2f, 1.6f, 1.25f, 0.8f, 1.0f * k * k);
		AddGlow(pr, c->pos, c->depth, c->pixelsPerTile * (0.4f + 0.9f * (1.0f - k)), { 255, 190, 90 }, 0.7f * k * k);
	}
	if (*GetOptions().Graphics.legendaryBeams)
		DrawLegendaryBeams(pr, dt, now);
	else
		motes.clear();
}

} // namespace devilution
