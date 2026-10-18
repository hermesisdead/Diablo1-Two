#include "custom_maps/building_library.hpp"

#include <algorithm>
#include <cstdio>

#include "custom_maps/custom_town.hpp"
#include "utils/file_util.h"
#include "utils/log.hpp"
#include "utils/paths.h"
#include "utils/str_cat.hpp"

namespace devilution {

namespace {

/** Pieces of classic Tristram, in its 48x48 megatile grid (inclusive). */
struct ClassicPiece {
	std::string_view name;
	int x0, y0, x1, y1;
};

constexpr ClassicPiece ClassicPieces[] {
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

std::vector<Building> Library;
bool Loaded = false;

constexpr std::string_view Extension = ".bld";

void PutU16(std::string &out, uint16_t v)
{
	out.push_back(static_cast<char>(v & 0xFF));
	out.push_back(static_cast<char>(v >> 8));
}

std::string Encode(const Building &b)
{
	// "BLD1", width, height, megatiles, hues, decor layer count, then per layer its ids and hues.
	const size_t n = static_cast<size_t>(b.width) * b.height;
	std::string out = "BLD1";
	PutU16(out, static_cast<uint16_t>(b.width));
	PutU16(out, static_cast<uint16_t>(b.height));
	for (size_t i = 0; i < n; i++)
		PutU16(out, b.tiles[i]);
	for (size_t i = 0; i < n; i++)
		out.push_back(static_cast<char>(b.hues[i]));
	out.push_back(static_cast<char>(MapDecorLayerCount));
	for (int layer = 0; layer < MapDecorLayerCount; layer++) {
		for (size_t i = 0; i < n; i++)
			PutU16(out, b.decor[layer][i]);
		for (size_t i = 0; i < n; i++)
			out.push_back(static_cast<char>(b.decorHues[layer][i]));
	}
	// Optional sections after the decor: "FNC" then a fence byte per world tile.
	out += "FNC";
	for (size_t i = 0; i < n * 4; i++)
		out.push_back(static_cast<char>(b.fences[i]));
	return out;
}

bool Decode(std::string_view d, Building &b)
{
	if (d.size() < 8 || d.substr(0, 4) != "BLD1")
		return false;
	auto u16 = [&d](size_t at) { return static_cast<uint16_t>(static_cast<uint8_t>(d[at]) | (static_cast<uint8_t>(d[at + 1]) << 8)); };
	const int w = u16(4), h = u16(6);
	if (w < 1 || h < 1 || w > 40 || h > 40)
		return false;
	const size_t n = static_cast<size_t>(w) * h;
	size_t at = 8;
	if (d.size() < at + n * 3 + 1)
		return false;
	b.Resize(w, h);
	for (size_t i = 0; i < n; i++, at += 2)
		b.tiles[i] = u16(at);
	for (size_t i = 0; i < n; i++, at++)
		b.hues[i] = static_cast<int8_t>(d[at]);
	const int layers = std::min(static_cast<int>(static_cast<uint8_t>(d[at++])), MapDecorLayerCount);
	for (int layer = 0; layer < layers && at + n * 3 <= d.size(); layer++) {
		for (size_t i = 0; i < n; i++, at += 2)
			b.decor[layer][i] = u16(at);
		for (size_t i = 0; i < n; i++, at++)
			b.decorHues[layer][i] = static_cast<int8_t>(d[at]);
	}
	if (at + 3 + n * 4 <= d.size() && d.substr(at, 3) == "FNC") {
		at += 3;
		for (size_t i = 0; i < n * 4; i++, at++)
			b.fences[i] = static_cast<uint8_t>(d[at]) <= 2 ? static_cast<uint8_t>(d[at]) : 0;
	}
	return true;
}

std::string PathOf(std::string_view name)
{
	return StrCat(BuildingsDirectory(), name, Extension);
}

void Load()
{
	Library.clear();
	const std::vector<uint16_t> &classic = ClassicTownMegaTiles();
	for (const ClassicPiece &piece : ClassicPieces) {
		Building b;
		b.name = std::string(piece.name);
		b.builtIn = true;
		b.Resize(piece.x1 - piece.x0 + 1, piece.y1 - piece.y0 + 1);
		for (int y = 0; y < b.height; y++) {
			for (int x = 0; x < b.width; x++)
				b.tiles[static_cast<size_t>(y) * b.width + x] = classic[static_cast<size_t>(piece.y0 + y) * 48 + piece.x0 + x];
		}
		Library.push_back(std::move(b));
	}
	std::vector<Building> saved;
	const std::string dir = BuildingsDirectory();
	if (DirectoryExists(dir.c_str())) {
		for (const std::string &file : ListFiles(dir.c_str())) {
			if (file.size() <= Extension.size() || file.compare(file.size() - Extension.size(), Extension.size(), Extension) != 0)
				continue;
			const std::string name = file.substr(0, file.size() - Extension.size());
			if (!IsValidBuildingName(name))
				continue;
			FILE *f = OpenFile((dir + file).c_str(), "rb");
			if (f == nullptr)
				continue;
			std::string data;
			char buffer[4096];
			size_t got;
			while ((got = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
				data.append(buffer, got);
			std::fclose(f);
			Building b;
			if (Decode(data, b)) {
				b.name = name;
				saved.push_back(std::move(b));
			} else {
				LogWarn("Building {} is not a building file", file);
			}
		}
	}
	std::sort(saved.begin(), saved.end(), [](const Building &a, const Building &b) { return a.name < b.name; });
	for (Building &b : saved)
		Library.push_back(std::move(b));
	Loaded = true;
}

} // namespace

void Building::Resize(int w, int h)
{
	width = w;
	height = h;
	const size_t n = static_cast<size_t>(w) * h;
	tiles.assign(n, 0);
	hues.assign(n, 0);
	for (int layer = 0; layer < MapDecorLayerCount; layer++) {
		decor[layer].assign(n, 0);
		decorHues[layer].assign(n, 0);
	}
	fences.assign(n * 4, 0);
}

const std::vector<Building> &Buildings()
{
	if (!Loaded)
		Load();
	return Library;
}

void ReloadBuildings()
{
	Loaded = false;
}

std::string BuildingsDirectory()
{
	return StrCat(paths::BasePath(), "mods" DIRECTORY_SEPARATOR_STR "custommaps" DIRECTORY_SEPARATOR_STR "buildings" DIRECTORY_SEPARATOR_STR);
}

bool IsValidBuildingName(std::string_view name)
{
	if (name.empty() || name.size() > 32 || name.front() == ' ' || name.back() == ' ')
		return false;
	return std::all_of(name.begin(), name.end(), [](char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ' ';
	});
}

bool SavedBuildingExists(std::string_view name)
{
	return IsValidBuildingName(name) && FileExists(PathOf(name).c_str());
}

std::expected<void, std::string> SaveBuilding(const Building &building)
{
	if (!IsValidBuildingName(building.name))
		return std::unexpected(StrCat("Not a valid building name: ", building.name));
	if (building.width < 1 || building.height < 1 || building.tiles.size() != static_cast<size_t>(building.width) * building.height)
		return std::unexpected("The building is empty");
	RecursivelyCreateDir(BuildingsDirectory().c_str());
	const std::string path = PathOf(building.name);
	FILE *f = OpenFile(path.c_str(), "wb");
	if (f == nullptr)
		return std::unexpected(StrCat("Cannot write ", path));
	const std::string data = Encode(building);
	const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
	std::fclose(f);
	ReloadBuildings();
	if (!ok)
		return std::unexpected(StrCat("Cannot write ", path));
	return {};
}

void DeleteSavedBuilding(std::string_view name)
{
	if (SavedBuildingExists(name))
		RemoveFile(PathOf(name).c_str());
	ReloadBuildings();
}

bool IsSceneryStamp(std::string_view name)
{
	return name == "Stream" || name == "Thicket" || name == "Graveyard" || name == "Fountain";
}

} // namespace devilution
