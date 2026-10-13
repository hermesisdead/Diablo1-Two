/**
 * @file orbit_view.cpp
 *
 * Third-person 3D view for action mode, built from the game's own art.
 *
 * Diablo draws each dungeon cell as a column of 32x32 micro tiles: a floor diamond at the
 * bottom and, above it, the wall art that stands on the cell's two back edges (west and
 * north). That maps directly onto 3D: the floor diamond is un-skewed into a square floor
 * texture, and the left/right halves of the column above it are un-skewed into upright
 * wall panels on the west/north edges of the cell. Transparent pixels stay transparent, so
 * arches, grates and broken walls keep their shapes. Everything is rasterised in software,
 * palettised, with the game's own light map and light tables.
 */
#include "engine/render/orbit_view.hpp"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <vector>

#include "engine/clx_sprite.hpp"
#include "engine/displacement.hpp"
#include "engine/light_tables.hpp"
#include "engine/palette.h"
#include "engine/render/clx_render.hpp"
#include "engine/render/dun_render.hpp"
#include "engine/render/light_render.hpp"
#include "engine/render/scrollrt.h"
#include "engine/surface.hpp"
#include "items.h"
#include "levels/dun_tile.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "missiles.h"
#include "monster.h"
#include "objects.h"
#include "options.h"
#include "player.h"
#include "towners.h"
#include "utils/log.hpp"

