/**
 * @file first_person.cpp
 *
 * Optional PSX-style first-person view.
 *
 * The world is a low resolution ray caster over the level's solid-tile grid (walls, doors and
 * free-standing columns become real geometry). Monsters, objects, items and missiles are
 * billboards that re-use the game's own isometric sprites, picking the frame that matches the
 * angle the viewer sees them from. Everything is rendered with palette indices into a small
 * buffer that is then scaled up with nearest neighbour sampling, with dithered lighting taken
 * from the game's own light map.
 */
#include "engine/render/first_person.hpp"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "controls/control_mode.hpp"
#include "cursor.h"
#include "diablo.h"
#include "engine/backbuffer_state.hpp"
#include "engine/clx_sprite.hpp"
#include "engine/displacement.hpp"
#include "engine/light_tables.hpp"
#include "engine/palette.h"
#include "engine/point.hpp"
#include "engine/render/clx_render.hpp"
#include "engine/render/primitive_render.hpp"
#include "engine/render/scrollrt.h"
#include "engine/render/text_render.hpp"
#include "engine/surface.hpp"
#include "items.h"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "lighting.h"
#include "missiles.h"
#include "monster.h"
#include "multi.h"
#include "objects.h"
#include "options.h"
#include "panels/spell_icons.hpp"
#include "player.h"
#include "spells.h"
#include "stores.h"
#include "towners.h"
#include "tables/spelldat.h"
#include "tables/playerdat.hpp"
#include "automap.h"
#include "control/control.hpp"
#include "DiabloUI/ui_flags.hpp"
#include "utils/algorithm/container.hpp"
#include "utils/language.h"
#include "utils/str_cat.hpp"

