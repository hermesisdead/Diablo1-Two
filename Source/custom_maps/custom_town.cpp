#include "custom_maps/custom_town.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>

#include <magic_enum/magic_enum.hpp>

#include "custom_maps/custom_map.hpp"
#include "custom_maps/map_editor.hpp"
#include "control/control.hpp"
#include "cursor.h"
#include "engine/direction.hpp"
#include "game_mode.hpp"
#include "interfac.h"
#include "levels/desert.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "levels/town_shuffle.hpp"
#include "missiles.h"
#include "msg.h"
#include "player.h"
#include "tables/townerdat.hpp"
#include "towners.h"
#include "engine/load_file.hpp"
#include "utils/endian_swap.hpp"
#include "utils/log.hpp"
#include "utils/str_cat.hpp"

namespace devilution {

namespace {

std::string TownName;
std::optional<CustomMap> TownMap;
std::optional<CustomMap> TristramLayerForEditor;

bool PendingArrival = false;
std::string PendingEntry;

struct LiveLink {
	Point tile;
	bool door;
	MapLink to;
};

std::vector<LiveLink> Links;
std::vector<MapEntryPoint> Entries;
bool LinksPlaced = false;
Point LastHeroTile { -1, -1 };

/** Walkable ground nearest to a tile (searching outwards a few tiles). */
Point NearestOpenTile(Point p)
{
	auto open = [](Point t) { return InDungeonBounds(t) && !IsTileSolid(t); };
	if (open(p))
		return p;
	for (int r = 1; r <= 6; r++) {
		for (int dy = -r; dy <= r; dy++) {
			for (int dx = -r; dx <= r; dx++) {
				if (std::max(std::abs(dx), std::abs(dy)) != r)
					continue;
				const Point t = p + Displacement { dx, dy };
				if (open(t))
					return t;
			}
		}
	}
	return p;
}

/** Fills one town megatile (1-based id, as in town .dun files) into the level's pieces. */
void FillMegaTile(int x, int y, int tile)
{
	if (tile <= 0)
		return;
	const MegaTile mega = pMegaTiles[tile - 1];
	dPiece[x + 0][y + 0] = Swap16LE(mega.micro1);
	dPiece[x + 1][y + 0] = Swap16LE(mega.micro2);
	dPiece[x + 0][y + 1] = Swap16LE(mega.micro3);
	dPiece[x + 1][y + 1] = Swap16LE(mega.micro4);
}

std::string LinkLabel(const LiveLink &link)
{
	const std::string_view what = link.door ? "Door" : "Portal";
	switch (link.to.kind) {
	case MapLinkKind::Map: return StrCat(what, " to ", link.to.map);
	case MapLinkKind::Town: return StrCat(what, " to Tristram");
	case MapLinkKind::Dungeon: return StrCat(what, " to dungeon level ", link.to.level);
	default: return std::string(what);
	}
}

} // namespace

const std::vector<uint16_t> &ClassicTownMegaTiles()
{
	static std::vector<uint16_t> grid;
	if (!grid.empty())
		return grid;
	grid.assign(48 * 48, 0);
	const struct {
		const char *path;
		int x, y;
	} sectors[] = {
		{ "levels\\towndata\\sector4s.dun", 0, 0 },
		{ "levels\\towndata\\sector2s.dun", 23, 0 },
		{ "levels\\towndata\\sector3s.dun", 0, 23 },
		{ "levels\\towndata\\sector1s.dun", 23, 23 },
	};
	for (const auto &sector : sectors) {
		const std::unique_ptr<uint16_t[]> dun = LoadFileInMem<uint16_t>(sector.path);
		const int w = Swap16LE(dun[0]), h = Swap16LE(dun[1]);
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				if (sector.x + x < 48 && sector.y + y < 48)
					grid[static_cast<size_t>(sector.y + y) * 48 + sector.x + x] = Swap16LE(dun[2 + y * w + x]);
			}
		}
	}
	return grid;
}