namespace devilution {

bool IsActionMode();

namespace {

constexpr float Pi = 3.14159265358979f;
/** Vertical art pixels per world unit (one tile = one unit), shared by walls and sprites so proportions match the original. */
constexpr float PxPerUnitZ = 44.0f;
/** Horizontal sprite pixels per world unit. */
constexpr float PxPerUnitW = 44.0f;
constexpr int ViewRadius = 15;
constexpr float NearPlane = 0.12f;
constexpr int TexW = 32;

struct V3 {
	float x, y, z;
};

V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

struct Camera {
	V3 pos {}, fwd {}, right {}, up {};
	float focal = 1;
	float cx = 0, cy = 0;
};

// ---------------------------------------------------------------------------------------------
// Camera state

float Yaw = -0.75f * Pi; // look "north" like the original view
float Pitch = 0.42f;
float Distance = 5.4f;
float ManualTurnTimer = 0;
Camera LastCamera;
int LastOutW = 0, LastOutH = 0, LastScale = 1;

float WrapAngle(float a)
{
	while (a > Pi)
		a -= 2 * Pi;
	while (a < -Pi)
		a += 2 * Pi;
	return a;
}

float LerpAngle(float from, float to, float t)
{
	return WrapAngle(from + WrapAngle(to - from) * std::clamp(t, 0.0f, 1.0f));
}

// ---------------------------------------------------------------------------------------------
// Smoothed render positions (hybrid grid: logic stays on tiles, rendering glides)

struct Smoothed {
	float x = 0, y = 0;
	bool valid = false;
};
std::array<Smoothed, MaxMonsters> MonsterSmooth;
Smoothed PlayerSmooth;
uint32_t LastSmoothTicks = 0;

void TileToWorld(Point tile, Displacement pixelOffset, float &x, float &y)
{
	const float ox = pixelOffset.deltaX / 64.0f + pixelOffset.deltaY / 32.0f;
	const float oy = pixelOffset.deltaY / 32.0f - pixelOffset.deltaX / 64.0f;
	x = tile.x + 0.5f + ox;
	y = tile.y + 0.5f + oy;
}

void SmoothTowards(Smoothed &s, float x, float y, float dt)
{
	if (!s.valid || std::abs(s.x - x) + std::abs(s.y - y) > 2.5f) {
		s = { x, y, true };
		return;
	}
	const float k = std::min(1.0f, dt * 14.0f);
	s.x += (x - s.x) * k;
	s.y += (y - s.y) * k;
}

void PlayerTruePosition(float &x, float &y)
{
	const Player &player = *MyPlayer;
	Displacement off {};
	if (player.isWalking())
		off = GetOffsetForWalking(player.AnimInfo, player._pdir);
	TileToWorld(player.position.tile, off, x, y);
}

void MonsterTruePosition(const Monster &monster, float &x, float &y)
{
	Displacement off {};
	if (monster.isWalking())
		off = GetOffsetForWalking(monster.animInfo, monster.direction);
	TileToWorld(monster.position.tile, off, x, y);
}

void UpdateSmoothing()
{
	const uint32_t now = SDL_GetTicks();
	const float dt = std::min(0.1f, (now - LastSmoothTicks) / 1000.0f);
	LastSmoothTicks = now;
	float px, py;
	PlayerTruePosition(px, py);
	SmoothTowards(PlayerSmooth, px, py, dt);
	std::array<bool, MaxMonsters> seen {};
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const unsigned id = ActiveMonsters[i];
		const Monster &monster = Monsters[id];
		float mx, my;
		MonsterTruePosition(monster, mx, my);
		SmoothTowards(MonsterSmooth[id], mx, my, dt);
		seen[id] = true;
	}
	for (size_t i = 0; i < MaxMonsters; i++)
		if (!seen[i])
			MonsterSmooth[i].valid = false;
}

// ---------------------------------------------------------------------------------------------
// Textures cut from the level's tile art

struct PieceTextures {
	bool hasFloor = false;
	std::array<uint8_t, TexW * TexW> floor {};
	int wallH = 0;
	bool hasWest = false, hasNorth = false;
	std::vector<uint16_t> west, north; // row-major, row 0 = floor level; 0xFFFF = transparent
};

std::unordered_map<uint16_t, PieceTextures> PieceCache;
const std::byte *CachedCels = nullptr;
int CachedLevel = -1;
std::optional<OwnedSurface> ScratchA, ScratchB, ScratchFloorA, ScratchFloorB;
std::vector<uint8_t> ZeroLight;

void EnsureScratch(int h)
{
	if (ScratchA && ScratchA->h() == h)
		return;
	ScratchA.emplace(64, h);
	ScratchB.emplace(64, h);
	ScratchFloorA.emplace(64, 48);
	ScratchFloorB.emplace(64, 48);
}

void FillSurface(const Surface &s, uint8_t value)
{
	for (int y = 0; y < s.h(); y++)
		std::memset(&s[Point { 0, y }], value, s.w());
}

Lightmap MakeFullLightmap(const Surface &s)
{
	const size_t need = static_cast<size_t>(s.pitch()) * s.h();
	if (ZeroLight.size() < need)
		ZeroLight.assign(need, 0);
	return Lightmap(s.begin(), std::span<const uint8_t>(ZeroLight.data(), need), s.pitch(), LightTables, FullyLitLightTable, FullyDarkLightTable);
}

void RenderPieceColumn(const Surface &s, uint16_t piece)
{
	const Lightmap lightmap = MakeFullLightmap(s);
	const uint8_t *tbl = FullyLitLightTable;
	const MICROS &micros = DPieceMicros[piece];
	const bool isFloor = !HasAnyOf(SOLData[piece], TileProperties::Solid | TileProperties::BlockMissile);
	Point p { 0, s.h() - 1 };
	for (int i = 0; i < 2; i++) {
		const LevelCelBlock block { micros.mt[i] };
		if (!block.hasValue())
			continue;
		const TileType type = block.type();
		if (isFloor || type == TileType::TransparentSquare)
			continue; // floor (drawn separately) or foliage
		RenderTile(s, lightmap, p + Displacement { i * 32, 0 }, pDungeonCels.get(), block, MaskType::Solid, tbl);
	}
	p.y -= 32;
	for (uint_fast8_t i = 2; i < MicroTileLen; i += 2) {
		for (int side = 0; side < 2; side++) {
			const LevelCelBlock block { micros.mt[i + side] };
			if (block.hasValue())
				RenderTile(s, lightmap, p + Displacement { side * 32, 0 }, pDungeonCels.get(), block, MaskType::Solid, tbl);
		}
		p.y -= 32;
	}
}

void RenderPieceFloor(const Surface &s, uint16_t piece)
{
	const Lightmap lightmap = MakeFullLightmap(s);
	const uint8_t *tbl = FullyLitLightTable;
	const MICROS &micros = DPieceMicros[piece];
	const Point p { 0, s.h() - 1 };
	const LevelCelBlock left { micros.mt[0] };
	if (left.hasValue())
		RenderTileFrame(s, lightmap, p, TileType::LeftTriangle, GetDunFrame(pDungeonCels.get(), left.frame()), DunFrameTriangleHeight, MaskType::Solid, tbl);
	const LevelCelBlock right { micros.mt[1] };
	if (right.hasValue())
		RenderTileFrame(s, lightmap, p + Displacement { 32, 0 }, TileType::RightTriangle, GetDunFrame(pDungeonCels.get(), right.frame()), DunFrameTriangleHeight, MaskType::Solid, tbl);
}

const PieceTextures &GetPieceTextures(uint16_t piece)
{
	auto it = PieceCache.find(piece);
	if (it != PieceCache.end())
		return it->second;
	PieceTextures &t = PieceCache[piece];
	const int columnH = static_cast<int>(MicroTileLen / 2) * 32 + 16;
	EnsureScratch(columnH);

	// Render twice over different backgrounds: pixels that differ are transparent.
	FillSurface(*ScratchFloorA, 0);
	FillSurface(*ScratchFloorB, 255);
	RenderPieceFloor(*ScratchFloorA, piece);
	RenderPieceFloor(*ScratchFloorB, piece);
	{
		const int top = ScratchFloorA->h() - 1 - 31;
		int opaque = 0;
		for (int j = 0; j < TexW; j++) {
			for (int i = 0; i < TexW; i++) {
				// Inset a little: the diamond's outermost pixels are anti-aliased against nothing.
				const float u = 0.05f + 0.9f * (i + 0.5f) / TexW, v = 0.05f + 0.9f * (j + 0.5f) / TexW;
				const int sx = std::clamp(static_cast<int>(32.0f + 32.0f * (u - v)), 0, 63);
				const int sy = std::clamp(static_cast<int>(top + 16.0f * (u + v)), 0, ScratchFloorA->h() - 1);
				const uint8_t a = (*ScratchFloorA)[Point { sx, sy }];
				const uint8_t b = (*ScratchFloorB)[Point { sx, sy }];
				t.floor[j * TexW + i] = a;
				if (a == b)
					opaque++;
			}
		}
		t.hasFloor = opaque > TexW * TexW / 2;
	}

	FillSurface(*ScratchA, 0);
	FillSurface(*ScratchB, 255);
	RenderPieceColumn(*ScratchA, piece);
	RenderPieceColumn(*ScratchB, piece);
	const int baseY = ScratchA->h() - 1;
	t.wallH = columnH - 16;
	t.west.assign(static_cast<size_t>(t.wallH) * TexW, 0xFFFF);
	t.north.assign(static_cast<size_t>(t.wallH) * TexW, 0xFFFF);
	int westCount = 0, northCount = 0;
	for (int c = 0; c < TexW; c++) {
		const float s = (c + 0.5f) / TexW;
		const float westBase = baseY - 15.5f - 15.5f * s;
		const float northBase = baseY - 31.0f + 15.5f * s;
		for (int h = 0; h < t.wallH; h++) {
			const int wy = static_cast<int>(std::lround(westBase - h));
			const int ny = static_cast<int>(std::lround(northBase - h));
			if (wy >= 0 && wy < ScratchA->h()) {
				const uint8_t a = (*ScratchA)[Point { c, wy }];
				const uint8_t b = (*ScratchB)[Point { c, wy }];
				if (a == b) {
					t.west[h * TexW + c] = a;
					if (h > 2)
						westCount++;
				}
			}
			if (ny >= 0 && ny < ScratchA->h()) {
				const uint8_t a = (*ScratchA)[Point { 32 + c, ny }];
				const uint8_t b = (*ScratchB)[Point { 32 + c, ny }];
				if (a == b) {
					t.north[h * TexW + c] = a;
					if (h > 2)
						northCount++;
				}
			}
		}
	}
	t.hasWest = westCount > 24;
	t.hasNorth = northCount > 24;
	// Trim the empty top so tall columns with short walls stay cheap.
	int topRow = 0;
	for (int h = 0; h < t.wallH; h++)
		for (int c = 0; c < TexW; c++)
			if (t.west[h * TexW + c] != 0xFFFF || t.north[h * TexW + c] != 0xFFFF)
				topRow = h;
	t.wallH = std::max(1, topRow + 1);
	t.west.resize(static_cast<size_t>(t.wallH) * TexW);
	t.north.resize(static_cast<size_t>(t.wallH) * TexW);
	return t;
}

void CheckCacheValid()
{
	const int levelKey = (setlevel ? 1000 : 0) + currlevel * 4 + static_cast<int>(leveltype);
	if (CachedCels != pDungeonCels.get() || CachedLevel != levelKey) {
		PieceCache.clear();
		CachedCels = pDungeonCels.get();
		CachedLevel = levelKey;
		for (Smoothed &s : MonsterSmooth)
			s.valid = false;
		PlayerSmooth.valid = false;
	}
}

// ---------------------------------------------------------------------------------------------
// Frame buffer

struct Frame {
	std::vector<uint8_t> color;
	std::vector<float> depth;
	int w = 0, h = 0;
};
Frame Fb;

constexpr uint8_t Bayer4[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

float LightAt(float wx, float wy)
{
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
	const float d = light + (Bayer4[py & 3][px & 3] - 7.5f) / 16.0f;
	const int level = std::clamp(static_cast<int>(d + 0.5f), 0, 15);
	return LightTables[level][color];
}

struct CamVert {
	V3 c;         // camera space: x right, y up, z forward
	float u, v;   // texture
	float wx, wy; // world position
	float light = 0;
};

struct ScreenVert {
	float sx, sy, iz, uz, vz, lz;
};

CamVert ToCamera(const Camera &cam, V3 p, float u, float v)
{
	const V3 rel = p - cam.pos;
	return { { Dot(rel, cam.right), Dot(rel, cam.up), Dot(rel, cam.fwd) }, u, v, p.x, p.y };
}

ScreenVert Project(const Camera &cam, const CamVert &v)
{
	const float iz = 1.0f / v.c.z;
	return { cam.cx + cam.focal * v.c.x * iz, cam.cy - cam.focal * v.c.y * iz, iz, v.u * iz, v.v * iz, v.light * iz };
}

struct WallTex {
	const uint16_t *pixels;
	int height;
};

void RasterTriangle(const ScreenVert &a, const ScreenVert &b, const ScreenVert &c, const WallTex &tex, bool fade)
{
	const float area = (b.sx - a.sx) * (c.sy - a.sy) - (b.sy - a.sy) * (c.sx - a.sx);
	if (std::fabs(area) < 1e-6f)
		return;
	const float invArea = 1.0f / area;
	const int minX = std::max(0, static_cast<int>(std::floor(std::min({ a.sx, b.sx, c.sx }))));
	const int maxX = std::min(Fb.w - 1, static_cast<int>(std::ceil(std::max({ a.sx, b.sx, c.sx }))));
	const int minY = std::max(0, static_cast<int>(std::floor(std::min({ a.sy, b.sy, c.sy }))));
	const int maxY = std::min(Fb.h - 1, static_cast<int>(std::ceil(std::max({ a.sy, b.sy, c.sy }))));
	// Barycentric weights are linear in x: step them instead of recomputing.
	const float dw0dx = -(c.sy - b.sy) * invArea;
	const float dw1dx = -(a.sy - c.sy) * invArea;
	for (int y = minY; y <= maxY; y++) {
		const float py = y + 0.5f;
		const float px0 = minX + 0.5f;
		float w0 = ((b.sx - px0) * (c.sy - py) - (b.sy - py) * (c.sx - px0)) * invArea;
		float w1 = ((c.sx - px0) * (a.sy - py) - (c.sy - py) * (a.sx - px0)) * invArea;
		bool entered = false;
		for (int x = minX; x <= maxX; x++, w0 += dw0dx, w1 += dw1dx) {
			const float w2 = 1.0f - w0 - w1;
			if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) {
				if (entered)
					break; // convex: once out again, the rest of the row is outside
				continue;
			}
			entered = true;
			if (fade && Bayer4[y & 3][x & 3] < 9)
				continue;
			const float iz = w0 * a.iz + w1 * b.iz + w2 * c.iz;
			const float z = 1.0f / iz;
			const size_t idx = static_cast<size_t>(y) * Fb.w + x;
			if (z >= Fb.depth[idx])
				continue;
			const float u = (w0 * a.uz + w1 * b.uz + w2 * c.uz) * z;
			const float v = (w0 * a.vz + w1 * b.vz + w2 * c.vz) * z;
			const int tu = std::clamp(static_cast<int>(u), 0, TexW - 1);
			const int tv = std::clamp(static_cast<int>(v), 0, tex.height - 1);
			const uint16_t texel = tex.pixels[tv * TexW + tu];
			if (texel == 0xFFFF)
				continue;
			const float light = (w0 * a.lz + w1 * b.lz + w2 * c.lz) * z;
			Fb.color[idx] = Shade(static_cast<uint8_t>(texel), light, x, y);
			Fb.depth[idx] = z;
		}
	}
}

/** Upright textured panel from (ax, ay) to (bx, by), clipped against the near plane. */
void DrawPanel(const Camera &cam, float ax, float ay, float bx, float by, const WallTex &tex, bool fade, float lightNudgeX, float lightNudgeY)
{
	const float top = tex.height / PxPerUnitZ;
	std::array<CamVert, 4> quad = { {
	    ToCamera(cam, { ax, ay, 0 }, 0.0f, 0.0f),
	    ToCamera(cam, { bx, by, 0 }, static_cast<float>(TexW), 0.0f),
	    ToCamera(cam, { bx, by, top }, static_cast<float>(TexW), static_cast<float>(tex.height)),
	    ToCamera(cam, { ax, ay, top }, 0.0f, static_cast<float>(tex.height)),
	} };
	// Light a hair in front of the panel so walls take the light of the room they face.
	const float lightA = LightAt(ax + lightNudgeX, ay + lightNudgeY);
	const float lightB = LightAt(bx + lightNudgeX, by + lightNudgeY);
	quad[0].light = quad[3].light = lightA;
	quad[1].light = quad[2].light = lightB;
	// Clip against the near plane.
	std::array<CamVert, 8> poly {};
	int count = 0;
	for (int i = 0; i < 4; i++) {
		const CamVert &p = quad[i];
		const CamVert &q = quad[(i + 1) % 4];
		const bool pin = p.c.z >= NearPlane, qin = q.c.z >= NearPlane;
		if (pin)
			poly[count++] = p;
		if (pin != qin) {
			const float t = (NearPlane - p.c.z) / (q.c.z - p.c.z);
			CamVert m;
			m.c = p.c + (q.c - p.c) * t;
			m.u = p.u + (q.u - p.u) * t;
			m.v = p.v + (q.v - p.v) * t;
			m.wx = p.wx + (q.wx - p.wx) * t;
			m.wy = p.wy + (q.wy - p.wy) * t;
			m.light = p.light + (q.light - p.light) * t;
			poly[count++] = m;
		}
	}
	if (count < 3)
		return;
	std::array<ScreenVert, 8> sv {};
	for (int i = 0; i < count; i++)
		sv[i] = Project(cam, poly[i]);
	for (int i = 1; i + 1 < count; i++)
		RasterTriangle(sv[0], sv[i], sv[i + 1], tex, fade);
}

bool SegmentsIntersect(float ax, float ay, float bx, float by, float cx, float cy, float dx, float dy)
{
	const auto cross = [](float ux, float uy, float vx, float vy) { return ux * vy - uy * vx; };
	const float d1 = cross(bx - ax, by - ay, cx - ax, cy - ay);
	const float d2 = cross(bx - ax, by - ay, dx - ax, dy - ay);
	const float d3 = cross(dx - cx, dy - cy, ax - cx, ay - cy);
	const float d4 = cross(dx - cx, dy - cy, bx - cx, by - cy);
	return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
}

// ---------------------------------------------------------------------------------------------
// Sprites

struct Billboard {
	float x, y;
	float depth;
	ClxSprite sprite;
	const uint8_t *trn = nullptr;
	bool lit = true;
	float lift = 0;
	float scale = 1;
};

std::optional<OwnedSurface> SpriteScratch;

void DrawBillboard(const Camera &cam, const Billboard &bb)
{
	const int sw = bb.sprite.width(), sh = bb.sprite.height();
	if (sw <= 0 || sh <= 0 || sw > 400 || sh > 400)
		return;
	const CamVert foot = ToCamera(cam, { bb.x, bb.y, bb.lift }, 0, 0);
	if (foot.c.z < NearPlane + 0.2f)
		return;
	const float worldH = sh / PxPerUnitZ * bb.scale;
	const CamVert head = ToCamera(cam, { bb.x, bb.y, bb.lift + worldH }, 0, 0);
	const ScreenVert f = Project(cam, foot);
	const ScreenVert h = Project(cam, head);
	const float screenH = f.sy - h.sy;
	if (screenH < 1 || screenH > Fb.h * 4)
		return;
	const float screenW = screenH * (sw / static_cast<float>(sh)) * (PxPerUnitZ / PxPerUnitW);
	if (!SpriteScratch)
		SpriteScratch.emplace(400, 400);
	OwnedSurface &scratch = *SpriteScratch;
	for (int y = 0; y < sh; y++)
		std::memset(&scratch[Point { 0, y }], 0, sw);
	if (bb.trn != nullptr)
		RenderClxSpriteWithTRN(scratch, bb.sprite, { 0, 0 }, bb.trn);
	else
		RenderClxSprite(scratch, bb.sprite, { 0, 0 });
	const float light = LightAt(bb.x, bb.y);
	const int left = static_cast<int>(std::floor(f.sx - screenW * 0.5f));
	const int topY = static_cast<int>(std::floor(f.sy - screenH));
	const int pw = std::max(1, static_cast<int>(std::ceil(screenW)));
	const int ph = std::max(1, static_cast<int>(std::ceil(screenH)));
	const float depth = foot.c.z - 0.35f;
	for (int dx = 0; dx < pw; dx++) {
		const int px = left + dx;
		if (px < 0 || px >= Fb.w)
			continue;
		const int srcX = std::min(sw - 1, dx * sw / pw);
		for (int dy = 0; dy < ph; dy++) {
			const int py = topY + dy;
			if (py < 0 || py >= Fb.h)
				continue;
			const size_t idx = static_cast<size_t>(py) * Fb.w + px;
			if (depth >= Fb.depth[idx])
				continue;
			const uint8_t c = scratch[Point { srcX, std::min(sh - 1, dy * sh / ph) }];
			if (c == 0)
				continue;
			Fb.color[idx] = bb.lit ? Shade(c, light, px, py) : c;
		}
	}
}

/** Direction from a grid angle (S = 45 degrees, increasing clockwise on screen). */
Direction AngleToDirection(float angle)
{
	int idx = static_cast<int>(std::lround((angle * 180.0f / Pi - 45.0f) / 45.0f));
	idx = ((idx % 8) + 8) % 8;
	return static_cast<Direction>(idx);
}

/** The sprite direction that shows an actor facing `facing` as seen along `viewAngle`. */
Direction ViewRelativeDirection(Direction facing, float viewAngle)
{
	const Direction view = AngleToDirection(viewAngle);
	const int rot = (static_cast<int>(view) - static_cast<int>(Direction::North) + 8) % 8;
	return static_cast<Direction>(((static_cast<int>(facing) - rot) % 8 + 8) % 8);
}

ClxSprite MonsterSpriteFor(const Monster &monster, float viewAngle)
{
	ClxSprite sprite = monster.animInfo.currentSprite();
	const Direction wanted = ViewRelativeDirection(monster.direction, viewAngle);
	for (int g = 0; g < 8; g++) {
		const auto graphic = static_cast<MonsterGraphic>(g);
		const AnimStruct &anim = monster.type().getAnimData(graphic);
		if (!anim.sprites)
			continue;
		const OptionalClxSpriteList current = anim.spritesForDirection(monster.direction);
		if (current && monster.animInfo.sprites && (*current).data() == (*monster.animInfo.sprites).data()) {
			const OptionalClxSpriteList rotated = anim.spritesForDirection(wanted);
			if (rotated) {
				const uint32_t frame = std::min<uint32_t>(monster.animInfo.getFrameToUseForRendering(), (*rotated).numSprites() - 1);
				sprite = (*rotated)[frame];
			}
			break;
		}
	}
	return sprite;
}

ClxSprite PlayerSpriteFor(const Player &player, float viewAngle)
{
	ClxSprite sprite = player.AnimInfo.currentSprite();
	const player_graphic graphic = player.getGraphic();
	const PlayerAnimationData &data = player.AnimationData[static_cast<size_t>(graphic)];
	if (!data.sprites)
		return sprite;
	const Direction wanted = ViewRelativeDirection(player._pdir, viewAngle);
	const ClxSpriteList list = data.spritesForDirection(wanted);
	const uint32_t frame = std::min<uint32_t>(player.AnimInfo.getFrameToUseForRendering(), list.numSprites() - 1);
	return list[frame];
}

void CollectBillboards(const Camera &cam, std::vector<Billboard> &out)
{
	const auto add = [&](float x, float y, ClxSprite sprite, float scale, float lift, const uint8_t *trn, bool lit) {
		const V3 rel = V3 { x, y, 0 } - cam.pos;
		const float depth = Dot(rel, cam.fwd);
		if (depth < 0.3f || depth > ViewRadius + 6)
			return;
		Billboard bb { x, y, depth, sprite };
		bb.scale = scale;
		bb.lift = lift;
		bb.trn = trn;
		bb.lit = lit;
		out.push_back(bb);
	};
	const auto viewAngleTo = [&](float x, float y) { return std::atan2(y - cam.pos.y, x - cam.pos.x); };

	// The hero.
	if (MyPlayer->AnimInfo.sprites)
		add(PlayerSmooth.x, PlayerSmooth.y, PlayerSpriteFor(*MyPlayer, viewAngleTo(PlayerSmooth.x, PlayerSmooth.y)), 1.0f, -6 / PxPerUnitZ, nullptr, true);

	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const unsigned id = ActiveMonsters[i];
		const Monster &monster = Monsters[id];
		if (monster.isInvalid || !monster.animInfo.sprites || !MonsterSmooth[id].valid)
			continue;
		if (!InDungeonBounds(monster.position.tile) || !IsTileLit(monster.position.tile))
			continue;
		const float x = MonsterSmooth[id].x, y = MonsterSmooth[id].y;
		add(x, y, MonsterSpriteFor(monster, viewAngleTo(x, y)), 1.0f, -6 / PxPerUnitZ, monster.isUnique() ? monster.uniqueMonsterTRN.get() : nullptr, true);
	}
	for (int i = 0; i < ActiveObjectCount; i++) {
		const Object &object = Objects[ActiveObjects[i]];
		if (object._otype == OBJ_NULL || !object._oAnimData || object.isDoor())
			continue;
		if (object._oAnimFrame < 1 || object._oAnimFrame > (*object._oAnimData).numSprites())
			continue;
		if (!IsTileLit(object.position) && leveltype != DTYPE_TOWN)
			continue;
		add(object.position.x + 0.5f, object.position.y + 0.5f, object.currentSprite(), 1.0f, -6 / PxPerUnitZ, nullptr, true);
	}
	for (int i = 0; i < ActiveItemCount; i++) {
		const Item &item = Items[ActiveItems[i]];
		if (item.isEmpty() || !item.AnimInfo.sprites)
			continue;
		add(item.position.x + 0.5f, item.position.y + 0.5f, item.AnimInfo.currentSprite(), 0.8f, 0.0f, nullptr, true);
	}
	for (const Missile &missile : Missiles) {
		if (!missile._miDrawFlag || !missile._miAnimData)
			continue;
		if (missile._miAnimFrame < 1 || static_cast<uint32_t>(missile._miAnimFrame) > (*missile._miAnimData).numSprites())
			continue;
		float x, y;
		TileToWorld(missile.position.tile, missile.position.offset, x, y);
		add(x, y, (*missile._miAnimData)[missile._miAnimFrame - 1], 1.0f, 0.4f, missile._miUniqTrans != 0 ? Monsters[missile._misource].uniqueMonsterTRN.get() : nullptr, missile._miLightFlag);
	}
	if (leveltype == DTYPE_TOWN) {
		for (const Towner &towner : Towners) {
			if (!towner.anim)
				continue;
			add(towner.position.x + 0.5f, towner.position.y + 0.5f, towner.currentSprite(), 1.0f, -6 / PxPerUnitZ, nullptr, true);
		}
	}
}

