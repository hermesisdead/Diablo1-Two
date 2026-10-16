#include "levels/desert.hpp"

#include "custom_maps/custom_town.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "control/control.hpp"
#include "cursor.h"
#include "engine/load_file.hpp"
#include "game_mode.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "levels/trigs.h"
#include "player.h"
#include "utils/language.h"
#include "utils/log.hpp"
#include "utils/str_cat.hpp"
#include "utils/str_split.hpp"

namespace devilution {

namespace {

bool ArrivedByCaravan = false;

struct StructureEntry {
	std::string name;
	std::string category;
	DesertStructure shape;
};

/** Piece groups, loaded once from txtdata\desert\pieces.tsv. */
std::vector<std::vector<uint16_t>> PieceStorage;
std::vector<StructureEntry> Structures;
std::unordered_map<std::string, std::vector<DesertStructure>> Categories;

void LoadPieceTable()
{
	if (!Structures.empty())
		return;
	size_t size = 0;
	const std::unique_ptr<char[]> text = LoadFileInMem<char>("txtdata\\desert\\pieces.tsv", &size);
	const std::string_view all(text.get(), size);
	bool header = true;
	PieceStorage.reserve(128);
	for (std::string_view line : SplitByChar(all, '\n')) {
		if (!line.empty() && line.back() == '\r')
			line.remove_suffix(1);
		if (line.empty())
			continue;
		if (header) {
			header = false;
			continue;
		}
		std::array<std::string_view, 5> fields;
		size_t n = 0;
		for (const std::string_view f : SplitByChar(line, '\t')) {
			if (n < fields.size())
				fields[n++] = f;
		}
		if (n < 5)
			continue;
		std::vector<uint16_t> ids;
		for (const std::string_view id : SplitByChar(fields[4], ','))
			ids.push_back(static_cast<uint16_t>(std::atoi(std::string(id).c_str())));
		PieceStorage.push_back(std::move(ids));
		StructureEntry entry;
		entry.name = std::string(fields[0]);
		entry.category = std::string(fields[1]);
		entry.shape.width = std::atoi(std::string(fields[2]).c_str());
		entry.shape.height = std::atoi(std::string(fields[3]).c_str());
		entry.shape.pieces = PieceStorage.back();
		Structures.push_back(entry);
	}
	for (const StructureEntry &entry : Structures)
		Categories[entry.category].push_back(entry.shape);
}

/** Deterministic little RNG for layouts (keeps the engine's seeds untouched). */
struct LayoutRng {
	uint64_t state;
	explicit LayoutRng(uint64_t seed)
	    : state(seed * 0x9E3779B97F4A7C15ULL + 0x2545F4914F6CDD1DULL)
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
	bool Chance(float p) { return static_cast<float>(Next() & 0xFFFF) / 65536.0f < p; }
};

uint16_t PickPiece(LayoutRng &rng, std::string_view category)
{
	const std::span<const DesertStructure> group = GetDesertCategory(category);
	if (group.empty())
		return 1;
	return group[rng.Range(static_cast<int>(group.size()))].pieces[0];
}

/** Tiles claimed by structures, so the scatter passes leave them alone. */
std::array<std::array<bool, MAXDUNY>, MAXDUNX> Claimed;

bool CanPlace(Point at, int w, int h)
{
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++) {
			const Point p = at + Displacement { i, j };
			if (!InDungeonBounds(p) || Claimed[p.x][p.y])
				return false;
		}
	return true;
}

void Place(std::string_view name, Point at)
{
	const DesertStructure s = GetDesertStructure(name);
	if (s.width == 0)
		return;
	for (int j = 0; j < s.height; j++) {
		for (int i = 0; i < s.width; i++) {
			const Point p = at + Displacement { i, j };
			if (!InDungeonBounds(p))
				continue;
			dPiece[p.x][p.y] = s.pieces[j * s.width + i];
			Claimed[p.x][p.y] = true;
		}
	}
}

/** Keeps a tile walkable sand (paths, NPC spots, portal spots). */
void Reserve(Point at, int w = 1, int h = 1)
{
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++) {
			const Point p = at + Displacement { i, j };
			if (InDungeonBounds(p))
				Claimed[p.x][p.y] = true;
		}
}