namespace devilution {

bool FirstPersonNoPanelsOpen();
void ResetCursorInfo();
bool FirstPersonGameRunning();

namespace {

constexpr float Pi = 3.14159265358979f;
constexpr float FieldOfView = 78.0f * Pi / 180.0f;
constexpr float WallHeight = 1.35f;
constexpr float EyeHeight = 0.62f;
constexpr float SpritePixelsPerUnit = 105.0f;

float Yaw = 0.0f;
bool YawInitialized = false;
uint32_t LastInputTicks = 0;
bool MouseCaptured = false;

// Result of last frame's billboard pass; used by the cursor.
struct TargetInfo {
	enum class Kind { None, Monster, Item, Object, Towner } kind = Kind::None;
	int id = -1;
	Point tile;
};
TargetInfo CurrentTarget;
float CenterWallDepth = 99.0f;
bool DebugFullBright = false;

struct Vec2 {
	float x, y;
};

Vec2 Forward() { return { std::cos(Yaw), std::sin(Yaw) }; }
Vec2 Right() { return { -std::sin(Yaw), std::cos(Yaw) }; }

float WrapAngle(float a)
{
	while (a > Pi)
		a -= 2 * Pi;
	while (a < -Pi)
		a += 2 * Pi;
	return a;
}

/** Direction enum -> grid angle (S = 45 degrees, increasing clockwise on screen). */
float DirectionAngle(Direction d)
{
	return (45.0f + 45.0f * static_cast<int>(d)) * Pi / 180.0f;
}

Direction AngleToDirection(float angle)
{
	int idx = static_cast<int>(std::lround((angle * 180.0f / Pi - 45.0f) / 45.0f));
	idx = ((idx % 8) + 8) % 8;
	return static_cast<Direction>(idx);
}

bool InMap(int x, int y)
{
	return x >= 0 && y >= 0 && x < MAXDUNX && y < MAXDUNY;
}

uint32_t Hash(uint32_t a, uint32_t b = 0, uint32_t c = 0)
{
	uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA6Bu ^ (c + 0x165667B1u) * 0xC2B2AE35u;
	h ^= h >> 15;
	h *= 0x2C1B3C6Du;
	h ^= h >> 12;
	return h;
}

constexpr uint8_t Bayer4[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

uint8_t NearestIndex(int r, int g, int b, int lo = 0, int hi = 255)
{
	int best = lo;
	int bestDist = 1 << 30;
	for (int i = lo; i <= hi; i++) {
		const SDL_Color &c = logical_palette[i];
		const int dr = c.r - r, dg = c.g - g, db = c.b - b;
		const int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
		if (d < bestDist) {
			bestDist = d;
			best = i;
		}
	}
	return static_cast<uint8_t>(best);
}

struct Materials {
	std::array<uint8_t, 4> stone;
	uint8_t mortar;
	std::array<uint8_t, 3> floor;
	uint8_t floorSeam;
	std::array<uint8_t, 3> wood;
	uint8_t iron;
	std::array<uint8_t, 2> ceiling;
	std::array<uint8_t, 3> sky;
};

Materials BuildMaterials()
{
	// Base stone colour per dungeon flavour; light tables later darken these.
	int r = 150, g = 138, b = 120;
	int fr = 110, fg = 100, fb = 88;
	switch (leveltype) {
	case DTYPE_CATACOMBS:
		r = 138, g = 128, b = 108;
		fr = 100, fg = 92, fb = 78;
		break;
	case DTYPE_CAVES:
		r = 140, g = 106, b = 72;
		fr = 112, fg = 84, fb = 58;
		break;
	case DTYPE_HELL:
		r = 160, g = 60, b = 44;
		fr = 120, fg = 44, fb = 34;
		break;
	case DTYPE_NEST:
		r = 110, g = 140, b = 70;
		fr = 80, fg = 108, fb = 54;
		break;
	case DTYPE_CRYPT:
		r = 130, g = 120, b = 126;
		fr = 96, fg = 88, fb = 94;
		break;
	case DTYPE_TOWN:
		r = 150, g = 140, b = 120;
		fr = 96, fg = 120, fb = 70;
		break;
	default:
		break;
	}
	const auto S = [](int base, float f) { return std::clamp(static_cast<int>(base * f), 0, 255); };
	const int hiIdx = leveltype == DTYPE_TOWN ? 255 : 127;
	Materials m;
	m.stone[0] = NearestIndex(S(r, 1.12f), S(g, 1.12f), S(b, 1.12f), 0, hiIdx);
	m.stone[1] = NearestIndex(r, g, b, 0, hiIdx);
	m.stone[2] = NearestIndex(S(r, 0.86f), S(g, 0.86f), S(b, 0.86f), 0, hiIdx);
	m.stone[3] = NearestIndex(S(r, 0.72f), S(g, 0.72f), S(b, 0.72f), 0, hiIdx);
	m.mortar = NearestIndex(S(r, 0.45f), S(g, 0.45f), S(b, 0.45f), 0, hiIdx);
	m.floor[0] = NearestIndex(S(fr, 1.1f), S(fg, 1.1f), S(fb, 1.1f), 0, hiIdx);
	m.floor[1] = NearestIndex(fr, fg, fb, 0, hiIdx);
	m.floor[2] = NearestIndex(S(fr, 0.82f), S(fg, 0.82f), S(fb, 0.82f), 0, hiIdx);
	m.floorSeam = NearestIndex(S(fr, 0.6f), S(fg, 0.6f), S(fb, 0.6f), 0, hiIdx);
	m.wood[0] = NearestIndex(130, 84, 44, 0, 255);
	m.wood[1] = NearestIndex(104, 66, 34, 0, 255);
	m.wood[2] = NearestIndex(78, 48, 24, 0, 255);
	m.iron = NearestIndex(70, 70, 76, 0, 255);
	m.ceiling[0] = NearestIndex(S(r, 0.42f), S(g, 0.42f), S(b, 0.42f), 0, hiIdx);
	m.ceiling[1] = NearestIndex(S(r, 0.34f), S(g, 0.34f), S(b, 0.34f), 0, hiIdx);
	m.sky[0] = NearestIndex(120, 150, 190, 0, 255);
	m.sky[1] = NearestIndex(160, 180, 205, 0, 255);
	m.sky[2] = NearestIndex(200, 205, 205, 0, 255);
	return m;
}

enum class CellKind : uint8_t { Empty, Wall, Door, Pillar };

CellKind ClassifyCell(int x, int y, bool &doorAlongX)
{
	doorAlongX = false;
	if (!InMap(x, y))
		return CellKind::Wall;
	const Point p { x, y };
	if (!IsTileSolid(p)) {
		return CellKind::Empty;
	}
	const Object *object = FindObjectAtPosition(p, false);
	const bool wallW = InMap(x - 1, y) ? IsTileSolid({ x - 1, y }) : true;
	const bool wallE = InMap(x + 1, y) ? IsTileSolid({ x + 1, y }) : true;
	const bool wallN = InMap(x, y - 1) ? IsTileSolid({ x, y - 1 }) : true;
	const bool wallS = InMap(x, y + 1) ? IsTileSolid({ x, y + 1 }) : true;
	if (object != nullptr && object->isDoor()) {
		// The door slab spans the opening; it lies along the axis that has walls beside it.
		doorAlongX = wallW && wallE;
		if (!doorAlongX && !(wallN && wallS))
			doorAlongX = (wallW || wallE);
		return CellKind::Door;
	}
	if (!wallW && !wallE && !wallN && !wallS)
		return CellKind::Pillar;
	return CellKind::Wall;
}

struct RayHit {
	float t = 1e9f;
	CellKind kind = CellKind::Empty;
	int cellX = 0, cellY = 0;
	float u = 0; // position along the face
	bool sideY = false;
	bool found = false;
};

/** Ray vs. axis aligned box; returns true and fills t/axis/u on hit. */
bool RayBox(Vec2 o, Vec2 d, float minX, float minY, float maxX, float maxY, float &tOut, bool &sideYOut, float &uOut)
{
	float tMin = -1e9f, tMax = 1e9f;
	bool sideY = false;
	const float invX = std::fabs(d.x) < 1e-8f ? 1e9f : 1.0f / d.x;
	const float invY = std::fabs(d.y) < 1e-8f ? 1e9f : 1.0f / d.y;
	float t1 = (minX - o.x) * invX, t2 = (maxX - o.x) * invX;
	if (t1 > t2)
		std::swap(t1, t2);
	if (t1 > tMin) {
		tMin = t1;
		sideY = false;
	}
	tMax = std::min(tMax, t2);
	t1 = (minY - o.y) * invY;
	t2 = (maxY - o.y) * invY;
	if (t1 > t2)
		std::swap(t1, t2);
	if (t1 > tMin) {
		tMin = t1;
		sideY = true;
	}
	tMax = std::min(tMax, t2);
	if (tMax < std::max(tMin, 0.0f))
		return false;
	if (tMin < 0)
		return false;
	tOut = tMin;
	sideYOut = sideY;
	const float hx = o.x + d.x * tMin, hy = o.y + d.y * tMin;
	uOut = sideY ? (hx - minX) : (hy - minY);
	return true;
}

RayHit CastRay(Vec2 o, Vec2 d, float maxDist)
{
	RayHit best;
	int mapX = static_cast<int>(std::floor(o.x));
	int mapY = static_cast<int>(std::floor(o.y));
	const float deltaX = std::fabs(d.x) < 1e-8f ? 1e9f : std::fabs(1.0f / d.x);
	const float deltaY = std::fabs(d.y) < 1e-8f ? 1e9f : std::fabs(1.0f / d.y);
	int stepX, stepY;
	float sideX, sideY;
	if (d.x < 0) {
		stepX = -1;
		sideX = (o.x - mapX) * deltaX;
	} else {
		stepX = 1;
		sideX = (mapX + 1.0f - o.x) * deltaX;
	}
	if (d.y < 0) {
		stepY = -1;
		sideY = (o.y - mapY) * deltaY;
	} else {
		stepY = 1;
		sideY = (mapY + 1.0f - o.y) * deltaY;
	}
	for (int i = 0; i < 96; i++) {
		float entryT;
		bool stepWasY;
		if (sideX < sideY) {
			entryT = sideX;
			sideX += deltaX;
			mapX += stepX;
			stepWasY = false;
		} else {
			entryT = sideY;
			sideY += deltaY;
			mapY += stepY;
			stepWasY = true;
		}
		if (entryT > maxDist)
			break;
		if (best.found && entryT > best.t)
			break;
		bool along = false;
		const CellKind kind = ClassifyCell(mapX, mapY, along);
		if (kind == CellKind::Empty)
			continue;
		float t = 0, u = 0;
		bool sideYHit = false;
		bool ok = false;
		if (kind == CellKind::Wall) {
			t = entryT;
			const float hx = o.x + d.x * t, hy = o.y + d.y * t;
			sideYHit = stepWasY;
			u = sideYHit ? (hx - mapX) : (hy - mapY);
			ok = true;
		} else if (kind == CellKind::Door) {
			const float half = 0.07f;
			if (along) // slab lies along x at the middle of the cell in y
				ok = RayBox(o, d, mapX, mapY + 0.5f - half, mapX + 1.0f, mapY + 0.5f + half, t, sideYHit, u);
			else
				ok = RayBox(o, d, mapX + 0.5f - half, mapY, mapX + 0.5f + half, mapY + 1.0f, t, sideYHit, u);
		} else {
			ok = RayBox(o, d, mapX + 0.3f, mapY + 0.3f, mapX + 0.7f, mapY + 0.7f, t, sideYHit, u);
			u = std::clamp(u / 0.4f, 0.0f, 1.0f);
		}
		if (ok && t < best.t) {
			best.t = t;
			best.kind = kind;
			best.cellX = mapX;
			best.cellY = mapY;
			best.u = u;
			best.sideY = sideYHit;
			best.found = true;
		}
	}
	return best;
}

float LightAt(float wx, float wy)
{
	if (DebugFullBright)
		return 2.0f;
	// Bilinear interpolation of the tile light map (0 = bright, 15 = dark).
	const float fx = wx - 0.5f, fy = wy - 0.5f;
	const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
	const float tx = fx - x0, ty = fy - y0;
	const auto L = [](int x, int y) -> float {
		x = std::clamp(x, 0, MAXDUNX - 1);
		y = std::clamp(y, 0, MAXDUNY - 1);
		return static_cast<float>(dLight[x][y]);
	};
	const float a = L(x0, y0) * (1 - tx) + L(x0 + 1, y0) * tx;
	const float b = L(x0, y0 + 1) * (1 - tx) + L(x0 + 1, y0 + 1) * tx;
	return a * (1 - ty) + b * ty;
}

inline uint8_t Shade(uint8_t color, float light, int px, int py)
{
	const float dithered = light + (Bayer4[py & 3][px & 3] - 7.5f) / 16.0f;
	const int level = std::clamp(static_cast<int>(std::floor(dithered + 0.5f)), 0, 15);
	return LightTables[level][color];
}

uint8_t WallTexel(const Materials &m, const RayHit &hit, float v)
{
	// v is the height above the floor in world units.
	const float course = 0.15f;
	const int row = static_cast<int>(std::floor(v / course));
	const float vf = v / course - row;
	if (hit.kind == CellKind::Door) {
		const float plank = hit.u * 6.0f;
		const int pi = static_cast<int>(plank);
		const float pf = plank - pi;
		if (pf < 0.07f)
			return m.wood[2];
		if ((v > 0.25f && v < 0.33f) || (v > 0.95f && v < 1.03f))
			return m.iron;
		return m.wood[(Hash(pi, hit.cellX, hit.cellY) & 1) ? 0 : 1];
	}
	if (hit.kind == CellKind::Pillar) {
		if (v < 0.14f || v > WallHeight - 0.14f)
			return m.stone[0];
		const float flute = std::sin(hit.u * Pi * 5.0f);
		return flute > 0.55f ? m.stone[0] : (flute < -0.5f ? m.stone[3] : m.stone[1]);
	}
	// Base course and cornice.
	if (v < 0.12f)
		return m.stone[3];
	if (v > WallHeight - 0.1f)
		return m.stone[0];
	const float brickW = 0.55f;
	const float uu = hit.u + ((row & 1) ? brickW * 0.5f : 0.0f);
	const int col = static_cast<int>(std::floor(uu / brickW));
	const float uf = uu / brickW - col;
	if (vf < 0.08f || uf < 0.04f)
		return m.mortar;
	return m.stone[Hash(hit.cellX * 7 + hit.cellY, row, col) & 3];
}

uint8_t FloorTexel(const Materials &m, float wx, float wy, bool &isCeiling)
{
	isCeiling = false;
	if (leveltype == DTYPE_TOWN) {
		// Grass and dirt speckle.
		const uint32_t h = Hash(static_cast<int>(std::floor(wx * 12)), static_cast<int>(std::floor(wy * 12)), 5);
		const uint32_t patch = Hash(static_cast<int>(std::floor(wx * 0.7f)), static_cast<int>(std::floor(wy * 0.7f)), 9);
		if ((patch & 7) == 0 && (h & 3) != 0)
			return m.floorSeam;
		return m.floor[h % 3];
	}
	// Irregular flagstones: rows of slabs with staggered joints.
	const float rowH = 0.5f;
	const float sy = wy / rowH;
	const int iy = static_cast<int>(std::floor(sy));
	const float fy = sy - iy;
	const float slabW = (Hash(iy, 17) & 1) ? 0.5f : 0.75f;
	const float sx = wx / slabW + (Hash(iy, 3) & 7) * 0.125f;
	const int ix = static_cast<int>(std::floor(sx));
	const float fx = sx - ix;
	if (fx < 0.05f || fy < 0.05f)
		return m.floorSeam;
	const uint32_t h = Hash(ix, iy, 91);
	// Mostly the base tone, occasionally a lighter or darker slab, with fine grit.
	const uint32_t grit = Hash(static_cast<int>(wx * 16), static_cast<int>(wy * 16)) & 15;
	if (grit == 0)
		return m.floor[2];
	return m.floor[(h % 5 == 0) ? 0 : ((h % 5 == 1) ? 2 : 1)];
}

// ---------------------------------------------------------------------------------------------
// Billboards

struct Billboard {
	float x, y;       // world position (tile units, centre of tile = +0.5)
	float depth;      // along view
	float lateral;    // along right
	ClxSprite sprite; // frame to draw
	const uint8_t *trn = nullptr;
	TargetInfo::Kind kind;
	int id;
	Point tile;
	bool lit = true;
	bool translucent = false;
	float lift = 0.0f; // world units above floor
	float scale = 1.0f;
};

struct RenderState {
	std::vector<uint8_t> color;
	std::vector<float> zbuf;
	int w = 0, h = 0;
};

std::optional<OwnedSurface> SpriteScratch;

Vec2 TileToWorld(Point tile, Displacement pixelOffset)
{
	// Pixel offset in the isometric projection -> tile units.
	const float ox = pixelOffset.deltaX / 64.0f + pixelOffset.deltaY / 32.0f;
	const float oy = pixelOffset.deltaY / 32.0f - pixelOffset.deltaX / 64.0f;
	return { tile.x + 0.5f + ox, tile.y + 0.5f + oy };
}

void BlitBillboard(RenderState &rs, const Billboard &bb, float focal, int cy, Vec2 camPos)
{
	const int sw = bb.sprite.width(), sh = bb.sprite.height();
	if (sw <= 0 || sh <= 0 || sw > 320 || sh > 320)
		return;
	if (!SpriteScratch)
		SpriteScratch.emplace(320, 320);
	OwnedSurface &scratch = *SpriteScratch;
	for (int y = 0; y < sh; y++)
		std::memset(&scratch[Point { 0, y }], 0, sw);
	if (bb.trn != nullptr)
		RenderClxSpriteWithTRN(scratch, bb.sprite, { 0, 0 }, bb.trn);
	else
		RenderClxSprite(scratch, bb.sprite, { 0, 0 });

	const float worldH = sh / SpritePixelsPerUnit * bb.scale;
	const float worldW = sw / SpritePixelsPerUnit * bb.scale;
	const float screenH = worldH * focal / bb.depth;
	const float screenW = worldW * focal / bb.depth;
	if (screenH < 1 || screenH > rs.h * 6)
		return;
	const float cx = rs.w * 0.5f + bb.lateral / bb.depth * focal;
	const float footY = cy + (EyeHeight - bb.lift) * focal / bb.depth;
	const int left = static_cast<int>(std::floor(cx - screenW * 0.5f));
	const int top = static_cast<int>(std::floor(footY - screenH));
	const int pw = std::max(1, static_cast<int>(std::ceil(screenW)));
	const int ph = std::max(1, static_cast<int>(std::ceil(screenH)));
	const float light = LightAt(bb.x, bb.y);
	(void)camPos;
	for (int dx = 0; dx < pw; dx++) {
		const int px = left + dx;
		if (px < 0 || px >= rs.w)
			continue;
		if (bb.depth >= rs.zbuf[px])
			continue;
		const int srcX = std::min(sw - 1, dx * sw / pw);
		for (int dy = 0; dy < ph; dy++) {
			const int py = top + dy;
			if (py < 0 || py >= rs.h)
				continue;
			const int srcY = std::min(sh - 1, dy * sh / ph);
			const uint8_t c = scratch[Point { srcX, srcY }];
			if (c == 0)
				continue;
			rs.color[py * rs.w + px] = bb.lit ? Shade(c, light, px, py) : LightTables[14][c];
		}
	}
}

bool IsLitTile(Point tile)
{
	if (DebugFullBright)
		return true;
	return dLight[tile.x][tile.y] < 13;
}

void AddMonsterBillboards(std::vector<Billboard> &out, Vec2 cam, Vec2 fwd, Vec2 right)
{
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const int id = ActiveMonsters[i];
		Monster &monster = Monsters[id];
		if (monster.isInvalid || !monster.animInfo.sprites)
			continue;
		if (!InMap(monster.position.tile.x, monster.position.tile.y))
			continue;
		Displacement off {};
		if (monster.isWalking())
			off = GetOffsetForWalking(monster.animInfo, monster.direction);
		const Vec2 wp = TileToWorld(monster.position.tile, off);
		const float relX = wp.x - cam.x, relY = wp.y - cam.y;
		const float depth = relX * fwd.x + relY * fwd.y;
		if (depth < 0.15f || depth > 18.0f)
			continue;
		const float lateral = relX * right.x + relY * right.y;
		if (std::fabs(lateral) > depth * 1.6f)
			continue;
		if (!IsLitTile(monster.position.tile))
			continue;
		// Choose the frame group that shows the monster as seen from the camera.
		ClxSprite sprite = monster.animInfo.currentSprite();
		const Direction view = AngleToDirection(std::atan2(relY, relX));
		const int rot = (static_cast<int>(view) - static_cast<int>(Direction::North) + 8) % 8;
		const Direction wanted = static_cast<Direction>(((static_cast<int>(monster.direction) - rot) % 8 + 8) % 8);
		for (int g = 0; g < 8; g++) {
			const auto graphic = static_cast<MonsterGraphic>(g);
			if (!monster.type().getAnimData(graphic).sprites)
				continue;
			const OptionalClxSpriteList current = monster.type().getAnimData(graphic).spritesForDirection(monster.direction);
			if (current && monster.animInfo.sprites && (*current).data() == (*monster.animInfo.sprites).data()) {
				const OptionalClxSpriteList rotated = monster.type().getAnimData(graphic).spritesForDirection(wanted);
				if (rotated) {
					const size_t frame = std::min<size_t>(monster.animInfo.getFrameToUseForRendering(), (*rotated).numSprites() - 1);
					sprite = (*rotated)[frame];
				}
				break;
			}
		}
		Billboard bb { wp.x, wp.y, depth, lateral, sprite };
		bb.trn = monster.isUnique() ? monster.uniqueMonsterTRN.get() : nullptr;
		bb.kind = TargetInfo::Kind::Monster;
		bb.id = id;
		bb.tile = monster.position.tile;
		bb.scale = 1.0f;
		out.push_back(bb);
	}
}

void AddObjectBillboards(std::vector<Billboard> &out, Vec2 cam, Vec2 fwd, Vec2 right)
{
	for (int i = 0; i < MAXOBJECTS; i++) {
		const int id = ActiveObjects[i];
		if (id < 0 || id >= MAXOBJECTS)
			continue;
		Object &object = Objects[id];
		if (object._otype == OBJ_NULL || !object._oAnimData || object.isDoor() || object.IsTrap())
			continue;
		if (object._oAnimFrame < 1 || object._oAnimFrame > (*object._oAnimData).numSprites())
			continue;
		const Vec2 wp { object.position.x + 0.5f, object.position.y + 0.5f };
		const float relX = wp.x - cam.x, relY = wp.y - cam.y;
		const float depth = relX * fwd.x + relY * fwd.y;
		if (depth < 0.15f || depth > 18.0f)
			continue;
		const float lateral = relX * right.x + relY * right.y;
		if (std::fabs(lateral) > depth * 1.6f)
			continue;
		if (!IsLitTile(object.position))
			continue;
		Billboard bb { wp.x, wp.y, depth, lateral, object.currentSprite() };
		bb.kind = TargetInfo::Kind::Object;
		bb.id = id;
		bb.tile = object.position;
		bb.scale = 0.85f;
		out.push_back(bb);
	}
}

void AddItemBillboards(std::vector<Billboard> &out, Vec2 cam, Vec2 fwd, Vec2 right)
{
	for (int i = 0; i < ActiveItemCount; i++) {
		const int id = ActiveItems[i];
		Item &item = Items[id];
		if (item.isEmpty() || !item.AnimInfo.sprites)
			continue;
		const Vec2 wp { item.position.x + 0.5f, item.position.y + 0.5f };
		const float relX = wp.x - cam.x, relY = wp.y - cam.y;
		const float depth = relX * fwd.x + relY * fwd.y;
		if (depth < 0.15f || depth > 16.0f)
			continue;
		const float lateral = relX * right.x + relY * right.y;
		if (std::fabs(lateral) > depth * 1.6f)
			continue;
		if (!IsLitTile(item.position))
			continue;
		Billboard bb { wp.x, wp.y, depth, lateral, item.AnimInfo.currentSprite() };
		bb.kind = TargetInfo::Kind::Item;
		bb.id = id;
		bb.tile = item.position;
		bb.scale = 0.7f;
		out.push_back(bb);
	}
}

void AddTownerBillboards(std::vector<Billboard> &out, Vec2 cam, Vec2 fwd, Vec2 right)
{
	if (leveltype != DTYPE_TOWN)
		return;
	for (size_t i = 0; i < Towners.size(); i++) {
		const Towner &towner = Towners[i];
		if (!towner.anim)
			continue;
		const Vec2 wp { towner.position.x + 0.5f, towner.position.y + 0.5f };
		const float relX = wp.x - cam.x, relY = wp.y - cam.y;
		const float depth = relX * fwd.x + relY * fwd.y;
		if (depth < 0.15f || depth > 30.0f)
			continue;
		const float lateral = relX * right.x + relY * right.y;
		if (std::fabs(lateral) > depth * 1.6f)
			continue;
		Billboard bb { wp.x, wp.y, depth, lateral, towner.currentSprite() };
		bb.kind = TargetInfo::Kind::Towner;
		bb.id = static_cast<int>(i);
		bb.tile = towner.position;
		out.push_back(bb);
	}
}

void AddMissileBillboards(std::vector<Billboard> &out, Vec2 cam, Vec2 fwd, Vec2 right)
{
	for (Missile &missile : Missiles) {
		if (!missile._miDrawFlag || !missile._miAnimData)
			continue;
		if (missile._miAnimFrame < 1 || static_cast<size_t>(missile._miAnimFrame) > (*missile._miAnimData).numSprites())
			continue;
		const Vec2 wp = TileToWorld(missile.position.tile, missile.position.offset);
		const float relX = wp.x - cam.x, relY = wp.y - cam.y;
		const float depth = relX * fwd.x + relY * fwd.y;
		if (depth < 0.2f || depth > 18.0f)
			continue;
		const float lateral = relX * right.x + relY * right.y;
		if (std::fabs(lateral) > depth * 1.8f)
			continue;
		Billboard bb { wp.x, wp.y, depth, lateral, (*missile._miAnimData)[missile._miAnimFrame - 1] };
		bb.kind = TargetInfo::Kind::None;
		bb.id = -1;
		bb.tile = missile.position.tile;
		bb.lift = 0.3f;
		bb.scale = 0.8f;
		bb.lit = missile._miLightFlag;
		out.push_back(bb);
	}
}

RenderState State;

void PresentScaled(const Surface &out, const RenderState &rs)
{
	const int ow = out.w(), oh = out.h();
	// Nearest neighbour upscale of the low-res buffer.
	std::vector<int> xmap(ow);
	for (int x = 0; x < ow; x++)
		xmap[x] = x * rs.w / ow;
	for (int y = 0; y < oh; y++) {
		const uint8_t *src = &rs.color[(y * rs.h / oh) * rs.w];
		uint8_t *dst = &out[Point { 0, y }];
		for (int x = 0; x < ow; x++)
			dst[x] = src[xmap[x]];
	}
}

} // namespace

bool IsFirstPersonView()
{
	return *GetOptions().Graphics.firstPerson && MyPlayer != nullptr && FirstPersonGameRunning();
}

bool IsFirstPersonFree()
{
	if (!IsFirstPersonView())
		return false;
	return FirstPersonNoPanelsOpen() && !IsPlayerInStore() && !ChatFlag && !AutomapActive;
}

bool IsFirstPersonHud()
{
	return IsFirstPersonView() && *GetOptions().Graphics.firstPersonHud;
}

void ToggleFirstPerson()
{
	auto &opt = GetOptions().Graphics.firstPerson;
	opt.SetValue(!*opt);
	YawInitialized = false;
	RedrawEverything();
	if (!*opt && MouseCaptured) {
		SDL_SetRelativeMouseMode(SDL_FALSE);
		MouseCaptured = false;
	}
}

void FirstPersonProcessInput()
{
	if (!IsFirstPersonView()) {
		if (MouseCaptured) {
			SDL_SetRelativeMouseMode(SDL_FALSE);
			MouseCaptured = false;
		}
		return;
	}
	Player &player = *MyPlayer;
	if (!YawInitialized) {
		Yaw = DirectionAngle(player._pdir);
		YawInitialized = true;
		LastInputTicks = SDL_GetTicks();
	}
	RedrawEverything();

	const uint32_t now = SDL_GetTicks();
	const float dt = std::min(0.1f, (now - LastInputTicks) / 1000.0f);
	LastInputTicks = now;

	const bool free = IsFirstPersonFree();
	if (free != MouseCaptured) {
		SDL_SetRelativeMouseMode(free ? SDL_TRUE : SDL_FALSE);
		MouseCaptured = free;
	}
	if (!free)
		return;

	const Uint8 *keys = SDL_GetKeyboardState(nullptr);
	float turn = 0;
	if (keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_Q])
		turn -= 1;
	if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_E])
		turn += 1;
	Yaw = WrapAngle(Yaw + turn * 2.4f * dt);
	int mx = 0, my = 0;
	SDL_GetRelativeMouseState(&mx, &my);
	Yaw = WrapAngle(Yaw + mx * 0.0032f);

	float moveF = 0, moveR = 0;
	if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP])
		moveF += 1;
	if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN])
		moveF -= 1;
	if (keys[SDL_SCANCODE_D])
		moveR += 1;
	if (keys[SDL_SCANCODE_A])
		moveR -= 1;
	if ((moveF == 0 && moveR == 0) || player.isWalking() || !player.CanChangeAction() || player._pmode != PM_STAND)
		return;
	const Vec2 f = Forward(), r = Right();
	const float mxv = f.x * moveF + r.x * moveR, myv = f.y * moveF + r.y * moveR;
	const Direction dir = AngleToDirection(std::atan2(myv, mxv));
	const Point target = player.position.future + dir;
	if (!PosOkPlayer(player, target))
		return;
	player._pdir = dir;
	NetSendCmdLoc(player.getId(), true, CMD_WALKXY, target);
}

