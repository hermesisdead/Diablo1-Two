#include "levels/forest.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include "DiabloUI/ui_flags.hpp"
#include "control/control.hpp"
#include "cursor.h"
#include "engine/random.hpp"
#include "engine/render/text_render.hpp"
#include "game_mode.hpp"
#include "items.h"
#include "msg.h"
#ifdef _DEBUG
#include "debug.h"
#endif
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "levels/trigs.h"
#include "monster.h"
#include "multi.h"
#include "player.h"
#include "utils/language.h"
#include "utils/log.hpp"
#include "utils/endian_swap.hpp"
#include "utils/status_macros.hpp"
#include "utils/str_cat.hpp"

namespace devilution {

namespace {

// ---------------------------------------------------------------------------------------------
// Small deterministic RNG, so the forest layout never disturbs the engine's own seeds.

struct ForestRng {
	uint64_t state;

	explicit ForestRng(uint64_t seed)
	    : state(seed * 0x9E3779B97F4A7C15ULL + 0x632BE59BD9B4E019ULL)
	{
	}

	uint32_t Next()
	{
		state += 0x9E3779B97F4A7C15ULL;
		uint64_t z = state;
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
		return static_cast<uint32_t>((z ^ (z >> 31)) >> 16);
	}

	int Range(int n) { return n <= 1 ? 0 : static_cast<int>(Next() % static_cast<uint32_t>(n)); }
	int Between(int lo, int hi) { return lo + Range(hi - lo + 1); }
	float Unit() { return static_cast<float>(Next() & 0xFFFFFF) / static_cast<float>(1 << 24); }
	bool Chance(float p) { return Unit() < p; }