namespace {

/** The piece classic Tristram has at a world tile. */
uint16_t ClassicTownPiece(Point p)
{
	if (p.x < 0 || p.y < 0 || p.x >= 96 || p.y >= 96)
		return 0;
	const uint16_t tile = ClassicTownMegaTiles()[static_cast<size_t>(p.y / 2) * 48 + p.x / 2];
	if (tile == 0)
		return 0;
	const MegaTile mega = pMegaTiles[tile - 1];
	const uint16_t micro[] = { mega.micro1, mega.micro2, mega.micro3, mega.micro4 };
	return Swap16LE(micro[(p.y % 2) * 2 + p.x % 2]);
}

/** This save's Tristram looks like the classic one around a tile (the shuffle left it alone). */
bool UnchangedAround(Point p)
{
	int differences = 0;
	for (int dy = -2; dy <= 2; dy++) {
		for (int dx = -2; dx <= 2; dx++) {
			const Point t = p + Displacement { dx, dy };
			if (InDungeonBounds(t) && dPiece[t.x][t.y] != ClassicTownPiece(t))
				differences++;
		}
	}
	return differences <= 2;
}

const CustomMap *TristramLayer()
{
	static std::optional<CustomMap> loaded;
	if (TristramLayerForEditor)
		return &*TristramLayerForEditor;
	loaded = ReadTristramLayer();
	return &*loaded;
}

} // namespace

bool InCustomTown()
{
	return !gbIsMultiplayer && !TownName.empty();
}

const std::string &CurrentCustomTown()
{
	return TownName;
}

void SetCurrentCustomTown(std::string name)
{
	if (name != TownName)
		TownMap.reset();
	TownName = std::move(name);
}

const CustomMap *CurrentCustomTownMap()
{
	if (!InCustomTown())
		return nullptr;
	if (!TownMap || TownMap->name != TownName) {
		std::expected<CustomMap, std::string> map = ReadCustomMap(TownName);
		if (!map.has_value() || map->style != DTYPE_TOWN) {
			LogError("Custom town {}: {}", TownName, map.has_value() ? std::string("not a town map") : map.error());
			return nullptr;
		}
		TownMap = std::move(*map);
	}
	return &*TownMap;
}

void SetCustomTownMapForEditor(const CustomMap &map)
{
	TownName = map.name;
	TownMap = map;
}

void SetTristramLayerForEditor(const CustomMap *layer)
{
	if (layer == nullptr)
		TristramLayerForEditor.reset();
	else
		TristramLayerForEditor = *layer;
}

std::expected<void, std::string> TravelToCustomTown(std::string_view name, std::string_view entry)
{
	if (gbIsMultiplayer)
		return std::unexpected("Custom towns are single player only.");
	std::expected<CustomMap, std::string> map = ReadCustomMap(name);
	if (!map.has_value())
		return std::unexpected(map.error());
	if (map->style != DTYPE_TOWN)
		return std::unexpected(StrCat(name, " is not a town map."));
	SetInDesertRegion(false);
	TownName = std::string(name);
	TownMap = std::move(*map);
	PendingArrival = true;
	PendingEntry = std::string(entry);
	StartNewLvl(*MyPlayer, WM_DIABRETOWN, 0);
	return {};
}

void TravelToTristram(std::string_view entry)
{
	TownName.clear();
	TownMap.reset();
	SetInDesertRegion(false);
	PendingArrival = true;
	PendingEntry = std::string(entry);
	StartNewLvl(*MyPlayer, WM_DIABRETOWN, 0);
}

void EnterCustomTownForEditor(std::string_view entry)
{
	SetInDesertRegion(false);
	PendingArrival = true;
	PendingEntry = std::string(entry);
	StartNewLvl(*MyPlayer, WM_DIABRETOWN, 0);
}

