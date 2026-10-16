/**
 * @file custom_maps/map_monsters.hpp
 *
 * Monster spawners and bosses of custom maps (map editor). Each time the hero enters a map, every
 * spawner places its pack and every boss appears with its minions, like the areas outside the
 * Rogue Encampment in Diablo II: leave and come back, and they are there again.
 */
#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace devilution {

/** @brief Level load (GetLevelMTypes): adds the monster types the active custom map's spawners and bosses need. */
std::expected<void, std::string> AddCustomMapMonsterTypes();

/** @brief Level load (InitMonsters): places the active custom map's spawner packs and bosses. */
std::expected<void, std::string> PlaceCustomMapMonsters();

/** @brief A monster type a spawner can use. */
struct SpawnableMonster {
	std::string key;   // as saved in map.ini (monster id such as MT_RFALLSP, or "auto")
	std::string label; // shown in the editor
};
/** @brief Monster types for spawners, "auto" first, then by dungeon depth. */
[[nodiscard]] const std::vector<SpawnableMonster> &SpawnableMonsters();
/** @brief Editor label of a spawner's monster key. */
[[nodiscard]] std::string SpawnerLabel(std::string_view key);

/** @brief Unique monsters that can be placed as bosses (names as saved in map.ini), by depth. */
[[nodiscard]] const std::vector<std::string> &PlaceableBosses();

} // namespace devilution
