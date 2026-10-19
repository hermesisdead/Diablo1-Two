#include "custom_maps/map_editor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <span>
#include <string>
#include <vector>

#include "control/control.hpp"
#include "controls/control_mode.hpp"
#include "cursor.h"
#include "custom_maps/building_library.hpp"
#include "custom_maps/campaign.hpp"
#include "custom_maps/custom_map.hpp"
#include "custom_maps/custom_npcs.hpp"
#include "custom_maps/custom_town.hpp"
#include "custom_maps/map_animals.hpp"
#include "custom_maps/map_monsters.hpp"
#include "diablo.h"
#include "engine/light_tables.hpp"
#include "engine/load_file.hpp"
#include "engine/palette.h"
#include "custom_maps/file_dialog.hpp"
#include "engine/events.hpp"
#include "engine/rectangle.hpp"
#include "engine/sound.h"
#include "engine/render/clx_render.hpp"
#include "engine/render/dungeon_mood.hpp"
#include "engine/render/dun_render.hpp"
#include "engine/render/light_render.hpp"
#include "engine/render/primitive_render.hpp"
#include "engine/render/text_render.hpp"
#include "engine/render/tile_decor.hpp"
#include "engine/render/tile_hue.hpp"
#include "engine/render/scrollrt.h"
#include "engine/render/world_projector.hpp"
#include "engine/surface.hpp"
#include "gamemenu.h"
#include "levels/desert.hpp"
#include "levels/drlg_l1.h"
#include "levels/drlg_l2.h"
#include "levels/drlg_l3.h"
#include "levels/drlg_l4.h"
#include "levels/gendung.h"
#include "objects.h"
#include "tables/objdat.h"
#include "levels/tile_properties.hpp"
#include "levels/town.h"
#include "lighting.h"
#include "menu.h"
#include "options.h"
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
	Fence,
	Animal,
	Prop,
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
	std::vector<int8_t> hues;
	std::array<std::vector<uint16_t>, MapDecorLayerCount> decor;
	std::array<std::vector<int8_t>, MapDecorLayerCount> decorHues;
	std::vector<uint8_t> fences;
	std::vector<MapAnimal> animals;
};

/** What the Paint tool lays: ground (a brush or one tile), decor on a decor layer, or a whole building. */
enum class PaintKind : uint8_t {
	Ground,
	Decor,
	Building,
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
	int brush = 0;            // ground brush, or -1 for a single tile (pickerTile)
	int stamp = 0;            // index in Buildings(), or -1 for the copied area
	Building clip;            // the copied area
	int npc = 0;

	// Paint tool: what is laid, and on which layer (0 ground, 1.. decor layers).
	PaintKind paintKind = PaintKind::Ground;
	uint16_t pickerDecor = DecorFlowerBase; // decor id painted (0 erases)
	int pickerBuilding = 0;                 // index in Buildings()
	int layer = 0;
	unsigned hiddenLayers = 0;              // decor layers hidden while editing (bit per layer)

	// Building maker: drag out an area, then type its name.
	bool capturing = false;
	bool naming = false;
	bool replaceAsked = false;
	std::string nameText;
	Building pending;
	std::string deleteAsk;
	uint32_t deleteUntil = 0;

	bool weatherMenu = false; // the weather list is open
	bool musicPanel = false;  // the map's music is being chosen
	int musicScroll = 0;
	std::vector<std::string> musicLibrary; // songs uploaded to the campaign

	int fenceKind = 0; // Fence tool: 0 fence, 1 gate, 2 remove
	int animal = 0;    // Animals tool: index in AnimalKinds()
	int prop = 0;      // Props tool: index in PropsForMap()
	std::vector<uint16_t> liveObjects; // the map's objects as they stand in the loaded level (dungeons, wilds)
	std::vector<std::pair<Point, uint8_t>> fenceGhost; // fencing previewed this frame, and what it covered

	// Tile picker (Paint tool): every megatile of the tileset as a picture.
	bool showTilePicker = false;
	int pickerTab = 0;         // Ground, Decor, Buildings, All
	uint16_t pickerTile = 1;   // painted when brush < 0
	int pickerScroll = 0;      // first row of pictures shown
	int pickerZoom = 2;        // picture size, 1 to 3
	int hueShift = 0;          // colour shift of painted tiles, in TileHueStepDegrees steps

	// Middle-mouse drag: scrolls the picker, or moves the camera over the map.
	bool panning = false;
	bool panningPicker = false;
	Point panLast {};
	Displacement panRemainder {};

	// What painting would lay under the cursor, put into the level for one frame so it is drawn
	// exactly as it will look; this remembers what it covered.
	struct GhostCell {
		Point tile;
		uint16_t piece;
		int8_t hue;
		int8_t special;
		std::array<uint16_t, DecorLayerCount> decorPiece;
		std::array<uint8_t, DecorLayerCount> decorFlower;
		std::array<int8_t, DecorLayerCount> decorHue;
	};
	std::vector<GhostCell> ghost;

	// Monster tools.
	int monster = 0;    // index in SpawnableMonsters()
	int packSize = 4;
	int boss = 0;       // index in PlaceableBosses()
	int paletteScroll = 0;

	// Link dialog for a portal or door: first where it leads, then (if there is a choice) where it arrives.
	int linkPortal = -1;
	std::vector<std::string> mapList;
	bool linkIsNew = false; // cancelling takes the just-placed door away again
	int linkPage = 0;       // 0 destinations, 1 arrival points
	int linkTarget = -1;    // page 1: index in linkTargets, or -2 for Tristram
	int linkScroll = 0;
	struct LinkTarget {
		std::string name;
		std::string style;
		std::vector<std::string> entries;
	};
	std::vector<LinkTarget> linkTargets;

	std::string message;
	uint32_t messageUntil = 0;
};

Session S;

// --- Helpers ---------------------------------------------------------------------------------

void Say(std::string text)
{
	S.message = std::move(text);
	S.messageUntil = SDL_GetTicks() + 3500;
}

const std::vector<Tool> &ToolsFor(Kind kind)
{
	static const std::vector<Tool> dungeon { Tool::Room, Tool::Erase, Tool::Paint, Tool::Prop, Tool::Spawner, Tool::Boss, Tool::Chest, Tool::Entry, Tool::Portal, Tool::Door };
	static const std::vector<Tool> town { Tool::Paint, Tool::Stamp, Tool::Copy, Tool::Fence, Tool::Animal, Tool::Npc, Tool::Entry, Tool::Portal, Tool::Door };
	static const std::vector<Tool> wilds { Tool::Paint, Tool::Stamp, Tool::Copy, Tool::Fence, Tool::Animal, Tool::Prop, Tool::Spawner, Tool::Boss, Tool::Chest, Tool::Entry, Tool::Portal, Tool::Door };
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
	S.undo.push_back({ S.map.plan, S.map.overrides, S.map.tiles, S.map.entries, S.map.portals, S.map.npcs, S.map.spawners, S.map.bosses, S.map.objects, S.map.hues, S.map.decor, S.map.decorHues, S.map.fences, S.map.animals });
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

/**
 * Brings the loaded level's objects (props, chests) in line with the map: what was placed or
 * removed since the level was built shows up (or goes) right away.
 */
void SyncLiveObjects()
{
	if (!IsAnyOf(S.kind, Kind::Dungeon, Kind::Wilds) || !OnEditorLevel())
		return;
	const size_t n = S.map.objects.size();
	if (S.liveObjects.size() != n) {
		S.liveObjects = S.map.objects; // the map was resized: the next build shows them
		return;
	}
	const int cols = std::max(1, 2 * S.map.width);
	for (size_t i = 0; i < n; i++) {
		if (S.liveObjects[i] == S.map.objects[i])
			continue;
		const Point t { 16 + static_cast<int>(i % cols), 16 + static_cast<int>(i / cols) };
		if (S.map.objects[i] == 0)
			RemoveObjectNow(t);
		else
			AddDunObjectNow(S.map.objects[i], t);
		S.liveObjects[i] = S.map.objects[i];
	}
}

/** Rebuilds the level from the map after an edit. */
void Rebuild()
{
	if (S.kind == Kind::Tristram)
		return; // Tristram's tiles are not edited
	SetHiddenDecorLayers(S.editing ? S.hiddenLayers : 0);
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
			SyncLiveObjects();
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
	// Decor painted over the floors, colour shifts, and props.
	ApplyDungeonMapDecor(S.map);
	SyncLiveObjects();
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
			if (S.map.hues.size() == S.map.tiles.size())
				S.map.hues[static_cast<size_t>(y) * S.map.width + x] = 0;
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

/** Town: hue shift of a megatile (in TileHueStepDegrees steps). */
int8_t &TownHue(Point piece)
{
	if (S.map.hues.size() != S.map.tiles.size())
		S.map.hues.assign(S.map.tiles.size(), 0);
	return S.map.hues[static_cast<size_t>(piece.y) * S.map.width + piece.x];
}

/** Town: the megatile painting lays at a piece (brushes vary it by position), 0 for none. */
uint16_t PaintTileAt(Point piece)
{
	if (S.brush < 0)
		return S.pickerTile;
	if (S.kind == Kind::Dungeon)
		return 0; // the "auto" brush: the tile the floor plan gives
	const std::vector<GroundBrush> &brushes = GroundBrushes();
	if (S.brush >= static_cast<int>(brushes.size()) || brushes[S.brush].tiles.empty())
		return 0;
	const std::vector<uint16_t> &choices = brushes[S.brush].tiles;
	const uint32_t h = static_cast<uint32_t>(piece.x) * 73856093u ^ static_cast<uint32_t>(piece.y) * 19349663u;
	return choices[(h >> 4) % choices.size()];
}

/** Town: fills a rectangle of megatiles with the current brush or picked tile, in the current hue. */
void PaintGround(Point a, Point b)
{
	const int x0 = std::max(0, std::min(a.x, b.x)), x1 = std::min(S.map.width - 1, std::max(a.x, b.x));
	const int y0 = std::max(0, std::min(a.y, b.y)), y1 = std::min(S.map.height - 1, std::max(a.y, b.y));
	if (x0 > x1 || y0 > y1 || (S.kind != Kind::Dungeon && PaintTileAt({ x0, y0 }) == 0))
		return;
	PushUndo();
	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++) {
			const uint16_t tile = PaintTileAt({ x, y });
			if (S.kind == Kind::Dungeon) {
				// Tiles mode: the tile is laid as it is; the floor plan's walls do not follow it.
				S.map.overrides[static_cast<size_t>(y) * Grid + x] = tile;
				TownHue({ x, y }) = static_cast<int8_t>(tile != 0 ? S.hueShift : 0);
			} else {
				TownTile({ x, y }) = tile;
				TownHue({ x, y }) = static_cast<int8_t>(S.hueShift);
			}
		}
	}
	Rebuild();
}

/** Town: decor id on a decor layer (0-based) at a megatile. */
uint16_t &TownDecor(int layer, Point piece)
{
	std::vector<uint16_t> &ids = S.map.decor[layer];
	if (ids.size() != S.map.tiles.size())
		ids.assign(S.map.tiles.size(), 0);
	return ids[static_cast<size_t>(piece.y) * S.map.width + piece.x];
}

int8_t &TownDecorHue(int layer, Point piece)
{
	std::vector<int8_t> &hues = S.map.decorHues[layer];
	if (hues.size() != S.map.tiles.size())
		hues.assign(S.map.tiles.size(), 0);
	return hues[static_cast<size_t>(piece.y) * S.map.width + piece.x];
}

/** Town: fencing on a world tile of the map, nullptr outside it. */
uint8_t *TownFence(Point world)
{
	const int cols = 2 * S.map.width, rows = 2 * S.map.height;
	const Point local = world - Displacement { 16, 16 };
	if (local.x < 0 || local.y < 0 || local.x >= cols || local.y >= rows)
		return nullptr;
	if (S.map.fences.size() != static_cast<size_t>(cols) * rows)
		S.map.fences.assign(static_cast<size_t>(cols) * rows, 0);
	return &S.map.fences[static_cast<size_t>(local.y) * cols + local.x];
}

/** The world tiles of a straight run of fence from a towards b (along whichever way is longer). */
std::vector<Point> FenceRun(Point a, Point b)
{
	std::vector<Point> run;
	const Displacement d = b - a;
	if (std::abs(d.deltaX) >= std::abs(d.deltaY)) {
		for (int x = std::min(a.x, b.x); x <= std::max(a.x, b.x); x++)
			run.push_back({ x, a.y });
	} else {
		for (int y = std::min(a.y, b.y); y <= std::max(a.y, b.y); y++)
			run.push_back({ a.x, y });
	}
	return run;
}

void SetFenceRun(Point a, Point b, uint8_t kind)
{
	PushUndo();
	int changed = 0;
	for (const Point t : FenceRun(a, b)) {
		if (uint8_t *f = TownFence(t); f != nullptr) {
			*f = kind;
			changed++;
		}
	}
	Rebuild();
	Say(kind == 0 ? StrCat("Removed fencing (", changed, " tiles).") : StrCat("Built ", changed, " tiles of ", kind == 2 ? "gate." : "fence."));
}

int AnimalAt(Point tile)
{
	for (size_t i = 0; i < S.map.animals.size(); i++) {
		if (S.map.animals[i].tile.WalkingDistance(tile) <= 1)
			return static_cast<int>(i);
	}
	return -1;
}

/** Town: the building the Stamp tool places (the copied area for stamp -1), nullptr if none. */
const Building *StampBuilding()
{
	if (S.stamp < 0)
		return S.clip.width > 0 ? &S.clip : nullptr;
	const std::vector<Building> &all = Buildings();
	return S.stamp < static_cast<int>(all.size()) ? &all[S.stamp] : nullptr;
}

/** Town: the building being placed now (Stamp tool, or a building chosen in the picker). */
const Building *PlacingBuilding()
{
	if (S.tool == Tool::Paint && S.paintKind == PaintKind::Building) {
		const std::vector<Building> &all = Buildings();
		return S.pickerBuilding >= 0 && S.pickerBuilding < static_cast<int>(all.size()) ? &all[S.pickerBuilding] : nullptr;
	}
	return S.tool == Tool::Stamp ? StampBuilding() : nullptr;
}

/** Town: size of the current stamp in megatiles. */
Size StampSize()
{
	const Building *b = PlacingBuilding();
	return b != nullptr ? Size { b->width, b->height } : Size { 0, 0 };
}

/** Town: top-left megatile of the stamp when centred on a megatile. */
Point StampOrigin(Point centre)
{
	const Size size = StampSize();
	return centre - Displacement { size.width / 2, size.height / 2 };
}

/** Lays a building into the map centred on a megatile: its ground, hues and decor layers (empty cells keep what is there). */
void PlaceBuilding(const Building &b, Point centre)
{
	if (b.width <= 0 || b.height <= 0)
		return;
	const Point origin = centre - Displacement { b.width / 2, b.height / 2 };
	PushUndo();
	for (int y = 0; y < b.height; y++) {
		for (int x = 0; x < b.width; x++) {
			const Point at = origin + Displacement { x, y };
			const size_t i = static_cast<size_t>(y) * b.width + x;
			if (!InMap(at) || b.tiles[i] == 0)
				continue;
			TownTile(at) = b.tiles[i];
			TownHue(at) = b.hues[i];
			for (int layer = 0; layer < MapDecorLayerCount; layer++) {
				TownDecor(layer, at) = b.decor[layer][i];
				TownDecorHue(layer, at) = b.decorHues[layer][i];
			}
			if (b.fences.size() == b.tiles.size() * 4) {
				for (int k = 0; k < 4; k++) {
					if (uint8_t *f = TownFence({ 16 + 2 * at.x + k % 2, 16 + 2 * at.y + k / 2 }); f != nullptr)
						*f = b.fences[static_cast<size_t>(2 * y + k / 2) * (2 * b.width) + 2 * x + k % 2];
				}
			}
		}
	}
	Rebuild();
	Say(StrCat("Placed ", &b == &S.clip ? std::string("the copied area") : b.name, "."));
}

void PlaceStamp(Point centre)
{
	if (const Building *b = PlacingBuilding(); b != nullptr)
		PlaceBuilding(*b, centre);
}

/** Copies an area of the map (ground, hues and decor) into a building. */
Building TakeArea(Point a, Point b)
{
	Building out;
	const int x0 = std::max(0, std::min(a.x, b.x)), x1 = std::min(S.map.width - 1, std::max(a.x, b.x));
	const int y0 = std::max(0, std::min(a.y, b.y)), y1 = std::min(S.map.height - 1, std::max(a.y, b.y));
	if (x0 > x1 || y0 > y1)
		return out;
	out.Resize(x1 - x0 + 1, y1 - y0 + 1);
	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++) {
			const size_t i = static_cast<size_t>(y - y0) * out.width + (x - x0);
			out.tiles[i] = TownTile({ x, y });
			out.hues[i] = TownHue({ x, y });
			for (int layer = 0; layer < MapDecorLayerCount; layer++) {
				out.decor[layer][i] = TownDecor(layer, { x, y });
				out.decorHues[layer][i] = TownDecorHue(layer, { x, y });
			}
			for (int k = 0; k < 4; k++) {
				const uint8_t *f = TownFence({ 16 + 2 * x + k % 2, 16 + 2 * y + k / 2 });
				out.fences[static_cast<size_t>(2 * (y - y0) + k / 2) * (2 * out.width) + 2 * (x - x0) + k % 2] = f != nullptr ? *f : 0;
			}
		}
	}
	return out;
}

void CopyArea(Point a, Point b)
{
	Building area = TakeArea(a, b);
	if (area.width == 0)
		return;
	S.clip = std::move(area);
	S.stamp = -1;
	S.tool = Tool::Stamp;
	Say(StrCat("Copied ", S.clip.width, "x", S.clip.height, ". Click to paste it (Stamp tool, \"Copied area\")."));
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
	// A second column when the first would run into the status line (small screens).
	const int perColumn = std::max(1, (gnViewportHeight - 18 - 2 - (TopBarHeight + 6)) / (ToolHeight + 4));
	const int column = index / perColumn, row = index % perColumn;
	return { { 4 + column * (ToolWidth + 4), TopBarHeight + 6 + row * (ToolHeight + 4) }, { ToolWidth, ToolHeight } };
}

int ToolButtonCount()
{
	const int base = static_cast<int>(ToolsFor(S.kind).size()) + (S.kind == Kind::Tristram ? 2 : 5);
	if (S.kind == Kind::Town)
		return base + (S.map.outer.empty() ? 1 : 2);
	if (S.kind == Kind::Wilds && !S.map.outer.empty())
		return base + 1;
	return base;
}

/** The map's music (not in Tristram), under the weather button. */
int MusicButtonIndex()
{
	return S.kind == Kind::Tristram ? -1 : static_cast<int>(ToolsFor(S.kind).size()) + 4;
}

