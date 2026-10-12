#include "levels/town_shuffle.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "engine/point.hpp"
#include "levels/dun_tile_data.hpp"
#include "utils/log.hpp"
#include "utils/enum_traits.h"

namespace devilution {

namespace {

constexpr int TownSize = 96;
constexpr int Border = 9;
/** Plain walkable ground tile used to fill vacated areas. */
constexpr uint16_t GroundPiece = 25;
constexpr size_t BuildingCount = static_cast<size_t>(TownBuilding::Count);

struct Rect {
	int x0, y0, x1, y1; // inclusive
	[[nodiscard]] constexpr int width() const { return x1 - x0 + 1; }
	[[nodiscard]] constexpr int height() const { return y1 - y0 + 1; }
};

/** Cut rectangles of the classic layout, in dPiece cells. Indexed by TownBuilding. */
constexpr std::array<Rect, BuildingCount> BuildingRects = { {
    { 14, 10, 33, 33 }, // Cathedral (includes its warp, the entry view point and the wounded townsman)
    { 44, 52, 57, 65 }, // Tavern
    { 58, 54, 73, 65 }, // Smithy
    { 46, 72, 57, 81 }, // Healer
    { 36, 62, 45, 69 }, // GillianHouse
    { 72, 14, 81, 21 }, // WitchHut
    { 68, 66, 75, 73 }, // SpawnHouse
    { 66, 76, 73, 85 }, // FarnhamHouse
    { 44, 38, 55, 47 }, // HouseA
    { 24, 46, 31, 53 }, // HouseB
} };

/** Landmarks (quest overlays, warps) whose art is painted onto fixed cells. They never move. */
constexpr std::array<Rect, 6> FixedRects = { {
    { 46, 16, 52, 24 }, // Catacombs
    { 33, 18, 40, 26 }, // Crypt / grave
    { 58, 68, 62, 72 }, // Fountain
    { 14, 66, 19, 73 }, // Caves
    { 38, 76, 43, 83 }, // Hell
    { 77, 59, 83, 65 }, // Nest / hive
} };

/** Points that stay where they are and must remain reachable (towners, spawn, warp landings). */
constexpr std::array<Point, 11> FixedPoints = { {
    { 49, 22 }, { 36, 26 }, { 18, 69 }, { 41, 79 }, { 80, 62 }, // warp landings
    { 62, 71 }, { 11, 53 }, { 75, 68 },                         // Cain, Wirt, spawn
    { 58, 16 }, { 56, 14 }, { 59, 20 },                         // cows
} };
constexpr size_t FixedPointCount = FixedPoints.size();

struct AttachedPoint {
	TownBuilding building;
	Point position;
};

/** Points that travel with a building. */
constexpr std::array<AttachedPoint, 9> AttachedPoints = { {
    { TownBuilding::Cathedral, { 25, 29 } },    // cathedral warp
    { TownBuilding::Cathedral, { 25, 31 } },    // entry view point
    { TownBuilding::Cathedral, { 24, 32 } },    // wounded townsman
    { TownBuilding::Tavern, { 55, 62 } },       // Ogden
    { TownBuilding::Smithy, { 62, 63 } },       // Griswold
    { TownBuilding::Healer, { 55, 79 } },       // Pepin
    { TownBuilding::GillianHouse, { 43, 66 } }, // Gillian
    { TownBuilding::WitchHut, { 80, 20 } },     // Adria
    { TownBuilding::FarnhamHouse, { 71, 84 } }, // Farnham
} };

std::array<Displacement, BuildingCount> BuildingShifts {};

/** Small deterministic generator, independent of the game's own random state. */
class LayoutRng {
public:
	explicit LayoutRng(uint64_t seed)
	    : state_(seed)
	{
	}

	uint32_t next()
	{
		state_ += 0x9E3779B97F4A7C15ULL;
		uint64_t z = state_;
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
		return static_cast<uint32_t>((z ^ (z >> 31)) >> 16);
	}

