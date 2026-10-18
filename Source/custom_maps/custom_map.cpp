#include "custom_maps/custom_map.hpp"
#include "custom_maps/custom_town.hpp"
#include "custom_maps/map_editor.hpp"
#include "custom_maps/campaign.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <direct.h>
#define DVL_GETCWD _getcwd
#else
#include <unistd.h>
#define DVL_GETCWD getcwd
#endif

#include "engine/palette.h"
#include "levels/drlg_l1.h"
#include "levels/drlg_l2.h"
#include "levels/drlg_l3.h"
#include "levels/drlg_l4.h"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "levels/town.h"
#include "levels/trigs.h"
#include "missiles.h"
#include "msg.h"
#include "objects.h"
#include "engine/sound.h"
#include "player.h"
#include "utils/endian_read.hpp"
#include "utils/file_util.h"
#include "utils/log.hpp"
#include "utils/parse_int.hpp"
#include "utils/paths.h"
#include "utils/str_cat.hpp"
#include "utils/status_macros.hpp"
#include "utils/str_split.hpp"

namespace devilution {

namespace {

std::optional<CustomMap> Active;
std::string ActiveEntry;
bool PortalsPlaced = false;
Point LastHeroTile { -1, -1 };

/** The level loaders read files through the asset system, which only takes absolute paths outside the game data. */
std::string AbsolutePath(const std::string &path)
{
	const bool absolute = (!path.empty() && (path[0] == '/' || path[0] == '\\')) || (path.size() > 2 && path[1] == ':');
	if (absolute)
		return path;
	char cwd[1024];
	if (DVL_GETCWD(cwd, sizeof(cwd)) == nullptr)
		return path;
	return StrCat(cwd, DIRECTORY_SEPARATOR_STR, path);
}

std::string MapFolder(std::string_view name)
{
	return StrCat(CustomMapsDirectory(), name, DIRECTORY_SEPARATOR_STR);
}

std::expected<std::string, std::string> ReadWholeFile(const std::string &path)
{
	FILE *file = OpenFile(path.c_str(), "rb");
	if (file == nullptr)
		return std::unexpected(StrCat("Cannot open ", path));
	std::string data;
	char buffer[4096];
	size_t got;
	while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
		data.append(buffer, got);
	std::fclose(file);
	return data;
}

std::expected<void, std::string> WriteWholeFile(const std::string &path, std::string_view data)
{
	FILE *file = OpenFile(path.c_str(), "wb");
	if (file == nullptr)
		return std::unexpected(StrCat("Cannot write ", path));
	const bool ok = std::fwrite(data.data(), 1, data.size(), file) == data.size();
	std::fclose(file);
	if (!ok)
		return std::unexpected(StrCat("Cannot write ", path));
	return {};
}

void PutU16(std::string &out, uint16_t v)
{
	out.push_back(static_cast<char>(v & 0xFF));
	out.push_back(static_cast<char>(v >> 8));
}

std::string BuildDun(const CustomMap &map)
{
	std::string out;
	const int w = map.width, h = map.height;
	const size_t big = static_cast<size_t>(w) * 2 * static_cast<size_t>(h) * 2;
	out.reserve(4 + static_cast<size_t>(w) * h * 2 + big * 8);
	PutU16(out, static_cast<uint16_t>(w));
	PutU16(out, static_cast<uint16_t>(h));
	for (int i = 0; i < w * h; i++)
		PutU16(out, map.tiles[i]);
	for (size_t i = 0; i < big; i++) // items layer (unused by the game)
		PutU16(out, 0);
	for (size_t i = 0; i < big; i++)
		PutU16(out, i < map.monsters.size() ? map.monsters[i] : 0);
	for (size_t i = 0; i < big; i++)
		PutU16(out, i < map.objects.size() ? map.objects[i] : 0);
	for (size_t i = 0; i < big; i++) // transparency: computed when the level loads
		PutU16(out, 0);
	return out;
}

std::string_view LinkKindName(MapLinkKind kind)
{
	switch (kind) {
	case MapLinkKind::Map: return "map";
	case MapLinkKind::Town: return "town";
	case MapLinkKind::Dungeon: return "dungeon";
	default: return "none";
	}
}

MapLinkKind ParseLinkKind(std::string_view s)
{
	if (s == "map") return MapLinkKind::Map;
	if (s == "town") return MapLinkKind::Town;
	if (s == "dungeon") return MapLinkKind::Dungeon;
	return MapLinkKind::None;
}

std::expected<void, std::string> LoadWildsLevel();

int ToInt(std::string_view s, int fallback = 0)
{
	const ParseIntResult<int> v = ParseInt<int>(s);
	return v.has_value() ? *v : fallback;
}

std::string BuildMeta(const CustomMap &map)
{
	std::string out = "# Custom map (map editor). Positions are world tiles.\n";
	out += StrCat("name=", map.name, "\n");
	out += StrCat("style=", MapStyleName(map.style), "\n");
	out += StrCat("weather=", map.weather, "\n");
	for (const std::string &song : map.music)
		out += StrCat("music=", song, "\n");
	out += StrCat("musicfade=", map.musicFade, "\n");
	for (const MapEntryPoint &e : map.entries)
		out += StrCat("entry=", e.name, ",", e.tile.x, ",", e.tile.y, "\n");
	for (const MapPortal &p : map.portals)
		out += StrCat("portal=", p.tile.x, ",", p.tile.y, ",", p.door ? 1 : 0, ",", LinkKindName(p.to.kind), ",", p.to.map, ",", p.to.entry, ",", p.to.level, "\n");
	for (const MapNpc &n : map.npcs)
		out += StrCat("npc=", n.type, ",", n.tile.x, ",", n.tile.y, ",", n.direction, "\n");
	for (const MapSpawner &s : map.spawners)
		out += StrCat("spawner=", s.tile.x, ",", s.tile.y, ",", s.monster, ",", s.count, "\n");
	for (const MapBoss &b : map.bosses)
		out += StrCat("boss=", b.tile.x, ",", b.tile.y, ",", b.name, ",", b.minions, "\n");
	for (const MapAnimal &a : map.animals)
		out += StrCat("animal=", a.tile.x, ",", a.tile.y, ",", a.kind, "\n");
	if (!map.outer.empty())
		out += StrCat("outer=", map.outer, ",", map.outerSide, "\n");
	return out;
}

void ParseMeta(std::string_view text, CustomMap &map)
{
	for (std::string_view line : SplitByChar(text, '\n')) {
		if (!line.empty() && line.back() == '\r')
			line.remove_suffix(1);
		if (line.empty() || line[0] == '#')
			continue;
		const size_t eq = line.find('=');
		if (eq == std::string_view::npos)
			continue;
		const std::string_view key = line.substr(0, eq);
		const std::string_view value = line.substr(eq + 1);
		std::vector<std::string_view> f;
		for (std::string_view part : SplitByChar(value, ','))
			f.push_back(part);
		if (key == "style") {
			if (const std::optional<dungeon_type> style = ParseMapStyle(value))
				map.style = *style;
		} else if (key == "weather") {
			map.weather = std::string(value);
		} else if (key == "music") {
			if (!value.empty())
				map.music.emplace_back(value);
		} else if (key == "musicfade") {
			map.musicFade = std::clamp(ToInt(value), 0, 20);
		} else if (key == "entry" && f.size() >= 3) {
			map.entries.push_back({ std::string(f[0]), { ToInt(f[1]), ToInt(f[2]) } });
		} else if (key == "portal" && f.size() >= 7) {
			MapPortal portal;
			portal.tile = { ToInt(f[0]), ToInt(f[1]) };
			portal.door = ToInt(f[2]) != 0;
			portal.to.kind = ParseLinkKind(f[3]);
			portal.to.map = std::string(f[4]);
			portal.to.entry = std::string(f[5]);
			portal.to.level = ToInt(f[6], 1);
			map.portals.push_back(std::move(portal));
		} else if (key == "npc" && f.size() >= 4) {
			map.npcs.push_back({ std::string(f[0]), { ToInt(f[1]), ToInt(f[2]) }, std::clamp(ToInt(f[3]), 0, 7) });
		} else if (key == "spawner" && f.size() >= 4) {
			map.spawners.push_back({ { ToInt(f[0]), ToInt(f[1]) }, std::string(f[2]), std::clamp(ToInt(f[3], 4), 1, 20) });
		} else if (key == "outer" && f.size() >= 2) {
			map.outer = std::string(f[0]);
			map.outerSide = std::clamp(ToInt(f[1]), 0, 3);
		} else if (key == "animal" && f.size() >= 3) {
			map.animals.push_back({ { ToInt(f[0]), ToInt(f[1]) }, std::string(f[2]) });
		} else if (key == "boss" && f.size() >= 3) {
			map.bosses.push_back({ { ToInt(f[0]), ToInt(f[1]) }, std::string(f[2]), std::clamp(f.size() >= 4 ? ToInt(f[3], 6) : 6, 0, 12) });
		}
	}
}

} // namespace

uint8_t MapStyleFloorPiece(dungeon_type style)
{
	switch (style) {
	case DTYPE_CATACOMBS: return 3;
	case DTYPE_CAVES: return 7;
	case DTYPE_HELL: return 6;
	default: return 13;
	}
}

bool IsTownTiledStyle(dungeon_type style)
{
	return style == DTYPE_TOWN || style == DTYPE_FOREST;
}

// --- Map data --------------------------------------------------------------------------------

void CustomMap::Resize(int w, int h)
{
	width = std::clamp(w, 4, 40);
	height = std::clamp(h, 4, 40);
	tiles.assign(static_cast<size_t>(width) * height, 0);
	hues.assign(static_cast<size_t>(width) * height, 0);
	for (int layer = 0; layer < MapDecorLayerCount; layer++) {
		decor[layer].assign(static_cast<size_t>(width) * height, 0);
		decorHues[layer].assign(static_cast<size_t>(width) * height, 0);
	}
	fences.assign(static_cast<size_t>(width) * 2 * height * 2, 0);
	monsters.assign(static_cast<size_t>(width) * 2 * height * 2, 0);
	objects.assign(static_cast<size_t>(width) * 2 * height * 2, 0);
	plan.assign(40 * 40, 0);
	overrides.assign(40 * 40, 0);
}

const MapEntryPoint *CustomMap::FindEntry(std::string_view entryName) const
{
	for (const MapEntryPoint &e : entries) {
		if (e.name == entryName)
			return &e;
	}
	return entries.empty() ? nullptr : &entries.front();
}

std::string CustomMapsDirectory()
{
	// Maps belong to a campaign; Diablo: Original has none (an empty folder name nothing is found in).
	const std::string campaign = ActiveCampaignDirectory();
	if (campaign.empty())
		return StrCat(paths::BasePath(), "mods" DIRECTORY_SEPARATOR_STR "custommaps" DIRECTORY_SEPARATOR_STR "original" DIRECTORY_SEPARATOR_STR);
	return StrCat(campaign, "maps" DIRECTORY_SEPARATOR_STR);
}

bool IsValidMapName(std::string_view name)
{
	if (name.empty() || name.size() > 32 || name == TristramLayerName)
		return false; // "tristram" is the editor's name for Tristram itself
	return std::all_of(name.begin(), name.end(), [](char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
	});
}

std::vector<std::string> ListCustomMaps()
{
	std::vector<std::string> names;
	const std::string dir = CustomMapsDirectory();
	if (!DirectoryExists(dir.c_str()))
		return names;
	for (std::string &name : ListDirectories(dir.c_str())) {
		if (IsValidMapName(name) && FileExists((MapFolder(name) + "map.dun").c_str()))
			names.push_back(std::move(name));
	}
	std::sort(names.begin(), names.end());
	return names;
}

bool CustomMapExists(std::string_view name)
{
	return IsValidMapName(name) && FileExists(CustomMapDunPath(name).c_str());
}

std::string CustomMapDunPath(std::string_view name)
{
	return MapFolder(name) + "map.dun";
}

std::expected<CustomMap, std::string> ReadCustomMap(std::string_view name)
{
	if (!IsValidMapName(name))
		return std::unexpected(StrCat("Not a valid map name: ", name));
	std::expected<std::string, std::string> dun = ReadWholeFile(CustomMapDunPath(name));
	if (!dun.has_value())
		return std::unexpected(dun.error());
	const std::string &d = *dun;
	if (d.size() < 4)
		return std::unexpected("map.dun is too short");
	auto u16 = [&d](size_t index) -> uint16_t {
		const size_t at = index * 2;
		if (at + 1 >= d.size())
			return 0;
		return static_cast<uint16_t>(static_cast<uint8_t>(d[at]) | (static_cast<uint8_t>(d[at + 1]) << 8));
	};
	CustomMap map;
	map.name = std::string(name);
	map.Resize(u16(0), u16(1));
	if (map.width != u16(0) || map.height != u16(1))
		return std::unexpected("map.dun has an unsupported size");
	const size_t tileCount = static_cast<size_t>(map.width) * map.height;
	const size_t big = static_cast<size_t>(map.width) * 2 * map.height * 2;
	for (size_t i = 0; i < tileCount; i++)
		map.tiles[i] = u16(2 + i);
	const size_t monstersAt = 2 + tileCount + big;
	for (size_t i = 0; i < big; i++) {
		map.monsters[i] = u16(monstersAt + i);
		map.objects[i] = u16(monstersAt + big + i);
	}
	if (std::expected<std::string, std::string> meta = ReadWholeFile(MapFolder(name) + "map.ini"); meta.has_value())
		ParseMeta(*meta, map);
	if (std::expected<std::string, std::string> plan = ReadWholeFile(MapFolder(name) + "map.plan"); plan.has_value() && plan->size() == map.plan.size()) {
		for (size_t i = 0; i < map.plan.size(); i++)
			map.plan[i] = static_cast<uint8_t>((*plan)[i]);
	}
	if (std::expected<std::string, std::string> over = ReadWholeFile(MapFolder(name) + "map.over"); over.has_value() && over->size() == map.overrides.size() * 2) {
		for (size_t i = 0; i < map.overrides.size(); i++)
			map.overrides[i] = static_cast<uint16_t>(static_cast<uint8_t>((*over)[i * 2]) | (static_cast<uint8_t>((*over)[i * 2 + 1]) << 8));
	}
	if (std::expected<std::string, std::string> hue = ReadWholeFile(MapFolder(name) + "map.hue"); hue.has_value() && hue->size() == map.hues.size()) {
		for (size_t i = 0; i < map.hues.size(); i++)
			map.hues[i] = static_cast<int8_t>((*hue)[i]);
	}
	if (std::expected<std::string, std::string> fence = ReadWholeFile(MapFolder(name) + "map.fence"); fence.has_value() && fence->size() == map.fences.size()) {
		for (size_t i = 0; i < map.fences.size(); i++)
			map.fences[i] = static_cast<uint8_t>((*fence)[i]) <= 2 ? static_cast<uint8_t>((*fence)[i]) : 0;
	}
	if (std::expected<std::string, std::string> dec = ReadWholeFile(MapFolder(name) + "map.decor"); dec.has_value()) {
		// "DCR1", layer count, then per layer: ids (u16 each) and hues (one byte each) for every map piece.
		const std::string &d = *dec;
		const size_t cells = map.tiles.size();
		if (d.size() >= 5 && d.compare(0, 4, "DCR1") == 0) {
			const int layers = std::min(static_cast<int>(static_cast<uint8_t>(d[4])), MapDecorLayerCount);
			size_t at = 5;
			for (int layer = 0; layer < layers && at + cells * 3 <= d.size(); layer++) {
				for (size_t i = 0; i < cells; i++, at += 2)
					map.decor[layer][i] = static_cast<uint16_t>(static_cast<uint8_t>(d[at]) | (static_cast<uint8_t>(d[at + 1]) << 8));
				for (size_t i = 0; i < cells; i++, at++)
					map.decorHues[layer][i] = static_cast<int8_t>(d[at]);
			}
		}
	}
	map.name = std::string(name);
	return map;
}

std::expected<void, std::string> WriteCustomMap(const CustomMap &map)
{
	if (!IsValidMapName(map.name))
		return std::unexpected(StrCat("Not a valid map name: ", map.name));
	const std::string folder = MapFolder(map.name);
	RecursivelyCreateDir(folder.c_str());
	if (std::expected<void, std::string> r = WriteWholeFile(folder + "map.dun", BuildDun(map)); !r.has_value())
		return r;
	if (!map.plan.empty()) {
		const std::string plan(map.plan.begin(), map.plan.end());
		if (std::expected<void, std::string> r = WriteWholeFile(folder + "map.plan", plan); !r.has_value())
			return r;
	}
	if (!map.overrides.empty()) {
		std::string over;
		for (const uint16_t v : map.overrides)
			PutU16(over, v);
		if (std::expected<void, std::string> r = WriteWholeFile(folder + "map.over", over); !r.has_value())
			return r;
	}
	if (map.hues.size() == map.tiles.size()) {
		// Colour shifts painted in the editor (one signed byte per map piece).
		const std::string hue(map.hues.begin(), map.hues.end());
		if (std::expected<void, std::string> r = WriteWholeFile(folder + "map.hue", hue); !r.has_value())
			return r;
	}
	{
		std::string dec = "DCR1";
		dec.push_back(static_cast<char>(MapDecorLayerCount));
		for (int layer = 0; layer < MapDecorLayerCount; layer++) {
			for (size_t i = 0; i < map.tiles.size(); i++)
				PutU16(dec, i < map.decor[layer].size() ? map.decor[layer][i] : 0);
			for (size_t i = 0; i < map.tiles.size(); i++)
				dec.push_back(static_cast<char>(i < map.decorHues[layer].size() ? map.decorHues[layer][i] : 0));
		}
		if (std::expected<void, std::string> r = WriteWholeFile(folder + "map.decor", dec); !r.has_value())
			return r;
	}
	if (map.fences.size() == map.tiles.size() * 4) {
		const std::string fence(map.fences.begin(), map.fences.end());
		if (std::expected<void, std::string> r = WriteWholeFile(folder + "map.fence", fence); !r.has_value())
			return r;
	}
	return WriteWholeFile(folder + "map.ini", BuildMeta(map));
}

std::string TristramLayerPath()
{
	const std::string campaign = ActiveCampaignDirectory();
	return campaign.empty() ? std::string {} : campaign + "tristram.ini";
}

CustomMap ReadTristramLayer()
{
	CustomMap layer;
	layer.name = std::string(TristramLayerName);
	layer.style = DTYPE_TOWN;
	layer.Resize(40, 40);
	if (const std::string path = TristramLayerPath(); !path.empty()) {
		if (std::expected<std::string, std::string> meta = ReadWholeFile(path); meta.has_value())
			ParseMeta(*meta, layer);
	}
	layer.name = std::string(TristramLayerName);
	layer.style = DTYPE_TOWN;
	return layer;
}

std::expected<void, std::string> WriteTristramLayer(const CustomMap &layer)
{
	const std::string dir = ActiveCampaignDirectory();
	if (dir.empty())
		return std::unexpected("Open a campaign first: Diablo: Original keeps Tristram as it is.");
	RecursivelyCreateDir(dir.c_str());
	std::string meta = "# Tristram's own layer (map editor): portals, doors and arrival points added to the town.\n"
	                   "# Positions are world tiles in the classic layout; each one follows its nearest building.\n";
	for (const MapEntryPoint &e : layer.entries)
		meta += StrCat("entry=", e.name, ",", e.tile.x, ",", e.tile.y, "\n");
	for (const MapPortal &p : layer.portals)
		meta += StrCat("portal=", p.tile.x, ",", p.tile.y, ",", p.door ? 1 : 0, ",", LinkKindName(p.to.kind), ",", p.to.map, ",", p.to.entry, ",", p.to.level, "\n");
	return WriteWholeFile(TristramLayerPath(), meta);
}

std::string_view MapStyleName(dungeon_type style)
{
	switch (style) {
	case DTYPE_TOWN: return "town";
	case DTYPE_CATACOMBS: return "catacombs";
	case DTYPE_CAVES: return "caves";
	case DTYPE_HELL: return "hell";
	case DTYPE_CRYPT: return "crypt";
	case DTYPE_NEST: return "nest";
	case DTYPE_FOREST: return "wilds";
	default: return "cathedral";
	}
}

std::optional<dungeon_type> ParseMapStyle(std::string_view name)
{
	if (name == "town") return DTYPE_TOWN;
	if (name == "cathedral") return DTYPE_CATHEDRAL;
	if (name == "catacombs") return DTYPE_CATACOMBS;
	if (name == "caves") return DTYPE_CAVES;
	if (name == "hell") return DTYPE_HELL;
	if (name == "crypt") return DTYPE_CRYPT;
	if (name == "nest") return DTYPE_NEST;
	if (name == "wilds") return DTYPE_FOREST;
	return std::nullopt;
}

// --- Playing ---------------------------------------------------------------------------------

namespace {

/** Wilds: town tiles painted like a town, but a fighting area (the land outside a town). */
std::expected<void, std::string> LoadWildsLevel()
{
	InitLevelGlobals();
	PaintTownStyleTiles(&*Active);
	InitTownStylePieces();
	// Chests (and any other objects) placed in the editor.
	{
		const std::string dun = BuildDun(*Active);
		std::vector<uint16_t> data(dun.size() / 2);
		std::memcpy(data.data(), dun.data(), data.size() * 2);
		RETURN_IF_ERROR(SetMapObjects(data.data(), 0, 0));
	}
	dminPosition = { 14, 14 };
	dmaxPosition = WorldTilePosition { static_cast<WorldTileCoord>(18 + 2 * Active->width), static_cast<WorldTileCoord>(18 + 2 * Active->height) };
	const Point arrival = ActiveCustomMapArrival();
	ViewPosition = arrival;
	DRLG_InitTrans();
	LoadRndLvlPal(DTYPE_FOREST, 0);
	numtrigs = 0;
	trigflag = false;
	PortalsPlaced = false;
	LastHeroTile = arrival;
	return {};
}

} // namespace

const CustomMap *ActiveCustomMap()
{
	return Active ? &*Active : nullptr;
}

std::string CurrentMapWeather()
{
	if (std::optional<std::string> edited = EditorMapWeather(); edited)
		return *edited;
	if (setlevel && setlvlnum == SL_CUSTOM && Active)
		return Active->weather;
	if (!setlevel && currlevel == 0 && leveltype == DTYPE_TOWN && InCustomTown()) {
		const CustomMap *town = CurrentCustomTownMap();
		return town != nullptr ? town->weather : std::string {};
	}
	return {};
}

void SetActiveCustomMap(CustomMap map, std::string_view entry)
{
	Active = std::move(map);
	ActiveEntry = std::string(entry);
}

void ClearActiveCustomMap()
{
	Active.reset();
	ActiveEntry.clear();
}

std::string_view MapSideName(int side)
{
	switch (side) {
	case 0: return "north-east";
	case 1: return "south-east";
	case 2: return "south-west";
	default: return "north-west";
	}
}

bool OnMapSide(const CustomMap &map, int side, Point tile, int depth)
{
	const int x0 = 16, y0 = 16, x1 = 16 + 2 * map.width - 1, y1 = 16 + 2 * map.height - 1;
	if (tile.x < x0 || tile.x > x1 || tile.y < y0 || tile.y > y1)
		return false;
	switch (side) {
	case 0: return tile.y < y0 + depth;
	case 1: return tile.x > x1 - depth;
	case 2: return tile.y > y1 - depth;
	default: return tile.x < x0 + depth;
	}
}

std::optional<MapLink> OuterEdgeLink(const CustomMap &map, Point tile)
{
	if (map.outer.empty() || !OnMapSide(map, map.outerSide, tile, 2))
		return std::nullopt;
	std::expected<CustomMap, std::string> other = ReadCustomMap(map.outer);
	if (!other.has_value())
		return std::nullopt;
	// Come out at the same place along the matching edge of the other map, a few steps in.
	const bool alongX = map.outerSide == 0 || map.outerSide == 2;
	const int along = alongX ? tile.x - 16 : tile.y - 16;
	const int ox1 = 16 + 2 * other->width - 1, oy1 = 16 + 2 * other->height - 1;
	constexpr int Depth = 4;
	Point at;
	switch ((map.outerSide + 2) % 4) {
	case 0: at = { std::clamp(16 + along, 16, ox1), 16 + Depth }; break;
	case 1: at = { ox1 - Depth, std::clamp(16 + along, 16, oy1) }; break;
	case 2: at = { std::clamp(16 + along, 16, ox1), oy1 - Depth }; break;
	default: at = { 16 + Depth, std::clamp(16 + along, 16, oy1) }; break;
	}
	return MapLink { MapLinkKind::Map, map.outer, StrCat("@", at.x, ",", at.y), 1 };
}

void DevWalkToOuterEdge(const CustomMap &map)
{
#ifdef _DEBUG
	// Test hook: DEVILUTIONX_DEV_WALKEDGE = walk the hero straight at the edge leading to the outer wilds (or back).
	if (std::getenv("DEVILUTIONX_DEV_WALKEDGE") == nullptr || map.outer.empty() || MyPlayer == nullptr)
		return;
	static int tick = 0;
	Player &player = *MyPlayer;
	if (player._pLvlChanging || player._pmode != PM_STAND || ++tick % 30 != 0)
		return;
	Point step = player.position.tile;
	switch (map.outerSide) {
	case 0: step.y = std::max(16, step.y - 8); break;
	case 1: step.x = std::min(16 + 2 * map.width - 1, step.x + 8); break;
	case 2: step.y = std::min(16 + 2 * map.height - 1, step.y + 8); break;
	default: step.x = std::max(16, step.x - 8); break;
	}
	LogInfo("WALKEDGE {} at {},{}", map.name, player.position.tile.x, player.position.tile.y);
	NetSendCmdLoc(MyPlayerId, true, CMD_WALKXY, step);
#else
	(void)map;
#endif
}

std::optional<Point> ParseArrivalTile(std::string_view entry)
{
	if (entry.size() < 4 || entry[0] != '@')
		return std::nullopt;
	const size_t comma = entry.find(',');
	if (comma == std::string_view::npos)
		return std::nullopt;
	const ParseIntResult<int> x = ParseInt<int>(entry.substr(1, comma - 1));
	const ParseIntResult<int> y = ParseInt<int>(entry.substr(comma + 1));
	if (!x.has_value() || !y.has_value())
		return std::nullopt;
	return Point { *x, *y };
}

Point ActiveCustomMapArrival()
{
	if (!Active)
		return { 16 + 20, 16 + 20 };
	if (const std::optional<Point> at = ParseArrivalTile(ActiveEntry); at) {
		// From the town next door: the nearest open ground to that spot.
		for (int r = 0; r <= 8; r++) {
			for (int dy = -r; dy <= r; dy++) {
				for (int dx = -r; dx <= r; dx++) {
					const Point t = *at + Displacement { dx, dy };
					if (std::max(std::abs(dx), std::abs(dy)) == r && InDungeonBounds(t) && IsTileWalkable(t))
						return t;
				}
			}
		}
		return *at;
	}
	if (const MapEntryPoint *e = Active->FindEntry(ActiveEntry); e != nullptr)
		return e->tile;
	// No entry point: the middle of the map.
	return { 16 + Active->width, 16 + Active->height };
}

std::expected<void, std::string> TravelToCustomMap(std::string_view name, std::string_view entry)
{
	std::expected<CustomMap, std::string> map = ReadCustomMap(name);
	if (!map.has_value())
		return std::unexpected(map.error());
	if (map->style == DTYPE_TOWN)
		return TravelToCustomTown(name, entry);
	SetActiveCustomMap(std::move(*map), entry);
	setlvltype = Active->style;
	StartNewLvl(*MyPlayer, WM_DIABSETLVL, SL_CUSTOM);
	return {};
}

void FollowMapLink(const MapLink &link)
{
	Player &player = *MyPlayer;
	switch (link.kind) {
	case MapLinkKind::Map:
		if (std::expected<void, std::string> r = TravelToCustomMap(link.map, link.entry); !r.has_value())
			LogError("Portal: {}", r.error());
		break;
	case MapLinkKind::Town:
		TravelToTristram(link.entry);
		break;
	case MapLinkKind::Dungeon:
		StartNewLvl(player, WM_DIABNEXTLVL, std::clamp(link.level, 1, 16));
		break;
	default:
		break;
	}
}

std::expected<void, std::string> LoadActiveCustomMapLevel()
{
	if (!Active)
		return std::unexpected("No custom map is active.");
	if (Active->style == DTYPE_FOREST)
		return LoadWildsLevel();
	// The level loaders read a .dun file: write the active map (which may hold unsaved edits).
	const std::string dir = CustomMapsDirectory();
	RecursivelyCreateDir(dir.c_str());
	const std::string path = AbsolutePath(dir + ".active.dun");
	if (std::expected<void, std::string> r = WriteWholeFile(path, BuildDun(*Active)); !r.has_value())
		return r;
	const Point arrival = ActiveCustomMapArrival();
	setlvltype = Active->style;
	switch (Active->style) {
	case DTYPE_CATACOMBS:
		RETURN_IF_ERROR(LoadL2Dungeon(path.c_str(), arrival));
		break;
	case DTYPE_CAVES:
	case DTYPE_NEST:
		RETURN_IF_ERROR(LoadL3Dungeon(path.c_str(), arrival));
		break;
	case DTYPE_HELL:
		RETURN_IF_ERROR(LoadL4Dungeon(path.c_str(), arrival));
		break;
	default:
		RETURN_IF_ERROR(LoadL1Dungeon(path.c_str(), arrival));
		break;
	}
	// See-through walls: flood the floor areas like the level generators do.
	DRLG_InitTrans();
	FloodTransparencyValues(MapStyleFloorPiece(Active->style));
	LoadRndLvlPal(Active->style, 0);
	numtrigs = 0;
	trigflag = false;
	PortalsPlaced = false;
	LastHeroTile = arrival;
	return {};
}

std::vector<Point> MapChests(const CustomMap &map)
{
	std::vector<Point> chests;
	const int w = map.width * 2, h = map.height * 2;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const size_t i = static_cast<size_t>(y) * w + x;
			if (i < map.objects.size() && map.objects[i] == MapChestObjectId)
				chests.push_back({ 16 + x, 16 + y });
		}
	}
	return chests;
}

