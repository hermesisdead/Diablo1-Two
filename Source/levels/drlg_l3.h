/**
 * @file levels/drlg_l3.h
 *
 * Interface of the caves level generation algorithms.
 */
#pragma once

#include <cstdint>
#include <expected>

#include "levels/gendung.h"

namespace devilution {

void CreateL3Dungeon(uint32_t rseed, lvl_entry entry);
void LoadPreL3Dungeon(const char *sFileName);
std::expected<void, std::string> LoadL3Dungeon(const char *sFileName, Point spawn);

/** @brief Map editor: builds caves tiles (40 x 40, [y * 40 + x]) from a floor plan (non-zero = floor). */
void BuildCavesFromFloorPlan(const uint8_t *floorPlan, uint16_t *tiles);
/** @brief Map editor: rebuilds the level's pieces from the tiles in `dungeon`. */
void RefreshCavesPieces();

} // namespace devilution
