/**
 * @file custom_maps/custom_npcs.hpp
 *
 * Townspeople made in the Campaign Editor (NPCs): a name, a look borrowed from one of the game's
 * townsfolk with its colours swapped, lines they say (each with an optional voice recording from
 * the campaign's voice folder), and optionally a shop. Saved in the campaign's npcs.ini and placed
 * in town maps with the map editor's People tool (as "custom:<id>").
 */
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_pixels.h>
#else
#include <SDL.h>
#endif

#include "engine/point.hpp"

namespace devilution {

struct Player;
struct Towner;
struct TownerDataEntry;

/** @brief Colours of the sprite to change: which colours, and into what. */
struct NpcRecolor {
	int source = 0;        // index into NpcColorSources(); 0 = no change
	int hue = 0;           // degrees: the new hue (or, for "All colours", how far to turn them)
	int saturation = 100;  // percent
	int brightness = 100;  // percent
};

struct NpcLine {
	std::string text;
	std::string voice; // file in the campaign's voice folder (.wav or .ogg), or empty
};

enum class NpcShop : uint8_t {
	None,
	Smith,
	Healer,
	Witch,
	Boy,
	Gambler,
	Storyteller,
};

struct CustomNpc {
	std::string id;
	std::string name = "Stranger";
	std::string look = "TOWN_BMAID";
	std::array<NpcRecolor, 2> recolor {};
	NpcShop shop = NpcShop::None;
	std::vector<NpcLine> lines;
};

/** @brief A look: one of the game's townsfolk sprites. */
struct NpcLook {
	std::string_view key;   // towner type it comes from
	std::string_view label; // shown in the editor
	std::string_view path;  // sprite file (without .cel)
	int frames;
	int delay;
};

/** @brief Looks whose sprites are in the game data. */
[[nodiscard]] const std::vector<NpcLook> &NpcLooks();
[[nodiscard]] const NpcLook *FindNpcLook(std::string_view key);
/** @brief Names of the colour groups a recolour can pick ("No change", "All colours", "Reds", ...). */
[[nodiscard]] const std::vector<std::string_view> &NpcColorSources();
[[nodiscard]] std::string_view NpcShopLabel(NpcShop shop);
inline constexpr int NpcShopCount = 7;

// --- Campaign data ---------------------------------------------------------------------------

[[nodiscard]] std::vector<CustomNpc> ReadCustomNpcs();
std::expected<void, std::string> WriteCustomNpcs(const std::vector<CustomNpc> &npcs);
[[nodiscard]] std::string NewNpcId(const std::vector<CustomNpc> &npcs);
/** @brief The open campaign's NPC with this id (cached), or nullptr. */
[[nodiscard]] const CustomNpc *FindCustomNpc(std::string_view id);
/** @brief Map npc type for a custom NPC: "custom:<id>". */
[[nodiscard]] std::string CustomNpcType(std::string_view id);
/** @brief The id in a "custom:<id>" type, or empty. */
[[nodiscard]] std::string_view CustomNpcIdOf(std::string_view type);

/** @brief Folder of voice recordings (ends with a separator); empty for Diablo: Original. */
[[nodiscard]] std::string CampaignVoiceDirectory();
/** @brief Recordings (.wav, .ogg) in the open campaign's voice folder. */
[[nodiscard]] std::vector<std::string> ListCampaignVoices();
/** @brief Plays a line's recording (stops the one playing). */
void PlayNpcVoice(std::string_view voiceFile);

/** @brief Colour swap table for an NPC's sprite, made from a palette (the town's). */
[[nodiscard]] std::array<uint8_t, 256> BuildNpcTrn(const CustomNpc &npc, const std::array<SDL_Color, 256> &palette);

// --- In town ---------------------------------------------------------------------------------

/** @brief Makes the townsperson table entry for a placed custom NPC. Returns false if unknown. */
bool MakeCustomNpcEntry(std::string_view id, Point position, int direction, TownerDataEntry &entry);
/** @brief Called by the towner init for TOWN_CUSTOM: name, colours. */
void InitCustomNpcTowner(Towner &towner);
/** @brief The custom NPC a townsperson is, or nullptr. */
[[nodiscard]] const CustomNpc *CustomNpcOf(const Towner &towner);
/** @brief Talk handler for TOWN_CUSTOM (a greeting when there is no quest to talk about). */
void TalkToCustomNpc(Player &player, Towner &towner);

} // namespace devilution