void SetMapChest(CustomMap &map, Point tile, bool chest)
{
	const int w = map.width * 2, h = map.height * 2;
	const int x = tile.x - 16, y = tile.y - 16;
	if (x < 0 || y < 0 || x >= w || y >= h)
		return;
	const size_t i = static_cast<size_t>(y) * w + x;
	if (i < map.objects.size())
		map.objects[i] = chest ? MapChestObjectId : 0;
}

std::string CampaignMusicDirectory()
{
	const std::string campaign = ActiveCampaignDirectory();
	if (campaign.empty()) // Diablo: Original's maps
		return StrCat(paths::BasePath(), "mods" DIRECTORY_SEPARATOR_STR "custommaps" DIRECTORY_SEPARATOR_STR "music" DIRECTORY_SEPARATOR_STR);
	return StrCat(campaign, "music" DIRECTORY_SEPARATOR_STR);
}

bool IsSongFile(std::string_view name)
{
	std::string lower(name);
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return lower.size() > 4 && (lower.ends_with(".ogg") || lower.ends_with(".wav") || lower.ends_with(".mp3"));
}

std::expected<std::string, std::string> ImportCampaignSong(const std::string &sourcePath)
{
	// The file's own name, kept to letters, digits, spaces and a few marks.
	const size_t slash = sourcePath.find_last_of("/\\");
	const std::string file = slash == std::string::npos ? sourcePath : sourcePath.substr(slash + 1);
	if (!IsSongFile(file))
		return std::unexpected(StrCat(file, " is not a song the game can play (use .ogg, .mp3 or .wav)."));
	const size_t dot = file.find_last_of('.');
	std::string stem;
	for (const char c : file.substr(0, dot)) {
		const unsigned char u = static_cast<unsigned char>(c);
		if (std::isalnum(u) != 0 || c == ' ' || c == '-' || c == '_' || c == '(' || c == ')')
			stem.push_back(c);
		else if (u < 0x80 && !stem.empty() && stem.back() != '_')
			stem.push_back('_');
	}
	while (!stem.empty() && (stem.back() == ' ' || stem.back() == '_'))
		stem.pop_back();
	if (stem.size() > 48)
		stem.resize(48);
	if (stem.empty())
		stem = "song";
	std::string ext = file.substr(dot);
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

	const std::expected<std::string, std::string> data = ReadWholeFile(sourcePath);
	if (!data || data->empty())
		return std::unexpected(StrCat("Cannot read ", sourcePath));
	const std::string dir = CampaignMusicDirectory();
	RecursivelyCreateDir(dir.c_str());
	std::string name;
	for (int n = 1; n < 1000; n++) {
		name = n == 1 ? StrCat(stem, ext) : StrCat(stem, " (", n, ")", ext);
		const std::string path = dir + name;
		if (!FileExists(path.c_str()))
			break;
		if (const std::expected<std::string, std::string> there = ReadWholeFile(path); there && *there == *data)
			return name; // already uploaded
	}
	const std::string path = dir + name;
	FILE *f = OpenFile(path.c_str(), "wb");
	if (f == nullptr)
		return std::unexpected(StrCat("Cannot write ", path));
	const bool ok = std::fwrite(data->data(), 1, data->size(), f) == data->size();
	std::fclose(f);
	if (!ok) {
		RemoveFile(path.c_str());
		return std::unexpected(StrCat("Cannot write ", path));
	}
	std::string why;
	if (!music_file_plays(path, why)) {
		RemoveFile(path.c_str());
		return std::unexpected(StrCat(file, " does not play: ", why.substr(0, why.find('\n'))));
	}
	return name;
}

