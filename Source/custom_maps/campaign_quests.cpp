#include "custom_maps/campaign_quests.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <functional>
#include <optional>
#include <unordered_map>

#include <magic_enum/magic_enum.hpp>

#include "control/control.hpp"
#include "cursor.h"
#include "custom_maps/campaign.hpp"
#include "custom_maps/custom_map.hpp"
#include "custom_maps/custom_npcs.hpp"
#include "stores.h"
#include "custom_maps/custom_town.hpp"
#include "diablo_msg.hpp"
#include "engine/palette.h"
#include "engine/rectangle.hpp"
#include "engine/render/primitive_render.hpp"
#include "engine/render/text_render.hpp"
#include "engine/sound.h"
#include "engine/sound_defs.hpp"
#include "capture.h"
#include "items.h"
#include "loadsave.h"
#include "monster.h"
#include "objects.h"
#include "levels/gendung.h"
#include "player.h"
#include "tables/itemdat.h"
#include "tables/monstdat.h"
#include "towners.h"
#include "utils/file_util.h"
#include "utils/log.hpp"
#include "utils/parse_int.hpp"
#include "utils/sdl_compat.h"
#include "utils/str_cat.hpp"
#include "utils/str_split.hpp"

namespace devilution {

namespace {

// --- Quest file ------------------------------------------------------------------------------

std::string QuestsPath()
{
	const std::string dir = ActiveCampaignDirectory();
	return dir.empty() ? std::string {} : dir + "quests.ini";
}

std::string OneLine(std::string_view text)
{
	std::string out(text);
	std::replace(out.begin(), out.end(), '\n', ' ');
	std::replace(out.begin(), out.end(), '\r', ' ');
	return out;
}

std::string_view GoalName(QuestGoal goal)
{
	switch (goal) {
	case QuestGoal::Find: return "find";
	case QuestGoal::Talk: return "talk";
	default: return "kill";
	}
}

QuestGoal ParseGoal(std::string_view s)
{
	if (s == "find") return QuestGoal::Find;
	if (s == "talk") return QuestGoal::Talk;
	return QuestGoal::Kill;
}

std::string_view RewardName(QuestReward reward)
{
	switch (reward) {
	case QuestReward::Experience: return "experience";
	case QuestReward::MagicItem: return "magic";
	case QuestReward::UniqueItem: return "unique";
	default: return "gold";
	}
}

QuestReward ParseReward(std::string_view s)
{
	if (s == "experience") return QuestReward::Experience;
	if (s == "magic") return QuestReward::MagicItem;
	if (s == "unique") return QuestReward::UniqueItem;
	return QuestReward::Gold;
}

/** The campaign's quests, read once per campaign (the editor writes through WriteCampaignQuests). */
std::string CachedCampaign = "\x01";
std::vector<CampaignQuest> Cached;

const std::vector<CampaignQuest> &CampaignQuestList()
{
	if (CachedCampaign != ActiveCampaign()) {
		CachedCampaign = ActiveCampaign();
		Cached = ReadCampaignQuests();
	}
	return Cached;
}

// --- Hero progress ---------------------------------------------------------------------------

std::unordered_map<std::string, QuestState> States;

QuestState StateOf(const CampaignQuest &quest)
{
	const auto it = States.find(quest.id);
	return it == States.end() ? QuestState::NotStarted : it->second;
}

bool Unlocked(const CampaignQuest &quest)
{
	if (quest.after.empty())
		return true;
	const auto it = States.find(quest.after);
	return it != States.end() && it->second == QuestState::Done;
}

std::string GiverName(const CampaignQuest &quest)
{
	const std::string label = QuestPersonLabel(quest.giver);
	const size_t paren = label.find(" (");
	return paren == std::string::npos ? label : label.substr(0, paren);
}

std::string KeyPart(std::string_view key, bool second)
{
	const size_t colon = key.find(':');
	if (colon == std::string_view::npos)
		return second ? std::string {} : std::string(key);
	return std::string(second ? key.substr(colon + 1) : key.substr(0, colon));
}

// --- Rewards ---------------------------------------------------------------------------------

void GiveReward(Player &player, const CampaignQuest &quest)
{
	const Point at = player.position.tile;
	switch (quest.reward) {
	case QuestReward::Gold: {
		int left = std::clamp(quest.amount, 1, 100000);
		while (left > 0) {
			const int stack = std::min(left, GOLD_MAX_LIMIT);
			DropGoldAt(at, stack);
			left -= stack;
		}
		InitDiabloMsg(StrCat("Reward: ", quest.amount, " gold"));
		break;
	}
	case QuestReward::Experience:
		player.addExperience(static_cast<uint32_t>(std::clamp(quest.amount, 1, 50000000)));
		InitDiabloMsg(StrCat("Reward: ", quest.amount, " experience"));
		break;
	case QuestReward::MagicItem:
		DropRewardItem(at, player.getCharacterLevel());
		InitDiabloMsg("Reward: a magic item");
		break;
	case QuestReward::UniqueItem: {
		for (size_t i = 0; i < UniqueItems.size(); i++) {
			if (UniqueItems[i].UIName == quest.uniqueItem) {
				SpawnUnique(static_cast<_unique_items>(i), at, player.getCharacterLevel(), /*sendmsg=*/false);
				InitDiabloMsg(StrCat("Reward: ", quest.uniqueItem));
				return;
			}
		}
		DropRewardItem(at, player.getCharacterLevel());
		InitDiabloMsg("Reward: a magic item");
		break;
	}
	}
}

// --- Conversation ----------------------------------------------------------------------------

struct Choice {
	std::string label;
	std::function<void()> action;
};

struct Conversation {
	bool open = false;
	int towner = -1;
	std::string speaker;
	std::string text;
	std::vector<Choice> choices;
};

Conversation Talk;
bool BypassQuestTalk = false;

std::string ToastText;
uint32_t ToastUntil = 0;

void Toast(std::string text)
{
	ToastText = std::move(text);
	ToastUntil = SDL_GetTicks() + 4000;
}

void Close()
{
	Talk = {};
}

void ContinueToTownerTalk(Player &player, int townerIndex)
{
	Close();
	BypassQuestTalk = true;
	TalkToTowner(player, townerIndex);
	BypassQuestTalk = false;
}

/** Key of the townsperson: "<town>:<TOWN_TYPE>". */
std::string TownerKey(const Towner &towner)
{
	const std::string town = InCustomTown() ? CurrentCustomTown() : std::string("tristram");
	if (const CustomNpc *npc = CustomNpcOf(towner); npc != nullptr)
		return StrCat(town, ":", CustomNpcType(npc->id));
	return StrCat(town, ":", magic_enum::enum_name(towner._ttype));
}

/** One of a campaign townsperson's lines, spoken (its voice recording plays). */
std::string SayLine(const CustomNpc &npc, int avoid = -1)
{
	if (npc.lines.empty())
		return "Greetings, stranger.";
	int pick = std::rand() % static_cast<int>(npc.lines.size());
	if (pick == avoid && npc.lines.size() > 1)
		pick = (pick + 1) % static_cast<int>(npc.lines.size());
	PlayNpcVoice(npc.lines[pick].voice);
	return npc.lines[pick].text.empty() ? std::string("...") : npc.lines[pick].text;
}

TalkID ShopStore(NpcShop shop)
{
	switch (shop) {
	case NpcShop::Smith: return TalkID::Smith;
	case NpcShop::Healer: return TalkID::Healer;
	case NpcShop::Witch: return TalkID::Witch;
	case NpcShop::Boy: return TalkID::Boy;
	case NpcShop::Gambler: return TalkID::Gambler;
	case NpcShop::Storyteller: return TalkID::Storyteller;
	default: return TalkID::None;
	}
}

int LastNpcTowner = -1;

void ReopenNpcTalk()
{
	if (LastNpcTowner >= 0 && static_cast<size_t>(LastNpcTowner) < Towners.size() && MyPlayer != nullptr)
		CampaignQuestTalk(*MyPlayer, LastNpcTowner);
}

/** Choices every campaign townsperson offers after the quest ones: their shop, more talk, goodbye. */
void AddNpcChoices(const CustomNpc &npc, int townerIndex)
{
	if (npc.shop != NpcShop::None) {
		const TalkID store = ShopStore(npc.shop);
		const std::string name = npc.name;
		Talk.choices.push_back({ "Let me see your wares", [store, name, townerIndex]() {
			                        Close();
			                        StoreKeeperName = name;
			                        StoreKeeperTalk = &ReopenNpcTalk;
			                        LastNpcTowner = townerIndex;
			                        StartStore(store);
		                        } });
	}
	if (npc.lines.size() > 1) {
		const std::string id = npc.id;
		Talk.choices.push_back({ "Tell me more", [id]() {
			                        if (const CustomNpc *n = FindCustomNpc(id); n != nullptr) {
				                        const std::string before = Talk.text;
				                        Talk.text = SayLine(*n);
				                        if (Talk.text == before)
					                        Talk.text = SayLine(*n);
			                        }
		                        } });
	}
	Talk.choices.push_back({ "Farewell", [] { Close(); } });
}

void SetState(const CampaignQuest &quest, QuestState state)
{
	LogVerbose("Quest {} -> {}", quest.id, static_cast<int>(state));
	States[quest.id] = state;
}

/** Layout of the conversation box (screen pixels). */
struct DialogLayout {
	Rectangle box;
	std::vector<std::string> lines;
	std::vector<Rectangle> choices;
};

DialogLayout Layout()
{
	DialogLayout layout;
	const int width = std::min(460, gnScreenWidth - 20);
	const std::string wrapped = WordWrapString(Talk.text, static_cast<unsigned>(width - 28));
	for (std::string_view line : SplitByChar(wrapped, '\n'))
		layout.lines.emplace_back(line);
	const int height = 36 + static_cast<int>(layout.lines.size()) * 16 + 10 + static_cast<int>(Talk.choices.size()) * 20 + 10;
	layout.box = { { (gnScreenWidth - width) / 2, std::max(10, (gnViewportHeight - height) / 2 - 40) }, { width, height } };
	int y = layout.box.position.y + 36 + static_cast<int>(layout.lines.size()) * 16 + 10;
	for (size_t i = 0; i < Talk.choices.size(); i++, y += 20)
		layout.choices.push_back({ { layout.box.position.x + 14, y }, { width - 28, 18 } });
	return layout;
}

void Choose(size_t index)
{
	if (index >= Talk.choices.size())
		return;
	const std::function<void()> action = Talk.choices[index].action;
	action();
}

} // namespace

// --- Campaign data ---------------------------------------------------------------------------

std::vector<CampaignQuest> ReadCampaignQuests()
{
	std::vector<CampaignQuest> quests;
	const std::string path = QuestsPath();
	if (path.empty())
		return quests;
	FILE *file = OpenFile(path.c_str(), "rb");
	if (file == nullptr)
		return quests;
	std::string text;
	char buffer[4096];
	size_t got;
	while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
		text.append(buffer, got);
	std::fclose(file);

	for (std::string_view line : SplitByChar(text, '\n')) {
		if (!line.empty() && line.back() == '\r')
			line.remove_suffix(1);
		if (line.empty() || line[0] == '#')
			continue;
		if (line.front() == '[' && line.back() == ']') {
			quests.emplace_back();
			quests.back().id = std::string(line.substr(1, line.size() - 2));
			continue;
		}
		if (quests.empty())
			continue;
		const size_t eq = line.find('=');
		if (eq == std::string_view::npos)
			continue;
		const std::string_view key = line.substr(0, eq);
		const std::string value(line.substr(eq + 1));
		CampaignQuest &q = quests.back();
		if (key == "title") q.title = value;
		else if (key == "giver") q.giver = value;
		else if (key == "goal") q.goal = ParseGoal(value);
		else if (key == "target") q.target = value;
		else if (key == "item") q.item = value;
		else if (key == "reward") q.reward = ParseReward(value);
		else if (key == "amount") {
			const ParseIntResult<int> v = ParseInt<int>(value);
			q.amount = v.has_value() ? *v : q.amount;
		} else if (key == "unique") q.uniqueItem = value;
		else if (key == "after") q.after = value;
		else if (key == "offer") q.offer = value;
		else if (key == "progress") q.progress = value;
		else if (key == "thanks") q.thanks = value;
		else if (key == "target_says") q.targetText = value;
	}
	return quests;
}

std::expected<void, std::string> WriteCampaignQuests(const std::vector<CampaignQuest> &quests)
{
	const std::string path = QuestsPath();
	if (path.empty())
		return std::unexpected("No campaign is open.");
	std::string text = "# Campaign quests (Campaign Editor). One [section] per quest.\n";
	for (const CampaignQuest &q : quests) {
		text += StrCat("\n[", q.id, "]\n");
		text += StrCat("title=", OneLine(q.title), "\n");
		text += StrCat("giver=", q.giver, "\n");
		text += StrCat("goal=", GoalName(q.goal), "\n");
		text += StrCat("target=", q.target, "\n");
		text += StrCat("item=", OneLine(q.item), "\n");
		text += StrCat("reward=", RewardName(q.reward), "\n");
		text += StrCat("amount=", q.amount, "\n");
		text += StrCat("unique=", q.uniqueItem, "\n");
		text += StrCat("after=", q.after, "\n");
		text += StrCat("offer=", OneLine(q.offer), "\n");
		text += StrCat("progress=", OneLine(q.progress), "\n");
		text += StrCat("thanks=", OneLine(q.thanks), "\n");
		text += StrCat("target_says=", OneLine(q.targetText), "\n");
	}
	FILE *file = OpenFile(path.c_str(), "wb");
	if (file == nullptr)
		return std::unexpected(StrCat("Cannot write ", path));
	const bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
	std::fclose(file);
	if (!ok)
		return std::unexpected(StrCat("Cannot write ", path));
	CachedCampaign = ActiveCampaign();
	Cached = quests;
	return {};
}

std::string NewQuestId(const std::vector<CampaignQuest> &quests)
{
	for (int n = 1;; n++) {
		std::string id = StrCat("q", n);
		if (std::none_of(quests.begin(), quests.end(), [&](const CampaignQuest &q) { return q.id == id; }))
			return id;
	}
}

std::string QuestPersonLabel(std::string_view key)
{
	if (key.empty())
		return "(nobody yet)";
	const std::string town = KeyPart(key, false);
	const std::string type = KeyPart(key, true);
	std::string name = type;
	if (const std::string_view customId = CustomNpcIdOf(type); !customId.empty()) {
		const CustomNpc *npc = FindCustomNpc(customId);
		name = npc != nullptr ? npc->name : std::string("(removed townsperson)");
	}
	for (const PlaceableTowner &t : PlaceableTowners()) {
		if (t.type == type)
			name = std::string(t.label);
	}
	return StrCat(name, " in ", town == "tristram" ? std::string("Tristram") : town);
}

std::string QuestGoalLabel(const CampaignQuest &quest)
{
	const std::string place = KeyPart(quest.target, false);
	const std::string what = KeyPart(quest.target, true);
	switch (quest.goal) {
	case QuestGoal::Kill:
		return quest.target.empty() ? std::string("Kill (no boss chosen)") : StrCat("Kill ", what, " (", place, ")");
	case QuestGoal::Find:
		return quest.target.empty() ? std::string("Find (no chest chosen)") : StrCat("Find ", quest.item.empty() ? std::string("the item") : quest.item, " in a chest (", place, ")");
	case QuestGoal::Talk:
		return quest.target.empty() ? std::string("Talk (nobody chosen)") : StrCat("Talk to ", QuestPersonLabel(quest.target));
	}
	return {};
}

std::string QuestRewardLabel(const CampaignQuest &quest)
{
	switch (quest.reward) {
	case QuestReward::Gold: return StrCat(quest.amount, " gold");
	case QuestReward::Experience: return StrCat(quest.amount, " experience");
	case QuestReward::MagicItem: return "A magic item";
	case QuestReward::UniqueItem: return quest.uniqueItem.empty() ? std::string("A unique item (none chosen)") : quest.uniqueItem;
	}
	return {};
}

// --- Hero progress ---------------------------------------------------------------------------

std::string SaveCampaignQuestState()
{
	std::string text;
	for (const auto &[id, state] : States) {
		if (state != QuestState::NotStarted)
			text += StrCat(id, "=", static_cast<int>(state), ";");
	}
	return text;
}

void LoadCampaignQuestState(std::string_view text)
{
	LogVerbose("Quest state loaded: {}", text);
	States.clear();
	Close();
	for (std::string_view entry : SplitByChar(text, ';')) {
		const size_t eq = entry.find('=');
		if (eq == std::string_view::npos)
			continue;
		const ParseIntResult<int> v = ParseInt<int>(entry.substr(eq + 1));
		if (v.has_value() && *v > 0 && *v <= static_cast<int>(QuestState::Done))
			States[std::string(entry.substr(0, eq))] = static_cast<QuestState>(*v);
	}
}

// --- Game hooks ------------------------------------------------------------------------------

bool CampaignQuestTalk(Player &player, int townerIndex)
{
	if (BypassQuestTalk || ActiveCampaign().empty() || &player != MyPlayer)
		return false;
	const Towner &towner = Towners[townerIndex];
	const std::string key = TownerKey(towner);
	const std::vector<CampaignQuest> &quests = CampaignQuestList();

	// What this person has to say, most pressing first.
	const CampaignQuest *ready = nullptr, *deliver = nullptr, *active = nullptr, *offer = nullptr;
	for (const CampaignQuest &q : quests) {
		const QuestState state = StateOf(q);
		if (q.giver == key && state == QuestState::Ready && ready == nullptr)
			ready = &q;
		else if (q.goal == QuestGoal::Talk && q.target == key && state == QuestState::Active && deliver == nullptr)
			deliver = &q;
		else if (q.giver == key && state == QuestState::Active && active == nullptr)
			active = &q;
		else if (q.giver == key && state == QuestState::NotStarted && Unlocked(q) && offer == nullptr)
			offer = &q;
	}
	const CustomNpc *npc = CustomNpcOf(towner);
	if (ready == nullptr && deliver == nullptr && active == nullptr && offer == nullptr && npc == nullptr)
		return false;

	Talk = {};
	Talk.open = true;
	Talk.towner = townerIndex;
	Talk.speaker = std::string(towner.name);
	Player *hero = &player;
	const auto talkOn = [hero, townerIndex]() { ContinueToTownerTalk(*hero, townerIndex); };
	LastNpcTowner = townerIndex;
	if (ready == nullptr && deliver == nullptr && active == nullptr && offer == nullptr) {
		// A campaign townsperson with nothing to ask: they greet the hero.
		Talk.text = SayLine(*npc);
		AddNpcChoices(*npc, townerIndex);
		return true;
	}
	if (ready != nullptr) {
		const CampaignQuest quest = *ready;
		if (!quest.thanks.empty())
			Talk.text = quest.thanks;
		else if (quest.goal == QuestGoal::Kill || quest.item.empty())
			Talk.text = "It is done, then. You have my thanks, and this.";
		else if (quest.goal == QuestGoal::Find)
			Talk.text = StrCat("You found the ", quest.item, "! You have my thanks, and this.");
		else
			Talk.text = StrCat("You brought the ", quest.item, ". You have my thanks, and this.");
		Talk.choices.push_back({ quest.item.empty() || quest.goal == QuestGoal::Kill ? std::string("Take the reward") : StrCat("Hand over the ", quest.item), [hero, quest]() {
			                        SetState(quest, QuestState::Done);
			                        Close();
			                        GiveReward(*hero, quest);
			                        Toast(StrCat("Quest complete: ", quest.title));
		                        } });
	} else if (deliver != nullptr) {
		const CampaignQuest quest = *deliver;
		const std::string giver = GiverName(quest);
		const std::string item = quest.item.empty() ? std::string("this") : quest.item;
		Talk.text = !quest.targetText.empty() ? quest.targetText : StrCat("So ", giver, " sent you? Here, take ", item, " back to them.");
		Talk.choices.push_back({ StrCat("Take ", item, " for ", giver), [quest, giver, item]() {
			                        SetState(quest, QuestState::Ready);
			                        Close();
			                        Toast(StrCat(quest.title, ": bring ", item, " to ", giver));
		                        } });
	} else if (active != nullptr) {
		const CampaignQuest quest = *active;
		Talk.text = !quest.progress.empty() ? quest.progress : StrCat("Have you done what I asked? ", QuestGoalLabel(quest), ".");
		Talk.choices.push_back({ "I am working on it", [] { Close(); } });
	} else {
		const CampaignQuest quest = *offer;
		Talk.text = !quest.offer.empty() ? quest.offer : StrCat("I have a task for you. ", QuestGoalLabel(quest), ".");
		Talk.choices.push_back({ StrCat("Accept: ", quest.title), [quest]() {
			                        SetState(quest, QuestState::Active);
			                        Close();
			                        Toast(StrCat("New quest: ", quest.title));
		                        } });
		Talk.choices.push_back({ "Not now", [] { Close(); } });
	}
	if (npc != nullptr)
		AddNpcChoices(*npc, townerIndex);
	else
		Talk.choices.push_back({ "Talk about something else", talkOn });
	return true;
}

void CampaignQuestMonsterKilled(std::string_view uniqueName)
{
	if (ActiveCampaign().empty() || !setlevel || setlvlnum != SL_CUSTOM || ActiveCustomMap() == nullptr)
		return;
	const std::string key = StrCat(ActiveCustomMap()->name, ":", uniqueName);
	for (const CampaignQuest &q : CampaignQuestList()) {
		if (q.goal == QuestGoal::Kill && q.target == key && StateOf(q) == QuestState::Active) {
			SetState(q, QuestState::Ready);
			Toast(StrCat(q.title, ": return to ", GiverName(q)));
		}
	}
}

void CampaignQuestChestOpened(Point tile)
{
	if (ActiveCampaign().empty() || !setlevel || setlvlnum != SL_CUSTOM || ActiveCustomMap() == nullptr)
		return;
	const std::string key = StrCat(ActiveCustomMap()->name, ":", tile.x, ",", tile.y);
	for (const CampaignQuest &q : CampaignQuestList()) {
		if (q.goal == QuestGoal::Find && q.target == key && StateOf(q) == QuestState::Active) {
			SetState(q, QuestState::Ready);
			InitDiabloMsg(StrCat("Found: ", q.item.empty() ? std::string("what you were looking for") : q.item));
			Toast(StrCat(q.title, ": bring it to ", GiverName(q)));
		}
	}
}

#ifdef _DEBUG
void CampaignQuestsDevTick()
{
	// Test hook: DEVILUTIONX_DEV_QUESTSCRIPT = steps separated by ';', one every 40 ticks in game:
	// talk TOWN_TYPE | choose N | kill (unique monsters here) | chests (open all) | map NAME | town | shot | save | log
	const char *script = std::getenv("DEVILUTIONX_DEV_QUESTSCRIPT");
	if (script == nullptr || MyPlayer == nullptr || MyPlayer->_pLvlChanging)
		return;
	static size_t step = 0;
	static int wait = 0;
	if (++wait < 40)
		return;
	wait = 0;
	std::vector<std::string_view> steps;
	for (std::string_view s : SplitByChar(script, ';'))
		steps.push_back(s);
	if (step >= steps.size())
		return;
	const std::string_view cmd = steps[step++];
	const std::string_view arg = cmd.find(' ') == std::string_view::npos ? std::string_view {} : cmd.substr(cmd.find(' ') + 1);
	Player &player = *MyPlayer;
	LogInfo("QUESTSCRIPT {}   (leveltype {} setlevel {} currlevel {})", cmd, static_cast<int>(leveltype), setlevel, currlevel);
	if (cmd.starts_with("talk")) {
		for (size_t i = 0; i < GetNumTowners(); i++) {
			if (magic_enum::enum_name(Towners[i]._ttype) == arg) {
				const bool opened = CampaignQuestTalk(player, static_cast<int>(i));
				LogInfo("QUESTSCRIPT talk {}: {}", arg, opened ? Talk.text : std::string("(no quest talk)"));
				for (const Choice &c : Talk.choices)
					LogInfo("QUESTSCRIPT   choice: {}", c.label);
			}
		}
	} else if (cmd.starts_with("choose")) {
		const ParseIntResult<int> n = ParseInt<int>(arg);
		if (n.has_value())
			Choose(static_cast<size_t>(*n - 1));
	} else if (cmd == "kill") {
		for (size_t i = 0; i < ActiveMonsterCount; i++) {
			Monster &monster = Monsters[ActiveMonsters[i]];
			if (monster.uniqueType != UniqueMonsterType::None && monster.hitPoints > 0 && monster.mode != MonsterMode::Death)
				M_StartKill(monster, player);
		}
	} else if (cmd == "chests") {
		for (int i = 0; i < ActiveObjectCount; i++) {
			Object &object = Objects[ActiveObjects[i]];
			if (object.IsChest())
				OperateObject(player, object);
		}
	} else if (cmd.starts_with("map")) {
		if (std::expected<void, std::string> r = TravelToCustomMap(arg, ""); !r.has_value())
			LogError("QUESTSCRIPT map: {}", r.error());
		wait = -60; // the level change takes a moment
	} else if (cmd == "shot") {
		CaptureScreen();
	} else if (cmd == "save") {
		SaveGame();
	} else if (cmd == "log") {
		LogInfo("QUESTSCRIPT state: {}", SaveCampaignQuestState());
		for (const Towner &t : Towners)
			LogInfo("QUESTSCRIPT towner {} at {},{} anim {} dMonster {} visible {}", t.name, t.position.x, t.position.y, t.anim ? 1 : 0, dMonster[t.position.x][t.position.y],
			    HasAnyOf(dFlags[t.position.x][t.position.y], DungeonFlag::Visible) ? 1 : 0);
		LogInfo("QUESTSCRIPT hero at {},{}", player.position.tile.x, player.position.tile.y);
	}
}
#endif

bool IsQuestDialogOpen()
{
	return Talk.open;
}

bool QuestDialogHandleEvent(const SDL_Event &event)
{
	if (!Talk.open)
		return false;
	if (leveltype != DTYPE_TOWN) {
		Close();
		return false;
	}
	switch (event.type) {
	case SDL_EVENT_MOUSE_BUTTON_DOWN: {
		const DialogLayout layout = Layout();
		for (size_t i = 0; i < layout.choices.size(); i++) {
			if (layout.choices[i].contains(MousePosition)) {
				Choose(i);
				return true;
			}
		}
		if (!layout.box.contains(MousePosition))
			Close();
		return true;
	}
	case SDL_EVENT_MOUSE_BUTTON_UP:
		return true;
	case SDL_EVENT_KEY_DOWN: {
		const SDL_Scancode sc = SDLC_EventScancode(event);
		if (sc == SDL_SCANCODE_ESCAPE)
			Close();
		else if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
			Choose(static_cast<size_t>(sc - SDL_SCANCODE_1));
		return true;
	}
	case SDL_EVENT_KEY_UP:
		return true;
	default:
		return false;
	}
}

void DrawCampaignQuests(const Surface &out)
{
	if (ActiveCampaign().empty() || MyPlayer == nullptr)
		return;
	if (Talk.open && leveltype != DTYPE_TOWN)
		Close();

	// Quests under way, top right.
	int y = 8;
	int shown = 0;
	for (const CampaignQuest &q : CampaignQuestList()) {
		const QuestState state = StateOf(q);
		if (state != QuestState::Active && state != QuestState::Ready)
			continue;
		if (++shown > 4)
			break;
		const std::string goal = state == QuestState::Ready ? StrCat("Return to ", GiverName(q), " (", KeyPart(q.giver, false), ")") : QuestGoalLabel(q);
		const int w = std::min(std::max(GetLineWidth(q.title), GetLineWidth(goal)) + 16, gnScreenWidth / 2);
		const int x = 8;
		DrawHalfTransparentRectTo(out, x, y, w, 34);
		DrawString(out, q.title, { { x + 8, y + 3 }, { w - 16, 14 } }, { .flags = UiFlags::ColorGold });
		DrawString(out, goal, { { x + 8, y + 18 }, { w - 16, 14 } }, { .flags = state == QuestState::Ready ? UiFlags::ColorWhitegold : UiFlags::ColorWhite });
		y += 38;
	}
	if (SDL_GetTicks() < ToastUntil) {
		const int w = std::min(GetLineWidth(ToastText, GameFont24) + 30, gnScreenWidth - 20);
		const int x = (gnScreenWidth - w) / 2;
		DrawHalfTransparentRectTo(out, x, 100, w, 34);
		DrawString(out, ToastText, { { x, 104 }, { w, 28 } }, { .flags = UiFlags::FontSize24 | UiFlags::ColorGold | UiFlags::AlignCenter });
	}

	if (!Talk.open)
		return;
	const DialogLayout layout = Layout();
	const Rectangle &box = layout.box;
	for (int i = 0; i < 3; i++)
		DrawHalfTransparentRectTo(out, box.position.x, box.position.y, box.size.width, box.size.height);
	DrawHorizontalLine(out, box.position, box.size.width, PAL16_YELLOW + 4);
	DrawHorizontalLine(out, box.position + Displacement { 0, box.size.height - 1 }, box.size.width, PAL16_YELLOW + 4);
	DrawVerticalLine(out, box.position, box.size.height, PAL16_YELLOW + 4);
	DrawVerticalLine(out, box.position + Displacement { box.size.width - 1, 0 }, box.size.height, PAL16_YELLOW + 4);
	DrawString(out, Talk.speaker, { box.position + Displacement { 14, 10 }, { box.size.width - 28, 16 } }, { .flags = UiFlags::ColorGold | UiFlags::AlignCenter });
	int ty = box.position.y + 36;
	for (const std::string &line : layout.lines) {
		DrawString(out, line, { { box.position.x + 14, ty }, { box.size.width - 28, 14 } }, { .flags = UiFlags::ColorWhite });
		ty += 16;
	}
	for (size_t i = 0; i < layout.choices.size(); i++) {
		const Rectangle &r = layout.choices[i];
		const bool hover = r.contains(MousePosition);
		if (hover)
			FillRect(out, r.position.x, r.position.y, r.size.width, r.size.height, PAL16_YELLOW + 13);
		DrawString(out, StrCat(i + 1, ". ", Talk.choices[i].label), { r.position + Displacement { 6, 2 }, { r.size.width - 12, 14 } }, { .flags = hover ? UiFlags::ColorWhitegold : UiFlags::ColorBlue });
	}
}

} // namespace devilution