uint8_t NearestIndex(int r, int g, int b)
{
	int best = 0;
	int bestDist = 1 << 30;
	for (int i = 0; i < 256; i++) {
		const SDL_Color &c = logical_palette[i];
		const int d = (c.r - r) * (c.r - r) * 3 + (c.g - g) * (c.g - g) * 4 + (c.b - b) * (c.b - b) * 2;
		if (d < bestDist) {
			bestDist = d;
			best = i;
		}
	}
	return static_cast<uint8_t>(best);
}

Camera BuildCamera(int w, int h, float focalScale)
{
	Camera cam;
	const float cp = std::cos(Pitch), sp = std::sin(Pitch);
	cam.fwd = { cp * std::cos(Yaw), cp * std::sin(Yaw), -sp };
	cam.right = { -std::sin(Yaw), std::cos(Yaw), 0 };
	cam.up = { cam.fwd.y * cam.right.z - cam.fwd.z * cam.right.y, cam.fwd.z * cam.right.x - cam.fwd.x * cam.right.z, cam.fwd.x * cam.right.y - cam.fwd.y * cam.right.x };
	const V3 target { PlayerSmooth.x, PlayerSmooth.y, 1.05f };
	cam.pos = target - cam.fwd * Distance;
	cam.focal = w * 0.5f / std::tan(62.0f * Pi / 180.0f * 0.5f) * focalScale;
	cam.cx = w * 0.5f;
	cam.cy = h * 0.46f;
	return cam;
}

} // namespace