/** Town: the outer wilds switch (off, then each edge in turn), under the weather button. */
int OuterToggleIndex()
{
	return S.kind == Kind::Town ? static_cast<int>(ToolsFor(S.kind).size()) + 5 : -1;
}

/** Jumps between a town and its outer wilds in the editor. */
int OuterGoIndex()
{
	if (S.map.outer.empty())
		return -1;
	if (S.kind == Kind::Town)
		return static_cast<int>(ToolsFor(S.kind).size()) + 6;
	return S.kind == Kind::Wilds ? static_cast<int>(ToolsFor(S.kind).size()) + 5 : -1;
}

std::string_view SideShort(int side)
{
	switch (side) {
	case 0: return "NE";
	case 1: return "SE";
	case 2: return "SW";
	default: return "NW";
	}
}

/** The weather button sits under Undo (not for Tristram's own layer). */
int WeatherButtonIndex()
{
	return S.kind == Kind::Tristram ? -1 : static_cast<int>(ToolsFor(S.kind).size()) + 3;
}

/** Weathers offered for this map: "changing" (empty), then those its style rolls from. */
std::vector<std::string> WeatherChoices()
{
	std::vector<std::string> names { "" };
	for (const DungeonMood mood : MoodsForLevelType(S.map.style))
		names.emplace_back(DungeonMoodName(mood));
	return names;
}

std::string WeatherTitle(std::string_view name)
{
	if (name.empty())
		return "Changing";
	for (int m = 0; m < static_cast<int>(DungeonMood::COUNT); m++) {
		if (name == DungeonMoodName(static_cast<DungeonMood>(m)))
			return std::string(DungeonMoodTitle(static_cast<DungeonMood>(m)));
	}
	return std::string(name);
}

constexpr int WeatherRow = 18;

Rectangle WeatherMenuRect()
{
	const Rectangle button = ToolButton(std::max(0, WeatherButtonIndex()));
	const int rows = static_cast<int>(WeatherChoices().size());
	return { { button.position.x + button.size.width + 4, button.position.y }, { 170, 22 + rows * WeatherRow + 4 } };
}

Rectangle WeatherRowRect(int row)
{
	const Rectangle m = WeatherMenuRect();
	return { m.position + Displacement { 4, 22 + row * WeatherRow }, { m.size.width - 8, WeatherRow - 2 } };
}

std::string_view ToolName(Tool tool)
{
	switch (tool) {
	case Tool::Room: return "Room";
	case Tool::Erase: return "Erase";
	case Tool::Entry: return "Arrival";
	case Tool::Portal: return "Portal";
	case Tool::Door: return "Door";
	case Tool::Paint: return S.kind == Kind::Dungeon ? "Tiles" : "Paint";
	case Tool::Stamp: return "Stamp";
	case Tool::Copy: return "Copy";
	case Tool::Npc: return "People";
	case Tool::Spawner: return "Monsters";
	case Tool::Boss: return "Boss";
	case Tool::Chest: return "Chest";
	case Tool::Fence: return "Fence";
	case Tool::Animal: return "Animals";
	case Tool::Prop: return "Props";
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
	case Tool::Paint:
		if (S.kind == Kind::Dungeon)
			return "Drag to lay the chosen tile as it is (walls don't follow). Right-click picks up. T: tiles   L: layer   F/G: hue";
		return "Drag to paint. Right-click picks up. T: picker   L: layer   F/G: hue";
	case Tool::Stamp: return "Click to place the chosen piece of Tristram (centred on the cursor).";
	case Tool::Copy: return "Drag over an area to copy it, then click to paste it anywhere.";
	case Tool::Npc: return "Click to place the chosen townsperson (or move them). Right-click removes.";
	case Tool::Spawner: return "Click to place a pack (click one to change it). Right-click removes. Wheel over the list scrolls it.";
	case Tool::Boss: return "Click to place the chosen boss with its minions. Right-click removes. Wheel over the list scrolls it.";
	case Tool::Chest: return "Click to place a chest (quests can hide an item in it). Right-click removes.";
	case Tool::Fence: return "Drag a straight run of fence (or a gate). Right-drag removes fencing.";
	case Tool::Animal: return "Click to place the chosen animal; it wanders around there, never past fences (cows stay put). Right-click removes.";
	case Tool::Prop: return "Click to place the chosen prop (it shows right away). Right-click removes. Wheel over the list scrolls it.";
	default: return "";
	}
}

std::string PaintingLabel();

/** A dungeon prop: an object the level loads from the map (.dun object id). */
struct PropKind {
	uint16_t id;
	std::string_view label;
};

/** Props that suit the map's style (objects drawn for another tileset's palette are left out). */
const std::vector<PropKind> &PropsForMap()
{
	static const std::vector<PropKind> all {
		{ 114, "Wall torch (left wall)" }, { 115, "Wall torch (right wall)" }, { 111, "Wall torch 2 (left wall)" }, { 110, "Wall torch 2 (right wall)" },
		{ 18, "Candle" }, { 19, "Candles" }, { 20, "Candle (lit)" }, { 144, "Candle (crypt)" },
		{ 16, "Burning cross" }, { 38, "Skull fire" }, { 39, "Pile of skulls" },
		{ 40, "Skull on a stick 1" }, { 41, "Skull on a stick 2" }, { 42, "Skull on a stick 3" }, { 43, "Skull on a stick 4" }, { 44, "Skull on a stick 5" },
		{ 6, "Banner (left)" }, { 7, "Banner (middle)" }, { 8, "Banner (right)" },
		{ 2, "Crucified skeleton 1" }, { 3, "Crucified skeleton 2" }, { 4, "Crucified skeleton 3" }, { 5, "Angel statue" },
		{ 55, "Tortured body 1" }, { 56, "Tortured body 2" }, { 57, "Tortured body 3" }, { 58, "Tortured body 4" }, { 59, "Tortured body 5" },
		{ 70, "Impaled body 1" }, { 71, "Impaled body 2" }, { 72, "Impaled body 3" }, { 73, "Impaled body 4" },
		{ 74, "Impaled body 5" }, { 75, "Impaled body 6" }, { 76, "Impaled body 7" }, { 65, "Hanging body" }, { 132, "Decapitated body" },
		{ 14, "Book (left)" }, { 15, "Book (right)" }, { 137, "Book stand" }, { 143, "Pile of books" },
		{ 120, "Bookshelf" }, { 138, "Bookshelf (right)" }, { 121, "Bookcase (left)" }, { 122, "Bookcase (right)" },
		{ 123, "Armour stand" }, { 124, "Weapon rack" }, { 21, "Cauldron" },
		{ 125, "Blood fountain" }, { 126, "Purifying spring" }, { 130, "Murky pool" }, { 131, "Fountain of tears" },
		{ 127, "Shrine (left)" }, { 128, "Shrine (right)" }, { 129, "Goat shrine" },
		{ 117, "Sarcophagus" }, { 146, "Sarcophagus (crypt)" },
		{ 118, "Barrel" }, { 119, "Explosive barrel" }, { 139, "Pod" }, { 140, "Explosive pod" }, { 141, "Urn" }, { 142, "Explosive urn" },
	};
	static std::vector<PropKind> fitting;
	static int builtFor = -1;
	if (builtFor == static_cast<int>(S.map.style))
		return fitting;
	builtFor = static_cast<int>(S.map.style);
	fitting.clear();
	for (const PropKind &prop : all) {
		const _object_id type = ObjTypeConv[prop.id];
		if (type == OBJ_NULL)
			continue;
		const dungeon_type home = AllObjects[type].olvltype;
		if (home == DTYPE_NONE || home == S.map.style || (S.map.style == DTYPE_FOREST && home == DTYPE_CATHEDRAL))
			fitting.push_back(prop);
	}
	return fitting;
}

/** The map's object id on a world tile, nullptr outside the map. */
uint16_t *MapObjectAt(Point tile)
{
	const int cols = 2 * S.map.width, rows = 2 * S.map.height;
	const Point local = tile - Displacement { 16, 16 };
	if (local.x < 0 || local.y < 0 || local.x >= cols || local.y >= rows)
		return nullptr;
	if (S.map.objects.size() != static_cast<size_t>(cols) * rows)
		S.map.objects.assign(static_cast<size_t>(cols) * rows, 0);
	return &S.map.objects[static_cast<size_t>(local.y) * cols + local.x];
}

/** Ground brushes of the Paint tool: town brushes, or in dungeons the one "auto" brush. */
int BrushCount()
{
	return S.kind == Kind::Dungeon ? 1 : static_cast<int>(GroundBrushes().size());
}

/** Rows of the palette panel for the current tool. */
std::vector<std::string> PaletteRows()
{
	std::vector<std::string> rows;
	switch (S.tool) {
	case Tool::Paint:
		// Ground brushes (a mix of variants), then any single tile from the picker.
		if (S.kind == Kind::Dungeon) {
			rows.emplace_back("Auto tiles (walls follow)");
			rows.push_back(S.paintKind != PaintKind::Ground || S.brush < 0 ? StrCat(PaintingLabel(), "  (T)") : std::string("Tiles and decor  (T)"));
			break;
		}
		for (const GroundBrush &b : GroundBrushes())
			rows.emplace_back(b.name);
		rows.push_back(S.paintKind != PaintKind::Ground || S.brush < 0 ? StrCat(PaintingLabel(), "  (T)") : std::string("Tiles, decor, buildings  (T)"));
		break;
	case Tool::Stamp:
		for (const Building &b : Buildings())
			rows.emplace_back(b.name);
		if (S.clip.width > 0)
			rows.push_back(StrCat("Copied area ", S.clip.width, "x", S.clip.height));
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
	case Tool::Fence:
		rows.push_back("Wooden fence");
		rows.push_back("Gate (people pass, animals don't)");
		rows.push_back("Remove fencing");
		break;
	case Tool::Animal:
		for (const AnimalKindInfo &a : AnimalKinds())
			rows.emplace_back(a.label);
		break;
	case Tool::Prop:
		for (const PropKind &prop : PropsForMap())
			rows.emplace_back(prop.label);
		break;
	default:
		break;
	}
	return rows;
}

std::string PaletteTitle()
{
	switch (S.tool) {
	case Tool::Paint: {
		const std::string hue = S.hueShift != 0 && S.paintKind != PaintKind::Building ? StrCat("  hue ", S.hueShift > 0 ? "+" : "", S.hueShift * TileHueStepDegrees) : std::string {};
		return StrCat(PaintingLabel(), hue);
	}
	case Tool::Stamp: return "Buildings";
	case Tool::Npc: return "Townspeople";
	case Tool::Spawner: return StrCat("Pack of ", S.packSize, "  (click +/right -)");
	case Tool::Boss: return "Bosses (with minions)";
	case Tool::Fence: return "Wooden fencing";
	case Tool::Animal: return StrCat("Animals (", S.map.animals.size(), " placed)");
	case Tool::Prop: return "Props";
	default: return "";
	}
}

int PaletteSelected()
{
	switch (S.tool) {
	case Tool::Paint:
		return S.paintKind != PaintKind::Ground || S.brush < 0 ? BrushCount() : S.brush;
	case Tool::Stamp: return S.stamp < 0 ? static_cast<int>(Buildings().size()) : S.stamp;
	case Tool::Npc: return S.npc;
	case Tool::Spawner: return S.monster;
	case Tool::Boss: return S.boss;
	case Tool::Fence: return S.fenceKind;
	case Tool::Animal: return S.animal;
	case Tool::Prop: return S.prop;
	default: return -1;
	}
}