bool FirstPersonUpdateCursor()
{
	if (!IsFirstPersonFree())
		return false;
	MousePosition = { gnScreenWidth / 2, gnScreenHeight / 2 };
	ResetCursorInfo();
	cursPosition = MyPlayer->position.future;
	switch (CurrentTarget.kind) {
	case TargetInfo::Kind::Monster:
		pcursmonst = CurrentTarget.id;
		cursPosition = Monsters[CurrentTarget.id].position.tile;
		break;
	case TargetInfo::Kind::Towner:
		pcursmonst = CurrentTarget.id;
		cursPosition = CurrentTarget.tile;
		break;
	case TargetInfo::Kind::Item:
		pcursitem = static_cast<int8_t>(CurrentTarget.id);
		cursPosition = CurrentTarget.tile;
		break;
	case TargetInfo::Kind::Object:
		ObjectUnderCursor = &Objects[CurrentTarget.id];
		cursPosition = CurrentTarget.tile;
		break;
	default: {
		// Walk target: a couple of tiles ahead, stopping before walls.
		const Vec2 f = Forward();
		const float dist = std::min(2.0f, std::max(0.0f, CenterWallDepth - 0.6f));
		const Point ahead { static_cast<int>(std::floor(MyPlayer->position.tile.x + 0.5f + f.x * std::max(1.0f, dist))), static_cast<int>(std::floor(MyPlayer->position.tile.y + 0.5f + f.y * std::max(1.0f, dist))) };
		cursPosition = ahead;
		break;
	}
	}
	return true;
}

