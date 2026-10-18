#include "engine/render/tile_decor.hpp"

#include <array>
#include <cstring>

#include "engine/light_tables.hpp"
#include "engine/palette.h"
#include "engine/render/dun_render.hpp"
#include "engine/render/tile_hue.hpp"
#include "levels/dun_tile_data.hpp"
#include "options.h"

namespace devilution {

namespace {

enum Shade : uint8_t {
	Green,
	DarkGreen,
	Red,
	DarkRed,
	Yellow,
	Orange,
	White,
	Purple,
	Blue,
	Pink,
	Wood,
	WoodLight,
	WoodDark,
	ShadeCount,
};

constexpr std::array<std::array<uint8_t, 3>, ShadeCount> ShadeRgb { {
    { 70, 120, 45 },  // green stems
    { 35, 70, 30 },   // dark green leaves
    { 205, 35, 30 },  // red
    { 120, 20, 20 },  // dark red
    { 235, 205, 50 }, // yellow
    { 230, 125, 35 }, // orange
    { 235, 235, 225 },// white
    { 150, 70, 175 }, // purple
    { 80, 100, 215 }, // blue
    { 230, 130, 170 },// pink
    { 125, 82, 45 },  // wood
    { 170, 120, 70 }, // sunlit wood
    { 62, 40, 22 },   // wood in shadow
} };

std::array<uint8_t, ShadeCount> ShadeIndex {};
std::array<SDL_Color, 256> ShadesForPalette {};
bool ShadesReady = false;

/** Palette entries nearest to the flower colours (recomputed when the palette changes). */
void RefreshShades()
{
	bool same = ShadesReady;
	for (size_t i = 0; same && i < logical_palette.size(); i++)
		same = logical_palette[i].r == ShadesForPalette[i].r && logical_palette[i].g == ShadesForPalette[i].g && logical_palette[i].b == ShadesForPalette[i].b;
	if (same)
		return;
	ShadesForPalette = logical_palette;
	ShadesReady = true;
	for (int s = 0; s < ShadeCount; s++) {
		int best = 1, bestDistance = INT32_MAX;
		for (int i = 1; i < 256; i++) {
			const SDL_Color &p = logical_palette[i];
			const int dr = p.r - ShadeRgb[s][0], dg = p.g - ShadeRgb[s][1], db = p.b - ShadeRgb[s][2];
			const int distance = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
			if (distance < bestDistance) {
				bestDistance = distance;
				best = i;
			}
		}
		ShadeIndex[s] = static_cast<uint8_t>(best);
	}
}

struct PatchStyle {
	std::string_view name;
	std::array<Shade, 3> petals; // picked at random per flower
	int minFlowers, maxFlowers;
	int bloom;   // 0 small, 1 medium, 2 large, 3 spike (heather)
	Shade centre;
	int tufts;   // extra grass tufts
};

constexpr std::array<PatchStyle, FlowerPatchCount> Styles { {
    { "Poppies", { Red, Red, DarkRed }, 7, 10, 1, DarkRed, 2 },
    { "Buttercups", { Yellow, Yellow, Orange }, 9, 13, 0, Orange, 2 },
    { "Daisies", { White, White, White }, 8, 11, 1, Yellow, 2 },
    { "Heather", { Purple, Purple, Pink }, 9, 13, 3, Purple, 1 },
    { "Bluebells", { Blue, Blue, Purple }, 8, 12, 0, Blue, 2 },
    { "Wildflowers", { Red, Yellow, White }, 9, 13, 1, Yellow, 3 },
    { "Blossoms", { Pink, Pink, White }, 6, 9, 2, Yellow, 2 },
    { "Meadow", { Purple, Yellow, White }, 14, 19, 0, Yellow, 6 },
} };

struct Rng {
	uint32_t state;
	uint32_t Next()
	{
		state = state * 1664525u + 1013904223u;
		return state >> 8;
	}
	int Between(int lo, int hi) { return lo + static_cast<int>(Next() % static_cast<uint32_t>(hi - lo + 1)); }
	float Unit() { return static_cast<float>(Next() & 0xFFFF) / 65535.0f; }
};

void Put(const Surface &out, Point p, Shade shade, const uint8_t *tbl)
{
	if (out.InBounds(p))
		*out.at(p.x, p.y) = tbl[ShadeIndex[shade]];
}

} // namespace

std::string_view FlowerPatchName(int patch)
{
	return patch >= 0 && patch < FlowerPatchCount ? Styles[patch].name : std::string_view {};
}

void DrawFlowerPatch(const Surface &out, Point position, int patch, uint32_t seed, const uint8_t *tbl)
{
	if (patch < 0 || patch >= FlowerPatchCount)
		return;
	RefreshShades();
	const PatchStyle &style = Styles[patch];
	Rng rng { seed * 2654435761u + static_cast<uint32_t>(patch) * 40503u + 7u };
	// The tile's floor diamond is centred 32 right of and 15 above its bottom-left corner.
	const float cx = position.x + 32.0f, cy = position.y - 15.0f;
	auto spot = [&]() {
		for (;;) {
			const float u = rng.Unit() * 2 - 1, v = rng.Unit() * 2 - 1;
			if (std::abs(u) + std::abs(v) <= 0.8f)
				return Point { static_cast<int>(cx + u * 32), static_cast<int>(cy + v * 15) };
		}
	};
	for (int i = 0; i < style.tufts; i++) {
		const Point g = spot();
		Put(out, g, DarkGreen, tbl);
		Put(out, g + Displacement { -1, -1 }, Green, tbl);
		Put(out, g + Displacement { 1, -1 }, Green, tbl);
	}
	const int flowers = rng.Between(style.minFlowers, style.maxFlowers);
	for (int i = 0; i < flowers; i++) {
		const Point g = spot();
		const Shade petal = style.petals[rng.Between(0, 2)];
		const int stem = rng.Between(1, 3);
		Put(out, g + Displacement { -1, 0 }, DarkGreen, tbl);
		Put(out, g + Displacement { 1, 0 }, DarkGreen, tbl);
		for (int s = 0; s < stem; s++)
			Put(out, g + Displacement { 0, -s }, Green, tbl);
		const Point head = g + Displacement { 0, -stem };
		switch (style.bloom) {
		case 0: // small: a dot with a petal either side
			Put(out, head, petal, tbl);
			Put(out, head + Displacement { rng.Between(0, 1) == 0 ? -1 : 1, 0 }, petal, tbl);
			break;
		case 1: // medium: a cross around a centre
			Put(out, head + Displacement { -1, 0 }, petal, tbl);
			Put(out, head + Displacement { 1, 0 }, petal, tbl);
			Put(out, head + Displacement { 0, -1 }, petal, tbl);
			Put(out, head + Displacement { 0, 1 }, petal, tbl);
			Put(out, head, style.centre, tbl);
			break;
		case 2: // large: a round bloom
			for (int dy = -2; dy <= 0; dy++) {
				for (int dx = -1; dx <= 1; dx++)
					Put(out, head + Displacement { dx, dy }, petal, tbl);
			}
			Put(out, head + Displacement { -2, -1 }, petal, tbl);
			Put(out, head + Displacement { 2, -1 }, petal, tbl);
			Put(out, head + Displacement { 0, -1 }, style.centre, tbl);
			break;
		default: // spike: a column of small flowers
			for (int s = 0; s < 3; s++)
				Put(out, head + Displacement { (s & 1) != 0 ? 1 : 0, -s }, s == 2 ? style.centre : petal, tbl);
			break;
		}
	}
}

namespace {

void Line(const Surface &out, Point a, Point b, Shade shade, const uint8_t *tbl)
{
	const int dx = std::abs(b.x - a.x), dy = -std::abs(b.y - a.y);
	const int sx = a.x < b.x ? 1 : -1, sy = a.y < b.y ? 1 : -1;
	int err = dx + dy;
	for (int guard = 0; guard < 200; guard++) {
		Put(out, a, shade, tbl);
		if (a == b)
			break;
		const int e2 = 2 * err;
		if (e2 >= dy) {
			err += dy;
			a.x += sx;
		}
		if (e2 <= dx) {
			err += dx;
			a.y += sy;
		}
	}
}

/** A wooden rail: a sunlit top edge over a shaded underside. */
void Rail(const Surface &out, Point a, Point b, const uint8_t *tbl, bool gate)
{
	Line(out, a, b, gate ? WoodLight : Wood, tbl);
	Line(out, a + Displacement { 0, -1 }, b + Displacement { 0, -1 }, WoodLight, tbl);
	Line(out, a + Displacement { 0, 1 }, b + Displacement { 0, 1 }, WoodDark, tbl);
}

void Post(const Surface &out, Point ground, int height, const uint8_t *tbl)
{
	for (int y = 0; y < height; y++) {
		Put(out, ground + Displacement { -1, -y }, WoodDark, tbl);
		Put(out, ground + Displacement { 0, -y }, Wood, tbl);
		Put(out, ground + Displacement { 1, -y }, WoodLight, tbl);
	}
	Put(out, ground + Displacement { 0, -height }, WoodDark, tbl);
}

} // namespace

void DrawFence(const Surface &out, Point position, uint8_t fence, std::array<bool, 4> links, const uint8_t *tbl)
{
	if (fence == 0)
		return;
	RefreshShades();
	const bool gate = fence == 2;
	// Ground point at the middle of the tile, and the middle of each edge it links through:
	// +x (down-right), -x (up-left), +y (down-left), -y (up-right).
	const Point centre = position + Displacement { 32, -16 };
	constexpr std::array<Displacement, 4> Edge { Displacement { 16, 8 }, Displacement { -16, -8 }, Displacement { -16, 8 }, Displacement { 16, -8 } };
	// Back edges first, so the near rails cover the far ones.
	for (int i : { 1, 3, 0, 2 }) {
		if (!links[i])
			continue;
		const Point end = centre + Edge[i];
		for (const int h : { 7, 15 })
			Rail(out, centre + Displacement { 0, -h }, end + Displacement { 0, -h }, tbl, gate);
		if (gate) // a cross brace on the gate's boards
			Line(out, centre + Displacement { 0, -7 }, end + Displacement { 0, -15 }, WoodDark, tbl);
	}
	Post(out, centre, gate ? 22 : 19, tbl);
}

bool PieceHasStandingParts(uint16_t piece)
{
	if (piece >= MAXTILES)
		return false;
	if (HasAnyOf(SOLData[piece], TileProperties::Solid | TileProperties::BlockMissile))
		return true;
	const MICROS &micros = DPieceMicros[piece];
	for (int half = 0; half < 2; half++) {
		const LevelCelBlock block { micros.mt[half] };
		if (block.hasValue() && block.type() == TileType::TransparentSquare)
			return true;
	}
	for (int b = 2; b < static_cast<int>(MicroTileLen); b++) {
		if (LevelCelBlock { micros.mt[b] }.hasValue())
			return true;
	}
	return false;
}

void DrawDecorPiece(const Surface &out, const Lightmap &lightmap, Point position, uint16_t piece, const uint8_t *tbl)
{
	if (piece >= MAXTILES || pDungeonCels == nullptr)
		return;
	constexpr int HalfWidth = static_cast<int>(DunFrameWidth);
	const bool floor = !HasAnyOf(SOLData[piece], TileProperties::Solid | TileProperties::BlockMissile);
	const MICROS &micros = DPieceMicros[piece];
	for (int half = 0; half < 2; half++) {
		const LevelCelBlock block { micros.mt[half] };
		if (!block.hasValue())
			continue;
		const Point p = position + Displacement { half * HalfWidth, 0 };
		if (floor && block.type() == TileType::TransparentSquare)
			RenderTileFoliage(out, lightmap, p, pDungeonCels.get(), block, tbl);
		else if (!floor)
			RenderTile(out, lightmap, p, pDungeonCels.get(), block, MaskType::Solid, tbl);
	}
	Point at = position + Displacement { 0, -TILE_HEIGHT };
	for (int b = 2; b + 1 < static_cast<int>(MicroTileLen); b += 2, at.y -= TILE_HEIGHT) {
		for (int half = 0; half < 2; half++) {
			const LevelCelBlock block { micros.mt[b + half] };
			if (block.hasValue())
				RenderTile(out, lightmap, at + Displacement { half * HalfWidth, 0 }, pDungeonCels.get(), block, MaskType::Solid, tbl);
		}
	}
}

void DrawDecorFloor(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightTableIndex)
{
	for (int layer = 0; layer < DecorLayerCount; layer++) {
		const uint8_t flower = dDecorFlower[layer][tilePosition.x][tilePosition.y];
		if (flower == 0)
			continue;
		const uint8_t *tbl = GetTileHueTable(dDecorHue[layer][tilePosition.x][tilePosition.y], lightTableIndex);
		const uint32_t seed = static_cast<uint32_t>(tilePosition.x) * 73856093u ^ static_cast<uint32_t>(tilePosition.y) * 19349663u ^ static_cast<uint32_t>(layer) * 83492791u;
		DrawFlowerPatch(out, targetBufferPosition, flower - 1, seed, tbl);
	}
}

void DrawDecorCell(const Surface &out, const Lightmap &lightmap, Point tilePosition, Point targetBufferPosition, int lightTableIndex)
{
	if (const uint8_t fence = dFence[tilePosition.x][tilePosition.y]; fence != 0) {
		auto fenced = [](Point p) { return InDungeonBounds(p) && dFence[p.x][p.y] != 0; };
		const std::array<bool, 4> links { fenced(tilePosition + Displacement { 1, 0 }), fenced(tilePosition + Displacement { -1, 0 }),
			fenced(tilePosition + Displacement { 0, 1 }), fenced(tilePosition + Displacement { 0, -1 }) };
		DrawFence(out, targetBufferPosition, fence, links, LightTables[lightTableIndex].data());
	}
	for (int layer = 0; layer < DecorLayerCount; layer++) {
		const uint16_t piece = dDecorPiece[layer][tilePosition.x][tilePosition.y];
		if (piece == 0)
			continue;
		const int hue = dDecorHue[layer][tilePosition.x][tilePosition.y];
		const HueLightTableSet *hueTables = GetTileHueLightTables(hue);
		const uint8_t *tbl = hueTables != nullptr ? (*hueTables)[lightTableIndex].data() : LightTables[lightTableIndex].data();
		uint8_t lightmapBuffer[TILE_WIDTH * TILE_HEIGHT];
		const Lightmap bled = Lightmap::bleedUp(*GetOptions().Graphics.perPixelLighting, lightmap, targetBufferPosition, lightmapBuffer);
		DrawDecorPiece(out, hueTables != nullptr ? bled.withLightTables(*hueTables) : bled, targetBufferPosition, static_cast<uint16_t>(piece - 1), tbl);
	}
}

} // namespace devilution
