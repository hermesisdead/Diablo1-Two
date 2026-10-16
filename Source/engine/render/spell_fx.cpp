#include "engine/render/spell_fx.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include "diablo.h"
#include "free_move.hpp"
#include "items.h"
#include "player.h"
#include "tables/itemdat.h"
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
	// Armour auras only.
	Shadow,
	Silver,
	Amber,
	Rainbow,
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
	static const Palette3 shadow { { 60, 30, 80 }, { 30, 10, 45 }, { 10, 0, 15 }, { 40, 15, 60 }, { 0.75f, 0.70f, 0.85f } };
	static const Palette3 silver { { 255, 255, 255 }, { 210, 225, 240 }, { 120, 140, 170 }, { 200, 215, 235 }, { 1.30f, 1.32f, 1.38f } };
	static const Palette3 amber { { 255, 235, 180 }, { 230, 170, 60 }, { 140, 80, 20 }, { 230, 170, 70 }, { 1.35f, 1.20f, 0.85f } };
	static Palette3 rainbow;
	switch (style) {
	case Style::Shadow: return shadow;
	case Style::Silver: return silver;
	case Style::Amber: return amber;
	case Style::Rainbow: {
		// Hue turns slowly through the colours.
		const float h = static_cast<float>(SDL_GetTicks() % 6000) / 6000.0f * 6.0f;
		auto hue = [h](float offset) {
			const float x = std::fmod(h + offset, 6.0f);
			auto ch = [x](float c) { const float d = std::fabs(std::fmod(x - c + 9.0f, 6.0f) - 3.0f); return static_cast<uint8_t>(std::clamp(2.0f - d, 0.0f, 1.0f) * 200.0f + 55.0f); };
			return OverlayColor { ch(0), ch(2), ch(4) };
		};
		const OverlayColor a = hue(0), b = hue(1.5f);
		rainbow = { { 255, 255, 255 }, a, b, a, { 0.8f + a.r / 255.0f * 0.6f, 0.8f + a.g / 255.0f * 0.6f, 0.8f + a.b / 255.0f * 0.6f } };
		return rainbow;
	}
	default: break;
	}
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

