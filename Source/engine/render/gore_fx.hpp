/**
 * @file gore_fx.hpp
 *
 * Gore on the full-colour overlay: blood sprays when monsters are hit, bursts with chunks when they
 * die, and lands on the floor where it stays for a while (under whatever stands on it). Skeletons
 * shed bone chips, stone creatures rubble, demons dark ichor. A monster that drops a legendary
 * (unique) item explodes, and a beam of golden light marks the item until it is picked up.
 */
#pragma once

#include "engine/point.hpp"
#include "engine/surface.hpp"

namespace devilution {

struct Monster;

/** @brief Updates and draws gore and legendary beams. Call while the colour overlay is recording. */
void DrawGore(const Surface &out);

/** @brief A monster dropped a unique item at a position: it bursts apart. */
void GoreLegendaryDrop(const Monster &monster);

/** @brief Drops all gore (level change). */
void ClearGore();

} // namespace devilution
