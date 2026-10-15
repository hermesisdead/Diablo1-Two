#pragma once

#include <cstdint>

#include "engine/animationinfo.h"
#include "engine/point.hpp"
#include "engine/world_tile.hpp"

namespace devilution {

/**
 * @brief Continuous ("free movement") position of an actor, in world tile units with tile centres
 * at integer coordinates. Only meaningful while generation matches the current free-movement
 * generation and anchor matches the actor's tile; anything else snaps back to the tile centre.
 * Not saved: a loaded game starts every actor at its tile centre.
 */
struct FreeMovePosition {
	float x = 0;
	float y = 0;
	/** Position at the start of the current game tick, for smooth rendering between ticks. */
	float prevX = 0;
	float prevY = 0;
	WorldTilePosition anchor;
	uint32_t generation = 0;
	/** The current walk is driven by free movement (not by the tile-step animation). */
	bool walking = false;
	// Monster stride: from start to aim over the walk animation.
	/** The last stride could not go where it aimed: the next one follows the plain tile step. */
	bool blocked = false;
	/** This stride follows the plain tile step (no body margin), so it cannot get stuck. */
	bool strict = false;
	bool hasClaim = false;
	int16_t strideTick = 0;
	int16_t strideTicks = 1;
	float startX = 0;
	float startY = 0;
	float aimX = 0;
	float aimY = 0;
	WorldTilePosition claim;
};

struct ActorPosition {
	WorldTilePosition tile;
	/** Future tile position. Set at start of walking animation. */
	WorldTilePosition future;
	/** Tile position of player. Set via network on player input. */
	WorldTilePosition last;
	/** Most recent position in dPlayer. */
	WorldTilePosition old;
	/** Used for referring to position of player when finishing moving one tile (also used to define target coordinates for spells and ranged attacks) */
	WorldTilePosition temp;
	/** Free movement: continuous position (see FreeMovePosition). */
	FreeMovePosition freeMove;

	/** @brief Calculates the offset for the walking animation. */
	DisplacementOf<int8_t> CalculateWalkingOffset(Direction dir, const AnimationInfo &animInfo) const;
	/** @brief Calculates the offset for the walking animation. */
	DisplacementOf<int16_t> CalculateWalkingOffsetShifted4(Direction dir, const AnimationInfo &animInfo) const;
	/** @brief Calculates the offset for the walking animation. */
	DisplacementOf<int16_t> CalculateWalkingOffsetShifted8(Direction dir, const AnimationInfo &animInfo) const;
	/** @brief Returns Pixel velocity while walking. */
	DisplacementOf<int16_t> GetWalkingVelocityShifted4(Direction dir, const AnimationInfo &animInfo) const;
	/** @brief Returns Pixel velocity while walking. */
	DisplacementOf<int16_t> GetWalkingVelocityShifted8(Direction dir, const AnimationInfo &animInfo) const;
};

} // namespace devilution