void DrawFirstPersonWorld(const Surface &out)
{
	if (!YawInitialized) {
		Yaw = DirectionAngle(MyPlayer->_pdir);
		YawInitialized = true;
	}
#ifdef _DEBUG
	if (const char *dbgYaw = std::getenv("DEVILUTIONX_DEV_FPYAW"); dbgYaw != nullptr && std::getenv("DEVILUTIONX_DEV_FPPEEK") == nullptr)
		Yaw = static_cast<float>(std::atof(dbgYaw)) * Pi / 180.0f;
#endif
	// Internal resolution: roughly 320 pixels wide.
	const int scale = std::max(1, static_cast<int>(std::lround(out.w() / 320.0)));
	RenderState &rs = State;
	rs.w = std::max(64, out.w() / scale);
	rs.h = std::max(48, out.h() / scale);
	rs.color.assign(static_cast<size_t>(rs.w) * rs.h, 0);
	rs.zbuf.assign(rs.w, 1e9f);

	const Materials mats = BuildMaterials();

	// Camera.
	const Player &player = *MyPlayer;
	Displacement off {};
	if (player.isWalking())
		off = GetOffsetForWalking(player.AnimInfo, player._pdir);
	Vec2 cam = TileToWorld(player.position.tile, off);
#ifdef _DEBUG
	if (std::getenv("DEVILUTIONX_DEV_FPPEEK") != nullptr) {
		// Debug: look at the nearest monster from a couple of tiles away.
		float bestD = 1e9f;
		for (size_t i = 0; i < ActiveMonsterCount; i++) {
			const Monster &m = Monsters[ActiveMonsters[i]];
			if (m.isInvalid || m.hitPoints <= 0 || !InMap(m.position.tile.x, m.position.tile.y))
				continue;
			const float dx = m.position.tile.x - cam.x, dy = m.position.tile.y - cam.y;
			const float d = dx * dx + dy * dy;
			if (d < bestD) {
				bestD = d;
				const Vec2 mp { m.position.tile.x + 0.5f, m.position.tile.y + 0.5f };
				for (int k = 0; k < 16; k++) {
					const float a = std::atan2(dy, dx) + k * Pi / 8.0f;
					const Vec2 c { mp.x - std::cos(a) * 2.6f, mp.y - std::sin(a) * 2.6f };
					if (!InMap(static_cast<int>(c.x), static_cast<int>(c.y)) || IsTileSolid({ static_cast<int>(c.x), static_cast<int>(c.y) }))
						continue;
					const RayHit h = CastRay(c, { std::cos(a), std::sin(a) }, 3.0f);
					if (h.found && h.t < 2.4f)
						continue;
					Yaw = a;
					cam = c;
					break;
				}
				DebugFullBright = true;
			}
		}
	}
#endif
	const Vec2 fwd = Forward();
	const Vec2 right = Right();
	const float planeLen = std::tan(FieldOfView * 0.5f);
	const float focal = rs.w * 0.5f / planeLen;
	const int cy = rs.h / 2;
	const bool outdoors = leveltype == DTYPE_TOWN;

	// Floor and ceiling.
	for (int y = 0; y < rs.h; y++) {
		if (y == cy)
			continue;
		const bool floorRow = y > cy;
		const float denom = std::fabs(static_cast<float>(y - cy)) + 0.001f;
		const float rowDist = (floorRow ? EyeHeight : (WallHeight - EyeHeight)) * focal / denom;
		for (int x = 0; x < rs.w; x++) {
			const float cxs = (2.0f * x / rs.w - 1.0f) * planeLen;
			const float wx = cam.x + (fwd.x + right.x * cxs) * rowDist;
			const float wy = cam.y + (fwd.y + right.y * cxs) * rowDist;
			uint8_t c;
			if (floorRow) {
				bool dummy;
				c = Shade(FloorTexel(mats, wx, wy, dummy), LightAt(wx, wy), x, y);
			} else if (outdoors) {
				const float k = 1.0f - static_cast<float>(y) / cy + (Bayer4[y & 3][x & 3] - 7.5f) / 40.0f;
				const int band = k < 0.3f ? 2 : (k < 0.65f ? 1 : 0);
				c = mats.sky[band];
			} else {
				const bool beam = std::fmod(std::fabs(wx), 2.0f) < 0.1f || std::fmod(std::fabs(wy), 2.0f) < 0.1f;
				c = Shade(mats.ceiling[beam ? 1 : 0], LightAt(wx, wy) + 1.0f, x, y);
			}
			rs.color[y * rs.w + x] = c;
		}
	}
	// Horizon row copy.
	for (int x = 0; x < rs.w; x++)
		rs.color[cy * rs.w + x] = rs.color[(cy + 1) * rs.w + x];

	// Walls.
	for (int x = 0; x < rs.w; x++) {
		const float cxs = (2.0f * x / rs.w - 1.0f) * planeLen;
		const Vec2 dir { fwd.x + right.x * cxs, fwd.y + right.y * cxs };
		const RayHit hit = CastRay(cam, dir, 40.0f);
		if (!hit.found) {
			rs.zbuf[x] = 1e9f;
			continue;
		}
		rs.zbuf[x] = hit.t;
		if (x == rs.w / 2)
			CenterWallDepth = hit.t;
		const float lineH = focal / hit.t; // height of one world unit in pixels
		const float topY = cy - (WallHeight - EyeHeight) * lineH;
		const float botY = cy + EyeHeight * lineH;
		const int y0 = std::max(0, static_cast<int>(std::floor(topY)));
		const int y1 = std::min(rs.h - 1, static_cast<int>(std::ceil(botY)) - 1);
		const float hx = cam.x + dir.x * hit.t, hy = cam.y + dir.y * hit.t;
		const float light = LightAt(hx - dir.x * 0.08f, hy - dir.y * 0.08f);
		for (int y = y0; y <= y1; y++) {
			const float v = (botY - (y + 0.5f)) / lineH;
			uint8_t c = WallTexel(mats, hit, v);
			float l = light + (hit.sideY ? 1.0f : 0.0f);
			rs.color[y * rs.w + x] = Shade(c, l, x, y);
		}
	}

	// Billboards.
	std::vector<Billboard> boards;
	boards.reserve(128);
	AddMonsterBillboards(boards, cam, fwd, right);
	AddObjectBillboards(boards, cam, fwd, right);
	AddItemBillboards(boards, cam, fwd, right);
	AddMissileBillboards(boards, cam, fwd, right);
	AddTownerBillboards(boards, cam, fwd, right);
	std::sort(boards.begin(), boards.end(), [](const Billboard &a, const Billboard &b) { return a.depth > b.depth; });
	for (const Billboard &bb : boards)
		BlitBillboard(rs, bb, focal, cy, cam);

	// Target under the crosshair: the closest selectable billboard whose screen box contains it.
	TargetInfo best;
	float bestDepth = 1e9f;
	int bestPriority = -1;
	for (const Billboard &bb : boards) {
		if (bb.kind == TargetInfo::Kind::None)
			continue;
		if (bb.kind == TargetInfo::Kind::Monster) {
			const Monster &m = Monsters[bb.id];
			if (m.hitPoints <= 0 || m.mode == MonsterMode::Death)
				continue;
		}
		const float screenW = bb.sprite.width() / SpritePixelsPerUnit * bb.scale * focal / bb.depth;
		const float screenH = bb.sprite.height() / SpritePixelsPerUnit * bb.scale * focal / bb.depth;
		const float cx = rs.w * 0.5f + bb.lateral / bb.depth * focal;
		const float footY = cy + (EyeHeight - bb.lift) * focal / bb.depth;
		const float hw = std::max(screenW * 0.22f, 3.0f); // only the body, not the whole frame
		if (std::fabs(cx - rs.w * 0.5f) > hw || cy > footY || cy < footY - screenH)
			continue;
		if (bb.depth > CenterWallDepth + 0.3f)
			continue;
		const int priority = (bb.kind == TargetInfo::Kind::Monster || bb.kind == TargetInfo::Kind::Towner) ? 2 : (bb.kind == TargetInfo::Kind::Item ? 1 : 0);
		if (priority > bestPriority || (priority == bestPriority && bb.depth < bestDepth)) {
			bestPriority = priority;
			bestDepth = bb.depth;
			best.kind = bb.kind;
			best.id = bb.id;
			best.tile = bb.tile;
		}
	}
	CurrentTarget = best;

	PresentScaled(out, rs);
}


