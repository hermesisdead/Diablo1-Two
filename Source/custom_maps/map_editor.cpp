#include "custom_maps/map_editor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "control/control.hpp"
#include "cursor.h"
#include "custom_maps/campaign.hpp"
#include "custom_maps/custom_map.hpp"
#include "custom_maps/custom_npcs.hpp"
#include "custom_maps/custom_town.hpp"
#include "custom_maps/map_monsters.hpp"
#include "diablo.h"
#include "engine/load_file.hpp"
#include "engine/palette.h"
#include "engine/rectangle.hpp"
#include "engine/render/primitive_render.hpp"
#include "engine/render/text_render.hpp"
#include "engine/render/world_projector.hpp"
#include "gamemenu.h"
#include "levels/desert.hpp"
#include "levels/drlg_l1.h"
#include "levels/drlg_l2.h"
#include "levels/drlg_l3.h"
#include "levels/drlg_l4.h"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "levels/town.h"
#include "lighting.h"
#include "menu.h"
#include "pfile.h"
#include "player.h"
#include "utils/endian_swap.hpp"
#include "utils/format.hpp"
#include "utils/log.hpp"
#include "utils/parse_int.hpp"
#include "utils/str_split.hpp"
#include "utils/language.h"
#include "utils/sdl_compat.h"
#include "utils/str_cat.hpp"
#include "utils/ui_fwd.h"
#include "utils/utf8.hpp"
#include "DiabloUI/diabloui.h"

namespace devilution {

namespace {

constexpr int Grid = 40; // DMAXX / DMAXY

enum class Tool : uint8_t {
	Room,
	Erase,
	Entry,
	Portal,
	Door,
	Paint,
	Stamp,
	Copy,
	Npc,
	Spawner,
	Boss,
	Chest,
	COUNT,
};

/** What is being edited: a dungeon map, a town map, wilds (town tiles with monsters), or Tristram's own layer of links. */
enum class Kind : uint8_t {
	Dungeon,
	Town,
	Wilds,
	Tristram,
};

struct Snapshot {
	std::vector<uint8_t> plan;
	std::vector<uint16_t> overrides;
	std::vector<uint16_t> tiles;
	std::vector<MapEntryPoint> entries;
	std::vector<MapPortal> portals;
	std::vector<MapNpc> npcs;
	std::vector<MapSpawner> spawners;
	std::vector<MapBoss> bosses;
	std::vector<uint16_t> objects;
};

/** Town ground brushes: town megatiles (1-based), one picked at random per tile for variety. */
struct GroundBrush {
	std::string_view name;
	std::vector<uint16_t> tiles;
};

const std::vector<GroundBrush> &GroundBrushes()
{
	static const std::vector<GroundBrush> brushes {
		{ "Grass", { 1, 2, 3, 4 } },
		{ "Worn grass", { 5, 6, 7, 8 } },
		{ "Dirt", { 70 } },
		{ "Shrubs", { 27, 28, 29, 30 } },
		{ "Stones", { 57, 58, 59, 60 } },
		{ "Boulders", { 61, 62, 63 } },
		{ "Small trees", { 34, 44, 47 } },
		{ "Large trees", { 41, 55, 96 } },
	};
	return brushes;
}

/** Pieces of classic Tristram that can be stamped into a town, in megatiles (inclusive). */
struct TownStamp {
	std::string_view name;
	int x0, y0, x1, y1;
};

const std::vector<TownStamp> &TownStamps()
{
	static const std::vector<TownStamp> stamps {
		{ "Cathedral", 7, 5, 16, 16 },
		{ "Tavern", 22, 26, 28, 32 },
		{ "Smithy", 29, 27, 36, 32 },
		{ "Healer's house", 23, 36, 28, 40 },
		{ "Witch's hut", 36, 7, 40, 10 },
		{ "Gillian's house", 18, 31, 22, 34 },
		{ "Farnham's house", 33, 38, 36, 42 },
		{ "Cottage", 34, 33, 37, 36 },
		{ "House (large)", 22, 19, 27, 23 },
		{ "House (small)", 12, 23, 15, 26 },
		{ "Fountain", 29, 34, 32, 36 },
		{ "Graveyard", 16, 9, 20, 13 },
		{ "Catacombs entrance", 23, 8, 26, 12 },
		{ "Caves entrance", 7, 33, 9, 36 },
		{ "Hell entrance", 17, 38, 23, 41 },
		{ "Stream", 0, 0, 4, 14 },
		{ "Thicket", 0, 32, 9, 41 },
	};
	return stamps;
}

struct Session {
	bool queued = false;
	bool running = false;
	bool editing = true;
	bool travelled = false;
	bool god = true;
	int levelKey = -1;
	Kind kind = Kind::Dungeon;

	CustomMap map;
	Point camera { 56, 56 };

	Tool tool = Tool::Room;
	bool dragging = false;
	bool dragErase = false;
	Point dragStart {}; // map piece
	int movingEntry = -1;
	int movingPortal = -1;
	bool grid = true;
	bool dirty = false;
	std::vector<Snapshot> undo;

	// Town tools.
	int brush = 0;            // ground brush, or -1 for the picked tile
	uint16_t pickedTile = 0;  // eyedropper
	int stamp = 0;            // town stamp, or -1 for the clipboard
	std::vector<uint16_t> clipboard;
	int clipWidth = 0;
	int clipHeight = 0;
	int npc = 0;

	// Tile picker panel.
	bool showTilePicker = false;  // showing visual tile picker
	uint16_t pickerTile = 1;      // current tile being previewed/selected
	int pickerZoom = 2;           // zoom level of tile picker (32px per megatile)
	Point pickerPan {};           // pan offset for tile picker
	bool pickerDragging = false;  // dragging with middle mouse
	Point pickerDragStart {};
	int hueShift = 0;             // hue shift for tiles (-30 to +30 degrees)

	// Monster tools.
	int monster = 0;    // index in SpawnableMonsters()
	int packSize = 4;
	int boss = 0;       // index in PlaceableBosses()
	int paletteScroll = 0;

	// Link dialog for a portal or door.
	int linkPortal = -1;
	std::vector<std::string> mapList;

	std::string message;
	uint32_t messageUntil = 0;
};

Session S;

// --- Helpers ---------------------------------------------------------------------------------

/** Get all tile variants from the current brush. */
std::vector<uint16_t> CurrentBrushVariants()
{
	if (S.brush < 0 || S.brush >= static_cast<int>(GroundBrushes().size()))
		return {};
	return GroundBrushes()[S.brush].tiles;
}

/** Format a tile variant label for display. */
std::string VariantLabel(uint16_t tileId)
{
	return StrCat("  Tile #", tileId);
}

void Say(std::string text)
{
	S.message = std::move(text);
	S.messageUntil = SDL_GetTicks() + 3500;
}

const std::vector<Tool> &ToolsFor(Kind kind)
{
	static const std::vector<Tool> dungeon { Tool::Room, Tool::Erase, Tool::Spawner, Tool::Boss, Tool::Chest, Tool::Entry, Tool::Portal, Tool::Door };
	static const std::vector<Tool> town { Tool::Paint, Tool::Stamp, Tool::Copy, Tool::Npc, Tool::Entry, Tool::Portal, Tool::Door };
	static const std::vector<Tool> wilds { Tool::Paint, Tool::Stamp, Tool::Copy, Tool::Spawner, Tool::Boss, Tool::Chest, Tool::Entry, Tool::Portal, Tool::Door };
	static const std::vector<Tool> tristram { Tool::Entry, Tool::Portal, Tool::Door };
	switch (kind) {
	case Kind::Town: return town;
	case Kind::Wilds: return wilds;
	case Kind::Tristram: return tristram;
	default: return dungeon;
	}
}

bool InMap(Point piece)
{
	return piece.x >= 0 && piece.y >= 0 && piece.x < S.map.width && piece.y < S.map.height;
}

Point PieceAt(Point worldTile)
{
	return { static_cast<int>(std::floor((worldTile.x - 16) / 2.0f)), static_cast<int>(std::floor((worldTile.y - 16) / 2.0f)) };
}

/** Where markers (arrival points, portals, doors, people) may go. */
bool CanPlaceAt(Point tile)
{
	if (S.kind == Kind::Tristram)
		return InDungeonBounds(tile) && tile.x >= dminPosition.x && tile.y >= dminPosition.y && tile.x < dmaxPosition.x && tile.y < dmaxPosition.y;
	return InMap(PieceAt(tile));
}

/** The level the editor works on is the one loaded. */
bool OnEditorLevel()
{
	switch (S.kind) {
	case Kind::Dungeon:
	case Kind::Wilds:
		return setlevel && setlvlnum == SL_CUSTOM;
	case Kind::Town:
		return S.travelled && !setlevel && currlevel == 0 && leveltype == DTYPE_TOWN && InCustomTown() && CurrentCustomTown() == S.map.name;
	case Kind::Tristram:
		return S.travelled && !setlevel && currlevel == 0 && leveltype == DTYPE_TOWN && !InCustomTown() && !InDesertRegion();
	}
	return false;
}

void PushUndo()
{
	S.undo.push_back({ S.map.plan, S.map.overrides, S.map.tiles, S.map.entries, S.map.portals, S.map.npcs, S.map.spawners, S.map.bosses, S.map.objects });
	if (S.undo.size() > 60)
		S.undo.erase(S.undo.begin());
	S.dirty = true;
}

/** Dungeon styles: tiles (walls and all) from the painted floor plan. */
void BuildFromFloorPlan(dungeon_type style, const uint8_t *plan, uint16_t *tiles)
{
	switch (style) {
	case DTYPE_CATACOMBS: BuildCatacombsFromFloorPlan(plan, tiles); break;
	case DTYPE_CAVES: BuildCavesFromFloorPlan(plan, tiles); break;
	case DTYPE_HELL: BuildHellFromFloorPlan(plan, tiles); break;
	default: BuildCathedralFromFloorPlan(plan, tiles); break;
	}
}

/** Rebuilds the level from the map after an edit. */
void Rebuild()
{
	if (S.kind == Kind::Tristram)
		return; // Tristram's tiles are not edited
	if (S.kind == Kind::Town) {
		SetCustomTownMapForEditor(S.map);
		if (OnEditorLevel())
			RefreshCustomTownLevel();
		return;
	}
	if (S.kind == Kind::Wilds) {
		if (OnEditorLevel()) {
			PaintTownStyleTiles(&S.map);
			InitTownStylePieces();
		}
		return;
	}
	std::array<uint16_t, Grid * Grid> autoTiles {};
	BuildFromFloorPlan(S.map.style, S.map.plan.data(), autoTiles.data());
	for (int y = 0; y < Grid; y++) {
		for (int x = 0; x < Grid; x++) {
			const size_t i = static_cast<size_t>(y) * Grid + x;
			uint16_t tile = S.map.overrides[i] != 0 ? S.map.overrides[i] : autoTiles[i];
			if (x >= S.map.width || y >= S.map.height)
				tile = autoTiles[0]; // rock outside the map
			dungeon[x][y] = static_cast<uint8_t>(tile);
			if (x < S.map.width && y < S.map.height)
				S.map.tiles[static_cast<size_t>(y) * S.map.width + x] = tile;
		}
	}
	if (!OnEditorLevel() || leveltype != S.map.style)
		return; // the tiles are kept in the map; the level shows them once it is loaded
	std::memset(dSpecial, 0, sizeof(dSpecial));
	switch (S.map.style) {
	case DTYPE_CATACOMBS: RefreshCatacombsPieces(); break;
	case DTYPE_CAVES: RefreshCavesPieces(); break;
	case DTYPE_HELL: RefreshHellPieces(); break;
	default: RefreshCathedralPieces(); break;
	}
	DRLG_InitTrans();
	FloodTransparencyValues(MapStyleFloorPiece(S.map.style));
}

void SetPlan(Point a, Point b, bool floor)
{
	const int x0 = std::max(0, std::min(a.x, b.x)), x1 = std::min(S.map.width - 1, std::max(a.x, b.x));
	const int y0 = std::max(0, std::min(a.y, b.y)), y1 = std::min(S.map.height - 1, std::max(a.y, b.y));
	if (x0 > x1 || y0 > y1)
		return;
	PushUndo();
	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++) {
			const size_t i = static_cast<size_t>(y) * Grid + x;
			S.map.plan[i] = floor ? 1 : 0;
			S.map.overrides[i] = 0;
		}
	}
	// Keep a ring of rock: walls need a piece of room around the floor.
	for (int y = 0; y < Grid; y++) {
		for (int x = 0; x < Grid; x++) {
			if (x == 0 || y == 0 || x >= S.map.width - 1 || y >= S.map.height - 1)
				S.map.plan[static_cast<size_t>(y) * Grid + x] = 0;
		}
	}
	Rebuild();
}

