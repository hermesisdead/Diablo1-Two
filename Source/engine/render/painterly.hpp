/**
 * @file painterly.hpp
 *
 * Painterly textures: the world is smoothed into brush strokes (a Kuwahara filter: every pixel
 * takes the average of the calmest of the four areas around it, so flat areas melt together while
 * edges stay sharp) and given a fine film grain, by a GPU shader over the game view. The interface
 * is left as it is (the HD panel and orbs, as in HUD Only).
 *
 * The shader runs on the OpenGL renderer: when the game draws through another one (or none),
 * the world shows as in HUD Only.
 */
#pragma once

#ifdef USE_SDL3
#include <SDL3/SDL_render.h>
#else
#include <SDL.h>
#endif

#include <string_view>

namespace devilution {

/** @brief The Painterly texture mode is chosen. */
[[nodiscard]] bool PainterlyChosen();

/**
 * @brief Draws the painted world over the frame just copied to the window.
 * @param frame The game's frame texture (as copied to the window this frame).
 */
void PainterlyPresent(SDL_Renderer *renderer, SDL_Texture *frame);

/** @brief Whether the shader ran on the last frame, and if not, why (for the F8 panel). */
[[nodiscard]] bool PainterlyRunning();
[[nodiscard]] std::string_view PainterlyStatus();

} // namespace devilution
