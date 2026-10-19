/**
 * @file tree_sway.hpp
 *
 * Trees in Tristram and the Darkwood sway a little in the wind: the upper parts of a tree are
 * drawn a pixel or two to the side, more towards the crown, following slow gusts that roll across
 * the land. Windy weather and storms make them sway a bit more.
 */
#pragma once

#include <cstdint>

#include "engine/point.hpp"

namespace devilution {

/**
 * @brief How far (game pixels, sideways) to draw one storey of a map piece.
 * @param piece The piece (dPiece value, 0-based).
 * @param tile Where it stands (dPiece coordinates).
 * @param storey Its block row above the ground, 1 for the first one standing.
 * @return 0 for anything that is not a tree, or when nothing sways.
 */
[[nodiscard]] int TreeSwayOffset(uint16_t piece, Point tile, int storey);

} // namespace devilution