std::string CampaignSongPath(std::string_view song)
{
	return AbsolutePath(StrCat(CampaignMusicDirectory(), song));
}

std::vector<std::string> MapMusicFiles(const CustomMap &map)
{
	std::vector<std::string> files;
	const std::string dir = CampaignMusicDirectory();
	for (const std::string &song : map.music) {
		if (song.empty() || song.find_first_of("/\\") != std::string::npos)
			continue;
		const std::string path = dir + song;
		if (FileExists(path.c_str()))
			files.push_back(AbsolutePath(path));
	}
	return files;
}

std::vector<std::string> ListCampaignMusic()
{
	std::vector<std::string> songs;
	const std::string dir = CampaignMusicDirectory();
	if (dir.empty() || !DirectoryExists(dir.c_str()))
		return songs;
	for (std::string &name : ListFiles(dir.c_str())) {
		std::string lower = name;
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (IsSongFile(lower))
			songs.push_back(std::move(name));
	}
	std::sort(songs.begin(), songs.end());
	return songs;
}

void PlayCurrentLevelMusic(uint8_t track)
{
	const CustomMap *map = nullptr;
	if (const CustomMap *edited = EditorMusicMap(); edited != nullptr)
		map = edited; // the map editor's map, with its latest songs
	else if (setlevel && setlvlnum == SL_CUSTOM)
		map = ActiveCustomMap();
	else if (!setlevel && currlevel == 0 && InCustomTown())
		map = CurrentCustomTownMap();
	if (map == nullptr) {
		music_crossfade_to(static_cast<_music_id>(track));
		return;
	}
	music_crossfade_to(static_cast<_music_id>(track), MapMusicFiles(*map), map->musicFade * 1000);
}