void PaintCustomTownTiles()
{
	// Everything outside the painted map is the town's empty filler.
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++)
			dPiece[x][y] = 426;
	}
	std::memset(dungeon, 0, sizeof(dungeon));
	const CustomMap *map = CurrentCustomTownMap();
	if (map == nullptr)
		return;
	for (int y = 0; y < map->height; y++) {
		for (int x = 0; x < map->width; x++)
			FillMegaTile(16 + 2 * x, 16 + 2 * y, map->tiles[static_cast<size_t>(y) * map->width + x]);
	}
}

void CreateCustomTownLevel(lvl_entry entry)
{
	PaintCustomTownTiles();
	const CustomMap *map = CurrentCustomTownMap();
	if (map == nullptr) {
		ViewPosition = { 56, 56 };
		dminPosition = { 10, 10 };
		dmaxPosition = { 84, 84 };
		return;
	}
	dminPosition = { 14, 14 };
	dmaxPosition = { 18 + 2 * map->width, 18 + 2 * map->height };
	ViewPosition = map->entries.empty() ? Point { 16 + map->width, 16 + map->height } : map->entries.front().tile;
	LogVerbose("Custom town {} created (entry {})", map->name, static_cast<int>(entry));
}

void FinishTownLinks()
{
	Links.clear();
	Entries.clear();
	LinksPlaced = false;
	LastHeroTile = { -1, -1 };
	if (gbIsMultiplayer || InDesertRegion()) {
		PendingArrival = false;
		return;
	}
	if (InCustomTown()) {
		if (const CustomMap *map = CurrentCustomTownMap(); map != nullptr) {
			Entries = map->entries;
			for (const MapPortal &p : map->portals)
				Links.push_back({ p.tile, p.door, p.to });
		}
	} else if (const CustomMap *layer = TristramLayer(); layer != nullptr) {
		// Added to the classic layout: follow the nearest building in this save's Tristram.
		for (const MapEntryPoint &e : layer->entries)
			Entries.push_back({ e.name, ResolveTristramPosition(e.tile) });
		for (const MapPortal &p : layer->portals)
			Links.push_back({ ResolveTristramPosition(p.tile), p.door, p.to });
	}
	if (PendingArrival) {
		PendingArrival = false;
		const MapEntryPoint *found = nullptr;
		for (const MapEntryPoint &e : Entries) {
			if (e.name == PendingEntry) {
				found = &e;
				break;
			}
		}
		if (found == nullptr && InCustomTown() && !Entries.empty())
			found = &Entries.front();
		if (found != nullptr)
			ViewPosition = NearestOpenTile(found->tile);
	}
}

void UpdateTownLinks()
{
	if (leveltype != DTYPE_TOWN || setlevel || currlevel != 0 || Links.empty() || MyPlayer == nullptr || IsMapEditing())
		return;
	Player &player = *MyPlayer;
	if (!LinksPlaced) {
		LinksPlaced = true;
		for (const LiveLink &link : Links) {
			if (!link.door)
				AddMissile(link.tile, link.tile, Direction::South, MissileID::RedPortal, TARGET_MONSTERS, player.getId(), 0, 0);
		}
		LastHeroTile = player.position.tile;
	}
#ifdef _DEBUG
	// Test hook: DEVILUTIONX_DEV_WALKLINK = index of a link to walk onto, once per level.
	if (const char *walk = std::getenv("DEVILUTIONX_DEV_WALKLINK"); walk != nullptr && !player._pLvlChanging) {
		static int walkedLevelTick = 0;
		const size_t index = static_cast<size_t>(std::atoi(walk));
		static std::string walkedTown = "-";
		if (walkedTown != TownName) {
			walkedTown = TownName;
			walkedLevelTick = 0;
		}
		if (++walkedLevelTick % 40 == 0 && walkedLevelTick <= 400 && index < Links.size() && player._pmode == PM_STAND && player.position.tile != Links[index].tile) {
			const Point to = Links[index].tile;
			Point step = player.position.tile;
			step.x += std::clamp(to.x - step.x, -12, 12);
			step.y += std::clamp(to.y - step.y, -12, 12);
			LogInfo("WALKLINK from {},{} to {},{} via {},{}", player.position.tile.x, player.position.tile.y, to.x, to.y, step.x, step.y);
			NetSendCmdLoc(MyPlayerId, true, CMD_WALKXY, step);
		}
	}
#endif
	if (player._pLvlChanging || player._pmode != PM_STAND)
		return;
	const Point tile = player.position.tile;
	if (tile == LastHeroTile)
		return; // only when stepping onto it (not when arriving on it)
	LastHeroTile = tile;
	for (const LiveLink &link : Links) {
		if (link.tile == tile && link.to.kind != MapLinkKind::None) {
			FollowMapLink(link.to);
			return;
		}
	}
}

