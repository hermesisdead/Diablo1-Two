/**
 * @file hd_layer.hpp
 *
 * The HD layer: the new textures drawn at the screen's own resolution. The game draws its world
 * and interface in a small frame (such as 640x480) that the renderer then stretches to the window,
 * which makes everything blocky. With HD textures, the new sprites, the panel and the orbs are
 * drawn again in a second image the size of the window, from their high-resolution art, smoothly
 * scaled, and put on top of the stretched frame. Each item says which pixel of the small frame
 * must still be there for it to show (so walls, people and text drawn in front of it keep it
 * hidden), and world sprites take on the light, weather and fog of the scene where they stand.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_surface.h>
#else
#include <SDL.h>
#endif

#include "engine/point.hpp"
#include "engine/surface.hpp"

namespace devilution {

/** A picture in full colour: straight (not premultiplied) RGBA. */
struct HdImage {
	int width = 0, height = 0;
	std::vector<uint8_t> rgba;
};

/** Frames at their art's resolution, each placed on a canvas of game pixels (the .clx frame's). */
struct HdHiresSprite {
	int canvasW = 0, canvasH = 0;
	struct Frame {
		float x, y, w, h; // where on the canvas, in game pixels (y down from the canvas top)
		HdImage image;
	};
	std::vector<Frame> frames;
};

/** @brief Loads (once) a .hdr file of high-resolution frames; nullptr when there is none. */
[[nodiscard]] std::shared_ptr<const HdHiresSprite> LoadHdHiresSprite(const std::string &path);

/** @brief The HD layer is drawn this frame (HD textures, and the game is stretched by a renderer). */
[[nodiscard]] bool HdLayerActive();
/** @brief World sprites can go on the HD layer now (not with the zoomed view, which is redrawn in place). */
[[nodiscard]] bool HdLayerWorldActive();

/** @brief Start of a frame: no items yet. */
void HdLayerBeginFrame();

/**
 * @brief A picture on the HD layer.
 * @param image Valid until the frame is shown.
 * @param x, y, w, h Where, in pixels of the game's frame (of `out`, so a sub-surface works).
 * @param mask The game pixels it covers, `maskW` wide, from (maskX, maskY) of `out`: the index that
 *   must still be there for it to show, or -1 (not covered).
 * @param tint Take on the scene's light and weather (world sprites; not for the interface).
 * @param cache The picture does not change (its scaled copy is kept).
 */
void HdLayerAdd(const Surface &out, const HdImage *image, float x, float y, float w, float h,
    int maskX, int maskY, int maskW, int maskH, std::vector<int16_t> mask, bool tint, bool cache);

/** @brief Draws the layer over the frame the renderer just drew (in RenderPresent). */
void HdLayerPresent(SDL_Renderer *renderer, const SDL_Surface *frame8, const SDL_Surface *frame32);

/** @brief The size of the layer (the window's drawing area), and pixels per game pixel. */
void HdLayerScale(float &sx, float &sy);

} // namespace devilution
