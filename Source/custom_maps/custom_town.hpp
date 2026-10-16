/**
 * @file custom_maps/custom_town.hpp
 *
 * Towns made in the map editor, and the links (portals, doors, arrival points) placed in towns.
 *
 * A custom town is a real town level (level 0), like Sand Tristram: safe, with townspeople the
 * map places, its own ground items, and town portals that return to it. The hero's current town
 * is remembered with the hero: whichever town they last walked into is "town" for town portals,
 * stairs up from level 1 and respawning.
 *
 * Tristram itself can be given portals, doors and arrival points too (editor: `editor tristram`).
 * They are placed on the classic layout and each one follows its nearest building when a save's
 * Tristram is rearranged.
 *
 * Single player only.
 */
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/point.hpp"
#include "levels/gendung_defs.hpp"

namespace devilution {

struct CustomMap;
struct TownerDataEntry;

/** @brief The hero is in (or based in) a custom town rather than Tristram. */
[[nodiscard]] bool InCustomTown();
/** @brief Name of the hero's custom town (empty: Tristram or Sand Tristram). */
[[nodiscard]] const std::string &CurrentCustomTown();
void SetCurrentCustomTown(std::string name);
/** @brief The custom town map being played, if InCustomTown(). */
[[nodiscard]] const CustomMap *CurrentCustomTownMap();

/** @brief Travel to a custom town, arriving at a named arrival point (or its first one). */
std::expected<void, std::string> TravelToCustomTown(std::string_view name, std::string_view entry);
/** @brief Travel back to Tristram, arriving at one of its added arrival points (or the usual spot). */
void TravelToTristram(std::string_view entry);

/** @brief Map editor: play or edit this (possibly unsaved) town map. */
void SetCustomTownMapForEditor(const CustomMap &map);
/** @brief Map editor: use this Tristram layer instead of the saved one (nullptr: the saved one). */
void SetTristramLayerForEditor(const CustomMap *layer);

/** @brief Map editor: (re)enter the town given to SetCustomTownMapForEditor, at an arrival point. */
void EnterCustomTownForEditor(std::string_view entry);

/** @brief Called by CreateTown for a custom town: builds the town from its map. */
void CreateCustomTownLevel(lvl_entry entry);
/** @brief Repaints the current custom town's tiles into the level (pieces only, nothing moves). */
void PaintCustomTownTiles();
/** @brief Paints a town-tiled map (a town or wilds) into the level's pieces, filler around it. */
void PaintTownStyleTiles(const CustomMap *map);
/**
 * @brief Called at the end of CreateTown (Tristram and custom towns): sets up the town's links and
 * moves the view to a requested arrival point.
 */
void FinishTownLinks();

/** @brief Each game tick in town: portals appear, stepping onto a portal or door travels. */
void UpdateTownLinks();
/** @brief The cursor is over a town portal or door: shows where it leads. */
bool ForceTownLinkTrig();

/** @brief Where a player's town portal opens in a custom town. */
[[nodiscard]] Point CustomTownPortalTile(size_t playerId);
/** @brief Save slot for a custom town's ground items. */
[[nodiscard]] uint8_t CustomTownSaveSlot();

/** @brief Replaces the townspeople table with the people the custom town places. */
void ApplyCustomTownTowners(std::vector<TownerDataEntry> &entries);

/** @brief Townspeople that can be placed in a town map (type names as in towners.tsv). */
struct PlaceableTowner {
	std::string_view type;
	std::string_view label;
};
[[nodiscard]] const std::vector<PlaceableTowner> &PlaceableTowners();

/** @brief Classic Tristram as town megatiles (1-based ids), 48 x 48, row by row. */
[[nodiscard]] const std::vector<uint16_t> &ClassicTownMegaTiles();

/** @brief Resolves an added Tristram position (classic layout) to this save's Tristram. */
[[nodiscard]] Point ResolveTristramPosition(Point classic);

} // namespace devilution
