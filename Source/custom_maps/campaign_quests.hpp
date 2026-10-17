/**
 * @file custom_maps/campaign_quests.hpp
 *
 * Quests of a campaign (made in the Campaign Editor, saved in the campaign's quests.ini). Any
 * townsperson can give one. Three kinds:
 *  - Kill: slay a boss placed in one of the campaign's maps, then return to the giver.
 *  - Find: an item is hidden in a chest placed in a map; open it and bring the item back.
 *  - Talk: go and talk to someone (often in another town); they hand over something for the giver.
 * Finishing one can unlock the next (a quest can wait for another), and each pays a reward: gold,
 * experience, a magic item or a named unique item. Progress is kept with the hero.
 */
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_events.h>
#else
#include <SDL.h>
#endif

#include "engine/point.hpp"
#include "engine/surface.hpp"

namespace devilution {

struct Player;
struct Towner;

enum class QuestGoal : uint8_t {
	Kill,  // target: "<map>:<boss name>"
	Find,  // target: "<map>:<x>,<y>" (a chest)
	Talk,  // target: "<town>:<TOWN_TYPE>"
};

enum class QuestReward : uint8_t {
	Gold,
	Experience,
	MagicItem,
	UniqueItem,
};

struct CampaignQuest {
	std::string id;
	std::string title;
	std::string giver; // "<town map>:<TOWN_TYPE>", or "tristram:<TOWN_TYPE>"
	QuestGoal goal = QuestGoal::Kill;
	std::string target;
	std::string item; // Find / Talk: what the hero brings back
	QuestReward reward = QuestReward::Gold;
	int amount = 500;       // gold or experience
	std::string uniqueItem; // QuestReward::UniqueItem
	std::string after;      // only offered once this quest is done
	std::string offer;      // what the giver asks
	std::string progress;   // what the giver says while it is not done
	std::string thanks;     // what the giver says when it is done
	std::string targetText; // Talk: what the person sought says
};

enum class QuestState : uint8_t {
	NotStarted,
	Active,
	Ready, // done; return to the giver
	Done,
};

// --- Campaign data (editor and game) ---------------------------------------------------------

[[nodiscard]] std::vector<CampaignQuest> ReadCampaignQuests();
std::expected<void, std::string> WriteCampaignQuests(const std::vector<CampaignQuest> &quests);
/** @brief A fresh quest id ("q1", "q2", ...) not used in the list. */
[[nodiscard]] std::string NewQuestId(const std::vector<CampaignQuest> &quests);

/** @brief Display name of a townsperson key ("valehold:TOWN_SMITH" -> "Griswold (smith) in valehold"). */
[[nodiscard]] std::string QuestPersonLabel(std::string_view key);
/** @brief One line describing what the quest asks (for lists and the tracker). */
[[nodiscard]] std::string QuestGoalLabel(const CampaignQuest &quest);
/** @brief Reward as text ("500 gold", "Magic item", "The Butcher's Cleaver"). */
[[nodiscard]] std::string QuestRewardLabel(const CampaignQuest &quest);

// --- Hero progress ---------------------------------------------------------------------------

/** @brief Progress as text, for the hero's save. */
[[nodiscard]] std::string SaveCampaignQuestState();
void LoadCampaignQuestState(std::string_view text);

// --- Game hooks ------------------------------------------------------------------------------

/** @brief The hero talks to a townsperson: opens a quest conversation if there is one (returns true). */
bool CampaignQuestTalk(Player &player, int townerIndex);
/** @brief A unique monster died in a custom map. */
void CampaignQuestMonsterKilled(std::string_view uniqueName);
/** @brief A chest was opened in a custom map. */
void CampaignQuestChestOpened(Point tile);

/** @brief The quest conversation is open (it takes the input). */
[[nodiscard]] bool IsQuestDialogOpen();
bool QuestDialogHandleEvent(const SDL_Event &event);
/** @brief The conversation, and the list of quests under way. */
void DrawCampaignQuests(const Surface &out);
#ifdef _DEBUG
/** @brief Test hook: DEVILUTIONX_DEV_QUESTSCRIPT. */
void CampaignQuestsDevTick();
#endif

} // namespace devilution