// ---------------------------------------------------------------------------------------------
// HUD

namespace {

struct BarFade {
	int last = -1;
	uint32_t changed = 0;
};
BarFade HealthFade, ManaFade, XpFade;

bool BarVisible(BarFade &fade, int value, int maxValue)
{
	const uint32_t now = SDL_GetTicks();
	if (value != fade.last) {
		fade.last = value;
		fade.changed = now;
	}
	return value < maxValue || now - fade.changed < 3500;
}

struct HudColors {
	uint8_t gold, goldDark, goldLight, black, red, redDark, redLight, blue, blueDark, blueLight, xp, xpDark, back, white, enemy;
};

HudColors BuildHudColors()
{
	HudColors c;
	c.gold = NearestIndex(196, 150, 60, 128);
	c.goldDark = NearestIndex(104, 76, 30, 128);
	c.goldLight = NearestIndex(244, 214, 128, 128);
	c.black = NearestIndex(8, 6, 6, 128);
	c.red = NearestIndex(176, 24, 24, 128);
	c.redDark = NearestIndex(84, 10, 10, 128);
	c.redLight = NearestIndex(232, 84, 72, 128);
	c.blue = NearestIndex(40, 70, 190, 128);
	c.blueDark = NearestIndex(14, 24, 84, 128);
	c.blueLight = NearestIndex(120, 150, 240, 128);
	c.xp = NearestIndex(200, 170, 70, 128);
	c.xpDark = NearestIndex(100, 80, 30, 128);
	c.back = NearestIndex(24, 16, 14, 128);
	c.white = NearestIndex(236, 230, 220, 128);
	c.enemy = NearestIndex(200, 40, 30, 128);
	return c;
}

/** Slim gothic bar: bronze frame, dark well, gradient fill, small corner studs. */
void DrawBar(const Surface &out, const HudColors &c, Point pos, int width, int height, float frac, uint8_t fill, uint8_t fillDark, uint8_t fillLight, bool fromCentre)
{
	frac = std::clamp(frac, 0.0f, 1.0f);
	FillRect(out, pos.x - 2, pos.y - 2, width + 4, height + 4, c.goldDark);
	FillRect(out, pos.x - 1, pos.y - 1, width + 2, height + 2, c.black);
	FillRect(out, pos.x, pos.y, width, height, c.back);
	const int fillW = static_cast<int>(width * frac);
	int x0 = pos.x;
	int w = fillW;
	if (fromCentre) {
		x0 = pos.x + (width - fillW) / 2;
	}
	if (w > 0) {
		FillRect(out, x0, pos.y, w, height, fillDark);
		FillRect(out, x0, pos.y, w, std::max(1, height * 2 / 3), fill);
		FillRect(out, x0, pos.y, w, 1, fillLight);
	}
	// Frame highlights.
	DrawHorizontalLine(out, { pos.x - 2, pos.y - 2 }, width + 4, c.gold);
	DrawHorizontalLine(out, { pos.x - 2, pos.y + height + 1 }, width + 4, c.goldDark);
	// End studs.
	for (int s : { -5, width + 2 }) {
		FillRect(out, pos.x + s, pos.y + height / 2 - 2, 3, 5, c.gold);
		FillRect(out, pos.x + s + 1, pos.y + height / 2 - 1, 1, 3, c.goldLight);
	}
}

void DrawCenteredText(const Surface &out, std::string_view text, Point centre, UiFlags flags)
{
	DrawString(out, text, Rectangle { Point { centre.x - 150, centre.y - 8 }, Size { 300, 16 } }, { .flags = flags | UiFlags::AlignCenter | UiFlags::VerticalCenter });
}

void DrawCompass(const Surface &out, const HudColors &c)
{
	const int width = std::min(out.w() - 40, 380);
	const int cx = out.w() / 2;
	const int top = 8;
	// Plate.
	FillRect(out, cx - width / 2, top, width, 18, c.black);
	DrawHorizontalLine(out, { cx - width / 2, top }, width, c.goldDark);
	DrawHorizontalLine(out, { cx - width / 2, top + 17 }, width, c.goldDark);
	DrawHorizontalLine(out, { cx - width / 2, top + 1 }, width, c.gold);
	FillRect(out, cx - width / 2 - 3, top - 1, 3, 20, c.gold);
	FillRect(out, cx + width / 2, top - 1, 3, 20, c.gold);
	const float bearing = std::fmod(Yaw * 180.0f / Pi + 135.0f + 720.0f, 360.0f);
	const float span = 140.0f; // visible degrees
	static const char *names[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
	for (int deg = 0; deg < 360; deg += 15) {
		float diff = static_cast<float>(deg) - bearing;
		while (diff > 180)
			diff -= 360;
		while (diff < -180)
			diff += 360;
		if (std::fabs(diff) > span * 0.5f)
			continue;
		const int x = cx + static_cast<int>(diff / span * width);
		if (x < cx - width / 2 + 4 || x > cx + width / 2 - 4)
			continue;
		if (deg % 45 == 0) {
			const char *label = names[deg / 45];
			const bool cardinal = deg % 90 == 0;
			DrawString(out, label, Rectangle { Point { x - 14, top + 1 }, Size { 28, 16 } }, { .flags = (cardinal ? UiFlags::ColorWhitegold : UiFlags::ColorGold) | UiFlags::AlignCenter | UiFlags::VerticalCenter });
		} else {
			FillRect(out, x, top + 6, 1, 6, c.goldDark);
		}
	}
	// Centre marker.
	for (int i = 0; i < 4; i++)
		DrawHorizontalLine(out, { cx - 3 + i, top + 20 + i }, 7 - 2 * i, c.goldLight);

	// Enemy markers.
	const Vec2 cam { MyPlayer->position.tile.x + 0.5f, MyPlayer->position.tile.y + 0.5f };
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const Monster &m = Monsters[ActiveMonsters[i]];
		if (m.isInvalid || m.hitPoints <= 0 || !InMap(m.position.tile.x, m.position.tile.y) || !IsLitTile(m.position.tile))
			continue;
		const float dx = m.position.tile.x + 0.5f - cam.x, dy = m.position.tile.y + 0.5f - cam.y;
		if (dx * dx + dy * dy > 14.0f * 14.0f)
			continue;
		float mb = std::fmod(std::atan2(dy, dx) * 180.0f / Pi + 135.0f + 720.0f, 360.0f);
		float diff = mb - bearing;
		while (diff > 180)
			diff -= 360;
		while (diff < -180)
			diff += 360;
		if (std::fabs(diff) > span * 0.5f)
			continue;
		const int x = cx + static_cast<int>(diff / span * width);
		FillRect(out, x - 1, top + 12, 3, 3, c.enemy);
		FillRect(out, x, top + 11, 1, 1, c.redLight);
	}
}

void DrawCrosshair(const Surface &out, const HudColors &c)
{
	const int cx = out.w() / 2, cy = out.h() / 2;
	const uint8_t col = CurrentTarget.kind == TargetInfo::Kind::Monster ? c.enemy : c.goldLight;
	FillRect(out, cx - 6, cy, 4, 1, col);
	FillRect(out, cx + 3, cy, 4, 1, col);
	FillRect(out, cx, cy - 6, 1, 4, col);
	FillRect(out, cx, cy + 3, 1, 4, col);
	FillRect(out, cx, cy, 1, 1, col);
}

void DrawTargetBar(const Surface &out, const HudColors &c)
{
	if (CurrentTarget.kind == TargetInfo::Kind::Towner && CurrentTarget.id < static_cast<int>(Towners.size())) {
		DrawCenteredText(out, Towners[CurrentTarget.id].name, { out.w() / 2, 44 }, UiFlags::ColorWhitegold);
		return;
	}
	if (CurrentTarget.kind != TargetInfo::Kind::Monster)
		return;
	const Monster &m = Monsters[CurrentTarget.id];
	if (m.hitPoints <= 0)
		return;
	const int cx = out.w() / 2;
	const int y = 44;
	const float frac = m.maxHitPoints > 0 ? static_cast<float>(m.hitPoints.raw()) / static_cast<float>(m.maxHitPoints.raw()) : 0;
	DrawCenteredText(out, m.name(), { cx, y }, m.isUnique() ? UiFlags::ColorGold : UiFlags::ColorWhitegold);
	DrawBar(out, c, { cx - 110, y + 12 }, 220, 5, frac, c.red, c.redDark, c.redLight, true);
}

void DrawStatusBars(const Surface &out, const HudColors &c)
{
	const Player &p = *MyPlayer;
	const int cx = out.w() / 2;
	const int y = out.h() - 22;
	const int hp = p._pHitPoints.whole(), maxHp = std::max(1, p._pMaxHP.whole());
	const int mana = p._pMana.whole(), maxMana = std::max(1, p._pMaxMana.whole());
	const uint32_t next = p.getNextExperienceThreshold();
	const int xp = static_cast<int>(p._pExperience);
	const bool noMana = p.hasNoMana() || HasAnyOf(p._pIFlags, ItemSpecialEffect::NoMana);

	const bool showHealth = BarVisible(HealthFade, hp, maxHp);
	if (showHealth)
		DrawBar(out, c, { cx - 130, y }, 260, 8, static_cast<float>(hp) / maxHp, c.red, c.redDark, c.redLight, true);
	if (!noMana && BarVisible(ManaFade, mana, maxMana))
		DrawBar(out, c, { cx - 130 - 20 - 150, y + 1 }, 150, 6, static_cast<float>(mana) / maxMana, c.blue, c.blueDark, c.blueLight, false);
	if (next > 0 && xp < static_cast<int>(next)) {
		const uint32_t prev = (p.getCharacterLevel() > 1 ? GetNextExperienceThresholdForLevel(p.getCharacterLevel() - 1) : 0U);
		const float frac = next > prev ? static_cast<float>(xp - static_cast<int>(prev)) / static_cast<float>(next - prev) : 0.0f;
		if (BarVisible(XpFade, xp, 1 << 30))
			DrawBar(out, c, { cx + 130 + 20, y + 1 }, 150, 6, frac, c.xp, c.xpDark, c.goldLight, false);
	}
	// Numbers on the health bar.
	if (showHealth)
		DrawCenteredText(out, StrCat(hp, " / ", maxHp), { cx, y - 12 }, UiFlags::ColorGold);

	// Current spell, bottom left.
	const SpellID spell = p._pRSpell;
	if (spell != SpellID::Invalid) {
		DrawSmallSpellIcon(out, { 18, out.h() - 14 }, spell);
		DrawString(out, GetSpellData(spell).sNameText, Rectangle { Point { 62, out.h() - 36 }, Size { 160, 16 } }, { .flags = UiFlags::ColorWhitegold | UiFlags::VerticalCenter });
	}
	// Belt quick slots, bottom right.
	int slotX = out.w() - 18 - 8 * 20;
	for (int i = 0; i < 8; i++) {
		const Item &it = p.SpdList[i];
		const int sx = slotX + i * 20;
		FillRect(out, sx, out.h() - 28, 17, 17, c.goldDark);
		FillRect(out, sx + 1, out.h() - 27, 15, 15, c.black);
		if (!it.isEmpty()) {
			FillRect(out, sx + 4, out.h() - 24, 9, 9, it._itype == ItemType::Gold ? c.gold : (it._iMiscId == IMISC_MANA || it._iMiscId == IMISC_FULLMANA ? c.blue : (it._iMiscId == IMISC_REJUV || it._iMiscId == IMISC_FULLREJUV ? c.redLight : c.red)));
		}
		DrawString(out, StrCat(i + 1), Rectangle { Point { sx, out.h() - 42 }, Size { 17, 12 } }, { .flags = UiFlags::ColorGold | UiFlags::AlignCenter | UiFlags::VerticalCenter });
	}
}

} // namespace

void DrawFirstPersonHud(const Surface &out)
{
	const HudColors c = BuildHudColors();
	DrawCompass(out, c);
	DrawTargetBar(out, c);
	DrawStatusBars(out, c);
	DrawCrosshair(out, c);
}

} // namespace devilution