uint16_t &TownTile(Point piece)
{
	return S.map.tiles[static_cast<size_t>(piece.y) * S.map.width + piece.x];
}

/** Town: get the tile to paint (handles variant selection and hue shift). */
uint16_t GetPaintTile()
{
	if (S.showTilePicker)
		return S.pickerTile;

	const auto &brushes = GroundBrushes();
	if (S.brush < 0 || S.brush >= static_cast<int>(brushes.size()))
		return 0;

	const auto &choices = brushes[S.brush].tiles;
	if (choices.empty())
		return 0;

	// Use random tile from the brush.
	return choices[0];
}

/** Town: fills a rectangle of megatiles with the current ground brush. */
void PaintGround(Point a, Point b)
{
	const int x0 = std::max(0, std::min(a.x, b.x)), x1 = std::min(S.map.width - 1, std::max(a.x, b.x));
	const int y0 = std::max(0, std::min(a.y, b.y)), y1 = std::min(S.map.height - 1, std::max(a.y, b.y));
	if (x0 > x1 || y0 > y1)
		return;

	std::vector<uint16_t> choices;
	if (S.showTilePicker) {
		choices = { S.pickerTile };
	} else {
		const auto &brushes = GroundBrushes();
		if (S.brush < 0 || S.brush >= static_cast<int>(brushes.size()))
			return;
		choices = brushes[S.brush].tiles;
	}

	if (choices.empty() || choices.front() == 0)
		return;

	PushUndo();
	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++) {
			uint16_t tile;
			if (S.showTilePicker) {
				// Use the picker tile directly
				tile = S.pickerTile;
			} else {
				// Random selection from brush
				const uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u;
				tile = choices[(h >> 4) % choices.size()];
			}
			TownTile({ x, y }) = tile;
		}
	}
	Rebuild();
}

/** Town: size of the current stamp in megatiles. */
Size StampSize()
{
	if (S.stamp < 0)
		return { S.clipWidth, S.clipHeight };
	const TownStamp &s = TownStamps()[S.stamp];
	return { s.x1 - s.x0 + 1, s.y1 - s.y0 + 1 };
}

/** Town: top-left megatile of the stamp when centred on a megatile. */
Point StampOrigin(Point centre)
{
	const Size size = StampSize();
	return centre - Displacement { size.width / 2, size.height / 2 };
}

void PlaceStamp(Point centre)
{
	const Size size = StampSize();
	if (size.width <= 0 || size.height <= 0)
		return;
	const Point origin = StampOrigin(centre);
	PushUndo();
	const std::vector<uint16_t> &classic = ClassicTownMegaTiles();
	for (int y = 0; y < size.height; y++) {
		for (int x = 0; x < size.width; x++) {
			const Point at = origin + Displacement { x, y };
			if (!InMap(at))
				continue;
			uint16_t tile;
			if (S.stamp < 0) {
				tile = S.clipboard[static_cast<size_t>(y) * S.clipWidth + x];
			} else {
				const TownStamp &s = TownStamps()[S.stamp];
				tile = classic[static_cast<size_t>(s.y0 + y) * 48 + s.x0 + x];
			}
			if (tile != 0)
				TownTile(at) = tile;
		}
	}
	Rebuild();
	Say(StrCat("Placed ", S.stamp < 0 ? std::string("the copied area") : std::string(TownStamps()[S.stamp].name), "."));
}

void CopyArea(Point a, Point b)
{
	const int x0 = std::max(0, std::min(a.x, b.x)), x1 = std::min(S.map.width - 1, std::max(a.x, b.x));
	const int y0 = std::max(0, std::min(a.y, b.y)), y1 = std::min(S.map.height - 1, std::max(a.y, b.y));
	if (x0 > x1 || y0 > y1)
		return;
	S.clipWidth = x1 - x0 + 1;
	S.clipHeight = y1 - y0 + 1;
	S.clipboard.clear();
	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++)
			S.clipboard.push_back(TownTile({ x, y }));
	}
	S.stamp = -1;
	S.tool = Tool::Stamp;
	Say(StrCat("Copied ", S.clipWidth, "x", S.clipHeight, ". Click to paste it (Stamp tool, \"Copied area\")."));
}

int EntryAt(Point tile)
{
	for (size_t i = 0; i < S.map.entries.size(); i++) {
		if (S.map.entries[i].tile.WalkingDistance(tile) <= 1)
			return static_cast<int>(i);
	}
	return -1;
}

int PortalAt(Point tile)
{
	for (size_t i = 0; i < S.map.portals.size(); i++) {
		if (S.map.portals[i].tile.WalkingDistance(tile) <= 1)
			return static_cast<int>(i);
	}
	return -1;
}

int NpcAt(Point tile)
{
	for (size_t i = 0; i < S.map.npcs.size(); i++) {
		if (S.map.npcs[i].tile.WalkingDistance(tile) <= 1)
			return static_cast<int>(i);
	}
	return -1;
}

int SpawnerAt(Point tile)
{
	for (size_t i = 0; i < S.map.spawners.size(); i++) {
		if (S.map.spawners[i].tile.WalkingDistance(tile) <= 1)
			return static_cast<int>(i);
	}
	return -1;
}

int BossAt(Point tile)
{
	for (size_t i = 0; i < S.map.bosses.size(); i++) {
		if (S.map.bosses[i].tile.WalkingDistance(tile) <= 1)
			return static_cast<int>(i);
	}
	return -1;
}

/** Townspeople the People tool places: the game's own, then the campaign's (NPC editor). */
struct Person {
	std::string type;
	std::string label;
};

std::vector<Person> People()
{
	std::vector<Person> people;
	for (const PlaceableTowner &t : PlaceableTowners())
		people.push_back({ std::string(t.type), std::string(t.label) });
	for (const CustomNpc &npc : ReadCustomNpcs())
		people.push_back({ CustomNpcType(npc.id), StrCat(npc.name, " (yours)") });
	return people;
}

std::string NpcLabel(std::string_view type)
{
	for (const Person &p : People()) {
		if (p.type == type)
			return p.label;
	}
	return std::string(type);
}

std::string NewEntryName()
{
	if (S.map.entries.empty())
		return "start";
	for (int n = 2;; n++) {
		std::string name = StrCat("entry", n);
		if (std::none_of(S.map.entries.begin(), S.map.entries.end(), [&](const MapEntryPoint &e) { return e.name == name; }))
			return name;
	}
}