// Camp landmarks (tile coordinates).
constexpr Point WellAt { 48, 48 };
constexpr Point WastesGate { 81, 47 };
constexpr Point CaravanArrival { 59, 58 };
constexpr int CampMin = 17;
constexpr int CampMax = 79;

} // namespace

bool InDesertRegion()
{
	return MyPlayer != nullptr && MyPlayer->inDesert && !gbIsMultiplayer;
}

void SetInDesertRegion(bool inDesert)
{
	if (MyPlayer != nullptr)
		MyPlayer->inDesert = inDesert;
}

bool IsDesertAvailable()
{
	return !gbIsMultiplayer;
}

bool ArrivingByCaravan()
{
	return ArrivedByCaravan;
}

void ClearCaravanArrival()
{
	ArrivedByCaravan = false;
}

void TravelByCaravan(bool toDesert)
{
	if (!IsDesertAvailable() || MyPlayer == nullptr)
		return;
	Player &player = *MyPlayer;
	SetCurrentCustomTown({}); // the caravan runs between Tristram and Sand Tristram
	SetInDesertRegion(toDesert);
	ArrivedByCaravan = true;
	StartNewLvl(player, WM_DIABRETOWN, 0);
}

DesertStructure GetDesertStructure(std::string_view name)
{
	LoadPieceTable();
	for (const StructureEntry &entry : Structures) {
		if (entry.name == name)
			return entry.shape;
	}
	return {};
}

std::span<const DesertStructure> GetDesertCategory(std::string_view category)
{
	LoadPieceTable();
	const auto it = Categories.find(std::string(category));
	if (it == Categories.end())
		return {};
	return it->second;
}

