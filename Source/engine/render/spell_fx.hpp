/**
 * @file spell_fx.hpp
 *
 * Particles and light for spells, drawn on the full-colour overlay: fire trails embers, lightning
 * throws sparks, magic leaves motes, acid drips, portals shimmer. Projectiles glow and light up
 * what is around them, and they burst with a flash of light where they end.
 */
#pragma once

#include "engine/surface.hpp"

namespace devilution {

/**
 * @brief Updates and draws spell particles and light. Call while the colour overlay is recording,
 * after the world (and weather) were drawn, with the same surface.
 */
void DrawSpellEffects(const Surface &out);

/** @brief Drops all spell particles (level change). */
void ClearSpellEffects();

} // namespace devilution
