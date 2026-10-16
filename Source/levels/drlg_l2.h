/**
 * @file levels/drlg_l2.h
 *
 * Interface of the catacombs level generation algorithms.
 */
#pragma once

#include <cstdint>
#include <expected>

#include "levels/gendung.h"

namespace devilution {

void CreateL2Dungeon(uint32_t rseed, lvl_entry entry);
void LoadPreL2Dungeon(const char *path);
std::expected<void, std::string> LoadL2Dungeon(const char *path, Point spawn);

/** @brief Map editor: builds catacombs tiles (40 x 40, [y * 40 + x]) from a floor plan (non-zero = floor). */
void BuildCatacombsFromFloorPlan(const uint8_t *floorPlan, uint16_t *tiles);
/** @brief Map editor: rebuilds the level's pieces from the tiles in `dungeon`. */
void RefreshCatacombsPieces();

} // namespace devilution
