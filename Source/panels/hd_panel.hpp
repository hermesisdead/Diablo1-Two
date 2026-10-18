/**
 * @file panels/hd_panel.hpp
 *
 * The main panel with the new textures (HD and HD Redux): a re-laid panel from the provided art
 * (buttons, belt, description box and spell button where the game expects them), and orbs that
 * hold real liquid: red for life, blue for mana, with a surface that sloshes when it changes,
 * rising bubbles, depth shading and the glass's reflections over it.
 */
#pragma once

#include "engine/surface.hpp"

namespace devilution {

/** @brief The new-texture panel is in use (setting, and its art could be loaded). */
[[nodiscard]] bool HdPanelActive();

/** @brief Puts the new or the original panel into the panel buffer (after loading, or a texture change). */
void ApplyPanelTextures();

/** @brief Each frame the panel is shown: its full colour (HD textures). */
void DrawHdPanelColour(const Surface &out);

/**
 * @brief Draws the life (or mana) orb's liquid over the panel.
 * @return false when the original flasks are to be drawn instead.
 */
bool DrawHdOrb(const Surface &out, bool life);

} // namespace devilution