void CreateDesertTown(lvl_entry entry)
{
	LoadPieceTable();
	LayoutRng rng(0xD35E27);
	for (auto &col : Claimed)
		col.fill(false);
	memset(dungeon, 0, sizeof(dungeon));

	// Sand everywhere, with a little detail; hard-baked mud around the well.
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			const int dx = x - WellAt.x - 1, dy = y - WellAt.y - 1;
			if (dx * dx + dy * dy < 22 && rng.Chance(0.25f))
				dPiece[x][y] = GetDesertStructure(rng.Chance(0.5f) ? "hardpan0" : "hardpan1").pieces[0];
			else if (rng.Chance(0.1f))
				dPiece[x][y] = PickPiece(rng, "floordetail");
			else
				dPiece[x][y] = PickPiece(rng, "floor");
		}
	}

	// Keep the paths, the portal spots and the NPCs' standing room clear.
	Reserve({ 56, 39 }, 9, 3);  // town portals land on 57..63, 40
	Reserve({ 66, 46 }, 16, 3); // road to the wastes
	Reserve(CaravanArrival + Displacement { -2, -2 }, 5, 4);
	Reserve({ 37, 46 }, 3, 3);  // smith
	Reserve({ 52, 55 }, 3, 3);  // Cain by the fire
	Reserve({ 61, 52 }, 5, 6);  // caravan and pack cow
	Reserve({ WellAt.x - 2, WellAt.y + 2 }, 2, 2);

	// The oasis.
	Place("well", WellAt);
	Place("palm0", WellAt + Displacement { -2, -1 });
	Place("palm1", WellAt + Displacement { 3, -1 });
	Place("palm0", WellAt + Displacement { 2, 3 });
	Place("campfire", { 54, 54 });

	// Griswold's forge and hut (west).
	Place("forge", { 36, 43 });
	Place("shack", { 31, 40 });
	Place("crates", { 34, 46 });
	Place("barrels", { 35, 47 });
	Place("urns", { 39, 42 });

	// The priest's pavilion (north-east), its rug and a lamp-post totem.
	Place("tent_big", { 56, 35 });
	Place("rug_red", { 57, 38 });
	Place("urns", { 61, 36 });
	Place("totem", { 55, 38 });

	// The caravan's stop (south-east).
	Place("stall_b", { 66, 50 });
	Place("crates", { 67, 54 });
	Place("sacks", { 68, 54 });
	Place("sacks", { 66, 55 });
	Place("barrels", { 69, 52 });
	Place("leanto", { 70, 56 });

	// The rest of the camp.
	Place("yurt", { 41, 30 });
	Place("tent_small", { 47, 33 });
	Place("hut", { 64, 27 });
	Place("tent_small_b", { 25, 50 });
	Place("yurt", { 29, 57 });
	Place("hut", { 40, 62 });
	Place("tent_small", { 48, 65 });
	Place("stall", { 36, 52 });
	Place("leanto", { 24, 36 });
	Place("tent_small_b", { 56, 66 });
	Place("rug_blue", { 46, 60 });
	Place("campfire", { 44, 58 });
	Place("totem", { 70, 45 });
	Place("totem", { 70, 49 });
	Place("deadtree0", { 22, 44 });
	Place("deadtree1", { 66, 64 });
	Place("palm1", { 33, 33 });

	// Fill out the camp: more shelters and clutter scattered around the oasis, never touching
	// each other so paths stay open between them.
	const auto placeScatter = [&](std::string_view name, int attempts, int minRadius, int maxRadius) {
		const DesertStructure shape = GetDesertStructure(name);
		for (int a = 0; a < attempts; a++) {
			const Point at { WellAt.x - maxRadius + rng.Range(2 * maxRadius), WellAt.y - maxRadius + rng.Range(2 * maxRadius) };
			const int dx = at.x - WellAt.x, dy = at.y - WellAt.y;
			const int d2 = dx * dx + dy * dy;
			if (d2 < minRadius * minRadius || d2 > maxRadius * maxRadius)
				continue;
			if (at.x < CampMin + 2 || at.y < CampMin + 2 || at.x + shape.width > CampMax - 2 || at.y + shape.height > CampMax - 2)
				continue;
			if (!CanPlace(at + Displacement { -1, -1 }, shape.width + 2, shape.height + 2))
				continue;
			Place(name, at);
		}
	};
	placeScatter("tent_small", 6, 10, 26);
	placeScatter("tent_small_b", 6, 10, 26);
	placeScatter("leanto", 5, 10, 26);
	placeScatter("yurt", 2, 14, 26);
	placeScatter("hut", 2, 16, 27);
	placeScatter("shack", 2, 14, 26);
	placeScatter("rug_blue", 3, 6, 22);
	placeScatter("campfire", 4, 8, 24);
	for (const std::string_view clutter : { "crates", "sacks", "barrels", "urns", "totem" })
		placeScatter(clutter, 14, 5, 28);
	placeScatter("palm1", 6, 3, 9);
	placeScatter("deadtree0", 6, 14, 30);
	placeScatter("deadtree1", 6, 14, 30);
	placeScatter("boulder0", 4, 22, 31);
	placeScatter("boulder1", 4, 22, 31);
	for (int k = 0; k < 5; k++)
		placeScatter(StrCat("rock", k), 4, 12, 31);

	// A ridge of rock all around, broken only where the road leaves for the wastes.
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			if (Claimed[x][y])
				continue;
			const int inside = std::min({ x - CampMin, y - CampMin, CampMax - x, CampMax - y });
			const bool road = y >= 46 && y <= 48 && x >= 66;
			if (inside < 0 && !(road && x <= WastesGate.x)) {
				dPiece[x][y] = PickPiece(rng, inside < -2 ? "pillar" : (rng.Chance(0.5f) ? "pillar" : "rock"));
				Claimed[x][y] = true;
			} else if (inside < 3 && !road && rng.Chance(inside == 0 ? 0.55f : 0.2f)) {
				dPiece[x][y] = PickPiece(rng, rng.Chance(0.6f) ? "rock" : "pillar");
				Claimed[x][y] = true;
			} else if (inside > 3 && !road && rng.Chance(0.012f)) {
				dPiece[x][y] = PickPiece(rng, rng.Chance(0.6f) ? "rock" : "tree");
				Claimed[x][y] = true;
			}
		}
	}

	dminPosition = { 10, 10 };
	dmaxPosition = { 84, 84 };

	switch (entry) {
	case ENTRY_MAIN:
		ViewPosition = CaravanArrival;
		break;
	case ENTRY_RTNLVL:
		ViewPosition = DesertWastesReturnPosition();
		break;
	default:
		ViewPosition = CaravanArrival;
		break;
	}
	ArrivedByCaravan = false;