bool ForceTownLinkTrig()
{
	if (Links.empty() || IsMapEditing())
		return false;
	for (const LiveLink &link : Links) {
		if (link.to.kind == MapLinkKind::None || cursPosition.WalkingDistance(link.tile) > 1)
			continue;
		InfoString = LinkLabel(link);
		cursPosition = link.tile;
		return true;
	}
	return false;
}

Point CustomTownPortalTile(size_t playerId)
{
	const CustomMap *map = CurrentCustomTownMap();
	Point base { 56, 56 };
	if (map != nullptr) {
		base = map->entries.empty() ? Point { 16 + map->width, 16 + map->height } : map->entries.front().tile;
		for (const MapEntryPoint &e : map->entries) {
			if (e.name == "portal")
				base = e.tile;
		}
	}
	return NearestOpenTile(base + Displacement { static_cast<int>(playerId) * 2 - 3, -3 });
}

uint8_t CustomTownSaveSlot()
{
	return static_cast<uint8_t>(200 + std::hash<std::string> {}(TownName) % 50);
}

void ApplyCustomTownTowners(std::vector<TownerDataEntry> &entries)
{
	const CustomMap *map = CurrentCustomTownMap();
	std::vector<TownerDataEntry> placed;
	if (map != nullptr) {
		for (const MapNpc &npc : map->npcs) {
			const std::optional<_talker_id> type = magic_enum::enum_cast<_talker_id>(npc.type);
			if (!type)
				continue;
			const bool already = std::any_of(placed.begin(), placed.end(), [&](const TownerDataEntry &e) { return e.type == *type; });
			if (already)
				continue; // one of each: the game looks townspeople up by type
			for (const TownerDataEntry &e : entries) {
				if (e.type != *type)
					continue;
				TownerDataEntry copy = e;
				copy.position = npc.tile;
				copy.direction = static_cast<Direction>(npc.direction);
				placed.push_back(std::move(copy));
				break;
			}
		}
	}
	entries = std::move(placed);
}

const std::vector<PlaceableTowner> &PlaceableTowners()
{
	static const std::vector<PlaceableTowner> list {
		{ "TOWN_SMITH", "Griswold (smith)" },
		{ "TOWN_WITCH", "Adria (witch)" },
		{ "TOWN_HEALER", "Pepin (healer)" },
		{ "TOWN_TAVERN", "Ogden (tavern)" },
		{ "TOWN_STORY", "Cain (elder)" },
		{ "TOWN_BMAID", "Gillian (barmaid)" },
		{ "TOWN_PEGBOY", "Wirt (peg-leg boy)" },
		{ "TOWN_DRUNK", "Farnham (drunk)" },
		{ "TOWN_GAMBLER", "Tremain (gambler)" },
		{ "TOWN_CARAVAN", "Jamal (caravan)" },
	};
	return list;
}

Point ResolveTristramPosition(Point classic)
{
	Point p = classic;
	if (UnchangedAround(classic))
		return NearestOpenTile(classic); // this part of town was not rearranged
	if (const std::optional<TownBuilding> building = NearestTownBuilding(classic, 10); building)
		p += TownBuildingShift(*building);
	return NearestOpenTile(p);
}

} // namespace devilution