void SelectPalette(int row)
{
	switch (S.tool) {
	case Tool::Paint:
		if (row < BrushCount()) {
			S.brush = row;
			S.paintKind = PaintKind::Ground;
			S.layer = 0;
			S.showTilePicker = false;
		} else {
			S.showTilePicker = true;
		}
		break;
	case Tool::Stamp: S.stamp = row >= static_cast<int>(Buildings().size()) ? -1 : row; break;
	case Tool::Npc: S.npc = row; break;
	case Tool::Spawner: S.monster = row; break;
	case Tool::Boss: S.boss = row; break;
	case Tool::Fence: S.fenceKind = std::clamp(row, 0, 2); break;
	case Tool::Animal: S.animal = row; break;
	case Tool::Prop: S.prop = row; break;
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

// --- Tile picker layout ------------------------------------------------------------------------

enum class PickerTab : uint8_t {
	Ground,
	Decor,
	Buildings,
	All,
};
constexpr std::array<std::string_view, 4> PickerTabNames { "Ground", "Decor", "Buildings", "All tiles" };
/** Dungeons: floors, decor laid over them, walls and other standing tiles, everything. */
constexpr std::array<std::string_view, 4> DungeonPickerTabNames { "Floors", "Decor", "Walls", "All tiles" };
constexpr std::array<std::string_view, 4> LayerNames { "Ground", "Decor 1", "Decor 2", "Decor 3" };
constexpr std::array<std::string_view, 4> PickerButtonLabels { "Hue -  (F)", "Hue +  (G)", "Hue off", "Close (T)" };
constexpr std::array<std::string_view, 4> PickerButtonShortLabels { "Hue -", "Hue +", "Reset", "Close" };
constexpr int PickerButtonRows = 3; // tabs, layers, hue
constexpr int PickerHeader = 20 + PickerButtonRows * 21 + 4;
constexpr int PickerGap = 4;
constexpr int PickerScrollBar = 6;

/** One picture in the picker. */
struct PickerItem {
	enum class Kind : uint8_t {
		Tile,        // a megatile laid as ground
		Decor,       // a decor id laid on a decor layer (0 erases)
		Building,    // index in Buildings()
		NewBuilding, // the building maker
	};
	Kind kind;
	int id;
	bool operator==(const PickerItem &other) const = default;
};

/** What a town megatile is: flat ground, something standing on the ground, or a piece of a building. */
enum class TileClass : uint8_t {
	Ground,
	Decor,
	Building,
};

const std::vector<TileClass> &TileClasses()
{
	static std::vector<TileClass> classes;
	static const MegaTile *builtFor = nullptr;
	static bool builtForDungeon = false;
	const bool dungeonTiles = S.kind == Kind::Dungeon;
	if (builtFor == pMegaTiles.get() && classes.size() == MegaTileCount && builtForDungeon == dungeonTiles)
		return classes;
	builtFor = pMegaTiles.get();
	builtForDungeon = dungeonTiles;
	classes.assign(MegaTileCount, TileClass::Ground);
	if (pMegaTiles == nullptr)
		return classes;
	// A building piece is one classic Tristram only uses inside its buildings.
	std::vector<uint8_t> inBuilding(MegaTileCount + 1, 0), elsewhere(MegaTileCount + 1, 0);
	if (!InDesertRegion() && !dungeonTiles) {
		const std::vector<uint16_t> &classic = ClassicTownMegaTiles();
		for (const Building &b : Buildings()) {
			if (!b.builtIn || IsSceneryStamp(b.name))
				continue;
			for (uint16_t t : b.tiles) {
				if (t >= 1 && t <= MegaTileCount)
					inBuilding[t] = 1;
			}
		}
		// Tiles also found outside every building in classic Tristram are scenery, not building parts.
		std::vector<bool> inside(48 * 48, false);
		for (const Building &b : Buildings()) {
			if (!b.builtIn || IsSceneryStamp(b.name))
				continue;
			// Built-in buildings are cut from the classic grid; find where (their first row matches).
			for (int oy = 0; oy + b.height <= 48; oy++) {
				for (int ox = 0; ox + b.width <= 48; ox++) {
					bool match = true;
					for (int y = 0; match && y < b.height; y++) {
						for (int x = 0; match && x < b.width; x++)
							match = classic[static_cast<size_t>(oy + y) * 48 + ox + x] == b.tiles[static_cast<size_t>(y) * b.width + x];
					}
					if (!match)
						continue;
					for (int y = 0; y < b.height; y++) {
						for (int x = 0; x < b.width; x++)
							inside[static_cast<size_t>(oy + y) * 48 + ox + x] = true;
					}
				}
			}
		}
		for (size_t i = 0; i < classic.size(); i++) {
			if (!inside[i] && classic[i] >= 1 && classic[i] <= MegaTileCount)
				elsewhere[classic[i]] = 1;
		}
	}
	for (size_t i = 0; i < MegaTileCount; i++) {
		const MegaTile mega = pMegaTiles[i];
		const std::array<uint16_t, 4> pieces { Swap16LE(mega.micro1), Swap16LE(mega.micro2), Swap16LE(mega.micro3), Swap16LE(mega.micro4) };
		// Decor rises off the ground (walls, rocks, trees, signs); low plants and water lie flat and count as ground.
		const bool standing = std::any_of(pieces.begin(), pieces.end(), [](uint16_t piece) {
			if (piece >= MAXTILES)
				return false;
			if (HasAnyOf(SOLData[piece], TileProperties::Solid | TileProperties::BlockMissile))
				return true;
			for (int b = 2; b < static_cast<int>(MicroTileLen); b++) {
				if (LevelCelBlock { DPieceMicros[piece].mt[b] }.hasValue())
					return true;
			}
			return false;
		});
		if (!standing)
			classes[i] = TileClass::Ground;
		else
			classes[i] = inBuilding[i + 1] != 0 && elsewhere[i + 1] == 0 ? TileClass::Building : TileClass::Decor;
	}
	return classes;
}

bool PickerShown()
{
	return S.editing && S.showTilePicker && S.tool == Tool::Paint;
}

PickerTab CurrentTab()
{
	return static_cast<PickerTab>(std::clamp(S.pickerTab, 0, 3));
}

/** The pictures of the current tab. */
const std::vector<PickerItem> &PickerItems()
{
	static std::vector<PickerItem> items;
	static int builtTab = -1;
	static size_t builtTiles = 0, builtBuildings = 0;
	static const MegaTile *builtFor = nullptr;
	static Kind builtKind = Kind::Tristram;
	const bool dungeonTiles = S.kind == Kind::Dungeon;
	// Dungeon tiles are kept in a byte per piece: the tileset's first 255 megatiles.
	const size_t tileCount = pMegaTiles == nullptr ? 0 : (dungeonTiles ? std::min<size_t>(MegaTileCount, 255) : MegaTileCount);
	if (builtTab == S.pickerTab && builtTiles == tileCount && builtBuildings == Buildings().size() && builtFor == pMegaTiles.get() && builtKind == S.kind)
		return items;
	builtKind = S.kind;
	builtTab = S.pickerTab;
	builtTiles = tileCount;
	builtBuildings = Buildings().size();
	builtFor = pMegaTiles.get();
	items.clear();
	const std::vector<TileClass> &classes = TileClasses();
	if (dungeonTiles) {
		// Floors lie flat; walls, pillars and rubble stand. Decor lays standing tiles over the floor.
		const PickerTab tab = CurrentTab();
		if (tab == PickerTab::Decor)
			items.push_back({ PickerItem::Kind::Decor, 0 });
		for (size_t i = 0; i < tileCount; i++) {
			const bool standing = classes[i] != TileClass::Ground;
			const int id = static_cast<int>(i + 1);
			if (tab == PickerTab::All || (tab == PickerTab::Ground && !standing) || (tab == PickerTab::Buildings && standing))
				items.push_back({ PickerItem::Kind::Tile, id });
			else if (tab == PickerTab::Decor && standing)
				items.push_back({ PickerItem::Kind::Decor, id });
		}
		return items;
	}
	switch (CurrentTab()) {
	case PickerTab::Ground:
		for (size_t i = 0; i < tileCount; i++) {
			if (classes[i] == TileClass::Ground)
				items.push_back({ PickerItem::Kind::Tile, static_cast<int>(i + 1) });
		}
		break;
	case PickerTab::Decor:
		items.push_back({ PickerItem::Kind::Decor, 0 });
		for (int f = 0; f < FlowerPatchCount; f++)
			items.push_back({ PickerItem::Kind::Decor, DecorFlowerBase + f });
		for (size_t i = 0; i < tileCount; i++) {
			if (classes[i] == TileClass::Decor)
				items.push_back({ PickerItem::Kind::Decor, static_cast<int>(i + 1) });
		}
		break;
	case PickerTab::Buildings:
		items.push_back({ PickerItem::Kind::NewBuilding, 0 });
		for (size_t i = 0; i < Buildings().size(); i++)
			items.push_back({ PickerItem::Kind::Building, static_cast<int>(i) });
		break;
	case PickerTab::All:
		for (size_t i = 0; i < tileCount; i++)
			items.push_back({ PickerItem::Kind::Tile, static_cast<int>(i + 1) });
		break;
	}
	return items;
}

/** The picker item matching what the Paint tool lays now. */
PickerItem SelectedPickerItem()
{
	switch (S.paintKind) {
	case PaintKind::Decor: return { PickerItem::Kind::Decor, S.pickerDecor };
	case PaintKind::Building: return { PickerItem::Kind::Building, S.pickerBuilding };
	default: return { PickerItem::Kind::Tile, S.brush < 0 ? S.pickerTile : -1 };
	}
}

int PickerCell()
{
	return S.pickerZoom <= 1 ? 52 : (S.pickerZoom == 2 ? 72 : 100);
}

int PickerColumns()
{
	const int room = std::max(PaletteWidth * 2, gnScreenWidth * 2 / 5);
	return std::clamp((room - PickerScrollBar - PickerGap) / (PickerCell() + PickerGap), 2, 8);
}

Rectangle PickerRect()
{
	const int w = std::max(300, PickerColumns() * (PickerCell() + PickerGap) + PickerGap + PickerScrollBar + 4);
	const int top = TopBarHeight + 4;
	return { { gnScreenWidth - w - 4, top }, { w, std::max(200, gnViewportHeight - 22 - top) } };
}

/** Short screens skip the large picture (the preview on the map still shows the tile in place). */
bool PickerHasPreview()
{
	return PickerRect().size.height >= 430;
}

/** The large picture of the hovered (or chosen) item at the bottom of the picker. */
Rectangle PickerPreviewRect()
{
	const Rectangle r = PickerRect();
	const int h = std::clamp(r.size.height / 3, 80, 190);
	return { { r.position.x + 4, r.position.y + r.size.height - h - 4 }, { r.size.width - 8, h } };
}

Rectangle PickerGridRect()
{
	const Rectangle r = PickerRect();
	const int top = r.position.y + PickerHeader;
	const int bottom = PickerHasPreview() ? PickerPreviewRect().position.y - 4 : r.position.y + r.size.height - 4;
	return { { r.position.x + PickerGap, top }, { r.size.width - PickerGap - PickerScrollBar - 4, bottom - top } };
}

/** A button in one of the header rows: 0 tabs, 1 layers, 2 hue and close. */
Rectangle PickerRowButton(int row, int index)
{
	const Rectangle r = PickerRect();
	constexpr int N = 4;
	const int w = (r.size.width - 8 - (N - 1) * 3) / N;
	return { { r.position.x + 4 + index * (w + 3), r.position.y + 20 + row * 21 }, { w, 18 } };
}

int PickerItemCount()
{
	return static_cast<int>(PickerItems().size());
}

int PickerRowCount()
{
	return (PickerItemCount() + PickerColumns() - 1) / PickerColumns();
}

int PickerVisibleRows()
{
	return std::max(1, PickerGridRect().size.height / (PickerCell() + PickerGap));
}

void ScrollPicker(int rows)
{
	S.pickerScroll = std::clamp(S.pickerScroll + rows, 0, std::max(0, PickerRowCount() - PickerVisibleRows()));
}

int PickerIndexOf(const PickerItem &item)
{
	const std::vector<PickerItem> &items = PickerItems();
	const auto it = std::find(items.begin(), items.end(), item);
	return it == items.end() ? -1 : static_cast<int>(it - items.begin());
}

/** Scrolls so the chosen item is on screen. */
void ShowPickerSelection()
{
	const int index = PickerIndexOf(SelectedPickerItem());
	if (index >= 0) {
		const int row = index / PickerColumns();
		if (row < S.pickerScroll)
			ScrollPicker(row - S.pickerScroll);
		else if (row >= S.pickerScroll + PickerVisibleRows())
			ScrollPicker(row - S.pickerScroll - PickerVisibleRows() + 1);
	}
	ScrollPicker(0);
}

/** Screen cell of an item (index in the tab), whether or not it is scrolled into view. */
Rectangle PickerCellRect(int index)
{
	const Rectangle grid = PickerGridRect();
	const int row = index / PickerColumns() - S.pickerScroll;
	const int col = index % PickerColumns();
	return { { grid.position.x + col * (PickerCell() + PickerGap), grid.position.y + row * (PickerCell() + PickerGap) }, { PickerCell(), PickerCell() } };
}

/** Item index under a screen point in the picker's grid, -1 if none. */
int PickerItemAt(Point p)
{
	const Rectangle grid = PickerGridRect();
	if (!grid.contains(p))
		return -1;
	const int stride = PickerCell() + PickerGap;
	const int col = (p.x - grid.position.x) / stride;
	const int row = (p.y - grid.position.y) / stride;
	if (col >= PickerColumns() || row >= PickerVisibleRows() || (p.x - grid.position.x) % stride >= PickerCell() || (p.y - grid.position.y) % stride >= PickerCell())
		return -1;
	const int index = (row + S.pickerScroll) * PickerColumns() + col;
	return index < PickerItemCount() ? index : -1;
}

void SetPickerTab(PickerTab tab)
{
	if (S.pickerTab == static_cast<int>(tab))
		return;
	S.pickerTab = static_cast<int>(tab);
	S.pickerScroll = 0;
	ShowPickerSelection();
}

/** The tab showing what is being painted. */
PickerTab TabForPaint()
{
	switch (S.paintKind) {
	case PaintKind::Decor: return PickerTab::Decor;
	case PaintKind::Building: return PickerTab::Buildings;
	default:
		if (S.kind == Kind::Dungeon && CurrentTab() == PickerTab::Buildings)
			return PickerTab::Buildings; // walls
		return CurrentTab() == PickerTab::All ? PickerTab::All : PickerTab::Ground;
	}
}

void OpenTilePicker()
{
	if (S.paintKind == PaintKind::Ground)
		S.brush = -1;
	S.showTilePicker = true;
	S.pickerTile = static_cast<uint16_t>(std::clamp<int>(S.pickerTile, 1, std::max<int>(1, static_cast<int>(MegaTileCount))));
	S.pickerTab = static_cast<int>(TabForPaint());
	ShowPickerSelection();
}

void ShiftHue(int steps)
{
	int hue = S.hueShift + steps;
	// Round the colour wheel: past half a turn one way is the same as coming from the other side.
	if (hue > MaxTileHueSteps)
		hue -= 2 * MaxTileHueSteps;
	if (hue < -MaxTileHueSteps)
		hue += 2 * MaxTileHueSteps;
	S.hueShift = hue;
}

/** Makes a layer the one painted on: ground takes tiles, decor layers take decor. */
void SelectLayer(int layer)
{
	S.layer = std::clamp(layer, 0, MapDecorLayerCount);
	if (S.layer == 0 && S.paintKind == PaintKind::Decor) {
		S.paintKind = PaintKind::Ground;
	} else if (S.layer > 0 && S.paintKind == PaintKind::Ground) {
		S.paintKind = PaintKind::Decor;
	}
	if (S.showTilePicker && S.paintKind != PaintKind::Building)
		SetPickerTab(TabForPaint());
	Say(StrCat("Painting on ", LayerNames[S.layer], ".", S.layer > 0 && (S.hiddenLayers & (1u << (S.layer - 1))) != 0 ? " (hidden: right-click its button to show it)" : ""));
}

void ToggleLayerHidden(int layer)
{
	if (layer < 1 || layer > MapDecorLayerCount)
		return;
	S.hiddenLayers ^= 1u << (layer - 1);
	Rebuild();
	Say(StrCat(LayerNames[layer], (S.hiddenLayers & (1u << (layer - 1))) != 0 ? " hidden while editing (it is still saved and shows in play)." : " shown."));
}

std::string DecorName(uint16_t id)
{
	if (id == 0)
		return "Erase decor";
	if (IsFlowerDecor(id))
		return std::string(FlowerPatchName(id - DecorFlowerBase));
	return StrCat("Decor #", id);
}

/** What the Paint tool lays, for titles. */
std::string PaintingLabel()
{
	switch (S.paintKind) {
	case PaintKind::Decor: return StrCat(LayerNames[std::max(1, S.layer)], ": ", DecorName(S.pickerDecor));
	case PaintKind::Building: {
		const std::vector<Building> &all = Buildings();
		return S.pickerBuilding >= 0 && S.pickerBuilding < static_cast<int>(all.size()) ? StrCat("Building: ", all[S.pickerBuilding].name) : std::string("Building");
	}
	default:
		if (S.kind == Kind::Dungeon && S.brush >= 0)
			return "Auto tiles";
		if (S.brush >= 0 && S.brush < static_cast<int>(GroundBrushes().size()))
			return std::string(GroundBrushes()[S.brush].name);
		return StrCat("Tile #", S.pickerTile);
	}
}

constexpr int DialogWidth = 400;
constexpr int DialogRow = 20;

/** A row of the link dialog. */
struct LinkRow {
	enum class Kind : uint8_t {
		Tristram,
		Dungeon,
		Map,      // index in linkTargets
		NewWilds,
		Nowhere,
		Arrival,  // page 1: index in the target's arrival points, -1 for Tristram's usual spot
	};
	Kind kind;
	int index;
	std::string label;
	std::string note; // right-aligned, dimmer
};

std::vector<std::string> TristramEntries();

std::vector<LinkRow> LinkRows()
{
	std::vector<LinkRow> rows;
	if (S.linkPage == 1) {
		if (S.linkTarget == -2) {
			rows.push_back({ LinkRow::Kind::Arrival, -1, "Tristram's usual arrival spot", "" });
			const std::vector<std::string> entries = TristramEntries();
			for (size_t i = 0; i < entries.size(); i++)
				rows.push_back({ LinkRow::Kind::Arrival, static_cast<int>(i), entries[i], "arrival point" });
		} else if (S.linkTarget >= 0 && S.linkTarget < static_cast<int>(S.linkTargets.size())) {
			const Session::LinkTarget &target = S.linkTargets[S.linkTarget];
			for (size_t i = 0; i < target.entries.size(); i++)
				rows.push_back({ LinkRow::Kind::Arrival, static_cast<int>(i), target.entries[i], i == 0 ? "first arrival point" : "arrival point" });
		}
		return rows;
	}
	rows.push_back({ LinkRow::Kind::Tristram, 0, "Tristram", "town" });
	const MapLink &l = S.map.portals[S.linkPortal].to;
	rows.push_back({ LinkRow::Kind::Dungeon, 0, StrCat("Dungeon level ", l.kind == MapLinkKind::Dungeon ? l.level : 1), "" });
	for (size_t i = 0; i < S.linkTargets.size(); i++)
		rows.push_back({ LinkRow::Kind::Map, static_cast<int>(i), S.linkTargets[i].name, S.linkTargets[i].style });
	rows.push_back({ LinkRow::Kind::NewWilds, 0, "New wilds area", "made now, with a way back" });
	rows.push_back({ LinkRow::Kind::Nowhere, 0, "Nowhere", "just decoration" });
	return rows;
}

int DialogVisibleRows()
{
	return std::max(4, (gnViewportHeight - TopBarHeight - 150) / DialogRow);
}

Rectangle DialogRect()
{
	const int shown = std::min(static_cast<int>(LinkRows().size()), DialogVisibleRows());
	const int h = 50 + shown * DialogRow + 34;
	return { { (gnScreenWidth - DialogWidth) / 2, std::max(TopBarHeight + 10, (gnViewportHeight - h) / 2) }, { DialogWidth, h } };
}

/** Screen row of a list row, if scrolled into view. */
std::optional<Rectangle> DialogRowRect(int row)
{
	const int shown = row - S.linkScroll;
	if (shown < 0 || shown >= DialogVisibleRows())
		return std::nullopt;
	const Rectangle d = DialogRect();
	return Rectangle { d.position + Displacement { 10, 48 + shown * DialogRow }, { DialogWidth - 20, DialogRow - 2 } };
}

/** The dungeon row's level buttons. */
Rectangle DialogLevelButton(Rectangle row, bool up)
{
	return { { row.position.x + row.size.width - (up ? 26 : 54), row.position.y + 1 }, { 24, row.size.height - 2 } };
}

/** Bottom button: Cancel (or Close) on the first page, Back on the second. */
Rectangle DialogFooterButton()
{
	const Rectangle d = DialogRect();
	return { { d.position.x + d.size.width / 2 - 50, d.position.y + d.size.height - 28 }, { 100, 20 } };
}

void ScrollDialog(int rows)
{
	S.linkScroll = std::clamp(S.linkScroll + rows, 0, std::max(0, static_cast<int>(LinkRows().size()) - DialogVisibleRows()));
}

void OpenLinkDialog(int portal, bool isNew = false)
{
	S.linkPortal = portal;
	S.linkIsNew = isNew;
	S.linkPage = 0;
	S.linkTarget = -1;
	S.linkScroll = 0;
	S.mapList.clear();
	S.linkTargets.clear();
	for (std::string &name : ListCustomMaps()) {
		if (S.kind != Kind::Tristram && name == S.map.name && S.kind != Kind::Dungeon)
			continue;
		Session::LinkTarget target { name, "", {} };
		if (name == S.map.name) {
			target.style = std::string(MapStyleName(S.map.style));
			for (const MapEntryPoint &e : S.map.entries)
				target.entries.push_back(e.name);
		} else if (std::expected<CustomMap, std::string> other = ReadCustomMap(name); other.has_value()) {
			target.style = std::string(MapStyleName(other->style));
			for (const MapEntryPoint &e : other->entries)
				target.entries.push_back(e.name);
		}
		S.mapList.push_back(name);
		S.linkTargets.push_back(std::move(target));
	}
}

/** Closes the link dialog; cancelling a just-placed door or portal takes it away again. */
void CloseLinkDialog(bool cancelled)
{
	if (cancelled && S.linkIsNew && S.linkPortal >= 0 && S.linkPortal < static_cast<int>(S.map.portals.size())) {
		S.map.portals.erase(S.map.portals.begin() + S.linkPortal);
		Say("Cancelled: nothing placed.");
	}
	S.linkPortal = -1;
	S.linkIsNew = false;
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
	const bool placingBuilding = S.tool == Tool::Paint && S.paintKind == PaintKind::Building && !S.capturing;
	if (IsAnyOf(S.tool, Tool::Room, Tool::Erase, Tool::Paint, Tool::Copy) && InMap(hover) && !placingBuilding) {
		const Point from = S.dragging ? S.dragStart : hover;
		uint8_t color = PAL16_YELLOW + 2;
		if (S.tool == Tool::Erase || (S.tool == Tool::Room && S.dragging && S.dragErase) || (S.tool == Tool::Paint && S.paintKind == PaintKind::Decor && S.pickerDecor == 0))
			color = PAL8_RED;
		else if (S.tool == Tool::Copy || (S.tool == Tool::Paint && S.capturing))
			color = PAL16_BLUE + 2;
		PieceOutline(view, pr, { std::min(from.x, hover.x), std::min(from.y, hover.y) }, { std::max(from.x, hover.x), std::max(from.y, hover.y) }, color);
	}
	if ((S.tool == Tool::Stamp || placingBuilding) && InMap(hover)) {
		const Size size = StampSize();
		if (size.width > 0) {
			const Point o = StampOrigin(hover);
			PieceOutline(view, pr, o, o + Displacement { size.width - 1, size.height - 1 }, PAL16_YELLOW + 2);
		}
	}
	if (!S.map.outer.empty() && S.kind != Kind::Tristram) {
		// The edge that leads to the outer wilds (or back to the town).
		const float x0 = 16, y0 = 16, x1 = 16.0f + 2 * S.map.width, y1 = 16.0f + 2 * S.map.height;
		std::array<float, 4> e {};
		switch (S.map.outerSide) {
		case 0: e = { x0, y0, x1, y0 }; break;
		case 1: e = { x1, y0, x1, y1 }; break;
		case 2: e = { x0, y1, x1, y1 }; break;
		default: e = { x0, y0, x0, y1 }; break;
		}
		const std::optional<Point> a = Screen(pr, e[0], e[1]), b = Screen(pr, e[2], e[3]);
		if (a && b) {
			for (int t = -1; t <= 1; t++)
				Line(view, *a + Displacement { 0, t }, *b + Displacement { 0, t }, PAL16_BLUE + 2);
			// Label the part of the edge nearest the middle of the view, kept on screen.
			const float dx = static_cast<float>(b->x - a->x), dy = static_cast<float>(b->y - a->y);
			const float len = std::max(1.0f, dx * dx + dy * dy);
			const float t = std::clamp(((view.w() / 2 - a->x) * dx + (view.h() / 2 - a->y) * dy) / len, 0.0f, 1.0f);
			const std::string label = StrCat(S.kind == Kind::Town ? "Walk out to " : "Walk back to ", S.map.outer);
			const int w = static_cast<int>(GetLineWidth(label)) + 10;
			const Point near { a->x + static_cast<int>(t * dx), a->y + static_cast<int>(t * dy) };
			const bool seen = near.x >= 0 && near.y >= 0 && near.x < view.w() && near.y < view.h();
			const int lx = std::clamp(a->x + static_cast<int>(t * dx) - w / 2, 2 * ToolWidth + 16, std::max(2 * ToolWidth + 16, view.w() - PaletteWidth - w - 8));
			const int ly = std::clamp(a->y + static_cast<int>(t * dy) - 20, TopBarHeight + 4, std::max(TopBarHeight + 4, view.h() - 40));
			if (seen) {
				DrawHalfTransparentRectTo(view, lx, ly, w, 15);
				DrawString(view, label, { { lx + 5, ly + 1 }, { w - 10, 12 } }, { .flags = UiFlags::ColorBlue });
			}
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
	if (S.tool == Tool::Animal) {
		for (const MapAnimal &a : S.map.animals)
			Marker(view, pr, a.tile, PAL16_GRAY + 2, AnimalLabel(a.kind), UiFlags::ColorWhite);
	}
	if (S.tool == Tool::Fence && CanPlaceAt(cursPosition) && (S.dragging ? S.dragErase : S.fenceKind == 2)) {
		// What would be removed, outlined in red.
		for (const Point t : S.dragging ? FenceRun(S.dragStart, cursPosition) : std::vector<Point> { cursPosition }) {
			const std::optional<Point> p0 = Screen(pr, t.x, t.y), p1 = Screen(pr, t.x + 1.0f, t.y), p2 = Screen(pr, t.x + 1.0f, t.y + 1.0f), p3 = Screen(pr, t.x, t.y + 1.0f);
			if (p0 && p1 && p2 && p3) {
				Line(view, *p0, *p1, PAL8_RED);
				Line(view, *p1, *p2, PAL8_RED);
				Line(view, *p2, *p3, PAL8_RED);
				Line(view, *p3, *p0, PAL8_RED);
			}
		}
	}
	for (const Point chest : MapChests(S.map))
		Marker(view, pr, chest, PAL16_YELLOW + 2, StrCat("Chest ", chest.x, ",", chest.y), UiFlags::ColorGold);
}

void Frame(const Surface &out, Rectangle r, uint8_t color, int thickness);

std::string LinkDescription(const MapLink &l)
{
	switch (l.kind) {
	case MapLinkKind::Town: return StrCat("Tristram", l.entry.empty() ? "" : StrCat(", arriving at ", l.entry));
	case MapLinkKind::Map: return StrCat(l.map, l.entry.empty() ? "" : StrCat(", arriving at ", l.entry));
	case MapLinkKind::Dungeon: return StrCat("dungeon level ", l.level);
	default: return "nowhere yet";
	}
}

void DrawLinkDialog(const Surface &out)
{
	const Rectangle d = DialogRect();
	for (int i = 0; i < 3; i++)
		DrawHalfTransparentRectTo(out, d.position.x, d.position.y, d.size.width, d.size.height);
	Frame(out, d, PAL16_YELLOW + 9, 1);
	const MapPortal &portal = S.map.portals[S.linkPortal];
	const std::string what = portal.door ? "door" : "portal";
	std::string title;
	if (S.linkPage == 1)
		title = StrCat("Arrive where in ", S.linkTarget == -2 ? std::string("Tristram") : S.linkTargets[S.linkTarget].name, "?");
	else
		title = StrCat("Where does this ", what, " lead?");
	DrawString(out, title, { d.position + Displacement { 0, 8 }, { d.size.width, 12 } }, { .flags = UiFlags::ColorWhitegold | UiFlags::AlignCenter });
	DrawString(out, S.linkPage == 1 ? std::string("Choose where the hero comes out.") : StrCat("Now: ", LinkDescription(portal.to)),
	    { d.position + Displacement { 0, 26 }, { d.size.width, 12 } }, { .flags = UiFlags::ColorGold | UiFlags::AlignCenter | UiFlags::KerningFitSpacing });

	const std::vector<LinkRow> rows = LinkRows();
	const MapLink &l = portal.to;
	for (int i = 0; i < static_cast<int>(rows.size()); i++) {
		const std::optional<Rectangle> r = DialogRowRect(i);
		if (!r)
			continue;
		const LinkRow &row = rows[i];
		bool selected = false;
		switch (row.kind) {
		case LinkRow::Kind::Tristram: selected = l.kind == MapLinkKind::Town; break;
		case LinkRow::Kind::Dungeon: selected = l.kind == MapLinkKind::Dungeon; break;
		case LinkRow::Kind::Map: selected = l.kind == MapLinkKind::Map && l.map == S.linkTargets[row.index].name; break;
		case LinkRow::Kind::Nowhere: selected = l.kind == MapLinkKind::None; break;
		case LinkRow::Kind::Arrival: {
			const bool sameTarget = S.linkTarget == -2 ? l.kind == MapLinkKind::Town : (l.kind == MapLinkKind::Map && l.map == S.linkTargets[S.linkTarget].name);
			selected = sameTarget && (row.index < 0 ? l.entry.empty() : l.entry == row.label);
			break;
		}
		default: break;
		}
		if (selected || r->contains(MousePosition))
			FillRect(out, r->position.x, r->position.y, r->size.width, r->size.height, selected ? PAL16_YELLOW + 12 : PAL16_GRAY + 13);
		DrawString(out, row.label, { r->position + Displacement { 8, 3 }, { r->size.width - 16, 12 } }, { .flags = selected ? UiFlags::ColorWhitegold : UiFlags::ColorWhite });
		if (row.kind == LinkRow::Kind::Dungeon) {
			Button(out, DialogLevelButton(*r, false), "-", false);
			Button(out, DialogLevelButton(*r, true), "+", false);
		} else if (!row.note.empty()) {
			DrawString(out, row.note, { r->position + Displacement { 8, 3 }, { r->size.width - 16, 12 } }, { .flags = UiFlags::ColorGold | UiFlags::AlignRight | UiFlags::KerningFitSpacing });
		}
	}
	if (static_cast<int>(rows.size()) > DialogVisibleRows())
		DrawString(out, "(wheel: more)", { d.position + Displacement { 10, d.size.height - 24 }, { 100, 12 } }, { .flags = UiFlags::ColorGold });
	Button(out, DialogFooterButton(), S.linkPage == 1 ? "Back" : (S.linkIsNew ? "Cancel" : "Close"), false);
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

/** Pieces (0-based level pieces) of a town megatile, or nullopt for none. */
std::optional<std::array<uint16_t, 4>> MegaPieces(int tile)
{
	if (tile <= 0 || pMegaTiles == nullptr || tile > static_cast<int>(MegaTileCount))
		return std::nullopt;
	const MegaTile mega = pMegaTiles[tile - 1];
	return std::array<uint16_t, 4> { Swap16LE(mega.micro1), Swap16LE(mega.micro2), Swap16LE(mega.micro3), Swap16LE(mega.micro4) };
}

/**
 * Draws a block of map pieces the way the game draws them: floors and flowers first, then
 * everything standing, back to front. `base` is the bottom-left corner of the floor of its
 * top world tile. Used for tiles, decor and whole buildings in the picker.
 */
void RenderBuilding(const Surface &out, Point base, const Building &b)
{
	if (pDungeonCels == nullptr || pMegaTiles == nullptr)
		return;
	constexpr int HalfTileWidth = static_cast<int>(DunFrameWidth);
	using Tables = std::span<const std::array<uint8_t, LightTableSize>, NumLightingLevels>;
	auto tablesFor = [](int hue) {
		const HueLightTableSet *hueTables = GetTileHueLightTables(hue);
		return hueTables != nullptr ? Tables(*hueTables) : Tables(LightTables);
	};
	static std::vector<uint8_t> fullLight;
	fullLight.assign(out.pitch(), 0);
	auto lightmapFor = [&](Tables tables) { return Lightmap(out.at(0, 0), out.pitch(), fullLight, 0, tables, FullyLitLightTable, FullyDarkLightTable); };
	auto isFloor = [](uint16_t piece) { return !HasAnyOf(SOLData[piece], TileProperties::Solid | TileProperties::BlockMissile); };
	const int cols = 2 * b.width, rows = 2 * b.height;
	auto screen = [&](int wx, int wy) { return base + Displacement { (wx - wy) * 32, (wx + wy) * 16 }; };
	auto cellOf = [&](int wx, int wy) { return static_cast<size_t>(wy / 2) * b.width + wx / 2; };
	auto pieceOf = [&](int tile, int wx, int wy) -> int {
		const std::optional<std::array<uint16_t, 4>> pieces = MegaPieces(tile);
		if (!pieces)
			return -1;
		const uint16_t piece = (*pieces)[(wy % 2) * 2 + wx % 2];
		return piece < MAXTILES ? piece : -1;
	};
	const bool townRoofs = pSpecialCels && IsAnyOf(leveltype, DTYPE_TOWN, DTYPE_FOREST) && !InDesertRegion();

	// Floors, and flowers on them.
	for (int wy = 0; wy < rows; wy++) {
		for (int wx = 0; wx < cols; wx++) {
			const size_t i = cellOf(wx, wy);
			const Point at = screen(wx, wy);
			if (const int piece = pieceOf(b.tiles[i], wx, wy); piece >= 0 && isFloor(static_cast<uint16_t>(piece))) {
				const Tables tables = tablesFor(b.hues[i]);
				const Lightmap lightmap = lightmapFor(tables);
				for (int half = 0; half < 2; half++) {
					const LevelCelBlock block { DPieceMicros[piece].mt[half] };
					if (block.hasValue())
						RenderTileFrame(out, lightmap, at + Displacement { half * HalfTileWidth, 0 }, half == 0 ? TileType::LeftTriangle : TileType::RightTriangle,
						    GetDunFrame(pDungeonCels.get(), block.frame()), DunFrameTriangleHeight, MaskType::Solid, tables[0].data());
				}
			}
			for (int layer = 0; layer < MapDecorLayerCount; layer++) {
				const uint16_t id = b.decor[layer][i];
				if (IsFlowerDecor(id))
					DrawFlowerPatch(out, at, id - DecorFlowerBase, static_cast<uint32_t>(wx) * 73856093u ^ static_cast<uint32_t>(wy) * 19349663u ^ static_cast<uint32_t>(layer) * 83492791u, tablesFor(b.decorHues[layer][i])[0].data());
			}
		}
	}
	// Everything standing, back to front.
	for (int sum = 0; sum <= cols + rows - 2; sum++) {
		for (int wx = std::max(0, sum - rows + 1); wx <= std::min(cols - 1, sum); wx++) {
			const int wy = sum - wx;
			const size_t i = cellOf(wx, wy);
			const Point at = screen(wx, wy);
			const int piece = pieceOf(b.tiles[i], wx, wy);
			if (piece >= 0) {
				const Tables tables = tablesFor(b.hues[i]);
				DrawDecorPiece(out, lightmapFor(tables), at, static_cast<uint16_t>(piece), tables[0].data());
			}
			for (int layer = 0; layer < MapDecorLayerCount; layer++) {
				const uint16_t id = b.decor[layer][i];
				if (id == 0 || IsFlowerDecor(id))
					continue;
				const int decorPiece = pieceOf(id, wx, wy);
				if (decorPiece < 0 || !PieceHasStandingParts(static_cast<uint16_t>(decorPiece)))
					continue;
				const Tables tables = tablesFor(b.decorHues[layer][i]);
				DrawDecorPiece(out, lightmapFor(tables), at, static_cast<uint16_t>(decorPiece), tables[0].data());
			}
			if (b.fences.size() == static_cast<size_t>(cols) * rows) {
				auto fenceAt = [&](int x, int y) { return x >= 0 && y >= 0 && x < cols && y < rows && b.fences[static_cast<size_t>(y) * cols + x] != 0; };
				const uint8_t fence = b.fences[static_cast<size_t>(wy) * cols + wx];
				if (fence != 0)
					DrawFence(out, at, fence, { fenceAt(wx + 1, wy), fenceAt(wx - 1, wy), fenceAt(wx, wy + 1), fenceAt(wx, wy - 1) }, LightTables[0].data());
			}
			if (townRoofs && piece >= 0) {
				const uint8_t special = TownSpecialForPiece(static_cast<uint16_t>(piece));
				if (special != 0 && special <= pSpecialCels->numSprites()) {
					if (b.hues[i] != 0)
						ClxDrawTRN(out, at, (*pSpecialCels)[special - 1], tablesFor(b.hues[i])[0].data());
					else
						ClxDraw(out, at, (*pSpecialCels)[special - 1]);
				}
			}
		}
	}
}

/** A scaled picture, kept so the picker does not redraw every tile each frame. */
struct Picture {
	int width = 0;
	int height = 0;
	std::vector<uint8_t> pixels; // 0 = see-through
};

std::unordered_map<std::string, Picture> PictureCache;
std::array<SDL_Color, 256> PictureCachePalette {};

/** Forgets cached pictures when the palette changed or there are too many. */
void RefreshPictureCache()
{
	bool same = true;
	for (size_t i = 0; same && i < logical_palette.size(); i++)
		same = logical_palette[i].r == PictureCachePalette[i].r && logical_palette[i].g == PictureCachePalette[i].g && logical_palette[i].b == PictureCachePalette[i].b;
	if (!same || PictureCache.size() > 1500) {
		PictureCache.clear();
		PictureCachePalette = logical_palette;
	}
}

/** Draws a block of pieces scaled into a box. A single map piece is drawn at the same scale as every other single piece. */
void DrawBuildingPicture(const Surface &out, Rectangle box, const Building &b, const std::string &key)
{
	if (box.size.width <= 0 || box.size.height <= 0 || b.width <= 0)
		return;
	const std::string fullKey = StrCat(key, "@", box.size.width, "x", box.size.height);
	auto it = PictureCache.find(fullKey);
	if (it == PictureCache.end()) {
		// Full size first, into a scratch surface big enough for the whole block and its tallest pieces.
		const int cols = 2 * b.width, rows = 2 * b.height;
		const int tall = static_cast<int>(MicroTileLen / 2) * TILE_HEIGHT + 48;
		OwnedSurface scratch((cols + rows) * 32, (cols + rows - 2) * 16 + tall + 8);
		for (int y = 0; y < scratch.h(); y++)
			std::memset(scratch.at(0, y), 0, scratch.w());
		RenderBuilding(scratch, { (rows - 1) * 32, tall }, b);
		int x0 = scratch.w(), y0 = scratch.h(), x1 = -1, y1 = -1;
		for (int y = 0; y < scratch.h(); y++) {
			const uint8_t *row = scratch.at(0, y);
			for (int x = 0; x < scratch.w(); x++) {
				if (row[x] != 0) {
					x0 = std::min(x0, x), x1 = std::max(x1, x);
					y0 = std::min(y0, y), y1 = std::max(y1, y);
				}
			}
		}
		Picture picture;
		if (x1 >= 0) {
			const int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
			float scale = std::min(static_cast<float>(box.size.width) / bw, static_cast<float>(box.size.height) / bh);
			if (b.width == 1 && b.height == 1)
				scale = std::min(scale, static_cast<float>(box.size.width) / (2 * TILE_WIDTH)); // flat ground fills the width; tall pieces shrink to fit
			scale = std::min(scale, 1.0f);
			picture.width = std::max(1, static_cast<int>(bw * scale));
			picture.height = std::max(1, static_cast<int>(bh * scale));
			picture.pixels.assign(static_cast<size_t>(picture.width) * picture.height, 0);
			for (int y = 0; y < picture.height; y++) {
				const uint8_t *row = scratch.at(0, y0 + std::min(bh - 1, static_cast<int>(y / scale)));
				for (int x = 0; x < picture.width; x++)
					picture.pixels[static_cast<size_t>(y) * picture.width + x] = row[x0 + std::min(bw - 1, static_cast<int>(x / scale))];
			}
		}
		it = PictureCache.emplace(fullKey, std::move(picture)).first;
	}
	const Picture &picture = it->second;
	if (picture.width == 0) {
		DrawString(out, "(empty)", { box.position + Displacement { 0, box.size.height / 2 - 6 }, { box.size.width, 12 } }, { .flags = UiFlags::ColorWhite | UiFlags::AlignCenter });
		return;
	}
	// Centred across, standing on the bottom of the box like it stands on the ground.
	const Point to { box.position.x + (box.size.width - picture.width) / 2, box.position.y + box.size.height - picture.height };
	for (int y = 0; y < picture.height; y++) {
		for (int x = 0; x < picture.width; x++) {
			const uint8_t color = picture.pixels[static_cast<size_t>(y) * picture.width + x];
			if (color != 0)
				out.SetPixel(to + Displacement { x, y }, color);
		}
	}
}

/** Draws a single tile (for the Ground tab and the hue preview). */
void DrawTilePicture(const Surface &out, Rectangle box, int tile, int hue)
{
	Building b;
	b.Resize(1, 1);
	b.tiles[0] = static_cast<uint16_t>(tile);
	b.hues[0] = static_cast<int8_t>(hue);
	DrawBuildingPicture(out, box, b, StrCat("t", tile, "h", hue));
}

void Frame(const Surface &out, Rectangle r, uint8_t color, int thickness)
{
	for (int i = 0; i < thickness; i++) {
		DrawHorizontalLine(out, r.position + Displacement { 0, i }, r.size.width, color);
		DrawHorizontalLine(out, r.position + Displacement { 0, r.size.height - 1 - i }, r.size.width, color);
		DrawVerticalLine(out, r.position + Displacement { i, 0 }, r.size.height, color);
		DrawVerticalLine(out, r.position + Displacement { r.size.width - 1 - i, 0 }, r.size.height, color);
	}
}

std::string HueLabel(int steps)
{
	if (steps == 0)
		return "hue: normal";
	return StrCat("hue ", steps > 0 ? "+" : "", steps * TileHueStepDegrees, " deg");
}

/** Draws one picker item into a box (pictures follow the current hue, except buildings, which keep their own). */
void DrawPickerItem(const Surface &out, Rectangle box, const PickerItem &item)
{
	switch (item.kind) {
	case PickerItem::Kind::Tile:
		DrawTilePicture(out, box, item.id, S.hueShift);
		break;
	case PickerItem::Kind::Decor: {
		if (item.id == 0) {
			const Point c = box.position + Displacement { box.size.width / 2, box.size.height / 2 };
			const int r = std::min(box.size.width, box.size.height) / 4;
			Line(out, c + Displacement { -r, -r }, c + Displacement { r, r }, PAL8_RED);
			Line(out, c + Displacement { -r, r }, c + Displacement { r, -r }, PAL8_RED);
			break;
		}
		Building b;
		b.Resize(1, 1);
		b.decor[0][0] = static_cast<uint16_t>(item.id);
		b.decorHues[0][0] = static_cast<int8_t>(S.hueShift);
		DrawBuildingPicture(out, box, b, StrCat("d", item.id, "h", S.hueShift));
		break;
	}
	case PickerItem::Kind::Building: {
		const std::vector<Building> &all = Buildings();
		if (item.id >= 0 && item.id < static_cast<int>(all.size()))
			DrawBuildingPicture(out, box, all[item.id], StrCat("b", all[item.id].builtIn ? "*" : "", all[item.id].name));
		break;
	}
	case PickerItem::Kind::NewBuilding: {
		const Point c = box.position + Displacement { box.size.width / 2, box.size.height / 2 - 6 };
		const int r = std::min(box.size.width, box.size.height) / 5;
		for (int t = -1; t <= 1; t++) {
			DrawHorizontalLine(out, c + Displacement { -r, t }, 2 * r + 1, PAL16_YELLOW + 2);
			DrawVerticalLine(out, c + Displacement { t, -r }, 2 * r + 1, PAL16_YELLOW + 2);
		}
		break;
	}
	}
}

/** Short name under an item's picture. */
std::string PickerItemLabel(const PickerItem &item)
{
	switch (item.kind) {
	case PickerItem::Kind::Tile: return StrCat(item.id);
	case PickerItem::Kind::Decor: return item.id == 0 ? std::string("Erase") : (IsFlowerDecor(static_cast<uint16_t>(item.id)) ? std::string(FlowerPatchName(item.id - DecorFlowerBase)) : StrCat(item.id));
	case PickerItem::Kind::Building: {
		const std::vector<Building> &all = Buildings();
		return item.id >= 0 && item.id < static_cast<int>(all.size()) ? all[item.id].name : std::string {};
	}
	case PickerItem::Kind::NewBuilding: return "New building";
	}
	return {};
}

/** Longer description for the large preview. */
std::string PickerItemDescription(const PickerItem &item)
{
	switch (item.kind) {
	case PickerItem::Kind::Tile: return StrCat("Tile #", item.id);
	case PickerItem::Kind::Decor:
		if (item.id == 0)
			return "Erase decor (on the chosen decor layer)";
		return IsFlowerDecor(static_cast<uint16_t>(item.id)) ? StrCat(FlowerPatchName(item.id - DecorFlowerBase), " (flowers)") : StrCat("Decor #", item.id, " (laid over the ground)");
	case PickerItem::Kind::Building: {
		const std::vector<Building> &all = Buildings();
		if (item.id < 0 || item.id >= static_cast<int>(all.size()))
			return {};
		const Building &b = all[item.id];
		return StrCat(b.name, " (", b.width, "x", b.height, ")", b.builtIn ? "" : "   right-click: delete");
	}
	case PickerItem::Kind::NewBuilding: return "New building: drag over an area of the map, then name it";
	}
	return {};
}

void DrawTilePicker(const Surface &out)
{
	RefreshPictureCache();
	const Rectangle r = PickerRect();
	DrawHalfTransparentRectTo(out, r.position.x, r.position.y, r.size.width, r.size.height);
	DrawHalfTransparentRectTo(out, r.position.x, r.position.y, r.size.width, r.size.height);
	DrawHalfTransparentRectTo(out, r.position.x, r.position.y, r.size.width, r.size.height);
	const std::vector<PickerItem> &items = PickerItems();
	const int hoveredIndex = PickerItemAt(MousePosition);
	const std::string title = S.capturing ? std::string("Drag over the area to save as a building")
	                                      : StrCat(PaintingLabel(), S.paintKind != PaintKind::Building ? StrCat("   ", HueLabel(S.hueShift)) : std::string {});
	DrawString(out, title, { r.position + Displacement { 4, 5 }, { r.size.width - 8, 12 } }, { .flags = UiFlags::ColorWhitegold | UiFlags::AlignCenter | UiFlags::KerningFitSpacing });

	const bool narrow = PickerRowButton(0, 0).size.width < 80;
	for (int i = 0; i < 4; i++)
		Button(out, PickerRowButton(0, i), S.kind == Kind::Dungeon ? DungeonPickerTabNames[i] : PickerTabNames[i], S.pickerTab == i);
	for (int i = 0; i < 4; i++) {
		const bool hidden = i > 0 && (S.hiddenLayers & (1u << (i - 1))) != 0;
		Button(out, PickerRowButton(1, i), hidden ? StrCat(narrow ? "D" : "Decor ", i, " (off)") : std::string(LayerNames[i]), S.layer == i);
	}
	for (int i = 0; i < 4; i++)
		Button(out, PickerRowButton(2, i), narrow ? PickerButtonShortLabels[i] : PickerButtonLabels[i], false);

	if (items.empty()) {
		DrawString(out, "Nothing here for this tileset.", { PickerGridRect().position + Displacement { 0, 20 }, { PickerGridRect().size.width, 12 } }, { .flags = UiFlags::ColorWhite | UiFlags::AlignCenter });
		return;
	}

	const PickerItem selected = SelectedPickerItem();
	const int first = S.pickerScroll * PickerColumns();
	const int last = std::min(static_cast<int>(items.size()), first + PickerVisibleRows() * PickerColumns()) - 1;
	const bool labelled = CurrentTab() == PickerTab::Buildings || CurrentTab() == PickerTab::Decor;
	for (int index = first; index <= last; index++) {
		const PickerItem &item = items[index];
		const Rectangle cell = PickerCellRect(index);
		FillRect(out, cell.position.x, cell.position.y, cell.size.width, cell.size.height, 0);
		const int labelRoom = labelled && item.kind != PickerItem::Kind::Tile ? 12 : 0;
		DrawPickerItem(out, { cell.position + Displacement { 3, 3 }, { cell.size.width - 6, cell.size.height - 6 - labelRoom } }, item);
		if (item == selected)
			Frame(out, cell, PAL16_YELLOW + 2, 2);
		else if (index == hoveredIndex)
			Frame(out, cell, PAL16_GRAY + 2, 1);
		const bool on = item == selected;
		if (labelRoom > 0)
			DrawString(out, PickerItemLabel(item), { cell.position + Displacement { 2, cell.size.height - 13 }, { cell.size.width - 4, 12 } }, { .flags = (on ? UiFlags::ColorWhitegold : UiFlags::ColorWhite) | UiFlags::AlignCenter | UiFlags::KerningFitSpacing });
		else if (item.kind == PickerItem::Kind::Tile || item.kind == PickerItem::Kind::Decor)
			DrawString(out, PickerItemLabel(item), { cell.position + Displacement { 3, 2 }, { cell.size.width - 6, 12 } }, { .flags = (on ? UiFlags::ColorWhitegold : UiFlags::ColorWhite) | UiFlags::KerningFitSpacing });
	}

	// Scroll bar.
	const Rectangle grid = PickerGridRect();
	const int rows = PickerRowCount(), shown = PickerVisibleRows();
	if (rows > shown) {
		const int track = grid.size.height;
		const int thumb = std::max(12, track * shown / rows);
		const int at = grid.position.y + (track - thumb) * S.pickerScroll / std::max(1, rows - shown);
		const int x = r.position.x + r.size.width - PickerScrollBar - 2;
		FillRect(out, x, grid.position.y, PickerScrollBar - 2, track, PAL16_GRAY + 13);
		FillRect(out, x, at, PickerScrollBar - 2, thumb, PAL16_YELLOW + 6);
	}

	if (!PickerHasPreview())
		return;
	// Large picture of what is under the mouse (or what is chosen).
	const Rectangle preview = PickerPreviewRect();
	const bool hovering = hoveredIndex >= 0;
	const PickerItem shown_ = hovering ? items[hoveredIndex] : selected;
	FillRect(out, preview.position.x, preview.position.y, preview.size.width, preview.size.height, 0);
	Frame(out, preview, PAL16_YELLOW + 9, 1);
	DrawString(out, StrCat(PickerItemDescription(shown_), hovering && !(shown_ == selected) && shown_.kind != PickerItem::Kind::NewBuilding ? "   (click to choose)" : ""),
	    { preview.position + Displacement { 6, 4 }, { preview.size.width - 12, 12 } }, { .flags = UiFlags::ColorWhitegold | UiFlags::KerningFitSpacing });
	DrawString(out, preview.size.width >= 360 ? "Wheel: scroll   Ctrl+wheel: size   Right-click layer: hide" : "Wheel: scroll   Ctrl+wheel: size",
	    { preview.position + Displacement { 6, preview.size.height - 16 }, { preview.size.width - 12, 12 } }, { .flags = UiFlags::ColorWhite | UiFlags::KerningFitSpacing });
	if (!(shown_.kind == PickerItem::Kind::Tile && shown_.id <= 0))
		DrawPickerItem(out, { preview.position + Displacement { 8, 20 }, { preview.size.width - 16, preview.size.height - 40 } }, shown_);
}

/** The building maker's name box. */
void DrawNamingDialog(const Surface &out)
{
	const int w = 340, h = 86;
	const Rectangle d { { (gnScreenWidth - w) / 2, std::max(TopBarHeight + 10, (gnViewportHeight - h) / 2) }, { w, h } };
	DrawHalfTransparentRectTo(out, d.position.x, d.position.y, d.size.width, d.size.height);
	DrawHalfTransparentRectTo(out, d.position.x, d.position.y, d.size.width, d.size.height);
	DrawHalfTransparentRectTo(out, d.position.x, d.position.y, d.size.width, d.size.height);
	Frame(out, d, PAL16_YELLOW + 9, 1);
	DrawString(out, StrCat("Save building (", S.pending.width, "x", S.pending.height, ")"), { d.position + Displacement { 0, 8 }, { w, 12 } }, { .flags = UiFlags::ColorWhitegold | UiFlags::AlignCenter });
	const bool blink = (SDL_GetTicks() / 400) % 2 == 0;
	DrawString(out, StrCat("Name: ", S.nameText, blink ? "_" : ""), { d.position + Displacement { 16, 32 }, { w - 32, 12 } }, { .flags = UiFlags::ColorWhite });
	DrawString(out, S.replaceAsked ? "That name exists. Enter again replaces it." : "Enter: save    Esc: cancel", { d.position + Displacement { 0, 60 }, { w, 12 } },
	    { .flags = (S.replaceAsked ? UiFlags::ColorRed : UiFlags::ColorGold) | UiFlags::AlignCenter });
}

// --- Music --------------------------------------------------------------------------------------

constexpr int MusicPanelWidth = 420;
constexpr int MusicRow = 20;

/** A row of the music panel. */
struct MusicRowInfo {
	enum class Kind : uint8_t {
		Song,    // a song of the map's playlist (index into S.map.music)
		Fade,    // the crossfade length
		Heading, // "Uploaded songs"
		Library, // an uploaded song not in the playlist (index into S.musicLibrary)
		Note,    // a line of text
	};
	Kind kind;
	int index;
	std::string label;
};

std::vector<MusicRowInfo> MusicRows()
{
	std::vector<MusicRowInfo> rows;
	if (S.map.music.empty())
		rows.push_back({ MusicRowInfo::Kind::Note, -1, "No songs: the usual music of this kind of place." });
	for (int i = 0; i < static_cast<int>(S.map.music.size()); i++)
		rows.push_back({ MusicRowInfo::Kind::Song, i, StrCat(i + 1, ". ", S.map.music[i]) });
	if (S.map.music.size() > 1)
		rows.push_back({ MusicRowInfo::Kind::Fade, -1, StrCat("Crossfade: ", S.map.musicFade, S.map.musicFade == 1 ? " second" : " seconds") });
	std::vector<int> unused;
	for (int i = 0; i < static_cast<int>(S.musicLibrary.size()); i++) {
		if (std::find(S.map.music.begin(), S.map.music.end(), S.musicLibrary[i]) == S.map.music.end())
			unused.push_back(i);
	}
	if (!unused.empty()) {
		rows.push_back({ MusicRowInfo::Kind::Heading, -1, "Uploaded before (click Add to use here too):" });
		for (const int i : unused)
			rows.push_back({ MusicRowInfo::Kind::Library, i, S.musicLibrary[i] });
	}
	return rows;
}

int MusicVisibleRows()
{
	return std::max(3, (gnViewportHeight - TopBarHeight - 40 - 100) / MusicRow);
}

Rectangle MusicPanelRect()
{
	const int shown = std::min(static_cast<int>(MusicRows().size()), MusicVisibleRows());
	const int h = 50 + shown * MusicRow + 66;
	const int w = std::min(MusicPanelWidth, gnScreenWidth - 8);
	return { { (gnScreenWidth - w) / 2, std::max(TopBarHeight + 6, (gnViewportHeight - h) / 2) }, { w, h } };
}

std::optional<Rectangle> MusicRowRect(int row)
{
	const int shown = row - S.musicScroll;
	if (shown < 0 || shown >= MusicVisibleRows())
		return std::nullopt;
	const Rectangle d = MusicPanelRect();
	return Rectangle { { d.position.x + 10, d.position.y + 46 + shown * MusicRow }, { d.size.width - 20, MusicRow - 2 } };
}

/** Small buttons at a row's right end, `slot` 0 the rightmost. */
Rectangle MusicRowButton(Rectangle row, int slot, int width = 26)
{
	return { { row.position.x + row.size.width - (slot + 1) * (width + 2), row.position.y + 1 }, { width, row.size.height - 2 } };
}

/** Footer: 0 upload songs, 1 done. */
Rectangle MusicFooterButton(int which)
{
	const Rectangle d = MusicPanelRect();
	const int y = d.position.y + d.size.height - 28;
	if (which == 0)
		return { { d.position.x + 10, y }, { 170, 20 } };
	return { { d.position.x + d.size.width - 90, y }, { 80, 20 } };
}

void ScrollMusicPanel(int rows)
{
	S.musicScroll = std::clamp(S.musicScroll + rows, 0, std::max(0, static_cast<int>(MusicRows().size()) - MusicVisibleRows()));
}

std::string MusicButtonLabel()
{
	if (S.map.music.empty())
		return "Music: usual";
	return StrCat("Music: ", S.map.music.size(), S.map.music.size() == 1 ? " song" : " songs");
}

/** Plays the map's songs now (from `startAt`), as the game will. */
void ApplyEditorMusic(std::optional<size_t> startAt = std::nullopt)
{
	music_crossfade_to(GetLevelMusic(leveltype), MapMusicFiles(S.map), S.map.musicFade * 1000, startAt);
}

void OpenMusicPanel()
{
	S.musicPanel = true;
	S.musicScroll = 0;
	S.weatherMenu = false;
	S.musicLibrary = ListCampaignMusic();
}

void CloseMusicPanel()
{
	S.musicPanel = false;
	ApplyEditorMusic(); // back from a song being tried
}

/** Copies songs from the player's computer into the campaign and adds them to this map's playlist. */
void AddSongFiles(const std::vector<std::string> &files)
{
	if (S.kind == Kind::Tristram || files.empty())
		return;
	int added = 0;
	std::optional<size_t> first;
	std::string problem;
	PushUndo();
	for (const std::string &file : files) {
		std::expected<std::string, std::string> song = ImportCampaignSong(file);
		if (!song.has_value()) {
			problem = song.error();
			LogWarn("Music upload: {}", song.error());
			continue;
		}
		auto it = std::find(S.map.music.begin(), S.map.music.end(), *song);
		if (it == S.map.music.end()) {
			S.map.music.push_back(*song);
			it = S.map.music.end() - 1;
			added++;
		}
		if (!first)
			first = static_cast<size_t>(it - S.map.music.begin());
	}
	S.musicLibrary = ListCampaignMusic();
	if (added > 0)
		S.dirty = true;
	if (first)
		ApplyEditorMusic(first);
	if (!problem.empty())
		Say(added > 0 ? StrCat("Added ", added, added == 1 ? " song. " : " songs. ", problem) : problem);
	else if (added > 0)
		Say(StrCat("Added ", added, added == 1 ? " song" : " songs", " to ", S.map.name, S.map.music.size() > 1 ? ": they play in turn, crossfading." : ": it loops."));
	else if (first)
		Say("That song is already on this map.");
}

void UploadSongs()
{
	const std::optional<std::vector<std::string>> files = ChooseFilesToOpen(StrCat("Choose music for ", S.map.name), { "ogg", "mp3", "wav" });
	if (!files) {
		Say("This system has no file chooser: drag music files onto the game window instead.");
		return;
	}
	if (files->empty())
		return; // cancelled
	AddSongFiles(*files);
}

void DrawMusicPanel(const Surface &out)
{
	const Rectangle d = MusicPanelRect();
	for (int i = 0; i < 3; i++)
		DrawHalfTransparentRectTo(out, d.position.x, d.position.y, d.size.width, d.size.height);
	Frame(out, d, PAL16_YELLOW + 9, 1);
	DrawString(out, StrCat("Music of ", S.map.name), { d.position + Displacement { 0, 8 }, { d.size.width, 12 } }, { .flags = UiFlags::ColorWhitegold | UiFlags::AlignCenter });
	const std::string sub = S.map.music.size() > 1 ? std::string("The songs play in turn, each crossfading into the next.")
	    : (S.map.music.size() == 1 ? std::string("One song: it loops. Add more to crossfade between them.") : std::string("Upload .ogg, .mp3 or .wav songs from your computer."));
	DrawString(out, sub, { d.position + Displacement { 0, 26 }, { d.size.width, 12 } }, { .flags = UiFlags::ColorGold | UiFlags::AlignCenter | UiFlags::KerningFitSpacing });
	const std::vector<MusicRowInfo> rows = MusicRows();
	const size_t playing = music_playlist_position();
	for (int i = 0; i < static_cast<int>(rows.size()); i++) {
		const std::optional<Rectangle> r = MusicRowRect(i);
		if (!r)
			continue;
		const MusicRowInfo &row = rows[i];
		int buttons = 0;
		switch (row.kind) {
		case MusicRowInfo::Kind::Song: {
			const bool now = static_cast<size_t>(row.index) == playing && S.map.music.size() > 1;
			FillRect(out, r->position.x, r->position.y, r->size.width, r->size.height, now ? PAL16_YELLOW + 12 : PAL16_GRAY + 13);
			Button(out, MusicRowButton(*r, 0), "x", false);
			Button(out, MusicRowButton(*r, 1), "dn", false);
			Button(out, MusicRowButton(*r, 2), "up", false);
			Button(out, MusicRowButton(*r, 3, 36), "play", false);
			buttons = 3 * 28 + 38;
			break;
		}
		case MusicRowInfo::Kind::Fade:
			Button(out, MusicRowButton(*r, 0), "+", false);
			Button(out, MusicRowButton(*r, 1), "-", false);
			buttons = 2 * 28;
			break;
		case MusicRowInfo::Kind::Library:
			if (r->contains(MousePosition))
				FillRect(out, r->position.x, r->position.y, r->size.width, r->size.height, PAL16_GRAY + 13);
			Button(out, MusicRowButton(*r, 0, 40), "Add", false);
			Button(out, MusicRowButton(*r, 1, 40), "try", false);
			buttons = 2 * 42;
			break;
		default: break;
		}
		const UiFlags color = row.kind == MusicRowInfo::Kind::Heading ? UiFlags::ColorGold : (row.kind == MusicRowInfo::Kind::Note ? UiFlags::ColorGold : UiFlags::ColorWhite);
		DrawString(out, row.label, { r->position + Displacement { 6, 3 }, { r->size.width - 12 - buttons, 12 } }, { .flags = color | UiFlags::KerningFitSpacing });
	}
	if (static_cast<int>(rows.size()) > MusicVisibleRows())
		DrawString(out, "(wheel: more)", { d.position + Displacement { 0, 8 }, { d.size.width - 10, 12 } }, { .flags = UiFlags::ColorGold | UiFlags::AlignRight });
	DrawString(out, "Upload, or drag music files from your computer onto this window.", { d.position + Displacement { 0, d.size.height - 48 }, { d.size.width, 12 } },
	    { .flags = UiFlags::ColorGold | UiFlags::AlignCenter | UiFlags::KerningFitSpacing });
	Button(out, MusicFooterButton(0), "Upload songs...", false);
	Button(out, MusicFooterButton(1), "Done", false);
}

void ClickMusicPanel(Point p, bool right)
{
	(void)right;
	if (MusicFooterButton(0).contains(p)) {
		UploadSongs();
		return;
	}
	if (MusicFooterButton(1).contains(p) || !MusicPanelRect().contains(p)) {
		CloseMusicPanel();
		return;
	}
	const std::vector<MusicRowInfo> rows = MusicRows();
	for (int i = 0; i < static_cast<int>(rows.size()); i++) {
		const std::optional<Rectangle> r = MusicRowRect(i);
		if (!r || !r->contains(p))
			continue;
		const MusicRowInfo &row = rows[i];
		switch (row.kind) {
		case MusicRowInfo::Kind::Song: {
			const int k = row.index;
			if (MusicRowButton(*r, 0).contains(p)) {
				PushUndo();
				S.map.music.erase(S.map.music.begin() + k);
				S.dirty = true;
				ApplyEditorMusic();
				ScrollMusicPanel(0);
				Say(S.map.music.empty() ? std::string("No songs: this map plays its usual music.") : std::string("Song taken off this map (it stays uploaded)."));
			} else if (MusicRowButton(*r, 1).contains(p) || MusicRowButton(*r, 2).contains(p)) {
				const int to = k + (MusicRowButton(*r, 1).contains(p) ? 1 : -1);
				if (to >= 0 && to < static_cast<int>(S.map.music.size())) {
					PushUndo();
					std::swap(S.map.music[k], S.map.music[to]);
					S.dirty = true;
					ApplyEditorMusic(); // same songs, new order: carries on
				}
			} else {
				ApplyEditorMusic(static_cast<size_t>(k)); // play from this song
				Say(StrCat("Playing ", S.map.music[k], "."));
			}
			return;
		}
		case MusicRowInfo::Kind::Fade:
			if (MusicRowButton(*r, 0).contains(p) || MusicRowButton(*r, 1).contains(p)) {
				S.map.musicFade = std::clamp(S.map.musicFade + (MusicRowButton(*r, 0).contains(p) ? 1 : -1), 0, 20);
				S.dirty = true;
				ApplyEditorMusic();
			}
			return;
		case MusicRowInfo::Kind::Library: {
			const std::string song = S.musicLibrary[row.index];
			if (MusicRowButton(*r, 0, 40).contains(p)) {
				PushUndo();
				S.map.music.push_back(song);
				S.dirty = true;
				ApplyEditorMusic(S.map.music.size() - 1);
				Say(StrCat("Added ", song, "."));
			} else {
				// Try it (the map's own songs come back when the panel closes).
				music_crossfade_to(GetLevelMusic(leveltype), { CampaignSongPath(song) }, 1500);
				Say(StrCat("Trying ", song, ". Add puts it on this map."));
			}
			return;
		}
		default: return;
		}
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
	if (WeatherButtonIndex() >= 0)
		Button(out, ToolButton(WeatherButtonIndex()), StrCat("Weather: ", WeatherTitle(S.map.weather)), S.weatherMenu);
	if (MusicButtonIndex() >= 0)
		Button(out, ToolButton(MusicButtonIndex()), MusicButtonLabel(), S.musicPanel);
	if (OuterToggleIndex() >= 0)
		Button(out, ToolButton(OuterToggleIndex()), S.map.outer.empty() ? std::string("Outer wilds: off") : StrCat("Outer wilds: ", SideShort(S.map.outerSide)), !S.map.outer.empty());
	if (OuterGoIndex() >= 0)
		Button(out, ToolButton(OuterGoIndex()), S.kind == Kind::Town ? "To outer wilds" : "Back to town", false);

	if (PickerShown())
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
		DrawString(out, "WASD or middle-drag: move   Wheel/Z: zoom   Ctrl+S: save   P: test", { { 8, y + 3 }, { gnScreenWidth - 16, 12 } }, { .flags = UiFlags::ColorWhite | UiFlags::AlignRight });

	if (S.linkPortal >= 0 && S.linkPortal < static_cast<int>(S.map.portals.size()))
		DrawLinkDialog(out);
	else if (S.musicPanel)
		DrawMusicPanel(out);
	if (S.weatherMenu && WeatherButtonIndex() >= 0) {
		const Rectangle m = WeatherMenuRect();
		DrawHalfTransparentRectTo(out, m.position.x, m.position.y, m.size.width, m.size.height);
		DrawHalfTransparentRectTo(out, m.position.x, m.position.y, m.size.width, m.size.height);
		DrawHalfTransparentRectTo(out, m.position.x, m.position.y, m.size.width, m.size.height);
		DrawString(out, MoodsEnabledFor(S.map.style) ? "Weather of this map" : "Weather (off in options)", { m.position + Displacement { 0, 5 }, { m.size.width, 12 } },
		    { .flags = UiFlags::ColorWhitegold | UiFlags::AlignCenter | UiFlags::KerningFitSpacing });
		const std::vector<std::string> choices = WeatherChoices();
		for (int i = 0; i < static_cast<int>(choices.size()); i++) {
			const Rectangle row = WeatherRowRect(i);
			const bool on = choices[i] == S.map.weather;
			if (on || row.contains(MousePosition))
				FillRect(out, row.position.x, row.position.y, row.size.width, row.size.height, on ? PAL16_YELLOW + 12 : PAL16_GRAY + 13);
			DrawString(out, i == 0 ? std::string("Changing (rolls and turns)") : WeatherTitle(choices[i]), { row.position + Displacement { 6, 2 }, { row.size.width - 12, 12 } },
			    { .flags = (on ? UiFlags::ColorWhitegold : UiFlags::ColorWhite) | UiFlags::KerningFitSpacing });
		}
	}
	if (S.naming)
		DrawNamingDialog(out);
}

// --- Input -----------------------------------------------------------------------------------

bool OnInterface(Point p)
{
	if (p.y < TopBarHeight)
		return true;
	if (!S.editing)
		return false;
	if (S.linkPortal >= 0 || S.musicPanel)
		return true;
	for (int i = 0; i < ToolButtonCount(); i++) {
		if (ToolButton(i).contains(p))
			return true;
	}
	if (S.weatherMenu && WeatherMenuRect().contains(p))
		return true;
	if (PickerShown())
		return PickerRect().contains(p) || p.y >= gnViewportHeight - 18;
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
	S.map.hues = s.hues;
	S.map.decor = s.decor;
	S.map.decorHues = s.decorHues;
	S.map.fences = s.fences;
	S.map.animals = s.animals;
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
		SetHiddenDecorLayers(0); // play-testing shows every layer
		LoadMapLevel(false);
	} else {
		SetHiddenDecorLayers(S.hiddenLayers);
		LoadMapLevel(true);
	}
}

void ExitEditor()
{
	if (S.dirty)
		SaveMap();
	S.running = false;
	SetHiddenDecorLayers(0);
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

/** Sets where the door leads, says so and closes the dialog. */
void ChooseLink(const MapLink &link)
{
	MapPortal &portal = S.map.portals[S.linkPortal];
	if (!S.linkIsNew)
		PushUndo();
	portal.to = link;
	S.dirty = true;
	Say(StrCat(portal.door ? "Door" : "Portal", " leads to ", LinkDescription(link), "."));
	S.linkIsNew = false;
	S.linkPortal = -1;
}

/** Town: switches the outer wilds off (-1) or on along an edge, making the wilds map the first time. */
void SetOuterWilds(int side)
{
	if (S.kind != Kind::Town)
		return;
	if (side < 0) {
		if (!S.map.outer.empty()) {
			if (std::expected<CustomMap, std::string> wilds = ReadCustomMap(S.map.outer); wilds.has_value()) {
				wilds->outer.clear();
				(void)WriteCustomMap(*wilds);
			}
			Say(StrCat("Outer wilds off: the town's edge is closed (", S.map.outer, " is kept as a map)."));
		}
		S.map.outer.clear();
		S.dirty = true;
		Rebuild();
		SaveMap();
		return;
	}
	std::string name = S.map.outer;
	if (name.empty()) {
		std::string base = S.map.name;
		if (base.size() > 24)
			base.resize(24);
		for (int n = 1; n < 100; n++) {
			name = n == 1 ? StrCat(base, "_outer") : StrCat(base, "_outer", n);
			if (!CustomMapExists(name))
				break;
			// This town's own outer wilds from before (switched off since): used again.
			if (std::expected<CustomMap, std::string> before = ReadCustomMap(name); before.has_value() && before->style == DTYPE_FOREST && (before->outer.empty() || before->outer == S.map.name))
				break;
		}
	}
	const int wildsSide = (side + 2) % 4;
	CustomMap wilds;
	bool clearEdge = false;
	if (std::expected<CustomMap, std::string> existing = ReadCustomMap(name); existing.has_value()) {
		wilds = std::move(*existing);
		clearEdge = wilds.outerSide != wildsSide || wilds.outer.empty();
	} else {
		wilds.name = name;
		wilds.style = DTYPE_FOREST;
		wilds.Resize(40, 40);
		FillMeadow(wilds);
		wilds.entries.push_back({ "start", { 16 + 2 * 20, 16 + 2 * 20 } });
		wilds.outerSide = wildsSide;
		// A few packs fitting the hero's level, kept away from the town's edge.
		for (const Point piece : { Point { 10, 10 }, Point { 29, 9 }, Point { 12, 28 }, Point { 28, 27 }, Point { 8, 20 }, Point { 31, 19 } }) {
			const Point tile { 16 + 2 * piece.x, 16 + 2 * piece.y };
			if (!OnMapSide(wilds, wildsSide, tile, 22))
				wilds.spawners.push_back({ tile, std::string(AutoMonster), 4 });
		}
		clearEdge = true;
	}
	wilds.outer = S.map.name;
	wilds.outerSide = wildsSide;
	if (clearEdge) {
		// The edge facing the town is open ground (the meadow's woods only ring its other edges).
		const std::vector<uint16_t> &grass = GroundBrushes()[0].tiles;
		const std::vector<uint16_t> &trees = GroundBrushes()[7].tiles;
		for (int y = 0; y < wilds.height; y++) {
			for (int x = 0; x < wilds.width; x++) {
				uint16_t &t = wilds.tiles[static_cast<size_t>(y) * wilds.width + x];
				if (OnMapSide(wilds, wildsSide, { 16 + 2 * x, 16 + 2 * y }, 6) && std::find(trees.begin(), trees.end(), t) != trees.end())
					t = grass[(static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u) % grass.size()];
			}
		}
	}
	if (std::expected<void, std::string> r = WriteCustomMap(wilds); !r.has_value()) {
		Say(StrCat("Could not make ", name, ": ", r.error()));
		return;
	}
	S.map.outer = name;
	S.map.outerSide = side;
	S.dirty = true;
	Rebuild();
	SaveMap();
	Say(StrCat("Outer wilds on: walking off the ", MapSideName(side), " edge leads to ", name, ". Monsters stay out there. Build it with \"To outer wilds\"."));
}

/** Saves this map and opens another in the editor. */
void SwitchEditorMap(const std::string &name)
{
	if (S.dirty)
		SaveMap();
	std::expected<CustomMap, std::string> map = ReadCustomMap(name);
	if (!map.has_value()) {
		Say(map.error());
		return;
	}
	S.map = std::move(*map);
	S.kind = S.map.style == DTYPE_TOWN ? Kind::Town : (S.map.style == DTYPE_FOREST ? Kind::Wilds : Kind::Dungeon);
	S.undo.clear();
	S.dirty = false;
	S.tool = ToolsFor(S.kind).front();
	S.showTilePicker = false;
	S.weatherMenu = false;
	S.linkPortal = -1;
	S.paletteScroll = 0;
	// Look at the edge shared with the town (or the middle).
	S.camera = { 16 + S.map.width, 16 + S.map.height };
	if (!S.map.outer.empty()) {
		switch (S.map.outerSide) {
		case 0: S.camera.y = 16 + 3; break;
		case 1: S.camera.x = 16 + 2 * S.map.width - 3; break;
		case 2: S.camera.y = 16 + 2 * S.map.height - 3; break;
		default: S.camera.x = 16 + 3; break;
		}
	}
	LoadMapLevel(true);
	Say(StrCat("Editing ", name, "."));
}

bool ClickDialog(Point p, bool right)
{
	MapPortal &portal = S.map.portals[S.linkPortal];
	if (DialogFooterButton().contains(p)) {
		if (S.linkPage == 1) {
			S.linkPage = 0;
			S.linkScroll = 0;
		} else {
			CloseLinkDialog(true);
		}
		return true;
	}
	const std::vector<LinkRow> rows = LinkRows();
	for (int i = 0; i < static_cast<int>(rows.size()); i++) {
		const std::optional<Rectangle> r = DialogRowRect(i);
		if (!r || !r->contains(p))
			continue;
		const LinkRow &row = rows[i];
		switch (row.kind) {
		case LinkRow::Kind::Tristram:
			if (TristramEntries().empty()) {
				ChooseLink({ MapLinkKind::Town, "", "", 1 });
			} else {
				S.linkPage = 1;
				S.linkTarget = -2;
				S.linkScroll = 0;
			}
			break;
		case LinkRow::Kind::Dungeon: {
			const int level = portal.to.kind == MapLinkKind::Dungeon ? portal.to.level : 1;
			if (DialogLevelButton(*r, false).contains(p) || DialogLevelButton(*r, true).contains(p) || right) {
				// Change the level without leaving the list.
				const bool up = DialogLevelButton(*r, true).contains(p);
				portal.to = { MapLinkKind::Dungeon, "", "", std::clamp(level + (up ? 1 : -1), 1, 16) };
				S.dirty = true;
			} else {
				ChooseLink({ MapLinkKind::Dungeon, "", "", level });
			}
			break;
		}
		case LinkRow::Kind::Map: {
			const Session::LinkTarget &target = S.linkTargets[row.index];
			if (target.entries.size() >= 2) {
				S.linkPage = 1;
				S.linkTarget = row.index;
				S.linkScroll = 0;
			} else {
				ChooseLink({ MapLinkKind::Map, target.name, target.entries.empty() ? std::string {} : target.entries.front(), 1 });
			}
			break;
		}
		case LinkRow::Kind::NewWilds:
			if (S.linkIsNew)
				S.linkIsNew = false; // keep the door: the wilds is made for it
			else
				PushUndo();
			MakeWildsBeyond(portal);
			S.linkPortal = -1;
			break;
		case LinkRow::Kind::Nowhere:
			ChooseLink({});
			break;
		case LinkRow::Kind::Arrival:
			if (S.linkTarget == -2)
				ChooseLink({ MapLinkKind::Town, "", row.index < 0 ? std::string {} : row.label, 1 });
			else
				ChooseLink({ MapLinkKind::Map, S.linkTargets[S.linkTarget].name, row.label, 1 });
			break;
		}
		return true;
	}
	return true; // the dialog keeps the clicks while it is open
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

void ChoosePickerItem(const PickerItem &item, bool right)
{
	switch (item.kind) {
	case PickerItem::Kind::Tile:
		S.pickerTile = static_cast<uint16_t>(item.id);
		S.brush = -1;
		S.paintKind = PaintKind::Ground;
		S.layer = 0;
		Say(StrCat("Tile #", item.id, " chosen. Drag on the map to paint it (it shows in place under the cursor)."));
		break;
	case PickerItem::Kind::Decor:
		S.pickerDecor = static_cast<uint16_t>(item.id);
		S.paintKind = PaintKind::Decor;
		if (S.layer == 0)
			S.layer = 1;
		Say(item.id == 0 ? StrCat("Drag on the map to erase decor from ", LayerNames[S.layer], ".")
		                 : StrCat(DecorName(static_cast<uint16_t>(item.id)), " chosen for ", LayerNames[S.layer], ". Drag on the map to lay it over the ground."));
		break;
	case PickerItem::Kind::Building: {
		const std::vector<Building> &all = Buildings();
		if (item.id < 0 || item.id >= static_cast<int>(all.size()))
			break;
		const Building &b = all[item.id];
		if (right) {
			if (b.builtIn) {
				Say("Tristram's own buildings cannot be deleted.");
			} else if (S.deleteAsk == b.name && SDL_GetTicks() < S.deleteUntil) {
				const std::string name = b.name;
				DeleteSavedBuilding(name);
				S.deleteAsk.clear();
				S.pickerBuilding = std::min(S.pickerBuilding, static_cast<int>(Buildings().size()) - 1);
				ScrollPicker(0);
				Say(StrCat("Deleted building \"", name, "\". Maps it was placed in keep it."));
			} else {
				S.deleteAsk = b.name;
				S.deleteUntil = SDL_GetTicks() + 3000;
				Say(StrCat("Right-click \"", b.name, "\" again to delete it from the building list."));
			}
			break;
		}
		S.pickerBuilding = item.id;
		S.paintKind = PaintKind::Building;
		Say(StrCat(b.name, " chosen. Click on the map to place it (it shows in place under the cursor)."));
		break;
	}
	case PickerItem::Kind::NewBuilding:
		S.capturing = true;
		S.dragging = false;
		Say("Building maker: drag over the area of the map to save. Right-click or Esc cancels.");
		break;
	}
}

void ClickPicker(Point p, bool right)
{
	for (int i = 0; i < 4; i++) {
		if (PickerRowButton(0, i).contains(p)) {
			SetPickerTab(static_cast<PickerTab>(i));
			return;
		}
		if (PickerRowButton(1, i).contains(p)) {
			if (right)
				ToggleLayerHidden(i);
			else
				SelectLayer(i);
			return;
		}
		if (PickerRowButton(2, i).contains(p)) {
			if (i == 0)
				ShiftHue(-1);
			else if (i == 1)
				ShiftHue(1);
			else if (i == 2)
				S.hueShift = 0;
			else
				S.showTilePicker = false;
			return;
		}
	}
	if (const int index = PickerItemAt(p); index >= 0)
		ChoosePickerItem(PickerItems()[index], right);
}

/** Right-click on the map with the Paint tool: picks up what is on the chosen layer there. */
void PickUp(Point piece)
{
	if (S.layer > 0) {
		const uint16_t id = TownDecor(S.layer - 1, piece);
		S.pickerDecor = id;
		if (id != 0)
			S.hueShift = TownDecorHue(S.layer - 1, piece);
		S.paintKind = PaintKind::Decor;
		Say(id == 0 ? StrCat("Nothing on ", LayerNames[S.layer], " here: painting now erases it.") : StrCat("Picked up ", DecorName(id), " (", HueLabel(S.hueShift), ") from ", LayerNames[S.layer], "."));
	} else {
		if (TownTile(piece) == 0)
			return;
		S.pickerTile = TownTile(piece);
		S.hueShift = TownHue(piece);
		S.brush = -1;
		S.paintKind = PaintKind::Ground;
		Say(StrCat("Picked up tile #", S.pickerTile, " (", HueLabel(S.hueShift), "). Drag to paint with it."));
	}
	if (S.showTilePicker) {
		SetPickerTab(TabForPaint());
		ShowPickerSelection();
	}
}

/** Lays the chosen decor (or erases) on the chosen decor layer over a rectangle of megatiles. */
void PaintDecor(Point a, Point b)
{
	const int x0 = std::max(0, std::min(a.x, b.x)), x1 = std::min(S.map.width - 1, std::max(a.x, b.x));
	const int y0 = std::max(0, std::min(a.y, b.y)), y1 = std::min(S.map.height - 1, std::max(a.y, b.y));
	if (x0 > x1 || y0 > y1)
		return;
	const int layer = std::max(1, S.layer) - 1;
	PushUndo();
	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++) {
			TownDecor(layer, { x, y }) = S.pickerDecor;
			TownDecorHue(layer, { x, y }) = static_cast<int8_t>(S.pickerDecor != 0 ? S.hueShift : 0);
		}
	}
	Rebuild();
	if ((S.hiddenLayers & (1u << layer)) != 0)
		Say(StrCat(LayerNames[layer + 1], " is hidden, so this does not show while editing."));
}

/** Building maker: the dragged area becomes a building waiting for its name. */
void CaptureArea(Point a, Point b)
{
	S.pending = TakeArea(a, b);
	S.capturing = false;
	if (S.pending.width == 0)
		return;
	S.naming = true;
	S.nameText.clear();
	S.replaceAsked = false;
	Say("Type a name for the building, then press Enter.");
}

void FinishNaming(bool save)
{
	if (!save) {
		S.naming = false;
		Say("Building not saved.");
		return;
	}
	std::string name = S.nameText;
	while (!name.empty() && name.back() == ' ')
		name.pop_back();
	if (!IsValidBuildingName(name)) {
		Say("Type a name: letters, numbers, spaces, - and _ (up to 32).");
		return;
	}
	for (const Building &b : Buildings()) {
		if (b.builtIn && b.name == name) {
			Say("That is the name of one of Tristram's buildings; pick another.");
			return;
		}
	}
	if (SavedBuildingExists(name) && !S.replaceAsked) {
		S.replaceAsked = true;
		return;
	}
	S.pending.name = name;
	if (std::expected<void, std::string> r = SaveBuilding(S.pending); !r.has_value()) {
		Say(r.error());
		return;
	}
	S.naming = false;
	const std::vector<Building> &all = Buildings();
	for (size_t i = 0; i < all.size(); i++) {
		if (!all[i].builtIn && all[i].name == name)
			S.pickerBuilding = static_cast<int>(i);
	}
	S.paintKind = PaintKind::Building;
	S.showTilePicker = true;
	S.pickerTab = static_cast<int>(PickerTab::Buildings);
	ShowPickerSelection();
	Say(StrCat("Saved \"", name, "\". It is in the Buildings tab (and the Stamp tool) in every town."));
}

/** Typing into the building maker's name box. */
void TypeName(SDL_Scancode sc, SDL_Keycode key)
{
	if (sc == SDL_SCANCODE_ESCAPE) {
		FinishNaming(false);
	} else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
		FinishNaming(true);
	} else if (sc == SDL_SCANCODE_BACKSPACE) {
		if (!S.nameText.empty())
			S.nameText.pop_back();
		S.replaceAsked = false;
	} else if (S.nameText.size() < 32) {
		const bool shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
		char c = 0;
		if (key >= 'a' && key <= 'z')
			c = static_cast<char>(shift ? key - 'a' + 'A' : key);
		else if (key >= '0' && key <= '9')
			c = static_cast<char>(key);
		else if (key == ' ')
			c = ' ';
		else if (key == '-')
			c = shift ? '_' : '-';
		if (c != 0) {
			S.nameText.push_back(c);
			S.replaceAsked = false;
		}
	}
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
	if (!S.editing || S.naming)
		return;
	if (S.linkPortal >= 0) {
		ClickDialog(p, right);
		return;
	}
	if (S.musicPanel) {
		ClickMusicPanel(p, right);
		return;
	}
	if (S.weatherMenu) {
		if (WeatherMenuRect().contains(p)) {
			const std::vector<std::string> choices = WeatherChoices();
			for (int i = 0; i < static_cast<int>(choices.size()); i++) {
				if (!WeatherRowRect(i).contains(p))
					continue;
				S.map.weather = choices[i];
				S.dirty = true;
				S.weatherMenu = false;
				if (S.kind == Kind::Town)
					SetCustomTownMapForEditor(S.map);
				Say(choices[i].empty() ? std::string("Weather: changing, as usual for this kind of place.")
				                       : StrCat("Weather: ", WeatherTitle(choices[i]), ", always, on this map.", MoodsEnabledFor(S.map.style) ? "" : " (Weather is switched off in the options: F8.)"));
			}
			return;
		}
		S.weatherMenu = false;
		if (!ToolButton(WeatherButtonIndex()).contains(p))
			return; // a click away just closes the list
	}
	const int n = static_cast<int>(ToolsFor(S.kind).size());
	for (int i = 0; i < ToolButtonCount(); i++) {
		if (!ToolButton(i).contains(p))
			continue;
		if (i == WeatherButtonIndex()) {
			S.weatherMenu = !S.weatherMenu;
			return;
		}
		if (i == MusicButtonIndex()) {
			OpenMusicPanel();
			return;
		}
		if (i == OuterToggleIndex()) {
			// Off, then each edge in turn (right-click goes back the other way).
			const int current = S.map.outer.empty() ? -1 : S.map.outerSide;
			const int next = right ? (current < 0 ? 3 : current - 1) : (current >= 3 ? -1 : current + 1);
			SetOuterWilds(next);
			return;
		}
		if (i == OuterGoIndex()) {
			SwitchEditorMap(S.map.outer);
			return;
		}
		if (i < n)
			SelectTool(i);
		else if (S.kind != Kind::Tristram && i == n + 1)
			S.grid = !S.grid;
		else if (i == n + (S.kind == Kind::Tristram ? 1 : 2))
			Undo();
		return;
	}
	if (PickerShown()) {
		if (PickerRect().contains(p)) {
			ClickPicker(p, right);
			return;
		}
	} else if (PaletteRect().contains(p)) {
		if (PaletteTitleRect().contains(p)) {
			if (S.tool == Tool::Spawner)
				S.packSize = std::clamp(S.packSize + (right ? -1 : 1), 1, 20);
			return;
		}
		const int rows = static_cast<int>(PaletteRows().size());
		for (int i = 0; i < rows; i++) {
			if (PaletteRowShown(i) && PaletteRowRect(i).contains(p))
				SelectPalette(i);
		}
		if (S.tool == Tool::Paint && S.showTilePicker)
			OpenTilePicker();
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
		if (S.capturing) {
			if (right) {
				S.capturing = false;
				Say("Building maker cancelled.");
			} else {
				S.dragging = true;
				S.dragStart = piece;
			}
			break;
		}
		if (right) {
			PickUp(piece);
		} else if (S.paintKind == PaintKind::Building) {
			if (const Building *b = PlacingBuilding(); b != nullptr)
				PlaceBuilding(*b, piece);
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
	case Tool::Fence:
		if (CanPlaceAt(tile)) {
			S.dragging = true;
			S.dragErase = right || S.fenceKind == 2;
			S.dragStart = tile;
		}
		break;
	case Tool::Animal: {
		const int at = AnimalAt(tile);
		if (right) {
			if (at >= 0) {
				PushUndo();
				Say(StrCat(AnimalLabel(S.map.animals[at].kind), " removed."));
				S.map.animals.erase(S.map.animals.begin() + at);
				Rebuild();
			}
			break;
		}
		if (!CanPlaceAt(tile) || !IsTileWalkable(tile) || dFence[tile.x][tile.y] != 0)
			break;
		const std::vector<AnimalKindInfo> &kinds = AnimalKinds();
		const AnimalKindInfo &kind = kinds[std::clamp(S.animal, 0, static_cast<int>(kinds.size()) - 1)];
		PushUndo();
		S.map.animals.push_back({ tile, std::string(kind.key) });
		Rebuild();
		Say(StrCat(kind.label, " placed. It wanders around here (test with P); fences keep it in."));
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
		SyncLiveObjects();
		break;
	}
	case Tool::Prop: {
		if (MapObjectAt(tile) == nullptr)
			break;
		if (right) {
			// The nearest prop within a tile (chests belong to the Chest tool).
			for (int r = 0; r <= 1; r++) {
				for (int dy = -r; dy <= r; dy++) {
					for (int dx = -r; dx <= r; dx++) {
						uint16_t *at = MapObjectAt(tile + Displacement { dx, dy });
						if (at == nullptr || *at == 0 || *at == MapChestObjectId)
							continue;
						PushUndo();
						*at = 0;
						SyncLiveObjects();
						Say("Prop removed.");
						return;
					}
				}
			}
			break;
		}
		const std::vector<PropKind> &props = PropsForMap();
		if (props.empty())
			break;
		const PropKind &prop = props[std::clamp(S.prop, 0, static_cast<int>(props.size()) - 1)];
		PushUndo();
		*MapObjectAt(tile) = prop.id;
		SyncLiveObjects();
		Say(StrCat(prop.label, " placed."));
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
			OpenLinkDialog(static_cast<int>(S.map.portals.size()) - 1, true);
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
		if (S.tool == Tool::Fence)
			SetFenceRun(S.dragStart, cursPosition, S.dragErase ? 0 : static_cast<uint8_t>(S.fenceKind + 1));
		else if (S.tool == Tool::Paint && S.capturing)
			CaptureArea(S.dragStart, end);
		else if (S.tool == Tool::Paint && S.paintKind == PaintKind::Decor)
			PaintDecor(S.dragStart, end);
		else if (S.tool == Tool::Paint)
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
	(void)TakeDroppedFiles(); // nothing dropped before the editor opened
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
	SetHiddenDecorLayers(0);
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

bool MapEditorRunning()
{
	return S.running;
}

void MapEditorNotice(std::string text)
{
	if (S.running)
		Say(std::move(text));
}

const CustomMap *EditorMusicMap()
{
	if (!S.running || S.kind == Kind::Tristram || !OnEditorLevel())
		return nullptr;
	return &S.map;
}

std::optional<std::string> EditorMapWeather()
{
	if (!S.running || S.kind == Kind::Tristram || !OnEditorLevel())
		return std::nullopt;
	return S.map.weather;
}

namespace {
bool HandleKey(SDL_Scancode sc);
void PlaceGhost();
void SetMapZoom(bool on);
void PanCamera(Displacement delta);
void PanPicker(Displacement delta);
} // namespace

void MapEditorTick()
{
	if (!S.running || MyPlayer == nullptr)
		return;
	if (!S.travelled && leveltype == DTYPE_TOWN && !MyPlayer->_pLvlChanging) {
		S.travelled = true;
		LoadMapLevel(S.editing);
		return;
	}
	if (std::vector<std::string> dropped = TakeDroppedFiles(); !dropped.empty()) {
		// Files dragged onto the window: songs for this map.
		if (S.editing && S.kind != Kind::Tristram && OnEditorLevel() && S.linkPortal < 0) {
			if (!S.musicPanel)
				OpenMusicPanel();
			AddSongFiles(dropped);
		} else if (S.editing) {
			Say(S.kind == Kind::Tristram ? "Tristram keeps its own music." : "Close the open window first, then drop the songs again.");
		}
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
	if (const char *script = std::getenv("DEVILUTIONX_DEV_EDITORSCRIPT"); script != nullptr && S.running && OnEditorLevel()) { // also while play-testing
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
				else if (w[0] == "placedoor" && n.size() >= 3) {
					// As if the Door tool clicked a tile: the door goes down and the dialog asks where it leads.
					PushUndo();
					MapPortal portal;
					portal.tile = { n[1], n[2] };
					portal.door = true;
					S.map.portals.push_back(portal);
					OpenLinkDialog(static_cast<int>(S.map.portals.size()) - 1, true);
				} else if ((w[0] == "linkrow" || w[0] == "linkminus" || w[0] == "linkplus") && n.size() >= 2 && S.linkPortal >= 0) {
					if (const std::optional<Rectangle> r = DialogRowRect(n[1]); r) {
						ControlMode = ControlTypes::KeyboardAndMouse;
						MousePosition = w[0] == "linkrow" ? r->position + Displacement { 30, 8 } : DialogLevelButton(*r, w[0] == "linkplus").position + Displacement { 5, 5 };
						MouseDown(MousePosition, false);
					}
				} else if (w[0] == "music") {
					OpenMusicPanel();
				} else if (w[0] == "musicadd" && w.size() >= 2) {
					std::string_view rest = cmds[step - 1];
					rest.remove_prefix(rest.find("musicadd") + 9);
					std::vector<std::string> files;
					for (std::string_view f : SplitByChar(rest, '|'))
						files.emplace_back(f);
					AddSongFiles(files);
				} else if ((w[0] == "musicbtn" || w[0] == "musicwide") && n.size() >= 3 && S.musicPanel) {
					if (const std::optional<Rectangle> r = MusicRowRect(n[1]); r) {
						ControlMode = ControlTypes::KeyboardAndMouse;
						MousePosition = MusicRowButton(*r, n[2], w[0] == "musicwide" ? 40 : (n[2] == 3 ? 36 : 26)).position + Displacement { 4, 4 };
						MouseDown(MousePosition, false);
					}
				} else if (w[0] == "musicdone" && S.musicPanel) {
					MouseDown(MusicFooterButton(1).position + Displacement { 4, 4 }, false);
				} else if (w[0] == "dropfile" && w.size() >= 2) {
#if !defined(USE_SDL3) && !defined(USE_SDL1)
					// As if a file were dragged onto the window.
					std::string_view rest = cmds[step - 1];
					rest.remove_prefix(rest.find("dropfile") + 9);
					SDL_Event drop {};
					drop.type = SDL_DROPFILE;
					drop.drop.file = SDL_strdup(std::string(rest).c_str());
					SDL_PushEvent(&drop);
#endif
				} else if (w[0] == "musiclog") {
					std::string all;
					for (const std::string &song : S.map.music)
						all += song + "; ";
					LogInfo("EDITORMUSIC fade={} playing={} songs={}", S.map.musicFade, music_playlist_position(), all);
				} else if (w[0] == "outer" && n.size() >= 2) {
					SetOuterWilds(n[1]);
				} else if (w[0] == "goouter") {
					SwitchEditorMap(S.map.outer);
				} else if (w[0] == "linkfooter" && S.linkPortal >= 0) {
					MousePosition = DialogFooterButton().position + Displacement { 10, 8 };
					MouseDown(MousePosition, false);
				} else if (w[0] == "portals") {
					std::string all;
					for (const MapPortal &pp : S.map.portals)
						all += StrCat(pp.tile.x, ",", pp.tile.y, "->", LinkDescription(pp.to), "; ");
					LogInfo("EDITORPORTALS {}", all);
				}
				else if (w[0] == "picker")
					OpenTilePicker();
				else if (w[0] == "pickertile" && n.size() >= 2) {
					S.pickerTile = static_cast<uint16_t>(std::clamp(n[1], 1, std::max(1, static_cast<int>(MegaTileCount))));
					S.brush = -1;
					S.paintKind = PaintKind::Ground;
					S.layer = 0;
					ShowPickerSelection();
				} else if (w[0] == "decor" && n.size() >= 2) {
					S.pickerDecor = static_cast<uint16_t>(n[1]);
					S.paintKind = PaintKind::Decor;
					S.layer = std::max(S.layer, 1);
				} else if (w[0] == "paintdecor" && n.size() >= 5) {
					PaintDecor({ n[1], n[2] }, { n[3], n[4] });
				} else if (w[0] == "prop" && n.size() >= 4) {
					if (uint16_t *slot = MapObjectAt({ n[2], n[3] }); slot != nullptr) {
						PushUndo();
						*slot = static_cast<uint16_t>(n[1]);
						SyncLiveObjects();
					}
				} else if (w[0] == "flower" && n.size() >= 2) {
					S.pickerDecor = static_cast<uint16_t>(DecorFlowerBase + n[1]);
					S.paintKind = PaintKind::Decor;
					S.layer = std::max(S.layer, 1);
				} else if (w[0] == "building" && n.size() >= 2) {
					S.pickerBuilding = n[1];
					S.paintKind = PaintKind::Building;
				} else if (w[0] == "tab" && n.size() >= 2)
					SetPickerTab(static_cast<PickerTab>(std::clamp(n[1], 0, 3)));
				else if (w[0] == "layer" && n.size() >= 2)
					SelectLayer(n[1]);
				else if (w[0] == "hidelayer" && n.size() >= 2)
					ToggleLayerHidden(n[1]);
				else if (w[0] == "hue" && n.size() >= 2)
					S.hueShift = std::clamp(n[1], -MaxTileHueSteps, MaxTileHueSteps);
				else if (w[0] == "mouse" && n.size() >= 3) {
					ControlMode = ControlTypes::KeyboardAndMouse; // a real mouse would switch to it
					MousePosition = { n[1], n[2] };
				} else if ((w[0] == "hoveritem" || w[0] == "clickitem" || w[0] == "rclickitem") && n.size() >= 2) {
					const Rectangle cell = PickerCellRect(n[1]);
					ControlMode = ControlTypes::KeyboardAndMouse;
					MousePosition = cell.position + Displacement { cell.size.width / 2, cell.size.height / 2 };
					if (w[0] != "hoveritem")
						MouseDown(MousePosition, w[0] == "rclickitem");
				} else if ((w[0] == "hovertile" || w[0] == "clicktile") && n.size() >= 2) {
					const int index = PickerIndexOf({ PickerItem::Kind::Tile, n[1] });
					if (index >= 0) {
						const Rectangle cell = PickerCellRect(index);
						ControlMode = ControlTypes::KeyboardAndMouse;
						MousePosition = cell.position + Displacement { cell.size.width / 2, cell.size.height / 2 };
						if (w[0] == "clicktile")
							MouseDown(MousePosition, false);
					}
				} else if (w[0] == "mousepiece" && n.size() >= 3) {
					// Put the mouse over a map piece (the camera is centred on it first).
					S.camera = { 16 + 2 * n[1] + 1, 16 + 2 * n[2] + 1 };
					ControlMode = ControlTypes::KeyboardAndMouse;
					MousePosition = { gnScreenWidth / 2, gnViewportHeight / 2 };
				} else if (w[0] == "drag" && n.size() >= 5) {
					// A Paint-tool drag from one map piece to another.
					S.dragging = true;
					S.dragStart = { n[1], n[2] };
					cursPosition = { 16 + 2 * n[3], 16 + 2 * n[4] };
					MouseUp();
				} else if (w[0] == "placeat" && n.size() >= 3) {
					if (const Building *b = PlacingBuilding(); b != nullptr)
						PlaceBuilding(*b, { n[1], n[2] });
				} else if (w[0] == "newbuilding")
					ChoosePickerItem({ PickerItem::Kind::NewBuilding, 0 }, false);
				else if (w[0] == "type" && w.size() >= 2) {
					for (size_t k = 1; k < w.size(); k++)
						S.nameText += StrCat(k > 1 ? " " : "", w[k]);
				} else if (w[0] == "enter")
					FinishNaming(true);
				else if (w[0] == "scrollpicker" && n.size() >= 2)
					ScrollPicker(n[1]);
				else if (w[0] == "zoom" && n.size() >= 2) {
					S.pickerZoom = std::clamp(n[1], 1, 3);
					ShowPickerSelection();
				} else if (w[0] == "fence" && n.size() >= 6) {
					// fence kind(0 fence, 1 gate, 2 remove) x0 y0 x1 y1 (world tiles)
					SetFenceRun({ n[2], n[3] }, { n[4], n[5] }, n[1] == 2 ? 0 : static_cast<uint8_t>(n[1] + 1));
				} else if (w[0] == "animal" && n.size() >= 4) {
					PushUndo();
					S.map.animals.push_back({ { n[2], n[3] }, std::string(w[1]) });
					Rebuild();
				} else if (w[0] == "animals") {
					std::string where;
					for (const Point p : AnimalPositions())
						where += StrCat(p.x, ",", p.y, " ");
					LogInfo("EDITORANIMALS {}", where);
				} else if (w[0] == "weathermenu")
					S.weatherMenu = true;
				else if (w[0] == "weatherrow" && n.size() >= 2) {
					ControlMode = ControlTypes::KeyboardAndMouse;
					MousePosition = WeatherRowRect(n[1]).position + Displacement { 20, 8 };
					MouseDown(MousePosition, false);
				} else if (w[0] == "closepicker")
					S.showTilePicker = false;
				else if (w[0] == "click" || w[0] == "rclick")
					MouseDown(MousePosition, w[0] == "rclick");
				else if (w[0] == "release")
					MouseUp();
				else if (w[0] == "key" && n.size() >= 2)
					HandleKey(static_cast<SDL_Scancode>(n[1]));
				else if (w[0] == "pan" && n.size() >= 3)
					PanCamera({ n[1], n[2] });
				else if (w[0] == "panpicker" && n.size() >= 2)
					PanPicker({ 0, n[1] });
				else if (w[0] == "undo")
					Undo();
				else if (w[0] == "mapzoom" && n.size() >= 2)
					SetMapZoom(n[1] != 0);
				else if (w[0] == "state") {
					const Point at = PieceAt(cursPosition);
					const bool in = InMap(at);
					LogInfo("EDITORSTATE kind {} tile {} decor {} building {} layer {} hidden {} tab {} items {} hue {} picker {} capturing {} naming {} name '{}' cursorPiece {},{} tileThere {} decorThere {},{},{} solid {} buildings {} undo {}",
					    static_cast<int>(S.paintKind), S.pickerTile, S.pickerDecor, S.pickerBuilding, S.layer, S.hiddenLayers, S.pickerTab, PickerItemCount(), S.hueShift, S.showTilePicker, S.capturing, S.naming, S.nameText,
					    at.x, at.y, in ? TownTile(at) : 0, in ? TownDecor(0, at) : 0, in ? TownDecor(1, at) : 0, in ? TownDecor(2, at) : 0,
					    in ? dDecorSolid[16 + 2 * at.x][16 + 2 * at.y] : false, Buildings().size(), S.undo.size());
				}
				LogInfo("EDITORSCRIPT {}", cmds[step - 1]);
			}
		}
	}
#endif
	const int key = static_cast<int>(currlevel) + (setlevel ? 1000 + setlvlnum : 0);
	if (key != S.levelKey) {
		S.levelKey = key;
		S.liveObjects = S.map.objects; // the level was just built with the map's objects
		if (IsMapEditing() && S.kind == Kind::Dungeon) {
			// Freshly built level: keep editing from the same spot.
			Rebuild();
		}
	}
}

namespace {

void RestoreFenceGhost();

/** Puts back what the paint preview covered. */
void RestoreGhost()
{
	for (auto it = S.ghost.rbegin(); it != S.ghost.rend(); ++it) {
		const Point t = it->tile;
		dPiece[t.x][t.y] = it->piece;
		dTileHue[t.x][t.y] = it->hue;
		dSpecial[t.x][t.y] = it->special;
		for (int layer = 0; layer < DecorLayerCount; layer++) {
			dDecorPiece[layer][t.x][t.y] = it->decorPiece[layer];
			dDecorFlower[layer][t.x][t.y] = it->decorFlower[layer];
			dDecorHue[layer][t.x][t.y] = it->decorHue[layer];
		}
	}
	S.ghost.clear();
	RestoreFenceGhost();
}

void RestoreFenceGhost()
{
	for (auto it = S.fenceGhost.rbegin(); it != S.fenceGhost.rend(); ++it)
		dFence[it->first.x][it->first.y] = it->second;
	S.fenceGhost.clear();
}

/** Shows the run of fence being dragged (or the tile under the cursor) in place for this frame. */
void PlaceFenceGhost()
{
	if (!S.editing || S.tool != Tool::Fence || S.linkPortal >= 0 || S.panning || OnInterface(MousePosition) || !CanPlaceAt(cursPosition))
		return;
	const bool erase = S.dragging ? S.dragErase : S.fenceKind == 2;
	const uint8_t kind = erase ? 0 : static_cast<uint8_t>(S.fenceKind + 1);
	for (const Point t : S.dragging ? FenceRun(S.dragStart, cursPosition) : std::vector<Point> { cursPosition }) {
		if (TownFence(t) == nullptr)
			continue;
		S.fenceGhost.emplace_back(t, dFence[t.x][t.y]);
		dFence[t.x][t.y] = kind;
	}
}

/** Remembers the four world tiles of a megatile before the preview changes them. */
void SaveGhostCell(Point cell)
{
	for (int k = 0; k < 4; k++) {
		const Point t { 16 + 2 * cell.x + k % 2, 16 + 2 * cell.y + k / 2 };
		Session::GhostCell g { t, dPiece[t.x][t.y], dTileHue[t.x][t.y], dSpecial[t.x][t.y], {}, {}, {} };
		for (int layer = 0; layer < DecorLayerCount; layer++) {
			g.decorPiece[layer] = dDecorPiece[layer][t.x][t.y];
			g.decorFlower[layer] = dDecorFlower[layer][t.x][t.y];
			g.decorHue[layer] = dDecorHue[layer][t.x][t.y];
		}
		S.ghost.push_back(g);
	}
}

void GhostGround(Point cell, int tile, int hue)
{
	const std::optional<std::array<uint16_t, 4>> pieces = MegaPieces(tile);
	if (!pieces)
		return;
	const bool townRoofs = !InDesertRegion() && S.kind != Kind::Dungeon;
	for (int k = 0; k < 4; k++) {
		const Point t { 16 + 2 * cell.x + k % 2, 16 + 2 * cell.y + k / 2 };
		dPiece[t.x][t.y] = (*pieces)[k];
		dTileHue[t.x][t.y] = static_cast<int8_t>(hue);
		if (townRoofs)
			dSpecial[t.x][t.y] = static_cast<int8_t>(TownSpecialForPiece((*pieces)[k]));
	}
}

void GhostDecor(Point cell, int layer, uint16_t id, int hue)
{
	if ((S.hiddenLayers & (1u << layer)) != 0)
		return;
	const std::optional<std::array<uint16_t, 4>> pieces = IsFlowerDecor(id) ? std::nullopt : MegaPieces(id);
	for (int k = 0; k < 4; k++) {
		const Point t { 16 + 2 * cell.x + k % 2, 16 + 2 * cell.y + k / 2 };
		dDecorFlower[layer][t.x][t.y] = IsFlowerDecor(id) ? static_cast<uint8_t>(id - DecorFlowerBase + 1) : 0;
		dDecorPiece[layer][t.x][t.y] = pieces && PieceHasStandingParts((*pieces)[k]) ? static_cast<uint16_t>((*pieces)[k] + 1) : 0;
		dDecorHue[layer][t.x][t.y] = static_cast<int8_t>(hue);
	}
}

/** Lays what painting would lay (the hovered piece, the dragged area, or the building) into the level for this frame's drawing. */
void PlaceGhost()
{
	if (!S.editing || S.tool != Tool::Paint || S.linkPortal >= 0 || S.panning || S.capturing || S.naming || S.kind == Kind::Tristram)
		return;
	if (S.kind == Kind::Dungeon && (!OnEditorLevel() || leveltype != S.map.style))
		return;
	if (OnInterface(MousePosition) || pMegaTiles == nullptr)
		return;
	const Point hover = PieceAt(cursPosition);
	if (!InMap(hover))
		return;
	if (S.paintKind == PaintKind::Building) {
		const Building *b = PlacingBuilding();
		if (b == nullptr)
			return;
		const Point origin = hover - Displacement { b->width / 2, b->height / 2 };
		for (int y = 0; y < b->height; y++) {
			for (int x = 0; x < b->width; x++) {
				const Point cell = origin + Displacement { x, y };
				const size_t i = static_cast<size_t>(y) * b->width + x;
				if (!InMap(cell) || b->tiles[i] == 0)
					continue;
				SaveGhostCell(cell);
				GhostGround(cell, b->tiles[i], b->hues[i]);
				for (int layer = 0; layer < MapDecorLayerCount; layer++)
					GhostDecor(cell, layer, b->decor[layer][i], b->decorHues[layer][i]);
			}
		}
		return;
	}
	const Point from = S.dragging ? S.dragStart : hover;
	const int x0 = std::max(0, std::min(from.x, hover.x)), x1 = std::min(S.map.width - 1, std::max(from.x, hover.x));
	const int y0 = std::max(0, std::min(from.y, hover.y)), y1 = std::min(S.map.height - 1, std::max(from.y, hover.y));
	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++) {
			SaveGhostCell({ x, y });
			if (S.paintKind == PaintKind::Decor)
				GhostDecor({ x, y }, std::max(1, S.layer) - 1, S.pickerDecor, S.pickerDecor != 0 ? S.hueShift : 0);
			else
				GhostGround({ x, y }, PaintTileAt({ x, y }), S.hueShift);
		}
	}
}

/** Close-up view of the map (the game's 2x zoom). */
void SetMapZoom(bool on)
{
	if (*GetOptions().Graphics.zoom == on)
		return;
	GetOptions().Graphics.zoom.SetValue(on);
	CalcViewportGeometry();
	Say(on ? "Zoomed in (Z, or wheel down over the map, to zoom back out)." : "Zoomed out.");
}

/** Middle-drag over the map: the map follows the mouse. */
void PanCamera(Displacement delta)
{
	// Kept in half world pixels: zoomed in, one screen pixel is half a world pixel.
	const int zoom = *GetOptions().Graphics.zoom ? 2 : 1;
	S.panRemainder += Displacement { delta.deltaX * 2 / zoom, delta.deltaY * 2 / zoom };
	// A world tile step moves the view by (32, 16) pixels per x and (-32, 16) per y.
	const float fx = -(S.panRemainder.deltaX / 64.0f + S.panRemainder.deltaY / 32.0f) / 2.0f;
	const float fy = -(S.panRemainder.deltaY / 32.0f - S.panRemainder.deltaX / 64.0f) / 2.0f;
	const int cx = static_cast<int>(fx), cy = static_cast<int>(fy);
	if (cx == 0 && cy == 0)
		return;
	S.camera.x = std::clamp(S.camera.x + cx, 10, MAXDUNX - 10);
	S.camera.y = std::clamp(S.camera.y + cy, 10, MAXDUNY - 10);
	S.panRemainder -= Displacement { -(cx - cy) * 64, -(cx + cy) * 32 };
}

/** Middle-drag over the picker: the pictures follow the mouse. */
void PanPicker(Displacement delta)
{
	S.panRemainder += delta;
	const int stride = PickerCell() + PickerGap;
	const int rows = S.panRemainder.deltaY / stride;
	if (rows != 0) {
		ScrollPicker(-rows);
		S.panRemainder.deltaY -= rows * stride;
	}
}

bool HandleKey(SDL_Scancode sc)
{
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
	if (S.musicPanel) {
		if (sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_RETURN)
			CloseMusicPanel();
		return true;
	}
	if (S.tool == Tool::Paint && S.linkPortal < 0 && IsAnyOf(S.kind, Kind::Town, Kind::Wilds)) {
		if (sc == SDL_SCANCODE_ESCAPE && S.capturing) {
			S.capturing = false;
			Say("Building maker cancelled.");
			return true;
		}
		if (sc == SDL_SCANCODE_L) {
			SelectLayer((S.layer + 1) % (MapDecorLayerCount + 1));
			return true;
		}
		if (sc == SDL_SCANCODE_TAB && S.showTilePicker) {
			SetPickerTab(static_cast<PickerTab>((S.pickerTab + 1) % 4));
			return true;
		}
		if (sc == SDL_SCANCODE_T) {
			if (S.showTilePicker)
				S.showTilePicker = false;
			else
				OpenTilePicker();
			return true;
		}
		if (S.showTilePicker) {
			switch (sc) {
			case SDL_SCANCODE_F: ShiftHue(-1); return true;
			case SDL_SCANCODE_G: ShiftHue(1); return true;
			case SDL_SCANCODE_PAGEUP: ScrollPicker(-PickerVisibleRows()); return true;
			case SDL_SCANCODE_PAGEDOWN: ScrollPicker(PickerVisibleRows()); return true;
			case SDL_SCANCODE_ESCAPE: S.showTilePicker = false; return true;
			default: break;
			}
		}
	}
	if (ctrl && sc == SDL_SCANCODE_S) {
		SaveMap();
	} else if (ctrl && sc == SDL_SCANCODE_Z) {
		Undo();
	} else if (sc == SDL_SCANCODE_Z) {
		SetMapZoom(!*GetOptions().Graphics.zoom);
	} else if (sc == SDL_SCANCODE_G) {
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
		if (S.linkPage == 1)
			S.linkPage = 0;
		else
			CloseLinkDialog(true);
	} else if (sc == SDL_SCANCODE_ESCAPE) {
		return false; // the game menu (save and quit from there, or use Exit)
	}
	return true; // no game hotkeys while editing
}

} // namespace

void MapEditorBeforeDraw()
{
	RestoreGhost();
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
	if (OnEditorLevel()) {
		PlaceGhost();
		PlaceFenceGhost();
	}
}

bool MapEditorHandleEvent(const SDL_Event &event)
{
	if (!S.running || !OnEditorLevel())
		return false;
	switch (event.type) {
	case SDL_EVENT_MOUSE_BUTTON_DOWN: {
		// The game only updates MousePosition after this handler, so use the event's own position.
		const Point at { SDLC_EventButtonIntX(event), SDLC_EventButtonIntY(event) };
		if (event.button.button == 2) { // middle button: drag the picker or the map
			if (!S.editing)
				return false;
			S.panning = true;
			S.panningPicker = PickerShown() && PickerRect().contains(at);
			S.panLast = at;
			S.panRemainder = {};
			return true;
		}
		if (event.button.button != SDL_BUTTON_LEFT && event.button.button != SDL_BUTTON_RIGHT)
			return S.editing;
		if (!S.editing && !TopButton(1).contains(at) && !TopButton(0).contains(at) && !TopButton(2).contains(at))
			return false; // play-testing: the game gets the click
		MouseDown(at, event.button.button == SDL_BUTTON_RIGHT);
		return true;
	}
	case SDL_EVENT_MOUSE_BUTTON_UP:
		if (event.button.button == 2) {
			const bool was = S.panning;
			S.panning = false;
			return was || S.editing;
		}
		if (!S.editing)
			return false;
		MouseUp();
		return true;
	case SDL_EVENT_MOUSE_MOTION:
		if (S.panning) {
			const Point at { SDLC_EventMotionIntX(event), SDLC_EventMotionIntY(event) };
			const Displacement delta = at - S.panLast;
			S.panLast = at;
			if (S.panningPicker)
				PanPicker(delta);
			else
				PanCamera(delta);
		}
		return false; // let the game track the mouse
	case SDL_EVENT_KEY_DOWN:
		if (S.naming && S.editing) {
			TypeName(SDLC_EventScancode(event), SDLC_EventKey(event));
			return true;
		}
		return HandleKey(SDLC_EventScancode(event));
	case SDL_EVENT_KEY_UP:
		return S.editing;
	case SDL_EVENT_MOUSE_WHEEL: {
		if (!S.editing)
			return false;
		const int wheel = SDLC_EventWheelIntY(event);
		if (wheel == 0)
			return true;
		if (S.linkPortal >= 0) {
			ScrollDialog(wheel > 0 ? -1 : 1);
			return true;
		}
		if (S.musicPanel) {
			ScrollMusicPanel(wheel > 0 ? -1 : 1);
			return true;
		}
		if (PickerShown() && PickerRect().contains(MousePosition)) {
			if ((SDL_GetModState() & SDL_KMOD_CTRL) != 0) {
				S.pickerZoom = std::clamp(S.pickerZoom + (wheel > 0 ? 1 : -1), 1, 3);
				ShowPickerSelection();
			} else {
				ScrollPicker(wheel > 0 ? -1 : 1);
			}
			return true;
		}
		if (!OnInterface(MousePosition)) {
			SetMapZoom(wheel > 0); // over the map: wheel up comes closer, wheel down backs out
			return true;
		}
		ScrollPalette(wheel > 0 ? -3 : 3);
		return true;
	}
	default:
		return false;
	}
}

void DrawMapEditor(const Surface &out)
{
	RestoreGhost(); // the world is drawn; the preview was only for that
	if (!S.running || !OnEditorLevel())
		return;
	if (S.editing)
		DrawWorldOverlay(out);
	DrawInterface(out);
}

} // namespace devilution