	template <typename T, size_t N>
	T Pick(const std::array<T, N> &values) { return values[Range(static_cast<int>(N))]; }
};

// ---------------------------------------------------------------------------------------------
// Town tileset megatiles (1-based ids, as in town.til)

constexpr std::array<int, 1> WoodsDense { 63 };
constexpr std::array<int, 6> WoodsEdge { 57, 58, 59, 60, 61, 62 };
constexpr std::array<int, 8> Grass { 1, 2, 3, 4, 5, 6, 7, 8 };
constexpr std::array<int, 12> GroundDetail { 35, 36, 37, 39, 40, 43, 45, 48, 49, 50, 51, 52 };
constexpr std::array<int, 5> Trees { 44, 47, 53, 55, 46 };

constexpr int MegaW = MAXDUNX / 2; // 56 megatiles cover the whole 112x112 map
constexpr int RegionMin = 9;       // carved area, in megatiles (pieces 18..93)
constexpr int RegionMax = 46;
constexpr int ExitMega = 47; // the exit trail runs to this column (pieces 94/95)

enum class Cell : uint8_t {
	Woods,
	Open,
	Trail,
};

std::array<std::array<Cell, MegaW>, MegaW> Grid;
std::array<std::array<bool, MAXDUNY>, MAXDUNX> Reachable;
std::vector<Point> ReachableTiles;
Point ForestExit { 95, 56 };
std::optional<Point> TownGate;

bool InRegion(int i, int j)
{
	return i >= RegionMin && i <= RegionMax && j >= RegionMin && j <= RegionMax;
}

void SetCell(int i, int j, Cell c)
{
	if (i < 0 || j < 0 || i >= MegaW || j >= MegaW)
		return;
	if (Grid[i][j] == Cell::Trail && c == Cell::Open)
		return;
	Grid[i][j] = c;
}

struct Clearing {
	int i, j, rx, ry;
};

void CarveClearing(ForestRng &rng, const Clearing &c)
{
	for (int dj = -c.ry - 1; dj <= c.ry + 1; dj++) {
		for (int di = -c.rx - 1; di <= c.rx + 1; di++) {
			const int i = c.i + di, j = c.j + dj;
			if (!InRegion(i, j))
				continue;
			const float fx = static_cast<float>(di) / (static_cast<float>(c.rx) + 0.5f);
			const float fy = static_cast<float>(dj) / (static_cast<float>(c.ry) + 0.5f);
			if (fx * fx + fy * fy <= 1.0f + (rng.Unit() - 0.5f) * 0.5f)
				SetCell(i, j, Cell::Open);
		}
	}
}

/** A meandering trail, one or two megatiles wide. */
void CarveTrail(ForestRng &rng, Point from, Point to, bool allowOutside)
{
	Point p = from;
	int guard = 0;
	while (p != to && guard++ < 400) {
		const int dx = to.x > p.x ? 1 : (to.x < p.x ? -1 : 0);
		const int dy = to.y > p.y ? 1 : (to.y < p.y ? -1 : 0);
		Point next = p;
		if (dx != 0 && dy != 0)
			next += rng.Chance(0.5f) ? Displacement { dx, 0 } : Displacement { 0, dy };
		else
			next += Displacement { dx, dy };
		// Wander sideways now and then so trails do not run ruler-straight.
		if (rng.Chance(0.22f)) {
			const Displacement side = dx != 0 ? Displacement { 0, rng.Chance(0.5f) ? 1 : -1 } : Displacement { rng.Chance(0.5f) ? 1 : -1, 0 };
			const Point wiggle = p + side;
			if (InRegion(wiggle.x, wiggle.y))
				next = wiggle;
		}
		p = next;
		if (!allowOutside && !InRegion(p.x, p.y))
			break;
		SetCell(p.x, p.y, Cell::Trail);
		if (rng.Chance(0.55f)) {
			const Point wide = p + (dx != 0 ? Displacement { 0, 1 } : Displacement { 1, 0 });
			if (InRegion(wide.x, wide.y))
				SetCell(wide.x, wide.y, Cell::Trail);
		}
	}
}

int OpenNeighbours4(int i, int j)
{
	int n = 0;
	for (const Displacement d : { Displacement { 1, 0 }, Displacement { -1, 0 }, Displacement { 0, 1 }, Displacement { 0, -1 } }) {
		const int x = i + d.deltaX, y = j + d.deltaY;
		if (x >= 0 && y >= 0 && x < MegaW && y < MegaW && Grid[x][y] != Cell::Woods)
			n++;
	}
	return n;
}

bool NearTrail(int i, int j, int r)
{
	for (int dj = -r; dj <= r; dj++)
		for (int di = -r; di <= r; di++) {
			const int x = i + di, y = j + dj;
			if (x >= 0 && y >= 0 && x < MegaW && y < MegaW && Grid[x][y] == Cell::Trail)
				return true;
		}
	return false;
}

void PlaceMegatile(int i, int j, int id)
{
	const MegaTile mega = pMegaTiles[id - 1];
	const int x = 2 * i, y = 2 * j;
	dPiece[x + 0][y + 0] = Swap16LE(mega.micro1);
	dPiece[x + 1][y + 0] = Swap16LE(mega.micro2);
	dPiece[x + 0][y + 1] = Swap16LE(mega.micro3);
	dPiece[x + 1][y + 1] = Swap16LE(mega.micro4);
	if (i >= 8 && j >= 8 && i < 8 + DMAXX && j < 8 + DMAXY)
		dungeon[i - 8][j - 8] = static_cast<uint8_t>(std::min(id, 255));
}

bool Walkable(Point p)
{
	return InDungeonBounds(p) && !IsTileSolid(p);
}

void FloodReachable(Point start)
{
	for (auto &col : Reachable)
		col.fill(false);
	ReachableTiles.clear();
	if (!Walkable(start))
		return;
	std::vector<Point> queue { start };
	Reachable[start.x][start.y] = true;
	for (size_t head = 0; head < queue.size(); head++) {
		const Point cur = queue[head];
		ReachableTiles.push_back(cur);
		for (int dx = -1; dx <= 1; dx++) {
			for (int dy = -1; dy <= 1; dy++) {
				const Point n = cur + Displacement { dx, dy };
				if ((dx == 0 && dy == 0) || !Walkable(n) || Reachable[n.x][n.y])
					continue;
				if (dx != 0 && dy != 0 && (!Walkable(cur + Displacement { dx, 0 }) || !Walkable(cur + Displacement { 0, dy })))
					continue;
				Reachable[n.x][n.y] = true;
				queue.push_back(n);
			}
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Waves

enum class WavePhase : uint8_t {
	Break,
	Spawning,
	Fighting,
};

struct WaveState {
	int wave = 0;
	WavePhase phase = WavePhase::Break;
	int phaseTicks = 0;
	int toSpawn = 0;
	int spawned = 0;
	int ticks = 0;
	int bannerTicks = 0;
	int lastAlive = 0;
	int lastProgressTick = 0;
	std::string banner;
};

WaveState Waves;

constexpr int TicksPerSecond = 20;
constexpr int MaxAliveInWave = 32;

int WaveSize(int wave)
{
	int n = 8 + 3 * (wave - 1);
	if (wave % 5 == 0)
		n += n / 2; // elite wave
	return std::min(n, 70);
}

float WaveHitPointScale(int wave) { return 1.0f + 0.10f * static_cast<float>(wave - 1); }
float WaveDamageScale(int wave) { return 1.0f + 0.05f * static_cast<float>(wave - 1); }

bool IsWaveMonster(const Monster &monster)
{
	return monster.type().type != MT_GOLEM && monster.mode != MonsterMode::Death && monster.hitPoints > Fixed26_6::fromInt(0);
}

int AliveWaveMonsters()
{
	int alive = 0;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		if (IsWaveMonster(Monsters[ActiveMonsters[i]]))
			alive++;
	}
	return alive;
}

void ShowBanner(std::string text)
{
	Waves.banner = std::move(text);
	Waves.bannerTicks = 3 * TicksPerSecond;
}

bool CanSpawnAt(Point p)
{
	return InDungeonBounds(p) && Reachable[p.x][p.y] && dMonster[p.x][p.y] == 0 && dPlayer[p.x][p.y] == 0
	    && dObject[p.x][p.y] == 0 && !IsTileSolid(p) && !IsTileVisible(p);
}

void ScaleForWave(Monster &monster, int wave)
{
	const float hp = WaveHitPointScale(wave);
	const float dmg = WaveDamageScale(wave);
	monster.maxHitPoints = Fixed26_6::fromRaw(static_cast<int32_t>(static_cast<float>(monster.maxHitPoints.raw()) * hp));
	monster.hitPoints = monster.maxHitPoints;
	const auto scale = [dmg](uint8_t v) { return static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(v * dmg)), 0, 255)); };
	monster.minDamage = scale(monster.minDamage);
	monster.maxDamage = scale(monster.maxDamage);
	monster.minDamageSpecial = scale(monster.minDamageSpecial);
	monster.maxDamageSpecial = scale(monster.maxDamageSpecial);
	monster.armorClass = static_cast<uint8_t>(std::min(255, monster.armorClass + (wave - 1)));
}

void Hunt(Monster &monster, const Player &player)
{
	monster.position.last = player.position.tile;
	monster.activeForTicks = UINT8_MAX;
}

std::array<std::array<int16_t, MAXDUNY>, MAXDUNX> PathDistance;

/** Walking distance from the hero to every reachable tile, so packs never spawn behind a long detour. */
void UpdatePathDistances(Point hero)
{
	for (auto &col : PathDistance)
		col.fill(-1);
	if (!InDungeonBounds(hero))
		return;
	std::vector<Point> queue { hero };
	PathDistance[hero.x][hero.y] = 0;
	for (size_t head = 0; head < queue.size(); head++) {
		const Point cur = queue[head];
		const int16_t d = PathDistance[cur.x][cur.y];
		if (d >= 40)
			continue;
		for (int dx = -1; dx <= 1; dx++) {
			for (int dy = -1; dy <= 1; dy++) {
				const Point n = cur + Displacement { dx, dy };
				if ((dx == 0 && dy == 0) || !Walkable(n) || PathDistance[n.x][n.y] >= 0)
					continue;
				if (dx != 0 && dy != 0 && (!Walkable(cur + Displacement { dx, 0 }) || !Walkable(cur + Displacement { 0, dy })))
					continue;
				PathDistance[n.x][n.y] = static_cast<int16_t>(d + 1);
				queue.push_back(n);
			}
		}
	}
}

std::optional<Point> FindSpawnPoint(int minPath, int maxPath)
{
	if (ReachableTiles.empty())
		return std::nullopt;
	for (int attempt = 0; attempt < 120; attempt++) {
		const Point p = ReachableTiles[GenerateRnd(static_cast<int>(ReachableTiles.size()))];
		const int d = PathDistance[p.x][p.y];
		if (d >= minPath && d <= maxPath && CanSpawnAt(p))
			return p;
	}
	return std::nullopt;
}

/** Spawns one pack just out of sight, 10 to 18 steps of walking away from the hero. */
int SpawnPack(const Player &player, int count)
{
	if (ReachableTiles.empty() || LevelMonsterTypeCount <= 1)
		return 0;
	const Point hero = player.position.tile;
	UpdatePathDistances(hero);
	const std::optional<Point> spot = FindSpawnPoint(10, 18);
	if (!spot)
		return 0;
	const Point anchor = *spot;

	// One monster type per pack, like the dungeon's own groups.
	const size_t typeIndex = 1 + static_cast<size_t>(GenerateRnd(static_cast<int>(LevelMonsterTypeCount - 1)));
	int placed = 0;
	for (int radius = 0; radius <= 3 && placed < count; radius++) {
		for (int dy = -radius; dy <= radius && placed < count; dy++) {
			for (int dx = -radius; dx <= radius && placed < count; dx++) {
				if (std::max(std::abs(dx), std::abs(dy)) != radius)
					continue;
				const Point p = anchor + Displacement { dx, dy };
				if (!CanSpawnAt(p) || ActiveMonsterCount >= MaxMonsters - 2)
					continue;
				Monster *monster = AddMonster(p, GetDirection(p, hero), typeIndex, true);
				if (monster == nullptr)
					return placed;
				ScaleForWave(*monster, Waves.wave);
				Hunt(*monster, player);
				placed++;
			}
		}
	}
	return placed;
}

/** Monsters that got hung up behind thickets are quietly moved to a fresh spot near the hero. */
void RelocateStragglers(const Player &player)
{
	UpdatePathDistances(player.position.tile);
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		Monster &monster = Monsters[ActiveMonsters[i]];
		if (!IsWaveMonster(monster) || monster.mode != MonsterMode::Stand || IsTileVisible(monster.position.tile))
			continue;
		const int d = PathDistance[monster.position.tile.x][monster.position.tile.y];
		if (d >= 0 && d <= 8)
			continue;
		const std::optional<Point> spot = FindSpawnPoint(7, 12);
		if (!spot)
			continue;
		M_ClearSquares(monster);
		dMonster[monster.position.tile.x][monster.position.tile.y] = 0;
		monster.position.tile = *spot;
		monster.position.future = *spot;
		monster.position.old = *spot;
		monster.occupyTile(*spot, false);
		monster.direction = GetDirection(*spot, player.position.tile);
		Hunt(monster, player);
	}
	MarkWalkabilityChanged();
}

