#include "custom_maps/custom_map.hpp"
#include "custom_maps/custom_town.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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
#include "levels/trigs.h"
#include "missiles.h"
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
	for (const MapEntryPoint &e : map.entries)
		out += StrCat("entry=", e.name, ",", e.tile.x, ",", e.tile.y, "\n");
	for (const MapPortal &p : map.portals)
		out += StrCat("portal=", p.tile.x, ",", p.tile.y, ",", p.door ? 1 : 0, ",", LinkKindName(p.to.kind), ",", p.to.map, ",", p.to.entry, ",", p.to.level, "\n");
	for (const MapNpc &n : map.npcs)
		out += StrCat("npc=", n.type, ",", n.tile.x, ",", n.tile.y, ",", n.direction, "\n");
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
		}
	}
}

/** Floor piece of each style, used to flood the see-through areas behind walls. */
uint8_t FloorPieceOf(dungeon_type style)
{
	switch (style) {
	case DTYPE_CATACOMBS: return 3;
	case DTYPE_CAVES: return 7;
	case DTYPE_HELL: return 6;
	default: return 13;
	}
}

} // namespace

// --- Map data --------------------------------------------------------------------------------

void CustomMap::Resize(int w, int h)
{
	width = std::clamp(w, 4, 40);
	height = std::clamp(h, 4, 40);
	tiles.assign(static_cast<size_t>(width) * height, 0);
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
	return StrCat(paths::BasePath(), "mods" DIRECTORY_SEPARATOR_STR "custommaps" DIRECTORY_SEPARATOR_STR "maps" DIRECTORY_SEPARATOR_STR);
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
	return WriteWholeFile(folder + "map.ini", BuildMeta(map));
}

std::string TristramLayerPath()
{
	return StrCat(paths::BasePath(), "mods" DIRECTORY_SEPARATOR_STR "custommaps" DIRECTORY_SEPARATOR_STR "tristram.ini");
}

CustomMap ReadTristramLayer()
{
	CustomMap layer;
	layer.name = std::string(TristramLayerName);
	layer.style = DTYPE_TOWN;
	layer.Resize(40, 40);
	if (std::expected<std::string, std::string> meta = ReadWholeFile(TristramLayerPath()); meta.has_value())
		ParseMeta(*meta, layer);
	layer.name = std::string(TristramLayerName);
	layer.style = DTYPE_TOWN;
	return layer;
}

std::expected<void, std::string> WriteTristramLayer(const CustomMap &layer)
{
	const std::string dir = StrCat(paths::BasePath(), "mods" DIRECTORY_SEPARATOR_STR "custommaps" DIRECTORY_SEPARATOR_STR);
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
	return std::nullopt;
}

// --- Playing ---------------------------------------------------------------------------------

const CustomMap *ActiveCustomMap()
{
	return Active ? &*Active : nullptr;
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

Point ActiveCustomMapArrival()
{
	if (!Active)
		return { 16 + 20, 16 + 20 };
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
	FloodTransparencyValues(FloorPieceOf(Active->style));
	LoadRndLvlPal(Active->style, 0);
	numtrigs = 0;
	trigflag = false;
	PortalsPlaced = false;
	LastHeroTile = arrival;
	return {};
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
}

} // namespace devilution
