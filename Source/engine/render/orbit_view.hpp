/**
 * @file orbit_view.hpp
 *
 * Third-person 3D view for action mode: the level is rebuilt from the game's own tile art
 * (floor diamonds become floor textures, the wall art of each tile's column becomes wall
 * panels on its back edges), viewed through an orbiting Elden Ring style camera. Monsters,
 * the hero, items and missiles stay as their original sprites, drawn as camera-facing
 * cut-outs that show the facing frame matching the viewing angle.
 */
#pragma once

#include <optional>

#include "engine/point.hpp"
#include "engine/surface.hpp"

namespace devilution {

/** @brief The 3D third-person view is enabled for action mode and a level is running. */
bool IsOrbitViewActive();

/**
 * @brief Advances the camera.
 * @param dt seconds since the last frame
 * @param turnX,turnY camera turn input in radians (mouse delta or stick * dt)
 * @param lockTarget tile of the locked enemy, if any
 * @param moving whether the hero is being moved by the player
 */
void OrbitCameraUpdate(float dt, float turnX, float turnY, std::optional<Point> lockTarget, bool moving, float moveAngle, bool manualTurn);

/** @brief Horizontal camera angle in grid space (radians); 0 looks along +x. */
float OrbitCameraYaw();

void OrbitCameraReset();

/** @brief Renders the world (floor, walls, sprites) into out. */
void DrawOrbitView(const Surface &out);

/** @brief Screen position of a world point at the given height (in tiles), for HUD markers. */
std::optional<Point> OrbitProject(float worldX, float worldY, float height, const Surface &out);

struct OrbitScreenPoint {
	Point pos;           // screen position in out's pixels (may be off screen)
	float depth;         // camera distance, comparable with OrbitSceneVisible
	float pixelsPerTile; // screen size of one tile at this distance
};

/** @brief Like OrbitProject, but also returns depth and scale, and keeps off-screen points. */
std::optional<OrbitScreenPoint> OrbitProjectEx(float worldX, float worldY, float height, const Surface &out);

/** @brief True if something at this depth would be in front of the rendered scene at the pixel. */
bool OrbitSceneVisible(Point screen, float depth);

/** @brief World position (tiles, height) of the scene surface drawn at a screen pixel, if any. */
bool OrbitScreenToWorld(Point screen, float &worldX, float &worldY, float &height);

/** @brief Smoothed render position of a monster (tile units, centre of tile = +0.5). */
bool OrbitMonsterRenderPosition(int monsterId, float &x, float &y);

/** @brief Releases cached textures (called on level change automatically). */
void OrbitClearCache();

} // namespace devilution