void GiveWaveReward(const Player &player)
{
	CreateRndUseful(player.position.tile, false);
	if (Waves.wave % 5 == 0) {
		CreateRndItem(player.position.tile, true, false, false);
		CreateRndItem(player.position.tile, true, false, false);
	}
}

void StartWave()
{
	Waves.wave++;
	Waves.phase = WavePhase::Spawning;
	Waves.toSpawn = WaveSize(Waves.wave);
	Waves.spawned = 0;
	Waves.phaseTicks = 0;
	if (Waves.wave % 5 == 0)
		ShowBanner(StrCat(_("Wave"), " ", Waves.wave, " - ", _("Elite")));
	else
		ShowBanner(StrCat(_("Wave"), " ", Waves.wave));
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Availability and tiers

bool IsForestAvailable()
{
	return !gbIsMultiplayer;
}

int ForestTierForCharacterLevel(int characterLevel)
{
	int base = 0;
	if (sgGameInitInfo.nDifficulty == DIFF_NIGHTMARE)
		base = 22;
	else if (sgGameInitInfo.nDifficulty == DIFF_HELL)
		base = 34;
	const float tier = 1.0f + 0.45f * static_cast<float>(std::max(0, characterLevel - base));
	return std::clamp(static_cast<int>(tier), 1, 15);
}

// ---------------------------------------------------------------------------------------------
// Town side

void AddTownForestTrigger()
{
	TownGate = std::nullopt;
	if (!IsForestAvailable() || numtrigs <= 0 || numtrigs >= MAXTRIGGERS)
		return;
	// Walk the town from the cathedral door and take the westernmost spot the hero can reach:
	// the edge of the old woods.
	Point start = trigs[0].position + Displacement { 0, 1 };
	if (!Walkable(start)) {
		for (const Displacement d : { Displacement { 1, 1 }, Displacement { -1, 1 }, Displacement { 0, 2 }, Displacement { 1, 0 } }) {
			if (Walkable(trigs[0].position + d)) {
				start = trigs[0].position + d;
				break;
			}
		}
	}
	FloodReachable(start);
	if (ReachableTiles.empty())
		return;
	Point best = ReachableTiles.front();
	for (const Point p : ReachableTiles) {
		// Only the band of the map that borders the woods; stay off the outermost row.
		if (p.x < 8)
			continue;
		if (p.x < best.x || (p.x == best.x && std::abs(p.y - 50) < std::abs(best.y - 50)))
			best = p;
	}
	TownGate = best;
	LogVerbose("Forest gate in town at {},{}", best.x, best.y);
	trigs[numtrigs].position = best;
	trigs[numtrigs]._tmsg = WM_DIABSETLVL;
	trigs[numtrigs]._tlvl = SL_FOREST;
	numtrigs++;
	ReachableTiles.clear();
}

std::optional<Point> TownForestGate()
{
	return TownGate;
}

bool ForceTownForestTrig()
{
	if (!TownGate)
		return false;
	if (cursPosition.WalkingDistance(*TownGate) > 1)
		return false;
	InfoString = _("Into the Darkwood");
	cursPosition = *TownGate;
	return true;
}

void EnterForest()
{
	Player &player = *MyPlayer;
	uint32_t seed = static_cast<uint32_t>(SDL_GetTicks()) * 2654435761U ^ static_cast<uint32_t>(std::time(nullptr)) ^ player.townSeed;
	if (seed == 0)
		seed = 1;
	player.forestSeed = seed;
	player.forestTier = static_cast<uint8_t>(ForestTierForCharacterLevel(player.getCharacterLevel()));
#ifdef _DEBUG
	if (const char *tier = std::getenv("DEVILUTIONX_DEV_FORESTTIER"); tier != nullptr)
		player.forestTier = static_cast<uint8_t>(std::clamp(std::atoi(tier), 1, 16));
	if (const char *fseed = std::getenv("DEVILUTIONX_DEV_FORESTSEED"); fseed != nullptr)
		player.forestSeed = static_cast<uint32_t>(std::strtoul(fseed, nullptr, 10));
#endif
	setlvltype = DTYPE_FOREST;
	StartNewLvl(player, WM_DIABSETLVL, SL_FOREST);
}

// ---------------------------------------------------------------------------------------------
// Forest level

void CreateForest(uint32_t seed)
{
	ForestRng rng(seed);
	for (auto &col : Grid)
		col.fill(Cell::Woods);

	// Clearings scattered across the region.
	std::vector<Clearing> clearings;
	const int wanted = rng.Between(11, 15);
	for (int attempt = 0; attempt < 400 && static_cast<int>(clearings.size()) < wanted; attempt++) {
		Clearing c { rng.Between(RegionMin + 3, RegionMax - 3), rng.Between(RegionMin + 3, RegionMax - 3), rng.Between(2, 5), rng.Between(2, 5) };
		const bool tooClose = std::any_of(clearings.begin(), clearings.end(), [&](const Clearing &o) {
			return std::abs(o.i - c.i) + std::abs(o.j - c.j) < 9;
		});
		if (!tooClose)
			clearings.push_back(c);
	}
	for (const Clearing &c : clearings)
		CarveClearing(rng, c);

	// Join the clearings with trails: a spanning tree plus a couple of loops.
	const size_t n = clearings.size();
	std::vector<bool> linked(n, false);
	if (n > 0)
		linked[0] = true;
	for (size_t added = 1; added < n; added++) {
		int bestD = INT32_MAX;
		size_t ba = 0, bb = 0;
		for (size_t a = 0; a < n; a++) {
			if (!linked[a])
				continue;
			for (size_t b = 0; b < n; b++) {
				if (linked[b])
					continue;
				const int d = std::abs(clearings[a].i - clearings[b].i) + std::abs(clearings[a].j - clearings[b].j);
				if (d < bestD) {
					bestD = d;
					ba = a;
					bb = b;
				}
			}
		}
		linked[bb] = true;
		CarveTrail(rng, { clearings[ba].i, clearings[ba].j }, { clearings[bb].i, clearings[bb].j }, false);
	}
	for (int loops = 0; loops < 3 && n > 3; loops++) {
		const Clearing &a = clearings[rng.Range(static_cast<int>(n))];
		const Clearing &b = clearings[rng.Range(static_cast<int>(n))];
		if (&a != &b && std::abs(a.i - b.i) + std::abs(a.j - b.j) < 20)
			CarveTrail(rng, { a.i, a.j }, { b.i, b.j }, false);
	}

	// The way back to Tristram: a trail from the easternmost clearing to the east edge.
	Clearing east = clearings.empty() ? Clearing { RegionMax - 4, (RegionMin + RegionMax) / 2, 3, 3 } : clearings.front();
	for (const Clearing &c : clearings)
		if (c.i > east.i)
			east = c;
	if (clearings.empty())
		CarveClearing(rng, east);
	const int exitJ = std::clamp(east.j, RegionMin + 2, RegionMax - 2);
	CarveTrail(rng, { east.i, east.j }, { RegionMax, exitJ }, false);
	for (int i = RegionMax - 1; i <= ExitMega; i++) {
		Grid[i][exitJ] = Cell::Trail;
		Grid[i][exitJ + 1] = Cell::Trail;
	}

	// Soften the woods line: fill notches so clearings get natural, rounded edges.
	for (int pass = 0; pass < 2; pass++) {
		for (int j = RegionMin; j <= RegionMax; j++)
			for (int i = RegionMin; i <= RegionMax; i++)
				if (Grid[i][j] == Cell::Woods && OpenNeighbours4(i, j) >= 3)
					Grid[i][j] = Cell::Open;
	}
	// A few lone thickets inside large clearings, for cover.
	for (int j = RegionMin + 1; j < RegionMax; j++) {
		for (int i = RegionMin + 1; i < RegionMax; i++) {
			if (Grid[i][j] != Cell::Open || NearTrail(i, j, 2))
				continue;
			bool allOpen = true;
			for (int dj = -1; dj <= 1 && allOpen; dj++)
				for (int di = -1; di <= 1; di++)
					if (Grid[i + di][j + dj] == Cell::Woods)
						allOpen = false;
			if (allOpen && rng.Chance(0.05f))
				Grid[i][j] = Cell::Woods;
		}
	}

	// Paint megatiles.
	memset(dungeon, 0, sizeof(dungeon));
	std::array<std::array<bool, MegaW>, MegaW> tree {};
	for (int j = 0; j < MegaW; j++) {
		for (int i = 0; i < MegaW; i++) {
			int id;
			switch (Grid[i][j]) {
			case Cell::Woods:
				if (i < 8 || j < 8 || i > ExitMega || j > ExitMega)
					id = WoodsDense[0]; // fully solid border ring: nothing walks or flies off the map
				else if (OpenNeighbours4(i, j) > 0 && InRegion(i, j) && rng.Chance(0.22f))
					id = rng.Pick(Trees); // a ragged tree line where the woods meet a clearing
				else
					id = (OpenNeighbours4(i, j) > 0 && rng.Chance(0.7f)) || rng.Chance(0.3f) ? rng.Pick(WoodsEdge) : rng.Pick(WoodsDense);
				break;
			case Cell::Trail:
				id = rng.Chance(0.12f) ? rng.Pick(GroundDetail) : rng.Pick(Grass);
				break;
			case Cell::Open:
			default: {
				const bool besideTree = (i > 0 && tree[i - 1][j]) || (j > 0 && tree[i][j - 1]) || (i > 0 && j > 0 && tree[i - 1][j - 1]) || (i + 1 < MegaW && j > 0 && tree[i + 1][j - 1]);
				if (!besideTree && !NearTrail(i, j, 0) && rng.Chance(0.07f)) {
					id = rng.Pick(Trees);
					tree[i][j] = true;
				} else if (rng.Chance(0.14f)) {
					id = rng.Pick(GroundDetail);
				} else {
					id = rng.Pick(Grass);
				}
				break;
			}
			}
			PlaceMegatile(i, j, id);
		}
	}

	dminPosition = { 16, 16 };
	dmaxPosition = { 96, 96 };

	// The exit is the easternmost walkable spot on the exit trail.
	const Point exitGuess { 2 * ExitMega + 1, 2 * exitJ + 1 };
	ForestExit = exitGuess;
	for (int x = 2 * ExitMega + 1; x >= 2 * (RegionMax - 2); x--) {
		bool done = false;
		for (const int dy : { 1, 2, 0, 3 }) {
			const Point p { x, 2 * exitJ + dy };
			if (Walkable(p)) {
				ForestExit = p;
				done = true;
				break;
			}
		}
		if (done)
			break;
	}
	Point spawn = ForestExit + Displacement { -3, 0 };
	for (int k = 3; k < 8 && !Walkable(spawn); k++)
		spawn = ForestExit + Displacement { -k, 0 };
	ViewPosition = spawn;
	FloodReachable(spawn);
	LogVerbose("Forest: seed {} clearings {} reachable {} exit {},{}", seed, clearings.size(), ReachableTiles.size(), ForestExit.x, ForestExit.y);
}

void InitForestTriggers()
{
	numtrigs = 1;
	trigs[0].position = ForestExit;
	trigs[0]._tmsg = WM_DIABRTNLVL;
	trigflag = false;
}

bool ForceForestTrig()
{
	if (cursPosition.WalkingDistance(ForestExit) > 1)
		return false;
	InfoString = _("Back to Tristram");
	cursPosition = ForestExit;
	return true;
}

std::expected<void, std::string> AddForestMonsterTypes()
{
	const Player &player = *MyPlayer;
	int tier = player.forestTier;
	if (tier <= 0)
		tier = ForestTierForCharacterLevel(player.getCharacterLevel());

	std::vector<_monster_id> candidates;
	for (size_t i = 0; i < MonstersData.size(); i++) {
		const MonsterData &data = MonstersData[i];
		if (data.availability == MonsterAvailability::Never)
			continue;
		if (gbIsSpawn && data.availability == MonsterAvailability::Retail)
			continue;
		if (static_cast<_monster_id>(i) == MT_GOLEM || data.minDunLvl <= 0)
			continue;
		if (tier >= data.minDunLvl && tier <= data.maxDunLvl)
			candidates.push_back(static_cast<_monster_id>(i));
	}
	// Same hero, same forest visit: same monster set (needed to reload a save made here).
	ForestRng rng(static_cast<uint64_t>(player.forestSeed) * 31 + static_cast<uint64_t>(tier));
	int added = 0;
	while (!candidates.empty() && added < 4 && LevelMonsterTypeCount < MaxLvlMTypes) {
		const int k = rng.Range(static_cast<int>(candidates.size()));
		const _monster_id type = candidates[k];
		candidates.erase(candidates.begin() + k);
		if (MonstersData[type].image > 4000 - monstimgtot)
			continue;
		RETURN_IF_ERROR(AddMonsterType(type, PLACE_SCATTER));
		added++;
	}
	LogVerbose("Forest: tier {} with {} monster types", tier, added);
	return {};
}

void ResetForestWaves()
{
	Waves = {};
	Waves.phase = WavePhase::Break;
	Waves.phaseTicks = 4 * TicksPerSecond; // a moment to look around before the first wave
	ShowBanner(std::string(_("The Darkwood")));
}

void UpdateForestWaves()
{
	if (leveltype != DTYPE_FOREST || MyPlayer == nullptr)
		return;
	Player &player = *MyPlayer;
	if (player._pHitPoints <= 0 || player._pLvlChanging)
		return;

	Waves.ticks++;
	if (Waves.bannerTicks > 0)
		Waves.bannerTicks--;

	switch (Waves.phase) {
	case WavePhase::Break:
		if (--Waves.phaseTicks <= 0)
			StartWave();
		break;
	case WavePhase::Spawning:
		if (++Waves.phaseTicks >= 2 * TicksPerSecond || Waves.spawned == 0) {
			Waves.phaseTicks = 0;
			if (AliveWaveMonsters() < MaxAliveInWave) {
				const int packSize = std::min(Waves.toSpawn - Waves.spawned, 3 + GenerateRnd(3) + std::min(3, Waves.wave / 4));
				Waves.spawned += std::max(1, SpawnPack(player, packSize)); // count failed packs too, so a wave never stalls
			}
			if (Waves.spawned >= Waves.toSpawn)
				Waves.phase = WavePhase::Fighting;
		}
		break;
	case WavePhase::Fighting:
		if (AliveWaveMonsters() == 0) {
			if (Waves.wave > player.forestBestWave)
				player.forestBestWave = static_cast<uint16_t>(Waves.wave);
			ShowBanner(StrCat(_("Wave"), " ", Waves.wave, " ", _("cleared")));
			LogVerbose("Forest wave {} cleared, hero life {}", Waves.wave, player._pHitPoints.whole());
			GiveWaveReward(player);
			Waves.phase = WavePhase::Break;
			Waves.phaseTicks = 6 * TicksPerSecond;
		}
		break;
	}

#ifdef _DEBUG
	// Test autopilot: god mode and swing at whatever is adjacent.
	if (std::getenv("DEVILUTIONX_DEV_AUTOFIGHT") != nullptr) {
		DebugGodMode = std::getenv("DEVILUTIONX_DEV_NOGOD") == nullptr;
		const char *autoSkill = std::getenv("DEVILUTIONX_DEV_AUTOSKILL");
		if (autoSkill != nullptr && player._pmode == PM_STAND && player.destAction == ACTION_NONE) {
			// Cast the readied skill at the nearest wave monster within 8 tiles.
			int best = -1;
			int bestDist = 9;
			for (size_t i = 0; i < ActiveMonsterCount; i++) {
				const Monster &monster = Monsters[ActiveMonsters[i]];
				const int dist = player.position.tile.WalkingDistance(monster.position.tile);
				if (IsWaveMonster(monster) && !monster.hasNoLife() && dist < bestDist) {
					best = static_cast<int>(ActiveMonsters[i]);
					bestDist = dist;
				}
			}
			if (best >= 0) {
				static int casts = 0;
				if (++casts <= 10 || casts % 20 == 0)
					LogInfo("AUTOSKILL cast #{} spell {} mana {}", casts, static_cast<int>(player._pRSpell), player._pMana.whole());
				NetSendCmdParam4(true, CMD_SPELLID, static_cast<uint16_t>(best), static_cast<int8_t>(player._pRSpell), static_cast<uint8_t>(SpellType::Spell), 0);
			}
		} else if (autoSkill == nullptr && player._pmode == PM_STAND) {
			for (size_t i = 0; i < ActiveMonsterCount; i++) {
				const Monster &monster = Monsters[ActiveMonsters[i]];
				if (IsWaveMonster(monster) && player.position.tile.WalkingDistance(monster.position.tile) <= 1) {
					NetSendCmdParam1(true, CMD_ATTACKID, static_cast<uint16_t>(ActiveMonsters[i]));
					break;
				}
			}
		}
	}
#endif

	// Watch for progress (kills or a fight at the hero's side); if a wave stalls, bring the
	// stragglers closer instead of leaving the hero waiting.
	if (Waves.phase != WavePhase::Break) {
		const int alive = AliveWaveMonsters();
		bool engaged = alive < Waves.lastAlive;
		for (size_t i = 0; i < ActiveMonsterCount && !engaged; i++) {
			const Monster &monster = Monsters[ActiveMonsters[i]];
			if (IsWaveMonster(monster) && player.position.tile.WalkingDistance(monster.position.tile) <= 3)
				engaged = true;
		}
		Waves.lastAlive = alive;
		if (engaged || Waves.phase == WavePhase::Spawning)
			Waves.lastProgressTick = Waves.ticks;
		else if (Waves.ticks - Waves.lastProgressTick > 10 * TicksPerSecond) {
			RelocateStragglers(player);
			Waves.lastProgressTick = Waves.ticks;
		}
	}

	// Keep every wave monster hunting the hero, even out of sight.
	if (Waves.ticks % TicksPerSecond == 0) {
		for (size_t i = 0; i < ActiveMonsterCount; i++) {
			Monster &monster = Monsters[ActiveMonsters[i]];
			if (IsWaveMonster(monster) && monster.activeForTicks < 200)
				Hunt(monster, player);
		}
	}
}

float ForestExperienceMultiplier()
{
	if (leveltype != DTYPE_FOREST || Waves.wave <= 0)
		return 1.0f;
	return 1.0f + 0.08f * static_cast<float>(Waves.wave - 1);
}

void DrawForestHud(const Surface &out)
{
	if (leveltype != DTYPE_FOREST)
		return;
	std::string line;
	if (Waves.wave == 0) {
		line = StrCat(_("First wave in"), " ", (Waves.phaseTicks + TicksPerSecond - 1) / TicksPerSecond);
	} else if (Waves.phase == WavePhase::Break) {
		line = StrCat(_("Wave"), " ", Waves.wave + 1, " ", _("in"), " ", (Waves.phaseTicks + TicksPerSecond - 1) / TicksPerSecond);
	} else {
		const int left = (Waves.toSpawn - Waves.spawned) + AliveWaveMonsters();
		line = StrCat(_("Wave"), " ", Waves.wave, "   ", left, " ", _("left"));
	}
	if (MyPlayer != nullptr && MyPlayer->forestBestWave > 0)
		StrAppend(line, "   ", _("Best"), " ", MyPlayer->forestBestWave);
	DrawString(out, line, { { 0, 8 }, { out.w(), 20 } }, { .flags = UiFlags::AlignCenter | UiFlags::ColorWhitegold | UiFlags::Outlined });

	if (Waves.bannerTicks > 0 && !Waves.banner.empty()) {
		const int y = out.h() / 4;
		DrawString(out, Waves.banner, { { 0, y }, { out.w(), 40 } }, { .flags = UiFlags::AlignCenter | UiFlags::FontSize30 | UiFlags::ColorGold | UiFlags::Outlined });
	}
}

} // namespace devilution