std::string LinkLabel(const MapLink &link)
{
	switch (link.kind) {
	case MapLinkKind::Map: return StrCat("-> ", link.map, link.entry.empty() ? "" : StrCat(" (", link.entry, ")"));
	case MapLinkKind::Town: return StrCat("-> Tristram", link.entry.empty() ? "" : StrCat(" (", link.entry, ")"));
	case MapLinkKind::Dungeon: return StrCat("-> Dungeon level ", link.level);
	default: return "-> (nowhere yet)";
	}
}

bool SaveMap()
{
	Rebuild();
	std::expected<void, std::string> r = S.kind == Kind::Tristram ? WriteTristramLayer(S.map) : WriteCustomMap(S.map);
	if (!r.has_value()) {
		Say(StrCat("Save failed: ", r.error()));
		return false;
	}
	S.dirty = false;
	if (S.kind == Kind::Tristram)
		Say("Saved Tristram's portals, doors and arrival points (mods/custommaps/tristram.ini).");
	else
		Say(StrCat("Saved ", S.map.name, " to ", CustomMapsDirectory(), S.map.name));
	return true;
}

/** (Re)builds the level from the map, for editing or play-testing. */
void LoadMapLevel(bool editing)
{
	S.editing = editing;
	const std::string entry = S.map.entries.empty() ? "" : S.map.entries.front().name;
	switch (S.kind) {
	case Kind::Dungeon:
	case Kind::Wilds:
		Rebuild();
		SetActiveCustomMap(S.map, entry);
		setlvltype = S.map.style;
		StartNewLvl(*MyPlayer, WM_DIABSETLVL, SL_CUSTOM);
		break;
	case Kind::Town:
		SetCustomTownMapForEditor(S.map);
		EnterCustomTownForEditor(entry);
		break;
	case Kind::Tristram:
		SetTristramLayerForEditor(&S.map);
		TravelToTristram(entry);
		break;
	}
}

// --- Interface layout ------------------------------------------------------------------------

constexpr int TopBarHeight = 22;
constexpr int ToolWidth = 92;
constexpr int ToolHeight = 20;
constexpr int PaletteWidth = 156;
constexpr int PaletteRow = 16;

Rectangle TopButton(int index)
{
	const int w = 64;
	return { { gnScreenWidth - (3 - index) * (w + 4) - 4, 2 }, { w, 18 } };
}

Rectangle ToolButton(int index)
{
	return { { 4, TopBarHeight + 6 + index * (ToolHeight + 4) }, { ToolWidth, ToolHeight } };
}

int ToolButtonCount()
{
	return static_cast<int>(ToolsFor(S.kind).size()) + (S.kind == Kind::Tristram ? 2 : 3);
}

std::string_view ToolName(Tool tool)
{
	switch (tool) {
	case Tool::Room: return "Room";
	case Tool::Erase: return "Erase";
	case Tool::Entry: return "Arrival";
	case Tool::Portal: return "Portal";
	case Tool::Door: return "Door";
	case Tool::Paint: return "Paint";
	case Tool::Stamp: return "Stamp";
	case Tool::Copy: return "Copy";
	case Tool::Npc: return "People";
	case Tool::Spawner: return "Monsters";
	case Tool::Boss: return "Boss";
	case Tool::Chest: return "Chest";
	default: return "";
	}
}

std::string_view ToolHint(Tool tool)
{
	switch (tool) {
	case Tool::Room: return "Drag to paint floor; walls follow. Right-drag removes.";
	case Tool::Erase: return "Drag to turn floor back into rock.";
	case Tool::Entry: return "Click to add an arrival point, drag to move, right-click to delete.";
	case Tool::Portal: return "Click to place a portal and choose where it leads. Right-click deletes.";
	case Tool::Door: return "Click to place a doorway exit and choose where it leads. Right-click deletes.";
	case Tool::Paint: return "Drag to paint ground. Right-click picks up the tile under the cursor.";
	case Tool::Stamp: return "Click to place the chosen piece of Tristram (centred on the cursor).";
	case Tool::Copy: return "Drag over an area to copy it, then click to paste it anywhere.";
	case Tool::Npc: return "Click to place the chosen townsperson (or move them). Right-click removes.";
	case Tool::Spawner: return "Click to place a pack (click one to change it). Right-click removes. Wheel: scroll list.";
	case Tool::Boss: return "Click to place the chosen boss with its minions. Right-click removes. Wheel: scroll list.";
	case Tool::Chest: return "Click to place a chest (quests can hide an item in it). Right-click removes.";
	default: return "";
	}
}

/** Rows of the palette panel for the current tool. */
std::vector<std::string> PaletteRows()
{
	std::vector<std::string> rows;
	switch (S.tool) {
	case Tool::Paint:
		// Show ground brushes.
		for (const GroundBrush &b : GroundBrushes())
			rows.emplace_back(b.name);
		rows.push_back("---");
		if (S.pickedTile != 0)
			rows.push_back(StrCat("Tile picker [", S.pickerTile, "]"));
		else
			rows.push_back("Open tile picker");
		break;
	case Tool::Stamp:
		for (const TownStamp &s : TownStamps())
			rows.emplace_back(s.name);
		if (!S.clipboard.empty())
			rows.push_back(StrCat("Copied area ", S.clipWidth, "x", S.clipHeight));
		break;
	case Tool::Npc:
		for (const Person &p : People())
			rows.push_back(p.label);
		break;
	case Tool::Spawner:
		for (const SpawnableMonster &m : SpawnableMonsters())
			rows.push_back(m.label);
		break;
	case Tool::Boss:
		for (const std::string &name : PlaceableBosses())
			rows.push_back(name);
		break;
	default:
		break;
	}
	return rows;
}

std::string PaletteTitle()
{
	switch (S.tool) {
	case Tool::Paint:
		if (S.showTilePicker) {
			const std::string hue = S.hueShift != 0 ? StrCat("  Hue: ", S.hueShift >= 0 ? "+" : "", S.hueShift, "°") : "";
			return StrCat("Tile Picker [#", S.pickerTile, "]", hue, "  F/G: hue");
		}
		if (S.brush >= 0 && S.brush < static_cast<int>(GroundBrushes().size()))
			return std::string(GroundBrushes()[S.brush].name);
		return "Ground";
	case Tool::Stamp: return "Pieces of Tristram";
	case Tool::Npc: return "Townspeople";
	case Tool::Spawner: return StrCat("Pack of ", S.packSize, "  (click +/right -)");
	case Tool::Boss: return "Bosses (with minions)";
	default: return "";
	}
}

int PaletteSelected()
{
	switch (S.tool) {
	case Tool::Paint:
		return S.brush < 0 ? static_cast<int>(GroundBrushes().size()) : S.brush;
	case Tool::Stamp: return S.stamp < 0 ? static_cast<int>(TownStamps().size()) : S.stamp;
	case Tool::Npc: return S.npc;
	case Tool::Spawner: return S.monster;
	case Tool::Boss: return S.boss;
	default: return -1;
	}
}

void SelectPalette(int row)
{
	switch (S.tool) {
	case Tool::Paint: {
		const auto &brushes = GroundBrushes();
		if (row < static_cast<int>(brushes.size())) {
			S.brush = row;
		} else if (row == static_cast<int>(brushes.size()) + 1) {
			// Separator
		} else {
			// Open tile picker
			S.showTilePicker = !S.showTilePicker;
		}
		break;
	}
	case Tool::Stamp: S.stamp = row >= static_cast<int>(TownStamps().size()) ? -1 : row; break;
	case Tool::Npc: S.npc = row; break;
	case Tool::Spawner: S.monster = row; break;
	case Tool::Boss: S.boss = row; break;
	default: break;
	}
}

/** Rows that fit on screen; longer lists scroll. */
int PaletteVisibleRows()
{
	return std::max(4, (gnViewportHeight - 18 - TopBarHeight - 24 - 12) / PaletteRow);
}

void ScrollPalette(int delta)
{
	const int rows = static_cast<int>(PaletteRows().size());
	S.paletteScroll = std::clamp(S.paletteScroll + delta, 0, std::max(0, rows - PaletteVisibleRows()));
}

/** Screen rectangle of a palette row (by list index), if scrolled into view. */
Rectangle PaletteRowRect(int row)
{
	return { { gnScreenWidth - PaletteWidth - 4, TopBarHeight + 24 + (row - S.paletteScroll) * PaletteRow }, { PaletteWidth, PaletteRow - 1 } };
}

bool PaletteRowShown(int row)
{
	return row >= S.paletteScroll && row < S.paletteScroll + PaletteVisibleRows();
}

Rectangle PaletteRect()
{
	const int rows = std::min(static_cast<int>(PaletteRows().size()), PaletteVisibleRows());
	if (rows == 0)
		return {};
	return { { gnScreenWidth - PaletteWidth - 8, TopBarHeight + 4 }, { PaletteWidth + 8, 24 + rows * PaletteRow + 4 } };
}

Rectangle PaletteTitleRect()
{
	const Rectangle panel = PaletteRect();
	return { panel.position, { panel.size.width, 20 } };
}

constexpr int DialogWidth = 320;
constexpr int DialogRow = 18;

int DialogRowCount()
{
	return 5 + static_cast<int>(S.mapList.size());
}

Rectangle DialogRect()
{
	const int h = 30 + DialogRowCount() * DialogRow + 8;
	return { { (gnScreenWidth - DialogWidth) / 2, std::max(TopBarHeight + 10, (gnViewportHeight - h) / 2) }, { DialogWidth, h } };
}

Rectangle DialogRowRect(int row)
{
	const Rectangle d = DialogRect();
	return { d.position + Displacement { 10, 28 + row * DialogRow }, { DialogWidth - 20, DialogRow - 2 } };
}

void OpenLinkDialog(int portal)
{
	S.linkPortal = portal;
	S.mapList.clear();
	for (std::string &name : ListCustomMaps()) {
		if (S.kind == Kind::Tristram || name != S.map.name || S.kind == Kind::Dungeon)
			S.mapList.push_back(std::move(name));
	}
}

