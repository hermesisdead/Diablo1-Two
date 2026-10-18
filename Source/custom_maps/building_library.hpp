/**
 * @file building_library.hpp
 *
 * Buildings for town-tiled maps: the buildings of classic Tristram, and ones saved from the map
 * editor. Saved buildings live in mods/custommaps/buildings/<name>.bld, shared by every map in
 * every campaign, so a building made for one town can be placed in another.
 */
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "custom_maps/custom_map.hpp"

namespace devilution {

struct Building {
	std::string name;
	bool builtIn = false;
	int width = 0;  // in map pieces (2x2 world tiles each)
	int height = 0;
	std::vector<uint16_t> tiles; // width * height megatiles, 0 = keep the ground that is there
	std::vector<int8_t> hues;
	std::array<std::vector<uint16_t>, MapDecorLayerCount> decor;
	std::array<std::vector<int8_t>, MapDecorLayerCount> decorHues;
	std::vector<uint8_t> fences; // (2 * width) * (2 * height) FenceKind per world tile

	/** Sizes every layer for width * height (cleared). */
	void Resize(int w, int h);
};

/** @brief Classic Tristram's buildings, then saved ones by name. */
[[nodiscard]] const std::vector<Building> &Buildings();
/** @brief Re-reads the saved buildings (after saving or deleting one). */
void ReloadBuildings();

[[nodiscard]] std::string BuildingsDirectory();
/** @brief Letters, digits, spaces, '-' and '_', at most 32 characters. */
[[nodiscard]] bool IsValidBuildingName(std::string_view name);
[[nodiscard]] bool SavedBuildingExists(std::string_view name);
std::expected<void, std::string> SaveBuilding(const Building &building);
void DeleteSavedBuilding(std::string_view name);

/** @brief Built-in buildings that are scenery rather than buildings (their pieces still count as decor). */
[[nodiscard]] bool IsSceneryStamp(std::string_view name);

} // namespace devilution
