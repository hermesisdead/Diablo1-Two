/**
 * @file town_shuffle.hpp
 *
 * Seeded rearrangement of Tristram's buildings. The classic town layout is cut into
 * building blocks (tile art intact) that are re-placed according to a per-save seed.
 */
#pragma once

#include <cstdint>

#include "engine/displacement.hpp"

namespace devilution {

enum class TownBuilding : uint8_t {
	Cathedral,
	Tavern,
	Smithy,
	Healer,
	GillianHouse,
	WitchHut,
	SpawnHouse,
	FarnhamHouse,
	HouseA,
	HouseB,
	Count,
};

/**
 * @brief Rearranges the buildings already loaded into dPiece.
 *
 * Does nothing (and returns false) for seed 0, which is the classic layout, or when no
 * valid layout is found. Must be called after the town sectors were loaded into dPiece and
 * after the level SOL data is available.
 */
bool ShuffleTown(uint32_t seed);

/** @brief Resets all building shifts to the classic layout. */
void ResetTownLayout();

/** @brief How far a building (and everything standing at it) moved from its classic position. */
Displacement TownBuildingShift(TownBuilding building);

} // namespace devilution