#ifdef _DEBUG
	if (const char *view = std::getenv("DEVILUTIONX_DEV_VIEWPOS"); view != nullptr) {
		char *end = nullptr;
		const long vx = std::strtol(view, &end, 10);
		if (end != nullptr && *end == ',')
			ViewPosition = { static_cast<int>(vx), static_cast<int>(std::strtol(end + 1, nullptr, 10)) };
		if (MyPlayer != nullptr)
			MyPlayer->position.tile = ViewPosition;
	}
#endif
#ifdef _DEBUG
	{
		// Debug check: everything that matters must be reachable from the caravan's stop.
		std::vector<Point> queue { CaravanArrival };
		std::array<std::array<bool, MAXDUNY>, MAXDUNX> seen {};
		seen[CaravanArrival.x][CaravanArrival.y] = true;
		for (size_t head = 0; head < queue.size(); head++) {
			const Point cur = queue[head];
			for (const Displacement d : { Displacement { 1, 0 }, Displacement { -1, 0 }, Displacement { 0, 1 }, Displacement { 0, -1 } }) {
				const Point n = cur + d;
				if (!InDungeonBounds(n) || seen[n.x][n.y] || HasAnyOf(SOLData[dPiece[n.x][n.y]], TileProperties::Solid))
					continue;
				seen[n.x][n.y] = true;
				queue.push_back(n);
			}
		}
		const auto ok = [&](Point p) { return seen[p.x][p.y] ? "ok" : "BLOCKED"; };
		LogVerbose("Sand Tristram reach: {} tiles; gate {} portal {} smith {} cain {} priest {} caravan {}", queue.size(), ok(WastesGate),
		    ok({ 58, 41 }), ok({ 38, 48 }), ok({ 53, 57 }), ok({ 60, 40 }), ok({ 62, 58 }));
	}
#endif
	LogVerbose("Sand Tristram created (entry {})", static_cast<int>(entry));
}

void InitDesertTownTriggers()
{
	numtrigs = 0;
	trigs[numtrigs].position = WastesGate;
	trigs[numtrigs]._tmsg = WM_DIABSETLVL;
	trigs[numtrigs]._tlvl = SL_FOREST;
	numtrigs++;
	trigflag = false;
}

bool ForceDesertTownTrig()
{
	if (cursPosition.WalkingDistance(WastesGate) > 1)
		return false;
	InfoString = _("Into the Desert Wastes");
	cursPosition = WastesGate;
	return true;
}

Point DesertWastesReturnPosition()
{
	return WastesGate + Displacement { -2, 0 };
}

void PaintDesertWastes(const uint8_t (&cells)[56][56], uint32_t seed)
{
	LoadPieceTable();
	LayoutRng rng(seed ^ 0x5A4D);
	for (int j = 0; j < 56; j++) {
		for (int i = 0; i < 56; i++) {
			const uint8_t cell = cells[i][j];
			for (int dj = 0; dj < 2; dj++) {
				for (int di = 0; di < 2; di++) {
					const int x = 2 * i + di, y = 2 * j + dj;
					uint16_t piece;
					if (cell == 0) {
						// Rock: sandstone columns packed into ridges, with the odd dead tree.
						piece = PickPiece(rng, rng.Chance(0.55f) ? "pillar" : (rng.Chance(0.9f) ? "rock" : "tree"));
					} else if (cell == 2) {
						piece = rng.Chance(0.15f) ? PickPiece(rng, "floordetail") : PickPiece(rng, "floor");
					} else if (rng.Chance(0.025f)) {
						piece = PickPiece(rng, rng.Chance(0.5f) ? "rock" : "tree");
					} else if (rng.Chance(0.12f)) {
						piece = PickPiece(rng, "floordetail");
					} else {
						piece = PickPiece(rng, "floor");
					}
					dPiece[x][y] = piece;
				}
			}
		}
	}
}

} // namespace devilution