/** Names of Tristram's added arrival points. */
std::vector<std::string> TristramEntries()
{
	std::vector<std::string> names;
	const CustomMap layer = S.kind == Kind::Tristram ? S.map : ReadTristramLayer();
	for (const MapEntryPoint &e : layer.entries)
		names.push_back(e.name);
	return names;
}

// --- Drawing ---------------------------------------------------------------------------------

void Line(const Surface &out, Point a, Point b, uint8_t color)
{
	const int dx = std::abs(b.x - a.x), dy = -std::abs(b.y - a.y);
	const int sx = a.x < b.x ? 1 : -1, sy = a.y < b.y ? 1 : -1;
	int err = dx + dy;
	for (int guard = 0; guard < 4000; guard++) {
		out.SetPixel(a, color);
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

std::optional<Point> Screen(const WorldProjector &pr, float x, float y)
{
	const std::optional<OrbitScreenPoint> p = pr.Project(x, y, 0.0f);
	if (!p)
		return std::nullopt;
	return p->pos;
}

/** Outline of a rectangle of map pieces on the floor. */
void PieceOutline(const Surface &view, const WorldProjector &pr, Point a, Point b, uint8_t color)
{
	const float x0 = 16.0f + 2 * a.x, y0 = 16.0f + 2 * a.y, x1 = 16.0f + 2 * (b.x + 1), y1 = 16.0f + 2 * (b.y + 1);
	const std::optional<Point> p0 = Screen(pr, x0, y0), p1 = Screen(pr, x1, y0), p2 = Screen(pr, x1, y1), p3 = Screen(pr, x0, y1);
	if (!p0 || !p1 || !p2 || !p3)
		return;
	Line(view, *p0, *p1, color);
	Line(view, *p1, *p2, color);
	Line(view, *p2, *p3, color);
	Line(view, *p3, *p0, color);
}

void Marker(const Surface &view, const WorldProjector &pr, Point tile, uint8_t color, std::string_view label, UiFlags labelColor)
{
	const std::optional<Point> c = Screen(pr, tile.x + 0.5f, tile.y + 0.5f);
	if (!c)
		return;
	const int r = 9 * pr.zoom;
	for (int a = 0; a < 48; a++) {
		const float t0 = a * 6.2832f / 48, t1 = (a + 1) * 6.2832f / 48;
		Line(view, { c->x + static_cast<int>(std::cos(t0) * r), c->y + static_cast<int>(std::sin(t0) * r / 2) }, { c->x + static_cast<int>(std::cos(t1) * r), c->y + static_cast<int>(std::sin(t1) * r / 2) }, color);
	}
	Line(view, { c->x, c->y }, { c->x, c->y - 24 * pr.zoom }, color);
	const int w = static_cast<int>(GetLineWidth(label)) + 8;
	const Point lp { c->x - w / 2, c->y - 24 * pr.zoom - 14 };
	if (lp.x < 0 || lp.y < 0 || lp.x + w >= view.w() || lp.y + 14 >= view.h())
		return;
	DrawHalfTransparentRectTo(view, lp.x, lp.y, w, 14);
	DrawString(view, label, { lp + Displacement { 4, 1 }, { w - 8, 12 } }, { .flags = labelColor });
}

void Button(const Surface &out, Rectangle r, std::string_view label, bool on)
{
	const bool hover = r.contains(MousePosition);
	FillRect(out, r.position.x, r.position.y, r.size.width, r.size.height, on ? PAL16_YELLOW + 12 : (hover ? PAL16_GRAY + 12 : PAL16_GRAY + 14));
	DrawHorizontalLine(out, r.position, r.size.width, PAL16_YELLOW + 9);
	DrawHorizontalLine(out, r.position + Displacement { 0, r.size.height - 1 }, r.size.width, PAL16_YELLOW + 9);
	DrawVerticalLine(out, r.position, r.size.height, PAL16_YELLOW + 9);
	DrawVerticalLine(out, r.position + Displacement { r.size.width - 1, 0 }, r.size.height, PAL16_YELLOW + 9);
	DrawString(out, label, { r.position + Displacement { 0, 3 }, { r.size.width, 12 } }, { .flags = (on ? UiFlags::ColorWhitegold : UiFlags::ColorWhite) | UiFlags::AlignCenter | UiFlags::KerningFitSpacing });
}

void DrawWorldOverlay(const Surface &out)
{
	const Surface view = out.subregionY(0, gnViewportHeight);
	const WorldProjector pr(view);
	if (S.grid && S.kind != Kind::Tristram) {
		// Faint lines between map pieces, and the map's border.
		for (int x = 0; x <= S.map.width; x++) {
			const std::optional<Point> a = Screen(pr, 16.0f + 2 * x, 16.0f), b = Screen(pr, 16.0f + 2 * x, 16.0f + 2 * S.map.height);
			if (a && b)
				Line(view, *a, *b, x == 0 || x == S.map.width ? PAL16_YELLOW + 6 : PAL16_GRAY + 9);
		}
		for (int y = 0; y <= S.map.height; y++) {
			const std::optional<Point> a = Screen(pr, 16.0f, 16.0f + 2 * y), b = Screen(pr, 16.0f + 2 * S.map.width, 16.0f + 2 * y);
			if (a && b)
				Line(view, *a, *b, y == 0 || y == S.map.height ? PAL16_YELLOW + 6 : PAL16_GRAY + 9);
		}
	}
	// What the brush would cover.
	const Point hover = PieceAt(cursPosition);
	if (IsAnyOf(S.tool, Tool::Room, Tool::Erase, Tool::Paint, Tool::Copy) && InMap(hover)) {
		const Point from = S.dragging ? S.dragStart : hover;
		uint8_t color = PAL16_YELLOW + 2;
		if (S.tool == Tool::Erase || (S.tool == Tool::Room && S.dragging && S.dragErase))
			color = PAL8_RED;
		else if (S.tool == Tool::Copy)
			color = PAL16_BLUE + 2;
		PieceOutline(view, pr, { std::min(from.x, hover.x), std::min(from.y, hover.y) }, { std::max(from.x, hover.x), std::max(from.y, hover.y) }, color);
	}
	if (S.tool == Tool::Stamp && InMap(hover)) {
		const Size size = StampSize();
		if (size.width > 0) {
			const Point o = StampOrigin(hover);
			PieceOutline(view, pr, o, o + Displacement { size.width - 1, size.height - 1 }, PAL16_YELLOW + 2);
		}
	}
	for (const MapEntryPoint &e : S.map.entries)
		Marker(view, pr, e.tile, PAL16_BLUE + 2, StrCat("Arrive: ", e.name), UiFlags::ColorBlue);
	for (const MapPortal &p : S.map.portals)
		Marker(view, pr, p.tile, p.door ? PAL16_YELLOW + 4 : PAL8_RED, StrCat(p.door ? "Door " : "Portal ", LinkLabel(p.to)), p.door ? UiFlags::ColorGold : UiFlags::ColorRed);
	for (const MapNpc &n : S.map.npcs)
		Marker(view, pr, n.tile, PAL16_GRAY + 2, NpcLabel(n.type), UiFlags::ColorWhite);
	for (const MapSpawner &s : S.map.spawners)
		Marker(view, pr, s.tile, PAL16_ORANGE + 2, StrCat(SpawnerLabel(s.monster), " x", s.count), UiFlags::ColorGold);
	for (const MapBoss &b : S.map.bosses)
		Marker(view, pr, b.tile, PAL16_RED + 4, StrCat("Boss: ", b.name), UiFlags::ColorRed);
	for (const Point chest : MapChests(S.map))
		Marker(view, pr, chest, PAL16_YELLOW + 2, StrCat("Chest ", chest.x, ",", chest.y), UiFlags::ColorGold);
}

void DrawLinkDialog(const Surface &out)
{
	const Rectangle d = DialogRect();
	DrawHalfTransparentRectTo(out, d.position.x, d.position.y, d.size.width, d.size.height);
	DrawHalfTransparentRectTo(out, d.position.x, d.position.y, d.size.width, d.size.height);
	const MapPortal &portal = S.map.portals[S.linkPortal];
	DrawString(out, StrCat("Where does this ", portal.door ? "door" : "portal", " lead?"), { d.position + Displacement { 0, 8 }, { d.size.width, 12 } }, { .flags = UiFlags::ColorWhitegold | UiFlags::AlignCenter });
	auto row = [&](int index, std::string_view text, bool selected) {
		const Rectangle r = DialogRowRect(index);
		if (r.contains(MousePosition) || selected)
			FillRect(out, r.position.x, r.position.y, r.size.width, r.size.height, selected ? PAL16_YELLOW + 12 : PAL16_GRAY + 13);
		DrawString(out, text, { r.position + Displacement { 6, 2 }, { r.size.width - 12, 12 } }, { .flags = selected ? UiFlags::ColorWhitegold : UiFlags::ColorWhite });
	};
	const MapLink &l = portal.to;
	row(0, "Nowhere (decoration only)", l.kind == MapLinkKind::None);
	const bool town = l.kind == MapLinkKind::Town;
	row(1, StrCat("Tristram", town && !l.entry.empty() ? StrCat("   arriving at ", l.entry, " (click: next)") : "   (click again: its arrival points)"), town);
	row(2, StrCat("Dungeon level ", l.level, "   (click +1, right-click -1)"), l.kind == MapLinkKind::Dungeon);
	row(3, "New wilds area (made now, with a way back)", false);
	for (size_t i = 0; i < S.mapList.size(); i++) {
		const bool selected = l.kind == MapLinkKind::Map && l.map == S.mapList[i];
		row(4 + static_cast<int>(i), StrCat("Map: ", S.mapList[i], selected && !l.entry.empty() ? StrCat("   arriving at ", l.entry, " (click: next)") : ""), selected);
	}
	row(DialogRowCount() - 1, "Done", false);
}

void DrawPalette(const Surface &out)
{
	const std::vector<std::string> rows = PaletteRows();
	if (rows.empty())
		return;
	const Rectangle panel = PaletteRect();
	DrawHalfTransparentRectTo(out, panel.position.x, panel.position.y, panel.size.width, panel.size.height);
	DrawHalfTransparentRectTo(out, panel.position.x, panel.position.y, panel.size.width, panel.size.height);
	DrawString(out, PaletteTitle(), { panel.position + Displacement { 0, 5 }, { panel.size.width, 12 } }, { .flags = UiFlags::ColorWhitegold | UiFlags::AlignCenter | UiFlags::KerningFitSpacing });
	const int selected = PaletteSelected();
	for (int i = 0; i < static_cast<int>(rows.size()); i++) {
		if (!PaletteRowShown(i))
			continue;
		const Rectangle r = PaletteRowRect(i);
		const bool on = i == selected;
		if (on || r.contains(MousePosition))
			FillRect(out, r.position.x, r.position.y, r.size.width, r.size.height, on ? PAL16_YELLOW + 12 : PAL16_GRAY + 13);
		DrawString(out, rows[i], { r.position + Displacement { 6, 1 }, { r.size.width - 12, 12 } }, { .flags = (on ? UiFlags::ColorWhitegold : UiFlags::ColorWhite) | UiFlags::KerningFitSpacing });
	}
	if (static_cast<int>(rows.size()) > PaletteVisibleRows()) {
		// Scroll bar.
		const int track = PaletteVisibleRows() * PaletteRow;
		const int top = TopBarHeight + 24;
		const int thumb = std::max(10, track * PaletteVisibleRows() / static_cast<int>(rows.size()));
		const int at = top + (track - thumb) * S.paletteScroll / std::max(1, static_cast<int>(rows.size()) - PaletteVisibleRows());
		FillRect(out, panel.position.x + 1, top, 3, track, PAL16_GRAY + 13);
		FillRect(out, panel.position.x + 1, at, 3, thumb, PAL16_YELLOW + 6);
	}
}

void DrawTilePicker(const Surface &out)
{
	const Rectangle panel = PaletteRect();
	DrawHalfTransparentRectTo(out, panel.position.x, panel.position.y, panel.size.width, panel.size.height);
	DrawHalfTransparentRectTo(out, panel.position.x, panel.position.y, panel.size.width, panel.size.height);
	DrawString(out, PaletteTitle(), { panel.position + Displacement { 0, 5 }, { panel.size.width, 12 } }, { .flags = UiFlags::ColorWhitegold | UiFlags::AlignCenter | UiFlags::KerningFitSpacing });

	// Draw a grid of tiles
	const int tileSize = S.pickerZoom * 16;  // zoom level 1 = 16px, 2 = 32px, etc.
	const int tilesPerRow = std::max(1, panel.size.width / (tileSize + 4));
	const int startX = panel.position.x + 4;
	const int startY = panel.position.y + 24;

	for (uint16_t tile = 1; tile <= 127; tile++) {
		const int row = (tile - 1) / tilesPerRow;
		const int col = (tile - 1) % tilesPerRow;
		const int x = startX + col * (tileSize + 4) + S.pickerPan.x;
		const int y = startY + row * (tileSize + 4) + S.pickerPan.y;

		// Skip tiles outside the visible area
		if (x + tileSize < panel.position.x || x > panel.position.x + panel.size.width ||
		    y + tileSize < startY || y > startY + (PaletteVisibleRows() * PaletteRow))
			continue;

		// Draw tile preview box with a color based on tile number
		// Hue shift changes which color palette is used (cycle through different ranges)
		int colorBase = PAL16_GRAY;
		if (S.hueShift > 0) {
			// Shift towards yellow/gold
			colorBase = PAL16_YELLOW;
		} else if (S.hueShift < 0) {
			// Shift towards blue
			colorBase = PAL16_BLUE;
		}
		const uint8_t tileColor = colorBase + (tile % 8);
		FillRect(out, x, y, tileSize, tileSize, tileColor);

		// Draw selection highlight with a gold border
		if (tile == S.pickerTile) {
			FillRect(out, x - 2, y - 2, tileSize + 4, 2, PAL16_YELLOW + 12);  // Top
			FillRect(out, x - 2, y + tileSize, tileSize + 4, 2, PAL16_YELLOW + 12);  // Bottom
			FillRect(out, x - 2, y, 2, tileSize, PAL16_YELLOW + 12);  // Left
			FillRect(out, x + tileSize, y, 2, tileSize, PAL16_YELLOW + 12);  // Right
		}

		// Draw tile number label prominently
		DrawString(out, StrCat(tile), { { x + 1, y + tileSize - 9 }, { tileSize - 2, 8 } }, { .flags = UiFlags::ColorWhite | UiFlags::AlignCenter });
	}
}

void DrawInterface(const Surface &out)
{
	// Top bar.
	DrawHalfTransparentRectTo(out, 0, 0, gnScreenWidth, TopBarHeight);
	DrawHalfTransparentRectTo(out, 0, 0, gnScreenWidth, TopBarHeight);
	std::string title;
	if (S.kind == Kind::Tristram)
		title = StrCat("Map editor: Tristram", S.dirty ? " *" : "", "  (links only)");
	else
		title = StrCat("Editor: ", S.map.name, S.dirty ? " *" : "", "  (", MapStyleName(S.map.style), ")",
		    S.map.spawners.empty() && S.map.bosses.empty() ? std::string {} : StrCat("  packs ", S.map.spawners.size(), ", bosses ", S.map.bosses.size()));
	DrawString(out, title, { { 8, 5 }, { gnScreenWidth - 3 * 68 - 16, 12 } }, { .flags = UiFlags::ColorWhitegold });
	Button(out, TopButton(0), "Save", false);
	Button(out, TopButton(1), S.editing ? "Test" : "Edit", !S.editing);
	Button(out, TopButton(2), "Exit", false);

	if (!S.editing) {
		const std::string banner = StrCat("Play-testing. P: back to editing.  G: god mode ", S.god ? "on" : "off");
		DrawHalfTransparentRectTo(out, 0, TopBarHeight, gnScreenWidth, 18);
		DrawString(out, banner, { { 0, TopBarHeight + 3 }, { gnScreenWidth, 12 } }, { .flags = UiFlags::ColorGold | UiFlags::AlignCenter });
		return;
	}

	// Tools.
	const std::vector<Tool> &tools = ToolsFor(S.kind);
	for (int i = 0; i < static_cast<int>(tools.size()); i++)
		Button(out, ToolButton(i), StrCat(i + 1, " ", ToolName(tools[i])), S.tool == tools[i]);
	const int n = static_cast<int>(tools.size());
	if (S.kind != Kind::Tristram)
		Button(out, ToolButton(n + 1), S.grid ? "G Grid: on" : "G Grid: off", false);
	Button(out, ToolButton(n + (S.kind == Kind::Tristram ? 1 : 2)), "Ctrl+Z Undo", false);

	if (S.tool == Tool::Paint && S.showTilePicker)
		DrawTilePicker(out);
	else
		DrawPalette(out);

	// Status line.
	const int y = gnViewportHeight - 18;
	DrawHalfTransparentRectTo(out, 0, y, gnScreenWidth, 18);
	std::string status = StrCat(ToolHint(S.tool), "   ");
	if (CanPlaceAt(cursPosition))
		status += StrCat("tile ", cursPosition.x, ",", cursPosition.y);
	if (SDL_GetTicks() < S.messageUntil)
		status = S.message;
	DrawString(out, status, { { 8, y + 3 }, { gnScreenWidth - 16, 12 } }, { .flags = SDL_GetTicks() < S.messageUntil ? UiFlags::ColorGold : UiFlags::ColorWhite });
	if (SDL_GetTicks() >= S.messageUntil && gnScreenWidth >= 900)
		DrawString(out, "WASD/arrows: move   Ctrl+S: save   P: test", { { 8, y + 3 }, { gnScreenWidth - 16, 12 } }, { .flags = UiFlags::ColorWhite | UiFlags::AlignRight });

	if (S.linkPortal >= 0 && S.linkPortal < static_cast<int>(S.map.portals.size()))
		DrawLinkDialog(out);
}

// --- Input -----------------------------------------------------------------------------------

bool OnInterface(Point p)
{
	if (p.y < TopBarHeight)
		return true;
	if (!S.editing)
		return false;
	if (S.linkPortal >= 0)
		return true;
	for (int i = 0; i < ToolButtonCount(); i++) {
		if (ToolButton(i).contains(p))
			return true;
	}
	if (PaletteRect().contains(p))
		return true;
	return p.y >= gnViewportHeight - 18;
}

void Undo()
{
	if (S.undo.empty()) {
		Say("Nothing to undo.");
		return;
	}
	const Snapshot s = std::move(S.undo.back());
	S.undo.pop_back();
	S.map.plan = s.plan;
	S.map.overrides = s.overrides;
	S.map.tiles = s.tiles;
	S.map.entries = s.entries;
	S.map.portals = s.portals;
	S.map.npcs = s.npcs;
	S.map.spawners = s.spawners;
	S.map.bosses = s.bosses;
	S.map.objects = s.objects;
	S.dirty = true;
	Rebuild();
}

void ToggleTest()
{
	if (S.editing) {
		Rebuild();
		if (S.map.entries.empty() && S.kind != Kind::Tristram) {
			S.map.entries.push_back({ "start", cursPosition });
			Say("No arrival point yet: added one under the cursor.");
		}
		LoadMapLevel(false);
	} else {
		LoadMapLevel(true);
	}
}

void ExitEditor()
{
	if (S.dirty)
		SaveMap();
	S.running = false;
	ClearActiveCustomMap();
	SetTristramLayerForEditor(nullptr);
	SetCurrentCustomTown({});
	gamemenu_exit_game(true);
	ReturnToMainMenu = true; // straight back to the Campaign Editor, not the hero list
}

/** Fills a town-tiled map with a meadow, woods around its edge. */
void FillMeadow(CustomMap &map)
{
	const std::vector<uint16_t> &grass = GroundBrushes()[0].tiles;
	const std::vector<uint16_t> &shrubs = GroundBrushes()[3].tiles;
	const std::vector<uint16_t> &trees = GroundBrushes()[7].tiles;
	for (int y = 0; y < map.height; y++) {
		for (int x = 0; x < map.width; x++) {
			const uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u;
			const int edge = std::min({ x, y, map.width - 1 - x, map.height - 1 - y });
			const std::vector<uint16_t> &pick = edge < 2 ? trees : ((edge == 2 && (h >> 7) % 3 == 0) || (h >> 9) % 23 == 0 ? shrubs : grass);
			map.tiles[static_cast<size_t>(y) * map.width + x] = pick[(h >> 4) % pick.size()];
		}
	}
}

/** Link dialog: makes a new wilds map beyond this portal or door, with a door back to an arrival point here. */
void MakeWildsBeyond(MapPortal &portal)
{
	std::string base = S.kind == Kind::Tristram ? std::string("tristram") : S.map.name;
	if (base.size() > 24)
		base.resize(24);
	std::string name;
	for (int n = 1; n < 100; n++) {
		name = n == 1 ? StrCat(base, "_wilds") : StrCat(base, "_wilds", n);
		if (!CustomMapExists(name) && std::find(S.mapList.begin(), S.mapList.end(), name) == S.mapList.end())
			break;
	}
	if (!IsValidMapName(name)) {
		Say("Could not name a new wilds map.");
		return;
	}
	// The way back arrives next to this portal.
	std::string back = "from_wilds";
	for (int n = 2; std::any_of(S.map.entries.begin(), S.map.entries.end(), [&](const MapEntryPoint &e) { return e.name == back; }); n++)
		back = StrCat("from_wilds", n);
	S.map.entries.push_back({ back, portal.tile + Displacement { 1, 1 } });

	CustomMap wilds;
	wilds.name = name;
	wilds.style = DTYPE_FOREST;
	wilds.Resize(40, 40);
	FillMeadow(wilds);
	wilds.entries.push_back({ "start", { 16 + 2 * 20, 16 + 2 * 35 } });
	MapPortal home;
	home.tile = { 16 + 2 * 20, 16 + 2 * 36 };
	home.door = true;
	home.to = S.kind == Kind::Tristram ? MapLink { MapLinkKind::Town, "", back, 1 } : MapLink { MapLinkKind::Map, S.map.name, back, 1 };
	wilds.portals.push_back(home);
	// A few packs to start with, fitting the hero's level.
	for (const Point piece : { Point { 10, 10 }, Point { 29, 9 }, Point { 12, 24 }, Point { 28, 23 }, Point { 20, 15 } })
		wilds.spawners.push_back({ { 16 + 2 * piece.x, 16 + 2 * piece.y }, std::string(AutoMonster), 4 });
	if (std::expected<void, std::string> r = WriteCustomMap(wilds); !r.has_value()) {
		Say(StrCat("Could not make ", name, ": ", r.error()));
		S.map.entries.pop_back();
		return;
	}
	portal.to = { MapLinkKind::Map, name, "start", 1 };
	S.mapList.push_back(name);
	SaveMap();
	Say(StrCat("Made ", name, " (with a door back here). Shape it with:  editor ", name));
}

bool ClickDialog(Point p, bool right)
{
	MapPortal &portal = S.map.portals[S.linkPortal];
	for (int row = 0; row < DialogRowCount(); row++) {
		if (!DialogRowRect(row).contains(p))
			continue;
		PushUndo();
		MapLink &l = portal.to;
		if (row == 0) {
			l = {};
		} else if (row == 1) {
			// Tristram; clicking again steps through its added arrival points.
			const std::vector<std::string> entries = TristramEntries();
			std::string entry;
			if (l.kind == MapLinkKind::Town && !entries.empty()) {
				auto it = std::find(entries.begin(), entries.end(), l.entry);
				entry = it == entries.end() ? entries.front() : (it + 1 == entries.end() ? std::string {} : *(it + 1));
			}
			l = { MapLinkKind::Town, "", entry, 1 };
		} else if (row == 2) {
			const int level = l.kind == MapLinkKind::Dungeon ? l.level + (right ? -1 : 1) : 1;
			l = { MapLinkKind::Dungeon, "", "", std::clamp(level, 1, 16) };
		} else if (row == 3) {
			MakeWildsBeyond(portal);
		} else if (row == DialogRowCount() - 1) {
			S.linkPortal = -1;
		} else {
			const std::string &name = S.mapList[row - 4];
			std::vector<std::string> entries;
			if (name == S.map.name) {
				for (const MapEntryPoint &e : S.map.entries)
					entries.push_back(e.name);
			} else if (std::expected<CustomMap, std::string> other = ReadCustomMap(name); other.has_value()) {
				for (const MapEntryPoint &e : other->entries)
					entries.push_back(e.name);
			}
			std::string entry = entries.empty() ? "" : entries.front();
			if (l.kind == MapLinkKind::Map && l.map == name && !entries.empty()) {
				// Clicking the chosen map again picks its next arrival point.
				auto it = std::find(entries.begin(), entries.end(), l.entry);
				entry = (it == entries.end() || it + 1 == entries.end()) ? entries.front() : *(it + 1);
			}
			l = { MapLinkKind::Map, name, entry, 1 };
		}
		return true;
	}
	if (!DialogRect().contains(p))
		S.linkPortal = -1;
	return true;
}

void SelectTool(int index)
{
	const std::vector<Tool> &tools = ToolsFor(S.kind);
	if (index < 0 || index >= static_cast<int>(tools.size()))
		return;
	S.tool = tools[index];
	// Show the selected entry of a long list.
	S.paletteScroll = 0;
	ScrollPalette(PaletteSelected() - PaletteVisibleRows() / 2);
}

void MouseDown(Point p, bool right)
{
	for (int i = 0; i < 3; i++) {
		if (!TopButton(i).contains(p))
			continue;
		if (i == 0)
			SaveMap();
		else if (i == 1)
			ToggleTest();
		else
			ExitEditor();
		return;
	}
	if (!S.editing)
		return;
	if (S.linkPortal >= 0) {
		ClickDialog(p, right);
		return;
	}
	const int n = static_cast<int>(ToolsFor(S.kind).size());
	for (int i = 0; i < ToolButtonCount(); i++) {
		if (!ToolButton(i).contains(p))
			continue;
		if (i < n)
			SelectTool(i);
		else if (S.kind != Kind::Tristram && i == n + 1)
			S.grid = !S.grid;
		else if (i == n + (S.kind == Kind::Tristram ? 1 : 2))
			Undo();
		return;
	}
	if (PaletteRect().contains(p)) {
		if (PaletteTitleRect().contains(p)) {
			if (S.tool == Tool::Spawner)
				S.packSize = std::clamp(S.packSize + (right ? -1 : 1), 1, 20);
			return;
		}
		// Tile picker click handling.
		if (S.showTilePicker) {
			const Rectangle panel = PaletteRect();
			const int tileSize = S.pickerZoom * 16;
			const int tilesPerRow = std::max(1, panel.size.width / (tileSize + 4));
			const int startX = panel.position.x + 4;
			const int startY = panel.position.y + 24;

			for (uint16_t tile = 1; tile <= 127; tile++) {
				const int row = (tile - 1) / tilesPerRow;
				const int col = (tile - 1) % tilesPerRow;
				const int x = startX + col * (tileSize + 4) + S.pickerPan.x;
				const int y = startY + row * (tileSize + 4) + S.pickerPan.y;

				if (Rectangle{{ x, y }, { tileSize, tileSize }}.contains(p)) {
					S.pickerTile = tile;
					return;
				}
			}
			return;
		}

		const int rows = static_cast<int>(PaletteRows().size());
		for (int i = 0; i < rows; i++) {
			if (PaletteRowShown(i) && PaletteRowRect(i).contains(p))
				SelectPalette(i);
		}
		return;
	}
	if (OnInterface(p))
		return;

	const Point tile = cursPosition;
	const Point piece = PieceAt(tile);
	switch (S.tool) {
	case Tool::Room:
	case Tool::Erase:
	case Tool::Copy:
		if (InMap(piece)) {
			S.dragging = true;
			S.dragErase = right || S.tool == Tool::Erase;
			S.dragStart = piece;
		}
		break;
	case Tool::Paint:
		if (!InMap(piece))
			break;
		if (right) {
			S.pickedTile = TownTile(piece);
			S.brush = -1;
			Say(StrCat("Picked up tile #", S.pickedTile, ". Drag to paint with it."));
		} else {
			S.dragging = true;
			S.dragStart = piece;
		}
		break;
	case Tool::Stamp:
		if (InMap(piece) && !right)
			PlaceStamp(piece);
		break;
	case Tool::Npc: {
		const int at = NpcAt(tile);
		if (right) {
			if (at >= 0) {
				PushUndo();
				S.map.npcs.erase(S.map.npcs.begin() + at);
			}
			break;
		}
		if (!CanPlaceAt(tile))
			break;
		const std::vector<Person> people = People();
		const Person &who = people[std::clamp(S.npc, 0, static_cast<int>(people.size()) - 1)];
		PushUndo();
		auto existing = std::find_if(S.map.npcs.begin(), S.map.npcs.end(), [&](const MapNpc &npc) { return npc.type == who.type; });
		if (existing != S.map.npcs.end()) {
			existing->tile = tile;
			Say(StrCat(who.label, " moved (one of each townsperson). Test to see them."));
		} else {
			S.map.npcs.push_back({ std::string(who.type), tile, 0 });
			Say(StrCat(who.label, " placed. Test (P) to see them in town."));
		}
		break;
	}
	case Tool::Spawner: {
		const int at = SpawnerAt(tile);
		if (right) {
			if (at >= 0) {
				PushUndo();
				S.map.spawners.erase(S.map.spawners.begin() + at);
			}
			break;
		}
		const std::vector<SpawnableMonster> &list = SpawnableMonsters();
		if (list.empty() || (at < 0 && !CanPlaceAt(tile)))
			break;
		const SpawnableMonster &chosen = list[std::clamp(S.monster, 0, static_cast<int>(list.size()) - 1)];
		PushUndo();
		if (at >= 0) {
			S.map.spawners[at].monster = chosen.key;
			S.map.spawners[at].count = S.packSize;
			Say(StrCat("Pack changed: ", SpawnerLabel(chosen.key), " x", S.packSize, "."));
		} else {
			S.map.spawners.push_back({ tile, chosen.key, S.packSize });
			Say(StrCat("Pack placed: ", SpawnerLabel(chosen.key), " x", S.packSize, ". They appear each time the hero comes here."));
		}
		break;
	}
	case Tool::Chest: {
		if (!CanPlaceAt(tile))
			break;
		std::optional<Point> near;
		for (const Point chest : MapChests(S.map)) {
			if (chest.WalkingDistance(tile) <= 1)
				near = chest;
		}
		PushUndo();
		if (near) {
			SetMapChest(S.map, *near, false);
			Say("Chest removed.");
		} else if (!right) {
			SetMapChest(S.map, tile, true);
			Say("Chest placed. A quest can hide its item here (Campaign Editor, Quests).");
		}
		break;
	}
	case Tool::Boss: {
		const int at = BossAt(tile);
		if (right) {
			if (at >= 0) {
				PushUndo();
				S.map.bosses.erase(S.map.bosses.begin() + at);
			}
			break;
		}
		const std::vector<std::string> &list = PlaceableBosses();
		if (list.empty() || (at < 0 && !CanPlaceAt(tile)))
			break;
		const std::string &chosen = list[std::clamp(S.boss, 0, static_cast<int>(list.size()) - 1)];
		PushUndo();
		if (at >= 0)
			S.map.bosses[at].name = chosen;
		else
			S.map.bosses.push_back({ tile, chosen, 6 });
		Say(StrCat("Boss: ", chosen, "."));
		break;
	}
	case Tool::Entry: {
		const int at = EntryAt(tile);
		if (right) {
			if (at >= 0) {
				PushUndo();
				S.map.entries.erase(S.map.entries.begin() + at);
			}
		} else if (at >= 0) {
			PushUndo();
			S.movingEntry = at;
		} else if (CanPlaceAt(tile)) {
			PushUndo();
			S.map.entries.push_back({ NewEntryName(), tile });
			Say(StrCat("Arrival point \"", S.map.entries.back().name, "\" added."));
		}
		break;
	}
	case Tool::Portal:
	case Tool::Door: {
		const int at = PortalAt(tile);
		if (right) {
			if (at >= 0) {
				PushUndo();
				S.map.portals.erase(S.map.portals.begin() + at);
			}
		} else if (at >= 0) {
			OpenLinkDialog(at);
		} else if (CanPlaceAt(tile)) {
			PushUndo();
			MapPortal portal;
			portal.tile = tile;
			portal.door = S.tool == Tool::Door;
			S.map.portals.push_back(portal);
			OpenLinkDialog(static_cast<int>(S.map.portals.size()) - 1);
		}
		break;
	}
	default:
		break;
	}
}

void MouseUp()
{
	if (S.dragging) {
		S.dragging = false;
		const Point end = PieceAt(cursPosition);
		if (S.tool == Tool::Paint)
			PaintGround(S.dragStart, end);
		else if (S.tool == Tool::Copy)
			CopyArea(S.dragStart, end);
		else
			SetPlan(S.dragStart, end, !S.dragErase);
	}
	S.movingEntry = -1;
	S.movingPortal = -1;
}

} // namespace

// --- Session ---------------------------------------------------------------------------------

void QueueMapEditor(std::string_view name, dungeon_type style, bool startTesting)
{
	S = {};
	S.queued = true;
	S.editing = !startTesting;
	if (name == TristramLayerName) {
		// Tristram itself: only links are added, on the classic layout.
		S.kind = Kind::Tristram;
		S.map = ReadTristramLayer();
		S.tool = Tool::Portal;
		S.camera = { 64, 58 };
		return;
	}
	if (std::expected<CustomMap, std::string> existing = ReadCustomMap(name); existing.has_value()) {
		S.map = std::move(*existing);
		S.kind = S.map.style == DTYPE_TOWN ? Kind::Town : (S.map.style == DTYPE_FOREST ? Kind::Wilds : Kind::Dungeon);
	} else if (style == DTYPE_TOWN) {
		S.kind = Kind::Town;
		S.map.name = std::string(name);
		S.map.style = DTYPE_TOWN;
		S.map.Resize(40, 40);
		// A new town starts as a meadow with an arrival point in the middle.
		const std::vector<uint16_t> &grass = GroundBrushes().front().tiles;
		for (int y = 0; y < S.map.height; y++) {
			for (int x = 0; x < S.map.width; x++) {
				const uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u;
				S.map.tiles[static_cast<size_t>(y) * S.map.width + x] = grass[(h >> 4) % grass.size()];
			}
		}
		S.map.entries.push_back({ "start", { 16 + 2 * 20, 16 + 2 * 20 } });
		S.dirty = true;
	} else if (style == DTYPE_FOREST) {
		// New wilds: a meadow ringed by woods.
		S.kind = Kind::Wilds;
		S.map.name = std::string(name);
		S.map.style = DTYPE_FOREST;
		S.map.Resize(40, 40);
		FillMeadow(S.map);
		S.map.entries.push_back({ "start", { 16 + 2 * 20, 16 + 2 * 20 } });
		S.dirty = true;
	} else {
		S.kind = Kind::Dungeon;
		S.map.name = std::string(name);
		S.map.style = style;
		S.map.Resize(40, 40);
		// A new map starts with a small room in the middle (the hero needs floor to stand on).
		for (int y = 16; y <= 23; y++) {
			for (int x = 16; x <= 23; x++)
				S.map.plan[static_cast<size_t>(y) * Grid + x] = 1;
		}
		S.map.entries.push_back({ "start", { 16 + 2 * 20, 16 + 2 * 20 } });
		std::array<uint16_t, Grid * Grid> tiles {};
		BuildFromFloorPlan(style, S.map.plan.data(), tiles.data());
		for (int y = 0; y < S.map.height; y++) {
			for (int x = 0; x < S.map.width; x++)
				S.map.tiles[static_cast<size_t>(y) * S.map.width + x] = tiles[static_cast<size_t>(y) * Grid + x];
		}
		S.dirty = true;
	}
	S.tool = S.kind == Kind::Dungeon ? Tool::Room : Tool::Paint;
	S.camera = { 16 + S.map.width, 16 + S.map.height };
	if (!S.map.entries.empty())
		S.camera = S.map.entries.front().tile;
}

bool HasQueuedMapEditor()
{
	return S.queued;
}

void StartQueuedMapEditor()
{
	S.queued = false;
	S.running = true;
	S.travelled = false;
	gbIsMultiplayer = false;
	// The editor plays with its own hero in a save slot hidden from the hero list.
	gSaveNumber = MapEditorHeroSlot;
	_uiheroinfo info {};
	info.saveNumber = MapEditorHeroSlot;
	CopyUtf8(info.name, "Map Editor", sizeof(info.name));
	SetNewHeroCampaign(ActiveCampaign()); // the editor's hero works in the open campaign
	info.heroclass = HeroClass::Warrior;
	if (Players.empty())
		Players.resize(MAX_PLRS);
	pfile_ui_save_create(&info);
	gbLoadGame = false;
	StartGame(true, true);
	S.running = false;
	ClearActiveCustomMap();
	SetTristramLayerForEditor(nullptr);
	SetCurrentCustomTown({});
}

bool IsEditingTristramLayer()
{
	return S.running && S.kind == Kind::Tristram;
}

bool IsMapEditorSession()
{
	return S.running;
}

bool IsMapEditing()
{
	return S.running && S.editing && OnEditorLevel();
}

void MapEditorTick()
{
	if (!S.running || MyPlayer == nullptr)
		return;
	if (!S.travelled && leveltype == DTYPE_TOWN && !MyPlayer->_pLvlChanging) {
		S.travelled = true;
		LoadMapLevel(S.editing);
		return;
	}
	Player &player = *MyPlayer;
	if (!S.editing && S.god && player._pHitPoints > 0) {
		player._pHitPoints = player._pMaxHP;
		player._pHPBase = player._pMaxHPBase;
	}
#ifdef _DEBUG
	// Test hook: DEVILUTIONX_DEV_EDITORSCRIPT = commands separated by ';' run one per tick once editing:
	// room x0 y0 x1 y1 | erase x0 y0 x1 y1 | paint x0 y0 x1 y1 brush | stamp index x y | copy x0 y0 x1 y1 |
	// npc index x y | entry x y | portal x y town [entry] | door x y map name | save | test | cam x y | tool n | dialog
	if (const char *script = std::getenv("DEVILUTIONX_DEV_EDITORSCRIPT"); script != nullptr && IsMapEditing()) {
		static size_t step = 0;
		static int wait = 0;
		std::vector<std::string_view> cmds;
		for (std::string_view c : SplitByChar(script, ';'))
			cmds.push_back(c);
		if (step < cmds.size() && ++wait > 6) {
			wait = 0;
			std::vector<int> n;
			std::vector<std::string_view> w;
			for (std::string_view part : SplitByChar(cmds[step], ' ')) {
				if (part.empty())
					continue;
				w.push_back(part);
				const ParseIntResult<int> v = ParseInt<int>(part);
				n.push_back(v.has_value() ? *v : 0);
			}
			step++;
			if (!w.empty()) {
				if ((w[0] == "room" || w[0] == "erase") && n.size() >= 5)
					SetPlan({ n[1], n[2] }, { n[3], n[4] }, w[0] == "room");
				else if (w[0] == "paint" && n.size() >= 6) {
					S.brush = n[5];
					PaintGround({ n[1], n[2] }, { n[3], n[4] });
				} else if (w[0] == "stamp" && n.size() >= 4) {
					S.stamp = n[1];
					PlaceStamp({ n[2], n[3] });
				} else if (w[0] == "copy" && n.size() >= 5)
					CopyArea({ n[1], n[2] }, { n[3], n[4] });
				else if (w[0] == "spawner" && n.size() >= 5) {
					PushUndo();
					S.map.spawners.push_back({ { n[1], n[2] }, std::string(w[3]), std::clamp(n[4], 1, 20) });
				} else if (w[0] == "boss" && n.size() >= 4) {
					PushUndo();
					S.map.bosses.push_back({ { n[1], n[2] }, PlaceableBosses()[std::clamp(n[3], 0, static_cast<int>(PlaceableBosses().size()) - 1)], 6 });
				} else if (w[0] == "chest" && n.size() >= 3) {
					PushUndo();
					SetMapChest(S.map, { n[1], n[2] }, true);
				} else if (w[0] == "newwilds" && !S.map.portals.empty()) {
					MakeWildsBeyond(S.map.portals.back());
				} else if (w[0] == "pick" && n.size() >= 2) {
					SelectPalette(n[1]);
					ScrollPalette(n[1] - S.paletteScroll - PaletteVisibleRows() / 2);
				} else if (w[0] == "npc" && n.size() >= 4) {
					PushUndo();
					S.map.npcs.push_back({ People()[n[1]].type, { n[2], n[3] }, 0 });
				} else if (w[0] == "entry" && n.size() >= 3)
					S.map.entries.push_back({ w.size() >= 4 ? std::string(w[3]) : NewEntryName(), { n[1], n[2] } });
				else if ((w[0] == "portal" || w[0] == "door") && n.size() >= 4) {
					MapPortal portal;
					portal.tile = { n[1], n[2] };
					portal.door = w[0] == "door";
					if (w[3] == "town")
						portal.to = { MapLinkKind::Town, "", w.size() >= 5 ? std::string(w[4]) : "", 1 };
					else if (w[3] == "map" && w.size() >= 5)
						portal.to = { MapLinkKind::Map, std::string(w[4]), w.size() >= 6 ? std::string(w[5]) : "", 1 };
					else if (w[3] == "dungeon" && n.size() >= 5)
						portal.to = { MapLinkKind::Dungeon, "", "", n[4] };
					S.map.portals.push_back(portal);
				} else if (w[0] == "save")
					SaveMap();
				else if (w[0] == "exit")
					ExitEditor();
				else if (w[0] == "test")
					ToggleTest();
				else if (w[0] == "cam" && n.size() >= 3)
					S.camera = { n[1], n[2] };
				else if (w[0] == "tool" && n.size() >= 2)
					SelectTool(n[1] - 1);
				else if (w[0] == "dialog")
					OpenLinkDialog(static_cast<int>(S.map.portals.size()) - 1);
				LogInfo("EDITORSCRIPT {}", cmds[step - 1]);
			}
		}
	}
#endif
	const int key = static_cast<int>(currlevel) + (setlevel ? 1000 + setlvlnum : 0);
	if (key != S.levelKey) {
		S.levelKey = key;
		if (IsMapEditing() && S.kind == Kind::Dungeon) {
			// Freshly built level: keep editing from the same spot.
			Rebuild();
		}
	}
}

void MapEditorBeforeDraw()
{
	if (!IsMapEditing())
		return;
	// Free camera.
	const auto *keys = SDLC_GetKeyState();
	static uint32_t last = 0;
	const uint32_t now = SDL_GetTicks();
	if (now - last >= 45 && S.linkPortal < 0) {
		last = now;
		Displacement d {};
		if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) d += Displacement { -1, -1 };
		if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]) d += Displacement { 1, 1 };
		if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]) d += Displacement { -1, 1 };
		if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) d += Displacement { 1, -1 };
		S.camera = S.camera + d;
		S.camera.x = std::clamp(S.camera.x, 10, MAXDUNX - 10);
		S.camera.y = std::clamp(S.camera.y, 10, MAXDUNY - 10);
	}
	ViewPosition = S.camera;
	// Everything lit and in view.
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			dLight[x][y] = 0;
			dFlags[x][y] |= DungeonFlag::Visible | DungeonFlag::Lit | DungeonFlag::Explored;
		}
	}
	// Dragging markers.
	if (S.movingEntry >= 0 && S.movingEntry < static_cast<int>(S.map.entries.size()))
		S.map.entries[S.movingEntry].tile = cursPosition;
}

