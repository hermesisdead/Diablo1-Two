/**
 * @file color_overlay.hpp
 *
 * Full-colour effects layer. The game draws in 256 palette colours, so effects such as fog or
 * rain used to be faked with an ordered dither (a fixed pattern of fully covered pixels). This
 * layer instead records fog, washes and particles with real transparency, and blends them in
 * full colour onto the 32-bit frame right after the 8-bit frame is copied to it. Pixels that the
 * interface drew over the world afterwards (panels, labels, cursor) are left untouched.
 *
 * When the frame does not go through a 32-bit surface, ColorOverlayBegin() returns false and
 * callers fall back to their palette-based drawing.
 */
#pragma once

#include <cstdint>

#ifdef USE_SDL3
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_surface.h>
#else
#include <SDL.h>
#endif

#include "engine/surface.hpp"

namespace devilution {

struct OverlayColor {
	uint8_t r, g, b;
};

/** @brief The current system (faded, brightness-adjusted) colour of a palette index. */
OverlayColor OverlayPaletteColor(uint8_t index);

/** @brief The frame reaches the screen through a 32-bit surface, so the overlay can be shown. */
[[nodiscard]] bool ColorOverlayAvailable();

/** @brief Start of a game frame: the previous frame's layer no longer applies. */
void ColorOverlayBeginFrame();
/** @brief End of a game frame (after it was presented). */
void ColorOverlayEndFrame();

/**
 * @brief Starts recording effects over a region of the back buffer for this frame.
 * @return false when the overlay is unavailable (callers draw their palette fallback).
 */
bool ColorOverlayBegin(const Surface &region);
[[nodiscard]] bool ColorOverlayRecording();

/** @brief A whole-region wash: before everything else (darkening) or after (flashes). */
void ColorOverlayWash(OverlayColor color, float alpha, bool last);

/** @brief Fog: one colour, per-pixel coverage. Coordinates are region-local. */
void ColorOverlaySetFogColor(OverlayColor color);
void ColorOverlaySetFog(int x, int y, float alpha);
void ColorOverlayAddFog(int x, int y, float alpha);

/** @brief Particles: paint a colour over what is there with partial coverage. */
void ColorOverlayOver(int x, int y, OverlayColor color, float alpha);
/** @brief Particles: add light (glows, sparks). */
void ColorOverlayAdd(int x, int y, OverlayColor color, float amount);

/** @brief The world is complete: remembers its pixels to tell them apart from the interface. */
void ColorOverlayFinish();

/** @brief Called right after the 8-bit back buffer was copied to the 32-bit output. */
void ColorOverlayComposite(SDL_Surface *src, const SDL_Rect *srcRect, SDL_Surface *dst, const SDL_Rect *dstRect);

/** @brief The composited 32-bit frame (for screenshots), or nullptr when the overlay was not shown. */
SDL_Surface *ColorOverlayShownSurface();

} // namespace devilution
