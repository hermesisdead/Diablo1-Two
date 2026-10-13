/**
 * @file action_mode.hpp
 *
 * Optional "action mode": a close-up camera over the original isometric art with
 * Souls-like controls: direct movement (WASD / left stick), lock-on, light attack,
 * spell, dodge with stamina and invulnerability frames, quick potions, and a slim HUD.
 */
#pragma once

#include <SDL.h>

#include "controls/controller_buttons.h"
#include "engine/surface.hpp"

namespace devilution {

struct Player;

/** @brief Action mode option is on and a game is running. */
bool IsActionMode();

/** @brief Action mode is on and no panel, store, menu or chat needs the input. */
bool IsActionModeFree();

/** @brief The slim HUD is replacing the main panel right now. */
bool IsActionHudShown();

/** @brief The slim HUD option is enabled (independent of panels being open). */
bool IsActionHudEnabled();

void ToggleActionMode();

/** @brief Called every frame before cursor handling: stamina, dodge, facing, camera zoom. */
void ActionModeUpdate();

/** @brief Called after the cursor/controller target was chosen; applies the lock-on target. */
void ActionModeAfterCursor();

/** @brief Keyboard press in game. Returns true if action mode consumed it. */
bool ActionModeHandleKey(SDL_Keycode key);

/** @brief Left mouse press over the game world. Returns true if consumed. */
bool ActionModeHandleLeftMouse(bool isShiftHeld);

/** @brief Points the spell target at the locked enemy before a right-click cast. */
void ActionModePrepareSpellTarget();

/** @brief Controller button press. Returns true if consumed. */
bool ActionModeHandleButton(ControllerButton button);

/** @brief Right stick motion. Returns true if consumed (target switching). */
bool ActionModeHandleRightStick(float x, float y);

/** @brief Per game tick movement. Returns true if action mode handled it. */
bool ActionModeMovement(Player &player);

/** @brief The mouse is steering the 3D camera (cursor hidden). */
bool IsActionMouseCaptured();

/** @brief True while the local player is in the invulnerable part of a dodge. */
bool IsPlayerDodging(const Player &player);

/** @brief Extra walk animation frames to skip (dodge is a fast dash). */
int ActionModeWalkSkipFrames(const Player &player);

void DrawActionHud(const Surface &out);

/** @brief Lock-on marker and enemy health above the locked target. */
void DrawActionWorldOverlay(const Surface &out);

} // namespace devilution
