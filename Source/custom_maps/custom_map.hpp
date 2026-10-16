/**
 * @file custom_maps/custom_map.hpp
 *
 * Hand-made maps (map editor): a tile layout in one of the game's styles, monster spawners and
 * bosses, named entry points and portals or doors that lead to other maps, town or a dungeon
 * level. Styles: the four dungeon tilesets, towns (safe, with townspeople) and wilds (town
 * tiles with monsters: the land outside a town). Maps live in mods/custommaps/maps/<name>/ as map.dun (the game's own map format) and
 * map.ini (everything the .dun format cannot hold).
 */
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/point.hpp"
#include "levels/gendung_defs.hpp"

namespace devilution {

enum class MapLinkKind : uint8_t {
	None,
	Map,     // another custom map, at a named entry point
	Town,    // Tristram
	Dungeon, // a dungeon level (arriving at its stairs up)
};

struct MapLink {
	MapLinkKind kind = MapLinkKind::None;
	std::string map;
	std::string entry;
	int level = 1;
};

struct MapEntryPoint {
	std::string name;
	Point tile; // world tile
};

/** @brief A townsperson placed in a town map (type as in towners.tsv, e.g. TOWN_SMITH). */
struct MapNpc {
	std::string type;
	Point tile;      // world tile
	int direction = 0; // Direction
};

/** @brief A pack of monsters placed each time the hero enters the map. */
struct MapSpawner {
	Point tile;          // world tile
	std::string monster; // monster id (e.g. MT_RFALLSP), or "auto": a type fitting the hero's level
	int count = 4;       // monsters in the pack
};

/** @brief A unique monster (with its minions) placed each time the hero enters the map. */
struct MapBoss {
	Point tile;       // world tile
	std::string name; // unique monster name, e.g. "Bonehead Keenaxe"
	int minions = 6;
};

/** @brief Sentinel monster for spawners that pick a type fitting the hero's level. */
inline constexpr std::string_view AutoMonster = "auto";

struct MapPortal {
	Point tile; // world tile
	bool door = false; // a doorway rather than a magic portal
	MapLink to;
};

struct CustomMap {
	std::string name;
	dungeon_type style = DTYPE_CATHEDRAL;
	int width = 40;  // in map pieces (2x2 world tiles each)
	int height = 40;
	std::vector<uint16_t> tiles;    // width * height piece ids, 0 = solid rock
	std::vector<uint16_t> monsters; // (2 * width) * (2 * height) .dun monster ids, 0 = none
	std::vector<uint16_t> objects;  // (2 * width) * (2 * height) .dun object ids, 0 = none
	std::vector<MapEntryPoint> entries;
	std::vector<MapPortal> portals;
	std::vector<MapNpc> npcs; // town maps only
	std::vector<MapSpawner> spawners; // not in towns
	std::vector<MapBoss> bosses;      // not in towns
	std::string weather; // mood name, empty for the level's usual roll

	// Editor layers (40 x 40, [y * 40 + x]): the painted floor plan the walls are built from, and
	// pieces placed by hand on top of it (0 = none). Saved next to the map, ignored by the game.
	std::vector<uint8_t> plan;
	std::vector<uint16_t> overrides;

	void Resize(int w, int h);
	[[nodiscard]] const MapEntryPoint *FindEntry(std::string_view entryName) const;
};

/** @brief Folder holding all custom maps (ends with a separator). */
std::string CustomMapsDirectory();
[[nodiscard]] bool IsValidMapName(std::string_view name);
[[nodiscard]] std::vector<std::string> ListCustomMaps();
[[nodiscard]] bool CustomMapExists(std::string_view name);
[[nodiscard]] std::string CustomMapDunPath(std::string_view name);
std::expected<CustomMap, std::string> ReadCustomMap(std::string_view name);
std::expected<void, std::string> WriteCustomMap(const CustomMap &map);

// --- Tristram's own layer ----------------------------------------------------------------------

/** @brief Name the editor uses for Tristram (`editor tristram`); not a map folder. */
inline constexpr std::string_view TristramLayerName = "tristram";
/** @brief Portals, doors and arrival points added to Tristram (mods/custommaps/tristram.ini). */
[[nodiscard]] CustomMap ReadTristramLayer();
std::expected<void, std::string> WriteTristramLayer(const CustomMap &layer);

[[nodiscard]] std::string_view MapStyleName(dungeon_type style);
[[nodiscard]] std::optional<dungeon_type> ParseMapStyle(std::string_view name);
/** @brief Styles built from town tiles (painted megatiles rather than a floor plan). */
[[nodiscard]] bool IsTownTiledStyle(dungeon_type style);
/** @brief Floor piece of a dungeon style (see-through walls flood from it). */
[[nodiscard]] uint8_t MapStyleFloorPiece(dungeon_type style);

// --- Playing custom maps ---------------------------------------------------------------------

/** @brief The custom map being played (or edited), if any. */
const CustomMap *ActiveCustomMap();
/** @brief Makes a map the active one; the hero arrives at the named entry point (or the first). */
void SetActiveCustomMap(CustomMap map, std::string_view entry);
void ClearActiveCustomMap();
/** @brief World tile where the hero arrives in the active map. */
[[nodiscard]] Point ActiveCustomMapArrival();

/** @brief In game: travel to a custom map. */
std::expected<void, std::string> TravelToCustomMap(std::string_view name, std::string_view entry);
/** @brief In game: follow a portal or door link. */
void FollowMapLink(const MapLink &link);

/** @brief Builds the level for the active map (called while loading SL_CUSTOM). */
std::expected<void, std::string> LoadActiveCustomMapLevel();

/** @brief Each game tick: the hero stepping onto a portal or door travels. */
void UpdateCustomMapPortals();

} // namespace devilution
