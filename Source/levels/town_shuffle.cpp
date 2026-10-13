#include "levels/town_shuffle.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
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
    { 46, 16, 52, 25 }, // Catacombs entrance
    { 14, 66, 19, 73 }, // Caves entrance
    { 34, 76, 47, 83 }, // Hell entrance
    { 58, 68, 65, 73 }, // Fountain (Cain stands next to it)
    { 32, 18, 41, 27 }, // Grave / crypt entrance
} };

/** The hive hugs the map edge and never moves. */
constexpr std::array<Rect, 1> FixedRects = { {
    { 77, 59, 87, 66 }, // Nest / hive
} };

/** Points that stay where they are and must remain reachable (towners, spawn, warp landings). */
constexpr std::array<Point, 6> FixedPoints = { {
    { 80, 62 },                 // nest warp landing
    { 11, 53 }, { 75, 68 },     // Wirt, spawn
    { 58, 16 }, { 56, 14 }, { 59, 20 }, // cows
} };
constexpr size_t FixedPointCount = FixedPoints.size();

struct AttachedPoint {
	TownBuilding building;
	Point position;
};

/** Points that travel with a building. */
constexpr std::array<AttachedPoint, 18> AttachedPoints = { {
    { TownBuilding::Cathedral, { 25, 29 } },    // cathedral warp
    { TownBuilding::Cathedral, { 25, 31 } },    // entry view point
    { TownBuilding::Cathedral, { 24, 32 } },    // wounded townsman
    { TownBuilding::Tavern, { 55, 62 } },       // Ogden
    { TownBuilding::Smithy, { 62, 63 } },       // Griswold
    { TownBuilding::Healer, { 55, 79 } },       // Pepin
    { TownBuilding::GillianHouse, { 43, 66 } }, // Gillian
    { TownBuilding::WitchHut, { 80, 20 } },     // Adria
    { TownBuilding::FarnhamHouse, { 71, 84 } }, // Farnham
    { TownBuilding::Catacombs, { 49, 21 } },    // catacombs warp
    { TownBuilding::Catacombs, { 49, 22 } },    // catacombs landing
    { TownBuilding::Caves, { 17, 69 } },        // caves warp
    { TownBuilding::Caves, { 18, 69 } },        // caves landing
    { TownBuilding::Hell, { 41, 80 } },         // hell warp
    { TownBuilding::Hell, { 41, 81 } },         // hell landing
    { TownBuilding::Fountain, { 62, 71 } },     // Cain
    { TownBuilding::Grave, { 36, 24 } },        // crypt warp
    { TownBuilding::Grave, { 36, 25 } },        // crypt landing
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

bool IsSolid(uint16_t piece)
{
	return HasAnyOf(SOLData[piece], TileProperties::Solid);
}

/** Houses that trade places. Landmarks (cathedral, dungeon entrances, fountain, graveyard) stay put. */
constexpr std::array<TownBuilding, 9> MovableBuildings = { {
    TownBuilding::Tavern,
    TownBuilding::Smithy,
    TownBuilding::Healer,
    TownBuilding::GillianHouse,
    TownBuilding::WitchHut,
    TownBuilding::SpawnHouse,
    TownBuilding::FarnhamHouse,
    TownBuilding::HouseA,
    TownBuilding::HouseB,
} };
constexpr size_t MovableCount = MovableBuildings.size();

/** Tight, megatile-aligned box around a building's main structure (its plot). */
Rect PlotOf(const Grid &classic, TownBuilding building)
{
	const Rect &r = BuildingRects[static_cast<size_t>(building)];
	// Largest connected group of solid cells in the cut: the house itself, not stray trees or fences.
	const int w = r.width(), h = r.height();
	std::vector<int> component(static_cast<size_t>(w * h), -1);
	int bestComponent = -1;
	size_t bestSize = 0;
	int next = 0;
	for (int sx = 0; sx < w; sx++) {
		for (int sy = 0; sy < h; sy++) {
			if (component[sx * h + sy] != -1 || !IsSolid(classic.at(r.x0 + sx, r.y0 + sy)))
				continue;
			std::deque<Point> queue { Point { sx, sy } };
			component[sx * h + sy] = next;
			size_t size = 0;
			while (!queue.empty()) {
				const Point c = queue.front();
				queue.pop_front();
				size++;
				for (int dx = -1; dx <= 1; dx++) {
					for (int dy = -1; dy <= 1; dy++) {
						const int nx = c.x + dx, ny = c.y + dy;
						if (nx < 0 || ny < 0 || nx >= w || ny >= h || component[nx * h + ny] != -1)
							continue;
						if (!IsSolid(classic.at(r.x0 + nx, r.y0 + ny)))
							continue;
						component[nx * h + ny] = next;
						queue.push_back({ nx, ny });
					}
				}
			}
			if (size > bestSize) {
				bestSize = size;
				bestComponent = next;
			}
			next++;
		}
	}
	Rect box { r.x1, r.y1, r.x0, r.y0 };
	for (int sx = 0; sx < w; sx++) {
		for (int sy = 0; sy < h; sy++) {
			if (component[sx * h + sy] != bestComponent)
				continue;
			box.x0 = std::min(box.x0, r.x0 + sx);
			box.y0 = std::min(box.y0, r.y0 + sy);
			box.x1 = std::max(box.x1, r.x0 + sx);
			box.y1 = std::max(box.y1, r.y0 + sy);
		}
	}
	if (bestComponent < 0)
		return r;
	// Margin for eaves and doorsteps (more at the front, where art overhangs), snapped to megatiles.
	box.x0 = std::max(r.x0, box.x0 - 1) & ~1;
	box.y0 = std::max(r.y0, box.y0 - 1) & ~1;
	box.x1 = std::min(r.x1, box.x1 + 2) | 1;
	box.y1 = std::min(r.y1, box.y1 + 2) | 1;
	return box;
}

struct GroundPattern {
	std::array<uint16_t, 4> pieces;
};

/** The most common plain-ground megatiles, used to fill a vacated plot so it blends in. */
std::vector<GroundPattern> FindGroundPatterns(const Grid &classic)
{
	std::vector<std::pair<GroundPattern, int>> counts;
	for (int x = Border; x < TownSize - Border; x += 2) {
		for (int y = Border; y < TownSize - Border; y += 2) {
			const GroundPattern g { { classic.at(x, y), classic.at(x + 1, y), classic.at(x, y + 1), classic.at(x + 1, y + 1) } };
			if (std::any_of(g.pieces.begin(), g.pieces.end(), IsSolid))
				continue;
			auto it = std::find_if(counts.begin(), counts.end(), [&](const auto &e) { return e.first.pieces == g.pieces; });
			if (it == counts.end())
				counts.emplace_back(g, 1);
			else
				it->second++;
		}
	}
	std::sort(counts.begin(), counts.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
	std::vector<GroundPattern> result;
	for (size_t i = 0; i < counts.size() && i < 3; i++)
		result.push_back(counts[i].first);
	if (result.empty())
		result.push_back({ { GroundPiece, GroundPiece, GroundPiece, GroundPiece } });
	return result;
}

bool IsPlainGround(const Grid &classic, const std::vector<GroundPattern> &patterns, int x, int y)
{
	const uint16_t piece = classic.at(x, y);
	if (IsSolid(piece))
		return false;
	for (const GroundPattern &g : patterns)
		if (std::find(g.pieces.begin(), g.pieces.end(), piece) != g.pieces.end())
			return true;
	return false;
}

bool Contains(const Rect &r, int x, int y)
{
	return x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1;
}

bool TryLayout(uint64_t seed, const Grid &classic, Grid &out, std::array<Displacement, BuildingCount> &shifts)
{
	LayoutRng rng(seed);
	out = classic;
	shifts.fill({ 0, 0 });

	const std::vector<GroundPattern> ground = FindGroundPatterns(classic);
	std::array<Rect, MovableCount> plots {};
	for (size_t i = 0; i < MovableCount; i++) {
		plots[i] = PlotOf(classic, MovableBuildings[i]);
		if (seed % 256 == 0)
			LogVerbose("town plot {}: ({},{})-({},{})", i, plots[i].x0, plots[i].y0, plots[i].x1, plots[i].y1);
	}

	// Clear every plot to plain ground, a whole megatile at a time.
	for (const Rect &plot : plots) {
		for (int x = plot.x0; x <= plot.x1; x += 2) {
			for (int y = plot.y0; y <= plot.y1; y += 2) {
				const GroundPattern &g = ground[(static_cast<uint32_t>(x * 7 + y * 13) ^ static_cast<uint32_t>(seed)) % ground.size()];
				out.at(x, y) = g.pieces[0];
				out.at(x + 1, y) = g.pieces[1];
				out.at(x, y + 1) = g.pieces[2];
				out.at(x + 1, y + 1) = g.pieces[3];
			}
		}
	}

	// Assign houses to plots. A house keeps its front (the +x/+y corner, where its door and
	// owner are) on the plot's front corner, so it still faces the road that plot was built on.
	std::array<size_t, MovableCount> lotOrder {};
	std::array<size_t, MovableCount> pool {};
	for (size_t i = 0; i < MovableCount; i++)
		lotOrder[i] = pool[i] = i;
	for (size_t i = MovableCount - 1; i > 0; i--) {
		std::swap(lotOrder[i], lotOrder[rng.range(0, static_cast<int>(i) + 1)]);
		std::swap(pool[i], pool[rng.range(0, static_cast<int>(i) + 1)]);
	}
	const auto fits = [&](size_t building, size_t lot, Rect &where) {
		const Rect &src = plots[building];
		const Rect &dst = plots[lot];
		where = { dst.x1 - src.width() + 1, dst.y1 - src.height() + 1, dst.x1, dst.y1 };
		if (where.x0 < Border || where.y0 < Border)
			return false;
		for (int x = where.x0; x <= where.x1; x++) {
			for (int y = where.y0; y <= where.y1; y++) {
				if (Contains(dst, x, y))
					continue;
				// Spilling over the plot is only allowed onto plain ground nobody else uses.
				if (!IsPlainGround(classic, ground, x, y))
					return false;
				for (size_t other = 0; other < MovableCount; other++)
					if (other != lot && Contains(plots[other], x, y))
						return false;
				for (size_t b = 0; b < BuildingCount; b++) {
					if (std::find(MovableBuildings.begin(), MovableBuildings.end(), static_cast<TownBuilding>(b)) != MovableBuildings.end())
						continue;
					if (Contains(BuildingRects[b], x, y))
						return false;
				}
				for (const Rect &r : FixedRects)
					if (Contains(r, x, y))
						return false;
			}
		}
		return true;
	};
	// Lots are the nine original plots: houses only move onto sites that already sit on Tristram's roads.
	const size_t lotCount = MovableCount;
	std::vector<size_t> lotTry(lotCount);
	for (size_t i = 0; i < lotCount; i++)
		lotTry[i] = i;

	// Big houses choose first so small ones cannot take the only plots they fit on.
	std::stable_sort(pool.begin(), pool.end(), [&](size_t l, size_t r) {
		return plots[l].width() * plots[l].height() > plots[r].width() * plots[r].height();
	});

	// Backtracking search: every house gets its own plot.
	std::array<size_t, MovableCount> assignment {};
	std::array<Rect, MovableCount> where {};
	std::vector<bool> lotUsed(lotCount, false);
	int budget = 6000;
	std::function<bool(size_t)> assign = [&](size_t depth) -> bool {
		if (depth == MovableCount)
			return true;
		if (--budget < 0)
			return false;
		const size_t building = pool[depth];
		for (size_t i = lotTry.size(); i > 1; i--)
			std::swap(lotTry[i - 1], lotTry[rng.range(0, static_cast<int>(i))]);
		const std::vector<size_t> order = lotTry;
		for (const size_t lot : order) {
			if (lotUsed[lot])
				continue;
			Rect w;
			if (!fits(building, lot, w))
				continue;
			lotUsed[lot] = true;
			assignment[building] = lot;
			where[building] = w;
			if (assign(depth + 1))
				return true;
			lotUsed[lot] = false;
		}
		return false;
	};
	if (!assign(0)) {
		LogVerbose("town layout: no assignment found");
		return false;
	}
	size_t moved = 0;
	for (size_t building = 0; building < MovableCount; building++) {
		const Rect &src = plots[building];
		const Rect &w = where[building];
		for (int x = 0; x < src.width(); x++)
			for (int y = 0; y < src.height(); y++)
				out.at(w.x0 + x, w.y0 + y) = classic.at(src.x0 + x, src.y0 + y);
		shifts[static_cast<size_t>(MovableBuildings[building])] = { w.x0 - src.x0, w.y0 - src.y0 };
		if (assignment[building] != building)
			moved++;
	}
	if (moved < 2) {
		LogVerbose("town layout: too similar to the classic town");
		return false;
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
	// The hive landing (index 0) only opens with its quest, so it is not required here.
	for (size_t i = 1; i < FixedPointCount; i++)
		if (!reachable(FixedPoints[i])) {
			LogVerbose("town layout: fixed point {} unreachable", i);
			return false;
		}
	for (const AttachedPoint &a : AttachedPoints) {
		const Displacement s = shifts[static_cast<size_t>(a.building)];
		const Point moved { a.position.x + s.deltaX, a.position.y + s.deltaY };
		if (!reachable(moved)) {
			LogVerbose("town layout: attached point ({}, {}) unreachable", moved.x, moved.y);
			return false;
		}
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
