/**
 * @file map_animals.hpp
 *
 * Animals placed in hand-made town-tiled maps (towns and wilds). Each wanders around the spot it
 * was placed: it strolls to nearby places, stops, grazes or pecks, and never steps onto a fence,
 * a gate or anything solid, so a fenced pen keeps it in. While the map editor is editing they
 * stand still where they were put; in play-testing and in the game they roam.
 */
#pragma once

#include <string_view>
#include <vector>

#include "custom_maps/custom_map.hpp"
#include "engine/point.hpp"
#include "engine/surface.hpp"

namespace devilution {

struct AnimalKindInfo {
	std::string_view key;   // saved in maps
	std::string_view label; // shown in the editor
};

[[nodiscard]] const std::vector<AnimalKindInfo> &AnimalKinds();
[[nodiscard]] std::string_view AnimalLabel(std::string_view key);

/** @brief The animals of the level just built from a map (replaces any others). */
void SetLevelAnimals(const std::vector<MapAnimal> &animals);

/** @brief Moves the animals on; call once per frame before drawing. */
void UpdateAnimals();

/** @brief Draws the animals standing on a world tile (called with that tile, so they sort with walls and people). */
void DrawAnimals(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightTableIndex);

/** @brief Draws an animal standing, feet at a screen point (editor palette). */
void DrawAnimalPicture(const Surface &out, Point feet, std::string_view kind);

/** @brief World tiles the animals of this level stand on now (for tests and the editor). */
[[nodiscard]] std::vector<Point> AnimalPositions();

} // namespace devilution