void UpdateCustomMapPortals()
{
	if (!Active || !setlevel || setlvlnum != SL_CUSTOM || MyPlayer == nullptr)
		return;
	Player &player = *MyPlayer;
	if (!PortalsPlaced) {
		PortalsPlaced = true;
		for (const MapPortal &portal : Active->portals) {
			if (!portal.door)
				AddMissile(portal.tile, portal.tile, Direction::South, MissileID::RedPortal, TARGET_MONSTERS, player.getId(), 0, 0);
		}
	}
	DevWalkToOuterEdge(*Active);
	if (player._pLvlChanging || player._pmode != PM_STAND)
		return;
	const Point tile = player.position.tile;
	if (tile == LastHeroTile)
		return; // only when stepping onto it (not when arriving on it)
	LastHeroTile = tile;
	for (const MapPortal &portal : Active->portals) {
		if (portal.tile == tile && portal.to.kind != MapLinkKind::None) {
			FollowMapLink(portal.to);
			return;
		}
	}
	// Walking off the edge that borders its town.
	if (const std::optional<MapLink> back = OuterEdgeLink(*Active, tile); back) {
		if (MapEditorRunning())
			MapEditorNotice(StrCat("The hero would walk back to ", Active->outer, " here (not while editing)."));
		else
			FollowMapLink(*back);
	}
}

} // namespace devilution