bool IsOrbitViewActive()
{
	return IsActionMode() && *GetOptions().Gameplay.actionCamera3D && leveltype >= DTYPE_TOWN;
}

void OrbitCameraReset()
{
	Yaw = -0.75f * Pi;
	Pitch = 0.42f;
	ManualTurnTimer = 0;
	PlayerSmooth.valid = false;
}

float OrbitCameraYaw()
{
	return Yaw;
}

void OrbitCameraUpdate(float dt, float turnX, float turnY, std::optional<Point> lockTarget, bool moving, float moveAngle, bool manualTurn)
{
	Yaw = WrapAngle(Yaw + turnX);
	Pitch = std::clamp(Pitch + turnY, 0.12f, 1.15f);
	ManualTurnTimer = manualTurn ? 0.8f : std::max(0.0f, ManualTurnTimer - dt);
	if (lockTarget && PlayerSmooth.valid) {
		// Keep hero and target in frame: look from behind the hero toward the enemy.
		const float desired = std::atan2(lockTarget->y + 0.5f - PlayerSmooth.y, lockTarget->x + 0.5f - PlayerSmooth.x);
		Yaw = LerpAngle(Yaw, desired, dt * 5.0f);
		Pitch += (0.36f - Pitch) * std::min(1.0f, dt * 3.0f);
	} else if (moving && ManualTurnTimer <= 0) {
		// Lazy follow: swing behind the hero when running sideways, not when running at the camera.
		const float diff = WrapAngle(moveAngle - Yaw);
		const float sideways = std::fabs(std::sin(diff));
		Yaw = WrapAngle(Yaw + diff * std::min(1.0f, dt * 1.6f * sideways));
	}
}

