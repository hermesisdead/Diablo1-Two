/**
 * @file custom_maps/file_dialog.hpp
 *
 * The system's own "open file" window, for bringing the player's files (songs) into the game:
 * Windows' file dialog, the macOS chooser, or zenity / kdialog on Linux.
 */
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace devilution {

/**
 * @brief Asks the player to choose files (blocks until the window closes).
 * @param title Window title.
 * @param extensions Accepted extensions without the dot (e.g. "ogg").
 * @return The chosen files (UTF-8 paths; empty when cancelled), or nullopt when this system has no such window.
 */
[[nodiscard]] std::optional<std::vector<std::string>> ChooseFilesToOpen(std::string_view title, const std::vector<std::string> &extensions);

} // namespace devilution
