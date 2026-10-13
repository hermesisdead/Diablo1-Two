/**
 * @file levels/forest.hpp
 *
 * The Darkwood: a randomly generated forest beyond Tristram's western woods, built from the
 * town tileset. Monsters there come in endless, escalating waves and are chosen to match the
 * hero's level. Single player only.
 */
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include "engine/point.hpp"
#include "engine/surface.hpp"

namespace devilution {

/** @brief The forest gate exists (single player only). */
[[nodiscard]] bool IsForestAvailable();

/** @brief Virtual dungeon level whose monsters the forest uses for a hero of this level. */
[[nodiscard]] int ForestTierForCharacterLevel(int characterLevel);

// --- Town side -------------------------------------------------------------------------------

/** @brief Finds the forest gate on the western edge of town and adds its trigger. */
void AddTownForestTrigger();

/** @brief The town tile the forest gate trigger sits on, once the town was generated. */
[[nodiscard]] std::optional<Point> TownForestGate();

/** @brief Hover text for the town gate; returns true when the cursor is on it. */
bool ForceTownForestTrig();

/** @brief Rolls a new forest for this visit and walks the hero through the gate. */
void EnterForest();

// --- Forest level ----------------------------------------------------------------------------

/** @brief Generates the forest map (dPiece) from the hero's current forest seed. */
void CreateForest(uint32_t seed);

/** @brief Sets up the exit trigger back to town. */
void InitForestTriggers();

/** @brief Hover text for the forest exit; returns true when the cursor is on it. */
bool ForceForestTrig();

/** @brief Adds the monster types of the hero's forest tier to the level. */
std::expected<void, std::string> AddForestMonsterTypes();

/** @brief Resets the wave state (called when the forest level is loaded). */
void ResetForestWaves();

/** @brief Per game tick: spawns waves and keeps them hunting the hero. */
void UpdateForestWaves();

/** @brief Experience multiplier for kills in the current wave (1.0 outside the forest). */
[[nodiscard]] float ForestExperienceMultiplier();

/** @brief Wave counter and banner. */
void DrawForestHud(const Surface &out);

} // namespace devilution