std::optional<Point> OrbitProject(float worldX, float worldY, float height, const Surface &out)
{
	if (LastOutW != out.w() || LastOutH != out.h())
		return std::nullopt;
	const Camera &cam = LastCamera;
	const CamVert v = ToCamera(cam, { worldX, worldY, height }, 0, 0);
	if (v.c.z < NearPlane)
		return std::nullopt;
	const ScreenVert s = Project(cam, v);
	const Point p { static_cast<int>(s.sx) * LastScale, static_cast<int>(s.sy) * LastScale };
	if (p.x < 0 || p.y < 0 || p.x >= out.w() || p.y >= out.h())
		return std::nullopt;
	return p;
}

std::optional<OrbitScreenPoint> OrbitProjectEx(float worldX, float worldY, float height, const Surface &out)
{
	if (LastOutW != out.w() || LastOutH != out.h())
		return std::nullopt;
	const Camera &cam = LastCamera;
	const CamVert v = ToCamera(cam, { worldX, worldY, height }, 0, 0);
	if (v.c.z < NearPlane)
		return std::nullopt;
	const ScreenVert s = Project(cam, v);
	return OrbitScreenPoint {
		Point { static_cast<int>(s.sx * LastScale), static_cast<int>(s.sy * LastScale) },
		v.c.z,
		cam.focal / v.c.z * LastScale,
	};
}