/** A soft dark disc (shadowy wisps): covers instead of adding light. */
void ShadowDisc(const WorldProjector &pr, Point c, float depth, float radius, OverlayColor color, float alpha)
{
	const int r = static_cast<int>(std::ceil(radius));
	if (r <= 0 || alpha <= 0.005f)
		return;
	const float inv = 1.0f / radius;
	for (int dy = -r; dy <= r; dy++) {
		for (int dx = -r; dx <= r; dx++) {
			const float d = std::sqrt(static_cast<float>(dx * dx + dy * dy)) * inv;
			if (d >= 1.0f)
				continue;
			const Point p { c.x + dx, c.y + dy };
			if (pr.Visible(p, depth))
				ColorOverlayOver(p.x, p.y, color, alpha * (1.0f - d) * (1.0f - d));
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
		if (s.style == Style::Shadow)
			ShadowDisc(pr, sp->pos, sp->depth, px * 2.2f + 1.0f, color, 0.5f * s.bright * fade);
		else
			GlowDisc(pr, sp->pos, sp->depth, px * 1.8f + 1.0f, color, s.bright * fade);
	}
}

// ---------------------------------------------------------------------------------------------
// Armour auras

struct AuraLook {
	Style style;
	float strength; // 0..1
};

/** The aura a hero's body armour gives, if any. */
std::optional<AuraLook> AuraOf(const Player &player)
{
	const Item &armor = player.InvBody[INVLOC_CHEST];
	if (armor.isEmpty())
		return std::nullopt;
	if (armor._iMagical == ITEM_QUALITY_UNIQUE && armor._iUid >= 0 && static_cast<size_t>(armor._iUid) < UniqueItems.size()) {
		const std::string_view name = UniqueItems[armor._iUid].UIName;
		if (name == "Demonspike Coat") return AuraLook { Style::Fire, 1.0f };
		if (name == "Sparking Mail") return AuraLook { Style::Lightning, 1.0f };
		if (name == "Naj's Light Plate") return AuraLook { Style::Holy, 0.9f };
		if (name == "The Rainbow Cloak") return AuraLook { Style::Rainbow, 0.9f };
		if (name == "Wisdom's Wrap") return AuraLook { Style::Arcane, 0.8f };
		if (name == "Nightscape") return AuraLook { Style::Shadow, 1.0f };
		if (name == "Torn Flesh of Souls") return AuraLook { Style::Blood, 0.9f };
		if (name == "Scavenger Carapace") return AuraLook { Style::Acid, 0.7f };
		if (name == "Leather of Aut") return AuraLook { Style::Amber, 0.7f };
		if (name == "The Gladiator's Bane") return AuraLook { Style::Blood, 0.5f };
		return AuraLook { Style::Amber, 0.6f }; // other uniques: a golden sheen
	}
	if (armor._iMagical != ITEM_QUALITY_MAGIC && armor._iMagical != ITEM_QUALITY_RARE)
		return std::nullopt;
	// Magic and rare armour glows in the colour of its strongest resistance.
	const int fire = armor._iPLFR, light = armor._iPLLR, magic = armor._iPLMR;
	const int low = std::min({ fire, light, magic });
	if (low >= 15)
		return AuraLook { Style::Silver, std::clamp(low / 50.0f, 0.4f, 1.0f) };
	const int high = std::max({ fire, light, magic });
	if (high < 15)
		return std::nullopt;
	const float strength = std::clamp(high / 60.0f, 0.35f, 1.0f);
	if (high == fire) return AuraLook { Style::Fire, strength };
	if (high == light) return AuraLook { Style::Lightning, strength };
	return AuraLook { Style::Arcane, strength };
}

struct AuraState {
	float emit = 0;
};
std::array<AuraState, MAX_PLRS> auraStates;

void HeroWorldPosition(const Player &player, float &x, float &y)
{
	const Displacement off = GetPlayerDrawOffset(player);
	x = player.position.tile.x + 0.5f + off.deltaX / 64.0f + off.deltaY / 32.0f;
	y = player.position.tile.y + 0.5f + off.deltaY / 32.0f - off.deltaX / 64.0f;
}

void EmitAuraMote(Style style, float x, float y, float strength)
{
	if (sparks.size() >= MaxSparks)
		return;
	Spark s {};
	s.style = style;
	const float a = RandRange(0.0f, 6.2832f);
	const float r = RandRange(0.05f, 0.22f);
	s.x = x + std::cos(a) * r;
	s.y = y + std::sin(a) * r;
	s.z = RandRange(0.1f, 1.1f);
	s.drag = 0.4f;
	s.bright = 0.7f * strength + 0.3f;
	switch (style) {
	case Style::Lightning:
		s.vx = RandRange(-1.5f, 1.5f);
		s.vy = RandRange(-1.5f, 1.5f);
		s.vz = RandRange(-0.5f, 1.5f);
		s.gravity = 1.0f;
		s.maxLife = RandRange(0.08f, 0.2f);
		s.size = 0.03f;
		s.streak = true;
		break;
	case Style::Shadow:
		s.vx = RandRange(-0.15f, 0.15f);
		s.vy = RandRange(-0.15f, 0.15f);
		s.vz = RandRange(0.25f, 0.55f);
		s.gravity = 0;
		s.maxLife = RandRange(0.8f, 1.4f);
		s.size = RandRange(0.06f, 0.1f);
		break;
	default:
		s.vx = RandRange(-0.12f, 0.12f);
		s.vy = RandRange(-0.12f, 0.12f);
		s.vz = RandRange(0.3f, 0.8f);
		s.gravity = style == Style::Fire ? -0.4f : -0.1f;
		s.maxLife = RandRange(0.6f, 1.2f);
		s.size = RandRange(0.035f, 0.06f);
		break;
	}
	s.life = s.maxLife;
	sparks.push_back(s);
}

void DrawArmorAuras(const WorldProjector &pr, float dt, uint32_t now)
{
#ifdef _DEBUG
	// Test hook: DEVILUTIONX_DEV_ARMOR = a unique armour's name, or fire/light/magic/all with a value.
	if (const char *dev = std::getenv("DEVILUTIONX_DEV_ARMOR"); dev != nullptr && MyPlayer != nullptr) {
		Item &chest = MyPlayer->InvBody[INVLOC_CHEST];
		const std::string_view want = dev;
		chest._itype = ItemType::HeavyArmor;
		chest._iMagical = ITEM_QUALITY_MAGIC;
		chest._iPLFR = chest._iPLLR = chest._iPLMR = 0;
		if (want == "fire") chest._iPLFR = 40;
		else if (want == "light") chest._iPLLR = 40;
		else if (want == "magic") chest._iPLMR = 40;
		else if (want == "all") chest._iPLFR = chest._iPLLR = chest._iPLMR = 30;
		else {
			for (size_t u = 0; u < UniqueItems.size(); u++) {
				if (UniqueItems[u].UIName == want) {
					chest._iMagical = ITEM_QUALITY_UNIQUE;
					chest._iUid = static_cast<int>(u);
				}
			}
		}
	}
#endif
	for (size_t i = 0; i < Players.size() && i < auraStates.size(); i++) {
		Player &player = Players[i];
		if (!player.plractive || !player.isOnActiveLevel() || player._pHitPoints <= 0)
			continue;
		const std::optional<AuraLook> look = AuraOf(player);
		if (!look)
			continue;
		float x, y;
		HeroWorldPosition(player, x, y);
		const Palette3 &pal = PaletteOf(look->style);
		const float pulse = 0.8f + 0.2f * std::sin(static_cast<float>(now) * 0.004f + static_cast<float>(i));
		const float k = look->strength * pulse;
		// Motes rising off the body.
		AuraState &state = auraStates[i];
		state.emit += (look->style == Style::Lightning ? 16.0f : 13.0f) * look->strength * dt;
		while (state.emit >= 1.0f) {
			state.emit -= 1.0f;
			EmitAuraMote(look->style, x, y, look->strength);
		}
		const std::optional<OrbitScreenPoint> body = pr.Project(x, y, 0.75f);
		const std::optional<OrbitScreenPoint> feet = pr.Project(x, y, 0.0f);
		if (!body || !feet)
			continue;
		const float tile = body->pixelsPerTile;
		if (look->style == Style::Shadow) {
			// A dark haze that swallows the light around the hero.
			LightPool(pr, feet->pos, tile * 1.6f, pal.light, 0.8f * k);
			continue;
		}
		LightPool(pr, feet->pos, tile * 1.7f, pal.light, 0.75f * k);
		// A soft upright glow over the body.
		const float rx = tile * 0.42f, ry = tile * 0.85f;
		const int ix = static_cast<int>(std::ceil(rx)), iy = static_cast<int>(std::ceil(ry));
		for (int dy = -iy; dy <= iy; dy++) {
			for (int dx = -ix; dx <= ix; dx++) {
				const float fx = dx / rx, fy = dy / ry;
				const float d = std::sqrt(fx * fx + fy * fy);
				if (d >= 1.0f)
					continue;
				const Point p { body->pos.x + dx, body->pos.y + dy };
				if (!pr.Visible(p, body->depth - 0.6f))
					continue;
				ColorOverlayAdd(p.x, p.y, pal.glow, 0.34f * k * (1.0f - d));
			}
		}
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
	if (*GetOptions().Graphics.armorGlows)
		DrawArmorAuras(pr, dt, now);
	DrawSparks(pr);
}

} // namespace devilution
