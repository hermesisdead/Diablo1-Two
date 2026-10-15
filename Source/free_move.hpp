/**
 * @file free_move.hpp
 *
 * Free movement: the hero and monsters move at any angle instead of hopping from tile centre to
 * tile centre. Every actor keeps its engine tile (used for occupancy, combat, pathing, lights,
 * saving) and gains a continuous position on top of it. The tile follows the continuous position:
 * it switches when the actor crosses into a neighbouring tile, under the same rules as a normal
 * step (walkable, not occupied, no cutting past solid corners), and the body keeps a small margin
 * from walls, so collision with the environment is the same as with tile steps.
 *
 * Single player only; toggled with F (Gameplay option "Free Movement").
 */
#pragma once

#include <cstdint>

#include "engine/direction.hpp"
#include "engine/displacement.hpp"
#include "engine/point.hpp"

namespace devilution {

struct ActorPosition;
struct Monster;
struct Player;

/** @brief Free movement is enabled and allowed (single player). */
[[nodiscard]] bool IsFreeMovement();
void ToggleFreeMovement();

/** @brief Start of a game tick: remembers positions for smooth rendering between ticks. */
void FreeMoveBeginTick();
/** @brief A level was (re)loaded: every actor starts at its tile centre. */
void FreeMoveResetLevel();

// --- Rendering -------------------------------------------------------------------------------

/** @brief Continuous world position for rendering (interpolated between game ticks). */
void GetActorWorldPosition(const ActorPosition &position, float &x, float &y);
/** @brief Screen offset of the drawn actor from its tile (free offset plus any tile-step walk). */
Displacement GetPlayerDrawOffset(const Player &player, bool cameraMode = false);
Displacement GetMonsterDrawOffset(const Monster &monster, bool cameraMode = false);
/** @brief Screen offset of the actor's current (not interpolated) free position from its tile. */
Displacement FreeMoveCurrentScreenOffset(const ActorPosition &position);
/** @brief Sub-tile light offset (1/8 tile units) for the actor's light. */
DisplacementOf<int8_t> GetPlayerLightOffset(const Player &player);
DisplacementOf<int8_t> GetMonsterLightOffset(const Monster &monster);

// --- Hero ------------------------------------------------------------------------------------

/** @brief The walk the player is in is driven by free movement. */
[[nodiscard]] bool IsPlayerFreeWalking(const Player &player);
/** @brief Starts (or continues) walking along player.walkpath; replaces the tile step. */
void FreeMoveStartWalk(Player &player);
/** @brief Per tick walk update; returns true when the walk step ended (like DoWalk). */
bool FreeMoveDoWalk(Player &player);
/** @brief A walk command was handled (path already built): remember the exact clicked point. */
void FreeMoveOnWalkCommand(Player &player, Point target);
/** @brief While the mouse is held on the destination tile, follow the exact cursor point. */
void FreeMoveUpdateHeldTarget(Player &player);
/** @brief Direct movement input (world vector) for this tick: WASD, stick. */
void FreeMoveSetHeading(Player &player, float worldX, float worldY);
/** @brief Direct movement input was released. */
void FreeMoveStopHeading(Player &player);
/** @brief Quick dash (dodge) along a world vector. Returns false if the hero cannot move there. */
bool FreeMoveDash(Player &player, float worldX, float worldY, float distance, float speedMultiplier);
[[nodiscard]] bool FreeMoveIsDashing(const Player &player);

// --- Cursor ----------------------------------------------------------------------------------

/** @brief Exact world point under the mouse (set by the cursor code each frame). */
void SetFreeMoveCursorPoint(float x, float y);
/** @brief The local hero is sending a walk request: remember the exact point under the mouse. */
void FreeMoveNoteWalkRequest(Point target);

// --- Monsters --------------------------------------------------------------------------------

/**
 * @brief Starts a free stride for a monster whose AI chose to walk in direction md (already
 * validated with DirOK). Sets mode, future tile and claim; returns the facing to animate.
 */
Direction FreeMoveStartMonsterStride(Monster &monster, Direction md);
/** @brief Must be called after the walk animation was set: measures the stride length in ticks. */
void FreeMoveSetMonsterStrideTicks(Monster &monster);
/** @brief Per tick stride update; returns true when the stride ended (caller starts standing). */
bool FreeMoveMonsterWalk(Monster &monster);
[[nodiscard]] bool IsMonsterFreeWalking(const Monster &monster);

// --- Helpers ---------------------------------------------------------------------------------

/** @brief Screen vector (x right, y down) to a normalised world vector. */
void ScreenToWorldVector(float screenX, float screenY, float &worldX, float &worldY);
/** @brief Sprite direction closest to a world vector as seen on screen. */
Direction FacingForWorldVector(float worldX, float worldY);

#ifdef _DEBUG
/** @brief Test harness: invariant checks and scripted walking (DEVILUTIONX_DEV_FREECHECK / _FREEWALK). */
void FreeMoveDebugTick();
#endif

} // namespace devilution
