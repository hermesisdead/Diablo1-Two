/**
 * @file custom_maps/map_editor.hpp
 *
 * The map editor: runs inside a real level built from the map being edited, with the game's own
 * rendering. A free camera, a toolbar and a few tools on top: paint rooms (walls follow the level
 * generator's rules), erase, place entry points, portals and doors and choose where they lead.
 * Save writes the map; Test plays it with a hero; pressing P (or Edit) returns to editing.
 *
 * Opened from the main-menu console (` key): `editor <name> [style]`.
 */
#pragma once

#include <optional>
#include <string>
#include <string_view>

#ifdef USE_SDL3
#include <SDL3/SDL_events.h>
#else
#include <SDL.h>
#endif

#include "engine/surface.hpp"
#include "levels/gendung_defs.hpp"

namespace devilution {

/** @brief A map editor session was asked for from the main menu (open or create `name`). */
void QueueMapEditor(std::string_view name, dungeon_type style, bool startTesting);
/** @brief The main menu should start the queued map editor session. */
[[nodiscard]] bool HasQueuedMapEditor();
/** @brief Starts the queued session (creates the editor hero, starts a game). */
void StartQueuedMapEditor();

/** @brief A map editor session is running (editing or testing). */
[[nodiscard]] bool IsMapEditorSession();
/** @brief The map is being edited (not play-tested): no hero, monsters still. */
[[nodiscard]] bool IsMapEditing();
/** @brief The map editor is open (editing or play-testing its map). */
[[nodiscard]] bool MapEditorRunning();
/** @brief A message on the editor's status line. */
void MapEditorNotice(std::string text);
/** @brief The weather chosen for the map being edited or play-tested (empty: changing), nullopt when not in the editor. */
[[nodiscard]] std::optional<std::string> EditorMapWeather();

/** @brief The editor is placing portals, doors and arrival points in Tristram itself. */
[[nodiscard]] bool IsEditingTristramLayer();

/** @brief Save slot reserved for the editor's hero (hidden from the hero list). */
constexpr unsigned MapEditorHeroSlot = 98;

/** @brief Each game tick. */
void MapEditorTick();
/** @brief Each frame before the view is drawn: free camera, everything lit and visible. */
void MapEditorBeforeDraw();
/** @brief Input while the session runs; returns true when the editor used the event. */
bool MapEditorHandleEvent(const SDL_Event &event);
/** @brief Editor overlay and interface (after the game view was drawn). */
void DrawMapEditor(const Surface &out);

} // namespace devilution
