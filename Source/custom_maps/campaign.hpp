/**
 * @file custom_maps/campaign.hpp
 *
 * Campaigns: a set of hand-made maps (towns, wilds, dungeons) with a place to start, made in the
 * map editor and shared as one folder: mods/custommaps/campaigns/<id>/ holds campaign.ini and
 * maps/<name>/ for each map (and tristram.ini when the campaign adds portals or doors to Tristram).
 *
 * Every hero belongs to one campaign, chosen when the hero is made ("Start in..."). "Diablo:
 * Original" is the plain game: Tristram as it is, no custom maps. The editor always works inside a
 * campaign. Single player only.
 */
#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace devilution {

struct Campaign {
	std::string id;          // folder name
	std::string title;       // shown in "Start in..."
	std::string description; // one line about it
	std::string start;       // town map heroes start in; empty: Tristram
};

/** @brief Title of the plain game in "Start in...". */
inline constexpr std::string_view OriginalCampaignTitle = "Diablo: Original";

[[nodiscard]] bool IsValidCampaignId(std::string_view id);
/** @brief Folder holding all campaigns (ends with a separator). */
[[nodiscard]] std::string CampaignsDirectory();
/** @brief Installed campaigns, by title. Moves loose maps from older versions into "My Maps" first. */
[[nodiscard]] std::vector<Campaign> ListCampaigns();
[[nodiscard]] std::optional<Campaign> ReadCampaign(std::string_view id);
std::expected<void, std::string> WriteCampaign(const Campaign &campaign);

/** @brief The campaign being played or edited ("" = Diablo: Original). */
[[nodiscard]] const std::string &ActiveCampaign();
void SetActiveCampaign(std::string id);
/** @brief Folder of the active campaign (ends with a separator); empty for Diablo: Original. */
[[nodiscard]] std::string ActiveCampaignDirectory();
/** @brief Title of the active campaign. */
[[nodiscard]] std::string ActiveCampaignTitle();

/** @brief The campaign the next hero made belongs to ("Start in..."). */
void SetNewHeroCampaign(std::string id);
[[nodiscard]] const std::string &NewHeroCampaign();
/** @brief Town a new hero of a campaign starts in, if the campaign has one that exists. */
[[nodiscard]] std::string CampaignStartTown(std::string_view id);

} // namespace devilution