bool OrbitSceneVisible(Point screen, float depth)
{
	const int x = screen.x / LastScale, y = screen.y / LastScale;
	if (x < 0 || y < 0 || x >= Fb.w || y >= Fb.h)
		return false;
	return depth < Fb.depth[static_cast<size_t>(y) * Fb.w + x];
}

bool OrbitMonsterRenderPosition(int monsterId, float &x, float &y)
{
	if (monsterId < 0 || monsterId >= static_cast<int>(MaxMonsters) || !MonsterSmooth[monsterId].valid)
		return false;
	x = MonsterSmooth[monsterId].x;
	y = MonsterSmooth[monsterId].y;
	return true;
}

void OrbitClearCache()
{
	PieceCache.clear();
	CachedCels = nullptr;
}

void DrawOrbitView(const Surface &out)
{
#ifdef _DEBUG
	const uint64_t perfStart = SDL_GetPerformanceCounter();
	static int perfFrames = 0;
	static double perfTotal = 0;
#endif
	CheckCacheValid();
	UpdateSmoothing();

	// Low internal resolution (about 400 pixels wide), scaled up: crisp pixel art, affordable in software.
	const int scale = std::max(1, static_cast<int>(std::lround(out.w() / 400.0)));
	Fb.w = std::max(64, out.w() / scale);
	Fb.h = std::max(48, out.h() / scale);
	Fb.color.assign(static_cast<size_t>(Fb.w) * Fb.h, 0);
	Fb.depth.assign(static_cast<size_t>(Fb.w) * Fb.h, 1e9f);

#ifdef _DEBUG
	if (const char *dbgYaw = std::getenv("DEVILUTIONX_DEV_ORBITYAW"); dbgYaw != nullptr)
		Yaw = static_cast<float>(std::atof(dbgYaw)) * Pi / 180.0f;
	if (const char *dbgPitch = std::getenv("DEVILUTIONX_DEV_ORBITPITCH"); dbgPitch != nullptr)
		Pitch = static_cast<float>(std::atof(dbgPitch)) * Pi / 180.0f;
#endif
	const Camera cam = BuildCamera(Fb.w, Fb.h, 1.0f);
	LastCamera = cam;
	LastOutW = out.w();
	LastOutH = out.h();
	LastScale = scale;

	const bool town = leveltype == DTYPE_TOWN;
	const uint8_t skyTop = town ? NearestIndex(70, 90, 130) : 0;
	const uint8_t skyLow = town ? NearestIndex(150, 160, 175) : 0;

	// Cache the textures of the visible window.
	const int px0 = static_cast<int>(PlayerSmooth.x), py0 = static_cast<int>(PlayerSmooth.y);
	constexpr int WindowSize = ViewRadius * 2 + 1;
	std::array<const PieceTextures *, WindowSize * WindowSize> window {};
	for (int dx = -ViewRadius; dx <= ViewRadius; dx++) {
		for (int dy = -ViewRadius; dy <= ViewRadius; dy++) {
			const int x = px0 + dx, y = py0 + dy;
			if (x < 0 || y < 0 || x >= MAXDUNX || y >= MAXDUNY)
				continue;
			window[(dx + ViewRadius) * WindowSize + (dy + ViewRadius)] = &GetPieceTextures(dPiece[x][y]);
		}
	}
	const auto texAt = [&](int x, int y) -> const PieceTextures * {
		const int dx = x - px0, dy = y - py0;
		if (dx < -ViewRadius || dx > ViewRadius || dy < -ViewRadius || dy > ViewRadius)
			return nullptr;
		return window[(dx + ViewRadius) * WindowSize + (dy + ViewRadius)];
	};

	// Floor (and sky in town). Along a screen row the floor is at constant depth, so the
	// world position steps linearly; light is sampled every few pixels and interpolated.
	for (int y = 0; y < Fb.h; y++) {
		const float b = (y + 0.5f - cam.cy) / cam.focal;
		const float dz = cam.fwd.z - cam.up.z * b;
		uint8_t *row = &Fb.color[static_cast<size_t>(y) * Fb.w];
		float *depthRow = &Fb.depth[static_cast<size_t>(y) * Fb.w];
		if (dz > -1e-4f) {
			if (town) {
				for (int x = 0; x < Fb.w; x++) {
					const float k = std::clamp(dz * 3.0f + (Bayer4[y & 3][x & 3] - 7.5f) / 30.0f, 0.0f, 1.0f);
					row[x] = k > 0.35f ? skyTop : skyLow;
				}
			}
			continue;
		}
		const float t = -cam.pos.z / dz;
		if (t > ViewRadius + 4)
			continue;
		const float a0 = (0.5f - cam.cx) / cam.focal;
		const float stepA = 1.0f / cam.focal;
		const V3 base = cam.fwd - cam.up * b;
		float wx = cam.pos.x + (base.x + cam.right.x * a0) * t;
		float wy = cam.pos.y + (base.y + cam.right.y * a0) * t;
		const float stepX = cam.right.x * stepA * t;
		const float stepY = cam.right.y * stepA * t;
		constexpr int LightSpan = 4;
		float lightStart = LightAt(wx, wy);
		float lightEnd = lightStart;
		for (int x = 0; x < Fb.w; x++, wx += stepX, wy += stepY) {
			if ((x % LightSpan) == 0) {
				lightStart = lightEnd;
				lightEnd = LightAt(wx + stepX * LightSpan, wy + stepY * LightSpan);
				if (x == 0)
					lightStart = LightAt(wx, wy);
			}
			const int cxl = static_cast<int>(std::floor(wx)), cyl = static_cast<int>(std::floor(wy));
			const PieceTextures *tex = texAt(cxl, cyl);
			if (tex == nullptr || !tex->hasFloor)
				continue;
			const int tu = std::clamp(static_cast<int>((wx - cxl) * TexW), 0, TexW - 1);
			const int tv = std::clamp(static_cast<int>((wy - cyl) * TexW), 0, TexW - 1);
			const float light = lightStart + (lightEnd - lightStart) * ((x % LightSpan) / static_cast<float>(LightSpan));
			row[x] = Shade(tex->floor[tv * TexW + tu], light, x, y);
			depthRow[x] = t;
		}
	}

	// Walls: each cell's column art stands on its west and north edges.
	const float camX = cam.pos.x, camY = cam.pos.y;
	for (int dx = -ViewRadius; dx <= ViewRadius; dx++) {
		for (int dy = -ViewRadius; dy <= ViewRadius; dy++) {
			const int x = px0 + dx, y = py0 + dy;
			const PieceTextures *tex = texAt(x, y);
			if (tex == nullptr || (!tex->hasWest && !tex->hasNorth))
				continue;
			const float cellDist = std::hypot(x + 0.5f - camX, y + 0.5f - camY);
			const bool nearCamera = cellDist < 1.3f;
			if (tex->hasWest) {
				// From (x, y+1) to (x, y).
				const bool fade = nearCamera || SegmentsIntersect(camX, camY, PlayerSmooth.x, PlayerSmooth.y, static_cast<float>(x), y + 1.0f, static_cast<float>(x), static_cast<float>(y));
				DrawPanel(cam, static_cast<float>(x), y + 1.0f, static_cast<float>(x), static_cast<float>(y), { tex->west.data(), tex->wallH }, fade, 0.08f, 0.0f);
			}
			if (tex->hasNorth) {
				// From (x, y) to (x+1, y).
				const bool fade = nearCamera || SegmentsIntersect(camX, camY, PlayerSmooth.x, PlayerSmooth.y, static_cast<float>(x), static_cast<float>(y), x + 1.0f, static_cast<float>(y));
				DrawPanel(cam, static_cast<float>(x), static_cast<float>(y), x + 1.0f, static_cast<float>(y), { tex->north.data(), tex->wallH }, fade, 0.0f, 0.08f);
			}
		}
	}

	// Sprites, far to near.
	std::vector<Billboard> boards;
	boards.reserve(128);
	CollectBillboards(cam, boards);
	std::sort(boards.begin(), boards.end(), [](const Billboard &a, const Billboard &b) { return a.depth > b.depth; });
	for (const Billboard &bb : boards)
		DrawBillboard(cam, bb);

	// Present, scaled up with nearest neighbour.
	std::vector<int> xmap(out.w());
	for (int x = 0; x < out.w(); x++)
		xmap[x] = std::min(Fb.w - 1, x / scale);
	for (int y = 0; y < out.h(); y++) {
		const uint8_t *src = &Fb.color[static_cast<size_t>(std::min(Fb.h - 1, y / scale)) * Fb.w];
		uint8_t *dst = &out[Point { 0, y }];
		for (int x = 0; x < out.w(); x++)
			dst[x] = src[xmap[x]];
	}
#ifdef _DEBUG
	perfTotal += static_cast<double>(SDL_GetPerformanceCounter() - perfStart) * 1000.0 / static_cast<double>(SDL_GetPerformanceFrequency());
	if (++perfFrames == 120) {
		LogVerbose("orbit view: {:.2f} ms per frame ({}x{} internal)", perfTotal / perfFrames, Fb.w, Fb.h);
		perfFrames = 0;
		perfTotal = 0;
	}
#endif
}

} // namespace devilution
