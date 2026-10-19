#include "engine/render/tree_sway.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include "engine/render/dungeon_mood.hpp"
#include "levels/desert.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "utils/endian_swap.hpp"
#include "utils/is_of.hpp"

namespace devilution {

namespace {

/** Town megatiles (1-based) that are trees: Tristram's trees, the editor's tree brushes and the Darkwood's. */
constexpr uint16_t TreeMegaTiles[] = { 31, 34, 41, 42, 44, 46, 47, 53, 55, 56, 96, 97 };

struct TreePieces {
	const MegaTile *builtFor = nullptr;
	size_t builtCount = 0;
	std::vector<uint8_t> top; // per piece: its highest standing storey (0: not a tree)
};

TreePieces Pieces;

const std::vector<uint8_t> &TreeTops()
{
	if (Pieces.builtFor == pMegaTiles.get() && Pieces.builtCount == MegaTileCount)
		return Pieces.top;
	Pieces.builtFor = pMegaTiles.get();
	Pieces.builtCount = MegaTileCount;
	Pieces.top.assign(MAXTILES, 0);
	if (pMegaTiles == nullptr)
		return Pieces.top;
	for (const uint16_t id : TreeMegaTiles) {
		if (id < 1 || id > MegaTileCount)
			continue;
		const MegaTile &mega = pMegaTiles[id - 1];
		for (const uint16_t raw : { mega.micro1, mega.micro2, mega.micro3, mega.micro4 }) {
			const uint16_t piece = Swap16LE(raw);
			if (piece >= MAXTILES)
				continue;
			int top = 0;
			for (int b = 2; b + 1 < static_cast<int>(MicroTileLen); b += 2) {
				if (LevelCelBlock { DPieceMicros[piece].mt[b] }.hasValue() || LevelCelBlock { DPieceMicros[piece].mt[b + 1] }.hasValue())
					top = b / 2;
			}
			Pieces.top[piece] = static_cast<uint8_t>(top);
		}
	}
	return Pieces.top;
}

/** How hard the wind blows now: the crown's sway in game pixels. */
float WindStrength()
{
	switch (GetDungeonMood()) {
	case DungeonMood::Storm: return 3.0f;
	case DungeonMood::Wind: return 2.6f;
	case DungeonMood::Rain:
	case DungeonMood::Snow: return 1.9f;
	default: return 1.5f;
	}
}

} // namespace

int TreeSwayOffset(uint16_t piece, Point tile, int storey)
{
	if (storey <= 0 || !IsAnyOf(leveltype, DTYPE_TOWN, DTYPE_FOREST) || InDesertRegion())
		return 0;
	const std::vector<uint8_t> &tops = TreeTops();
	if (piece >= tops.size() || tops[piece] == 0)
		return 0;
	// Gusts roll across the land: neighbouring pieces of one tree move almost together.
	const float t = static_cast<float>(SDL_GetTicks() % 3600000u) / 1000.0f;
	const float phase = static_cast<float>(tile.x + tile.y) * 0.11f + static_cast<float>(tile.x - tile.y) * 0.05f;
	const float gust = 0.6f * std::sin(t * 1.25f + phase) + 0.4f * std::sin(t * 0.47f + phase * 1.7f + 1.3f);
	// The trunk stays put; the crown moves most.
	const float height = std::min(1.0f, static_cast<float>(storey) / static_cast<float>(tops[piece]));
	float gain = 1.0f;
#ifdef _DEBUG
	// Test hook: DEVILUTIONX_DEV_SWAYGAIN = multiplier, to see which pieces sway.
	if (const char *g = std::getenv("DEVILUTIONX_DEV_SWAYGAIN"); g != nullptr)
		gain = static_cast<float>(std::atof(g));
#endif
	return static_cast<int>(std::lround(gain * WindStrength() * std::pow(height, 1.6f) * gust));
}

} // namespace devilution
