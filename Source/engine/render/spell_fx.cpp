#include "engine/render/spell_fx.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <unordered_map>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include "diablo.h"
#include "engine/render/color_overlay.hpp"
#include "engine/render/world_projector.hpp"
#include "levels/gendung.h"
#include "missiles.h"
#include "options.h"
#include "tables/misdat.h"

namespace devilution {

namespace {

enum class Style : uint8_t {
	None,
	Fire,
	Lightning,
	Arcane,
	Holy,
	Blood,
	Acid,
	Portal,
};

struct Palette3 {
	OverlayColor hot, mid, cool; // colour over a particle's life: start, middle, end
	OverlayColor glow;           // halo and light colour
	float light[3];              // multiplier for the light cast on the scene
};

const Palette3 &PaletteOf(Style style)
{
	static const Palette3 fire { { 255, 240, 170 }, { 255, 150, 40 }, { 170, 40, 10 }, { 255, 130, 40 }, { 1.55f, 1.15f, 0.70f } };
	static const Palette3 lightning { { 255, 255, 255 }, { 170, 210, 255 }, { 60, 90, 255 }, { 120, 170, 255 }, { 1.05f, 1.25f, 1.65f } };
	static const Palette3 arcane { { 250, 220, 255 }, { 190, 120, 255 }, { 90, 40, 200 }, { 170, 110, 255 }, { 1.30f, 1.05f, 1.60f } };
	static const Palette3 holy { { 255, 255, 235 }, { 255, 225, 140 }, { 200, 150, 60 }, { 255, 230, 160 }, { 1.50f, 1.40f, 1.10f } };
	static const Palette3 blood { { 255, 190, 170 }, { 230, 40, 30 }, { 110, 0, 0 }, { 230, 40, 30 }, { 1.55f, 0.85f, 0.80f } };
	static const Palette3 acid { { 220, 255, 170 }, { 110, 220, 50 }, { 40, 110, 20 }, { 110, 230, 60 }, { 1.05f, 1.50f, 0.85f } };
	static const Palette3 portal { { 230, 245, 255 }, { 110, 170, 255 }, { 40, 70, 210 }, { 100, 160, 255 }, { 0.95f, 1.20f, 1.65f } };
	switch (style) {
	case Style::Fire: return fire;
	case Style::Lightning: return lightning;
	case Style::Arcane: return arcane;
	case Style::Holy: return holy;
	case Style::Blood: return blood;
	case Style::Acid: return acid;
	default: return portal;
	}
}

Style StyleOf(const Missile &missile)
{
	switch (missile._mitype) {
	case MissileID::TownPortal: return Style::Portal;
	case MissileID::RedPortal: return Style::Blood;
	case MissileID::HolyBolt: return Style::Holy;
	case MissileID::BloodStar: return Style::Blood;
	default: break;
	}
	switch (GetMissileData(missile._mitype).damageType()) {
	case DamageType::Fire: return Style::Fire;
	case DamageType::Lightning: return Style::Lightning;
	case DamageType::Magic: return Style::Arcane;
	case DamageType::Acid: return Style::Acid;
	default: return Style::None;
	}
}

struct Spark {
	float x, y, z;
	float vx, vy, vz;
	float life, maxLife;
	float size;    // in tiles
	float gravity; // tiles / s^2 (negative rises)
	float drag;    // fraction of speed kept per second
	Style style;
	bool streak;  // drawn as a short line along its motion (fast sparks)
	float bright; // glow strength
};

struct Tracked {
	MissileID type;
	Style style;
	float x, y, z;
	float dirX, dirY; // direction of travel (0 when stationary)
	int frames;
	float emit; // particles owed (fractional)
	bool seen;
};

struct Flash {
	float x, y, z;
	float age, duration;
	float radius; // tiles
	Style style;
};

constexpr size_t MaxSparks = 2400;

std::vector<Spark> sparks;
std::vector<Flash> flashes;
std::unordered_map<const Missile *, Tracked> tracked;
uint64_t rng = 0x9E3779B97F4A7C15ULL;
uint32_t lastTicks = 0;
int levelKey = -1;

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

OverlayColor Mix(OverlayColor a, OverlayColor b, float t)
{
	auto m = [t](uint8_t x, uint8_t y) { return static_cast<uint8_t>(x + (y - x) * t); };
	return { m(a.r, b.r), m(a.g, b.g), m(a.b, b.b) };
}

/** Colour of a particle at a fraction of its life (0 = born, 1 = gone). */
OverlayColor ColorAt(const Palette3 &p, float t)
{
	return t < 0.4f ? Mix(p.hot, p.mid, t / 0.4f) : Mix(p.mid, p.cool, (t - 0.4f) / 0.6f);
}

/** World position of a missile's glowing core. */
void MissileWorldPosition(const Missile &missile, bool orbit, float &x, float &y, float &z, Style style)
{
	const WorldTilePosition tile = orbit ? missile.position.tile : missile.position.tileForRendering;
	const Displacement off = orbit ? missile.position.offset : missile.position.offsetForRendering;
	x = tile.x + 0.5f + off.deltaX / 64.0f + off.deltaY / 32.0f;
	y = tile.y + 0.5f + off.deltaY / 32.0f - off.deltaX / 64.0f;
	// The bright part of a spell sprite sits about half a tile above the ground (lightning higher).
	z = style == Style::Lightning ? 0.95f : (style == Style::Portal ? 0.9f : 0.55f);
}

void Emit(const Tracked &t, bool stationary)
{
	if (sparks.size() >= MaxSparks)
		return;
	Spark s {};
	s.style = t.style;
	s.x = t.x + RandRange(-0.08f, 0.08f);
	s.y = t.y + RandRange(-0.08f, 0.08f);
	s.z = t.z + RandRange(-0.06f, 0.06f);
	s.drag = 0.25f;
	// Embers from walls of fire land on bright flame sprites: keep them dimmer.
	s.bright = stationary && t.style == Style::Fire ? 0.35f : 0.85f;
	switch (t.style) {
	case Style::Fire:
		// Embers fall behind and rise as they cool.
		s.vx = -t.dirX * RandRange(0.4f, 1.2f) + RandRange(-0.35f, 0.35f);
		s.vy = -t.dirY * RandRange(0.4f, 1.2f) + RandRange(-0.35f, 0.35f);
		s.vz = RandRange(0.2f, 0.9f);
		s.gravity = -0.6f;
		s.maxLife = RandRange(0.35f, stationary ? 1.0f : 0.7f);
		s.size = RandRange(0.035f, 0.07f);
		if (stationary)
			s.z = RandRange(0.05f, 0.6f);
		break;
	case Style::Lightning: {
		// Crackling sparks flying off in every direction.
		const float a = RandRange(0.0f, 6.2832f);
		const float speed = RandRange(1.5f, 3.6f);
		s.vx = std::cos(a) * speed;
		s.vy = std::sin(a) * speed;
		s.vz = RandRange(-1.5f, 2.0f);
		s.gravity = 3.0f;
		s.drag = 0.05f;
		s.maxLife = RandRange(0.10f, 0.26f);
		s.size = RandRange(0.025f, 0.045f);
		s.streak = true;
		break;
	}
	case Style::Acid:
		// Drips that fall and splash.
		s.vx = RandRange(-0.3f, 0.3f) - t.dirX * 0.3f;
		s.vy = RandRange(-0.3f, 0.3f) - t.dirY * 0.3f;
		s.vz = RandRange(-0.2f, 0.4f);
		s.gravity = 5.0f;
		s.maxLife = RandRange(0.4f, 0.8f);
		s.size = RandRange(0.035f, 0.06f);
		break;
	case Style::Portal: {
		// Motes rising out of a ring.
		const float a = RandRange(0.0f, 6.2832f);
		const float r = RandRange(0.25f, 0.45f);
		s.x = t.x + std::cos(a) * r;
		s.y = t.y + std::sin(a) * r;
		s.z = RandRange(0.0f, 0.4f);
		s.vx = -std::sin(a) * 0.4f;
		s.vy = std::cos(a) * 0.4f;
		s.vz = RandRange(0.5f, 1.0f);
		s.gravity = -0.2f;
		s.maxLife = RandRange(0.8f, 1.5f);
		s.size = RandRange(0.03f, 0.05f);
		break;
	}
	default: {
		// Magic motes drifting in a lazy swirl around the path.
		const float a = RandRange(0.0f, 6.2832f);
		s.vx = std::cos(a) * RandRange(0.2f, 0.6f) - t.dirX * 0.3f;
		s.vy = std::sin(a) * RandRange(0.2f, 0.6f) - t.dirY * 0.3f;
		s.vz = RandRange(-0.2f, 0.5f);
		s.gravity = -0.1f;
		s.maxLife = RandRange(0.45f, 0.9f);
		s.size = RandRange(0.03f, 0.055f);
		break;
	}
	}
	s.life = s.maxLife;
	sparks.push_back(s);
}

void Burst(const Tracked &t)
{
	const int count = t.style == Style::Lightning ? 18 : 26;
	for (int i = 0; i < count && sparks.size() < MaxSparks; i++) {
		Spark s {};
		s.style = t.style;
		s.x = t.x;
		s.y = t.y;
		s.z = t.z;
		const float a = RandRange(0.0f, 6.2832f);
		const float speed = RandRange(0.8f, t.style == Style::Lightning ? 4.0f : 2.4f);
		s.vx = std::cos(a) * speed;
		s.vy = std::sin(a) * speed;
		s.vz = RandRange(0.0f, 2.2f);
		s.gravity = t.style == Style::Fire ? 1.5f : 3.5f;
		s.drag = 0.08f;
		s.maxLife = RandRange(0.25f, 0.6f);
		s.size = RandRange(0.03f, 0.06f);
		s.streak = true;
		s.bright = 0.85f;
		s.life = s.maxLife;
		sparks.push_back(s);
	}
	flashes.push_back({ t.x, t.y, t.z, 0.0f, 0.28f, t.style == Style::Fire ? 2.4f : 1.9f, t.style });
}

void UpdateTracked(bool orbit, float dt)
{
	for (auto &[ptr, t] : tracked)
		t.seen = false;
	for (const Missile &missile : Missiles) {
		if (!missile._miDrawFlag)
			continue;
		const Style style = StyleOf(missile);
		if (style == Style::None)
			continue;
		float x, y, z;
		MissileWorldPosition(missile, orbit, x, y, z, style);
		auto it = tracked.find(&missile);
		if (it != tracked.end() && it->second.type != missile._mitype) {
			// The slot was reused by a different missile: the old one ended.
			Burst(it->second);
			tracked.erase(it);
			it = tracked.end();
		}
		if (it == tracked.end())
			it = tracked.emplace(&missile, Tracked { missile._mitype, style, x, y, z, 0, 0, 0, 0, true }).first;
		Tracked &t = it->second;
		// Missiles the game does not move (walls, explosions) are stationary whatever their velocity says.
		const bool fixed = GetMissileData(missile._mitype).movementDistribution == MissileMovementDistribution::Disabled;
		const Displacement v = fixed ? Displacement {} : missile.position.velocity;
		const float wvx = v.deltaX / 64.0f + v.deltaY / 32.0f, wvy = v.deltaY / 32.0f - v.deltaX / 64.0f;
		const float speed = std::sqrt(wvx * wvx + wvy * wvy);
		t.dirX = speed > 0 ? wvx / speed : 0;
		t.dirY = speed > 0 ? wvy / speed : 0;
		t.x = x;
		t.y = y;
		t.z = z;
		t.seen = true;
		t.frames++;
		const bool stationary = speed <= 0;
		float rate;
		switch (style) {
		case Style::Fire: rate = stationary ? 22.0f : 85.0f; break;
		case Style::Lightning: rate = stationary ? 30.0f : 60.0f; break;
		case Style::Portal: rate = 26.0f; break;
		case Style::Acid: rate = stationary ? 10.0f : 35.0f; break;
		default: rate = stationary ? 14.0f : 45.0f; break;
		}
		t.emit += rate * dt;
		while (t.emit >= 1.0f) {
			t.emit -= 1.0f;
			Emit(t, stationary);
		}
	}
	// Spells that are gone burst where they ended (moving projectiles only; walls just fade).
	for (auto it = tracked.begin(); it != tracked.end();) {
		if (it->second.seen) {
			++it;
			continue;
		}
		const Tracked &t = it->second;
		if (t.frames > 2 && (t.dirX != 0 || t.dirY != 0) && t.style != Style::Portal)
			Burst(t);
		it = tracked.erase(it);
	}
}

void Simulate(float dt)
{
	for (Spark &s : sparks) {
		const float keep = std::pow(s.drag, dt);
		s.vx *= keep;
		s.vy *= keep;
		s.vz = s.vz * keep - s.gravity * dt;
		s.x += s.vx * dt;
		s.y += s.vy * dt;
		s.z += s.vz * dt;
		if (s.z < 0.02f) {
			s.z = 0.02f;
			s.vz = -s.vz * 0.3f;
			s.vx *= 0.6f;
			s.vy *= 0.6f;
		}
		s.life -= dt;
	}
	std::erase_if(sparks, [](const Spark &s) { return s.life <= 0; });
	for (Flash &f : flashes)
		f.age += dt;
	std::erase_if(flashes, [](const Flash &f) { return f.age >= f.duration; });
}

/** A soft additive glow disc. */
void GlowDisc(const WorldProjector &pr, Point c, float depth, float radius, OverlayColor color, float amount)
{
	const int r = static_cast<int>(std::ceil(radius));
	if (r <= 0 || amount <= 0.005f)
		return;
	const float inv = 1.0f / radius;
	for (int dy = -r; dy <= r; dy++) {
		for (int dx = -r; dx <= r; dx++) {
			const float d = std::sqrt(static_cast<float>(dx * dx + dy * dy)) * inv;
			if (d >= 1.0f)
				continue;
			const Point p { c.x + dx, c.y + dy };
			if (!pr.Visible(p, depth))
				continue;
			const float fall = (1.0f - d) * (1.0f - d);
			ColorOverlayAdd(p.x, p.y, color, amount * fall);
		}
	}
}

/** Light cast on the scene: brightens and tints what is around a point. */
void LightPool(const WorldProjector &pr, Point c, float radius, const float (&tint)[3], float strength)
{
	const int r = static_cast<int>(std::ceil(radius));
	if (r <= 0 || strength <= 0.01f)
		return;
	const float inv = 1.0f / radius;
	// Squashed vertically: the pool lies on the floor of an isometric view.
	const float squash = pr.orbit ? 1.0f : 2.0f;
	const int ry = static_cast<int>(std::ceil(radius / squash));
	for (int dy = -ry; dy <= ry; dy++) {
		for (int dx = -r; dx <= r; dx++) {
			const float fy = dy * squash;
			const float d = std::sqrt(static_cast<float>(dx * dx) + fy * fy) * inv;
			if (d >= 1.0f)
				continue;
			const Point p { c.x + dx, c.y + dy };
			if (p.x < 0 || p.y < 0 || p.x >= pr.out.w() || p.y >= pr.out.h())
				continue;
			const float k = strength * (1.0f - d) * (1.0f - d);
			ColorOverlayMulLight(p.x, p.y, 1.0f + k * (tint[0] - 1.0f) + k * 0.25f, 1.0f + k * (tint[1] - 1.0f) + k * 0.25f, 1.0f + k * (tint[2] - 1.0f) + k * 0.25f);
		}
	}
}

void DrawSparks(const WorldProjector &pr)
{
	for (const Spark &s : sparks) {
		const std::optional<OrbitScreenPoint> sp = pr.Project(s.x, s.y, s.z);
		if (!sp)
			continue;
		const float age = 1.0f - s.life / s.maxLife;
		const Palette3 &pal = PaletteOf(s.style);
		const OverlayColor color = ColorAt(pal, age);
		const float fade = std::min(1.0f, s.life / (s.maxLife * 0.35f));
		const float px = std::max(0.8f, s.size * sp->pixelsPerTile);
		if (s.streak) {
			const std::optional<OrbitScreenPoint> tail = pr.Project(s.x - s.vx * 0.025f, s.y - s.vy * 0.025f, s.z - s.vz * 0.025f);
			if (tail) {
				const int dx = tail->pos.x - sp->pos.x, dy = tail->pos.y - sp->pos.y;
				const int steps = std::min(24, std::max({ std::abs(dx), std::abs(dy), 1 }));
				for (int i = 0; i <= steps; i++) {
					const Point p { sp->pos.x + dx * i / steps, sp->pos.y + dy * i / steps };
					if (pr.Visible(p, sp->depth))
						ColorOverlayAdd(p.x, p.y, color, fade * (1.0f - 0.7f * i / steps));
				}
			}
		}
		GlowDisc(pr, sp->pos, sp->depth, px * 1.8f + 1.0f, color, s.bright * fade);
	}
}

} // namespace

void ClearSpellEffects()
{
	sparks.clear();
	flashes.clear();
	tracked.clear();
}

void DrawSpellEffects(const Surface &out)
{
	const uint32_t now = SDL_GetTicks();
	float dt = std::min(0.1f, static_cast<float>(now - lastTicks) / 1000.0f);
	lastTicks = now;
#ifdef _DEBUG
	if (std::getenv("DEVILUTIONX_DEV_CAPTURE_FRAMES") != nullptr)
		dt = 1.0f / 30.0f;
#endif
	if (!ColorOverlayRecording() || !*GetOptions().Graphics.spellEffects || MyPlayer == nullptr) {
		ClearSpellEffects();
		return;
	}
	const int key = static_cast<int>(currlevel) + (static_cast<int>(leveltype) << 8) + (setlevel ? 1 << 16 : 0);
	if (key != levelKey) {
		levelKey = key;
		ClearSpellEffects();
	}
	if (PauseMode != 0)
		dt = 0;

	const WorldProjector pr(out);
	UpdateTracked(pr.orbit, dt);
	Simulate(dt);

	// Light from the spells in flight, then their glow.
	const float flicker = 0.85f + 0.15f * std::sin(static_cast<float>(now) * 0.023f);
	for (const auto &[ptr, t] : tracked) {
		const std::optional<OrbitScreenPoint> core = pr.Project(t.x, t.y, t.z);
		const std::optional<OrbitScreenPoint> ground = pr.Project(t.x, t.y, 0.0f);
		if (!core || !ground)
			continue;
		const Palette3 &pal = PaletteOf(t.style);
		const float tile = core->pixelsPerTile;
		// Walls of fire are made of many segments: each one lights less.
		const bool moving = t.dirX != 0 || t.dirY != 0;
		LightPool(pr, ground->pos, tile * 2.2f, pal.light, (moving ? 0.55f : 0.22f) * flicker);
		GlowDisc(pr, core->pos, core->depth, tile * 0.55f, pal.glow, (moving ? 0.55f : 0.18f) * flicker);
		if (moving)
			GlowDisc(pr, core->pos, core->depth, tile * 0.18f, pal.hot, 0.6f);
	}
	for (const Flash &f : flashes) {
		const std::optional<OrbitScreenPoint> core = pr.Project(f.x, f.y, f.z);
		const std::optional<OrbitScreenPoint> ground = pr.Project(f.x, f.y, 0.0f);
		if (!core || !ground)
			continue;
		const float k = 1.0f - f.age / f.duration;
		const Palette3 &pal = PaletteOf(f.style);
		LightPool(pr, ground->pos, ground->pixelsPerTile * f.radius, pal.light, 0.9f * k * k);
		GlowDisc(pr, core->pos, core->depth, core->pixelsPerTile * (0.35f + 0.4f * (1.0f - k)), pal.glow, 0.45f * k * k);
	}
	DrawSparks(pr);
}

} // namespace devilution
