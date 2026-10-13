/**
 * @file first_person.hpp
 *
 * Optional PSX-style first-person view with billboarded sprites and a Skyrim-style HUD.
 */
#pragma once

#include "engine/surface.hpp"

namespace devilution {

/** @brief True when the first-person option is on and a game level is being played. */
bool IsFirstPersonView();

/** @brief True when first-person view is on and no panel/menu needs the mouse. */
bool IsFirstPersonFree();

/** @brief True when the first-person HUD replaces the main panel. */
bool IsFirstPersonHud();

void ToggleFirstPerson();

/** @brief Per frame: turning (keys + mouse) and movement commands. */
void FirstPersonProcessInput();

/** @brief Replaces CheckCursMove while in first person. Returns true if it handled the cursor. */
bool FirstPersonUpdateCursor();

/** @brief Draws the 3D world into the given surface (whole surface). */
void DrawFirstPersonWorld(const Surface &out);

/** @brief Draws the HUD (compass, bars, target). */
void DrawFirstPersonHud(const Surface &out);

} // namespace devilution
