/**
 * @file world_projector.hpp
 *
 * Maps world positions (tiles, with height) to screen pixels for effects drawn over the game view,
 * in the classic isometric view and in the 3D view.
 */
#pragma once

#include <optional>

#include "engine/point.hpp"
#include "engine/render/orbit_view.hpp"
#include "engine/render/scrollrt.h"
#include "engine/surface.hpp"
#include "levels/dun_tile.hpp"
#include "options.h"
#include "player.h"

namespace devilution {

struct WorldProjector {
	const Surface &out;
	bool orbit;
	int zoom;
	float refX, refY;   // world position of the reference tile centre
	float refSX, refSY; // its screen position (unzoomed)

	explicit WorldProjector(const Surface &surface)
	    : out(surface)
	    , orbit(IsOrbitViewActive())
	    , zoom(*GetOptions().Graphics.zoom ? 2 : 1)
	{
		const Point tile = MyPlayer != nullptr ? Point(MyPlayer->position.tile) : Point { 0, 0 };
		refX = tile.x + 0.5f;
		refY = tile.y + 0.5f;
		const Point s = GetScreenPosition(tile);
		refSX = static_cast<float>(s.x + TILE_WIDTH / 2);
		refSY = static_cast<float>(s.y - TILE_HEIGHT / 2);
	}

	/** Screen point, depth and tile scale for a world position. Depth is 0 in the classic view. */
	[[nodiscard]] std::optional<OrbitScreenPoint> Project(float x, float y, float z) const
	{
		if (orbit)
			return OrbitProjectEx(x, y, z, out);
		const float dx = x - refX, dy = y - refY;
		const float sx = (refSX + (dx - dy) * 32.0f) * zoom;
		const float sy = (refSY + (dx + dy) * 16.0f - z * 32.0f) * zoom;
		return OrbitScreenPoint { Point { static_cast<int>(sx), static_cast<int>(sy) }, 0.0f, 45.0f * zoom };
	}

	/** Ground position under a classic-view screen pixel. */
	void Unproject(float sx, float sy, float &x, float &y) const
	{
		const float a = (sx / zoom - refSX) / 32.0f;
		const float b = (sy / zoom - refSY) / 16.0f;
		x = refX + (a + b) * 0.5f;
		y = refY + (b - a) * 0.5f;
	}

	[[nodiscard]] bool Visible(Point p, float depth) const
	{
		if (p.x < 0 || p.y < 0 || p.x >= out.w() || p.y >= out.h())
			return false;
		return !orbit || OrbitSceneVisible(p, depth);
	}
};

} // namespace devilution
