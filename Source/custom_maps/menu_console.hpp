/**
 * @file custom_maps/menu_console.hpp
 *
 * A small command console on the main menu, opened with the ` key: `help`, `maps`,
 * `editor <name> [style]` and `play <name>`.
 */
#pragma once

#ifdef USE_SDL3
#include <SDL3/SDL_events.h>
#else
#include <SDL.h>
#endif

namespace devilution {

/** @brief Main-menu events; returns true when the console used the event. */
bool MenuConsoleHandleEvent(SDL_Event &event);
/** @brief Draws the console over the main menu when it is open. */
void DrawMenuConsole();
/** @brief A command asked the main menu to close (to start the map editor). */
[[nodiscard]] bool MenuConsoleWantsMapEditor();
void MenuConsoleClearRequest();
#ifdef _DEBUG
/** @brief Test hook: types DEVILUTIONX_DEV_MENUTYPE into the console. */
void MenuConsoleDevScript();
#endif

} // namespace devilution