	int range(int lo, int hiExclusive) { return lo + static_cast<int>(next() % static_cast<uint32_t>(hiExclusive - lo)); }

private:
	uint64_t state_;
};

class Grid {
public:
	Grid()
	    : cells_(TownSize * TownSize)
	{
	}
	uint16_t &at(int x, int y) { return cells_[static_cast<size_t>(x) * TownSize + y]; }
	[[nodiscard]] uint16_t at(int x, int y) const { return cells_[static_cast<size_t>(x) * TownSize + y]; }

private:
	std::vector<uint16_t> cells_;
};

bool InFixedRect(int x, int y)
{
	return std::any_of(FixedRects.begin(), FixedRects.end(), [&](const Rect &r) { return x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1; });
}

bool IsSolid(uint16_t piece)
{
	return HasAnyOf(SOLData[piece], TileProperties::Solid);
}

bool InBuildingRect(int x, int y)
{
	return std::any_of(BuildingRects.begin(), BuildingRects.end(), [&](const Rect &r) { return x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1; });
}

/** Single-cell solid objects (trees, rocks, stumps) of the classic town's interior, to be scattered again. */
std::vector<uint16_t> CollectDecorations(const Grid &classic)
{
	std::vector<uint16_t> result;
	for (int x = Border; x < TownSize - Border; x++) {
		for (int y = Border; y < TownSize - Border; y++) {
			if (!IsSolid(classic.at(x, y)) || InFixedRect(x, y) || InBuildingRect(x, y))
				continue;
			bool isolated = true;
			for (int dx = -1; dx <= 1 && isolated; dx++)
				for (int dy = -1; dy <= 1 && isolated; dy++)
					if ((dx != 0 || dy != 0) && IsSolid(classic.at(x + dx, y + dy)))
						isolated = false;
			if (isolated)
				result.push_back(classic.at(x, y));
		}
	}
	return result;
}

bool TryLayout(uint64_t seed, const Grid &classic, Grid &out, std::array<Displacement, BuildingCount> &shifts)
{
	LayoutRng rng(seed);
	out = classic;

	// Cut every building out and clear re-scatterable interior decoration.
	for (const Rect &r : BuildingRects) {
		for (int x = r.x0; x <= r.x1; x++)
			for (int y = r.y0; y <= r.y1; y++)
				out.at(x, y) = GroundPiece;
	}
	std::vector<bool> occupied(TownSize * TownSize, false);
	auto occ = [&](int x, int y) -> std::vector<bool>::reference { return occupied[static_cast<size_t>(x) * TownSize + y]; };
	for (int x = 0; x < TownSize; x++) {
		for (int y = 0; y < TownSize; y++) {
			const bool inBorder = std::min({ x, y, TownSize - 1 - x, TownSize - 1 - y }) < Border;
			if (InFixedRect(x, y)) {
				occ(x, y) = true;
			} else if (inBorder) {
				if (IsSolid(out.at(x, y)))
					occ(x, y) = true;
			} else if (IsSolid(out.at(x, y))) {
				out.at(x, y) = GroundPiece;
			}
		}
	}
	for (size_t i = 0; i < FixedPointCount; i++) {
		for (int x = FixedPoints[i].x - 2; x <= FixedPoints[i].x + 2; x++)
			for (int y = FixedPoints[i].y - 2; y <= FixedPoints[i].y + 2; y++)
				if (x >= 0 && y >= 0 && x < TownSize && y < TownSize)
					occ(x, y) = true;
	}

	// Largest buildings first.
	std::array<size_t, BuildingCount> order {};
	for (size_t i = 0; i < BuildingCount; i++)
		order[i] = i;
	std::stable_sort(order.begin(), order.end(), [](size_t a, size_t b) {
		return BuildingRects[a].width() * BuildingRects[a].height() > BuildingRects[b].width() * BuildingRects[b].height();
	});

	for (const size_t index : order) {
		const Rect &src = BuildingRects[index];
		const int w = src.width();
		const int h = src.height();
		bool placed = false;
		for (int attempt = 0; attempt < 400 && !placed; attempt++) {
			const int nx = rng.range(3, TownSize - 3 - w) & ~1;
			const int ny = rng.range(3, TownSize - 3 - h) & ~1;
			bool free = true;
			for (int x = nx - 1; x <= nx + w && free; x++)
				for (int y = ny - 1; y <= ny + h && free; y++)
					if (x >= 0 && y >= 0 && x < TownSize && y < TownSize && occ(x, y))
						free = false;
			if (!free)
				continue;
			for (int x = 0; x < w; x++)
				for (int y = 0; y < h; y++)
					out.at(nx + x, ny + y) = classic.at(src.x0 + x, src.y0 + y);
			for (int x = nx - 2; x < nx + w + 2; x++)
				for (int y = ny - 2; y < ny + h + 2; y++)
					if (x >= 0 && y >= 0 && x < TownSize && y < TownSize)
						occ(x, y) = true;
			shifts[index] = { nx - src.x0, ny - src.y0 };
			placed = true;
		}
		if (!placed)
			return false;
	}

	// Scatter the town's trees and rocks again. Never next to another solid cell, so they cannot seal off paths.
	for (const uint16_t decoration : CollectDecorations(classic)) {
		for (int attempt = 0; attempt < 30; attempt++) {
			const int x = rng.range(Border, TownSize - Border);
			const int y = rng.range(Border, TownSize - Border);
			if (occ(x, y))
				continue;
			bool clear = true;
			for (int dx = -1; dx <= 1 && clear; dx++)
				for (int dy = -1; dy <= 1 && clear; dy++)
					if (IsSolid(out.at(x + dx, y + dy)))
						clear = false;
			if (!clear)
				continue;
			out.at(x, y) = decoration;
			break;
		}
	}

	// Everything that must stay reachable has to connect to the player spawn.
	std::vector<bool> seen(TownSize * TownSize, false);
	auto vis = [&](int x, int y) -> std::vector<bool>::reference { return seen[static_cast<size_t>(x) * TownSize + y]; };
	std::deque<Point> queue;
	const Point spawn { 75, 68 };
	vis(spawn.x, spawn.y) = true;
	queue.push_back(spawn);
	while (!queue.empty()) {
		const Point p = queue.front();
		queue.pop_front();
		constexpr int Dx[4] = { 1, -1, 0, 0 };
		constexpr int Dy[4] = { 0, 0, 1, -1 };
		for (int d = 0; d < 4; d++) {
			const int x = p.x + Dx[d];
			const int y = p.y + Dy[d];
			if (x < 0 || y < 0 || x >= TownSize || y >= TownSize || vis(x, y) || IsSolid(out.at(x, y)))
				continue;
			vis(x, y) = true;
			queue.push_back({ x, y });
		}
	}
	auto reachable = [&](Point p) {
		for (int x = p.x - 1; x <= p.x + 1; x++)
			for (int y = p.y - 1; y <= p.y + 1; y++)
				if (x >= 0 && y >= 0 && x < TownSize && y < TownSize && vis(x, y))
					return true;
		return false;
	};
	for (size_t i = 0; i < FixedPointCount; i++)
		if (!reachable(FixedPoints[i]))
			return false;
	for (const AttachedPoint &a : AttachedPoints) {
		const Displacement s = shifts[static_cast<size_t>(a.building)];
		if (!reachable({ a.position.x + s.deltaX, a.position.y + s.deltaY }))
			return false;
	}
	return true;
}

} // namespace

void ResetTownLayout()
{
	BuildingShifts.fill({ 0, 0 });
}

Displacement TownBuildingShift(TownBuilding building)
{
	return BuildingShifts[static_cast<size_t>(building)];
}

bool ShuffleTown(uint32_t seed)
{
	ResetTownLayout();
	if (seed == 0)
		return false;

	Grid classic;
	for (int x = 0; x < TownSize; x++)
		for (int y = 0; y < TownSize; y++)
			classic.at(x, y) = dPiece[x][y];

	for (uint32_t attempt = 0; attempt < 64; attempt++) {
		Grid result;
		std::array<Displacement, BuildingCount> shifts {};
		if (!TryLayout((static_cast<uint64_t>(seed) << 8) + attempt, classic, result, shifts))
			continue;
		for (int x = 0; x < TownSize; x++)
			for (int y = 0; y < TownSize; y++)
				dPiece[x][y] = result.at(x, y);
		BuildingShifts = shifts;
		for (size_t i = 0; i < BuildingCount; i++)
			LogVerbose("town layout seed {}: building {} shifted by ({}, {})", seed, i, shifts[i].deltaX, shifts[i].deltaY);
		return true;
	}
	return false;
}

} // namespace devilution