bool MapEditorHandleEvent(const SDL_Event &event)
{
	if (!S.running || !OnEditorLevel())
		return false;
	switch (event.type) {
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
		if (event.button.button == 2 && S.editing && S.showTilePicker) {  // 2 = middle mouse button
			// Middle mouse to drag tile picker
			S.pickerDragging = true;
			S.pickerDragStart = MousePosition;
			return true;
		}
		if (event.button.button != SDL_BUTTON_LEFT && event.button.button != SDL_BUTTON_RIGHT)
			return S.editing;
		if (!S.editing && !TopButton(1).contains(MousePosition) && !TopButton(0).contains(MousePosition) && !TopButton(2).contains(MousePosition))
			return false; // play-testing: the game gets the click
		MouseDown(MousePosition, event.button.button == SDL_BUTTON_RIGHT);
		return true;
	case SDL_EVENT_MOUSE_BUTTON_UP:
		if (S.pickerDragging) {
			S.pickerDragging = false;
			return true;
		}
		if (!S.editing)
			return false;
		MouseUp();
		return true;
	case SDL_EVENT_KEY_DOWN: {
		const SDL_Scancode sc = SDLC_EventScancode(event);
		const bool ctrl = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
		if (sc == SDL_SCANCODE_P) {
			ToggleTest();
			return true;
		}
		if (!S.editing) {
			if (sc == SDL_SCANCODE_G) {
				S.god = !S.god;
				return true;
			}
			return false;
		}
		// Hue shift when tile picker is open
		if (S.tool == Tool::Paint && S.showTilePicker) {
			if (sc == SDL_SCANCODE_F) {
				S.hueShift = std::max(S.hueShift - 5, -30);
				return true;
			} else if (sc == SDL_SCANCODE_G) {
				S.hueShift = std::min(S.hueShift + 5, 30);
				return true;
			}
		}
		if (ctrl && sc == SDL_SCANCODE_S) {
			SaveMap();
		} else if (ctrl && sc == SDL_SCANCODE_Z) {
			Undo();
		} else if (sc == SDL_SCANCODE_G && !(S.tool == Tool::Paint && S.showTilePicker)) {
			S.grid = !S.grid;
		} else if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) {
			SelectTool(sc - SDL_SCANCODE_1);
		} else if (sc == SDL_SCANCODE_PAGEUP) {
			ScrollPalette(-PaletteVisibleRows());
		} else if (sc == SDL_SCANCODE_PAGEDOWN) {
			ScrollPalette(PaletteVisibleRows());
		} else if (sc == SDL_SCANCODE_EQUALS || sc == SDL_SCANCODE_KP_PLUS) {
			S.packSize = std::min(S.packSize + 1, 20);
		} else if (sc == SDL_SCANCODE_MINUS || sc == SDL_SCANCODE_KP_MINUS) {
			S.packSize = std::max(S.packSize - 1, 1);
		} else if (sc == SDL_SCANCODE_ESCAPE && S.linkPortal >= 0) {
			S.linkPortal = -1;
		} else if (sc == SDL_SCANCODE_ESCAPE) {
			return false; // the game menu (save and quit from there, or use Exit)
		}
		return true; // no game hotkeys while editing
	}
	case SDL_EVENT_KEY_UP:
		return S.editing;
	case SDL_EVENT_MOUSE_WHEEL:
		if (!S.editing)
			return false;
		if (S.showTilePicker) {
			// Zoom the tile picker
			S.pickerZoom = std::clamp(S.pickerZoom + (event.wheel.y > 0 ? 1 : -1), 1, 4);
			return true;
		}
		ScrollPalette(event.wheel.y > 0 ? -3 : 3);
		return true;
	case SDL_EVENT_MOUSE_MOTION:
		if (S.pickerDragging && S.showTilePicker) {
			// Pan the tile picker with middle mouse drag
			const auto delta = MousePosition - S.pickerDragStart;
			S.pickerPan = S.pickerPan + delta;
			S.pickerDragStart = MousePosition;
			return true;
		}
		return false;
	default:
		return false;
	}
}

void DrawMapEditor(const Surface &out)
{
	if (!S.running || !OnEditorLevel())
		return;
	if (S.editing)
		DrawWorldOverlay(out);
	DrawInterface(out);
}

} // namespace devilution
