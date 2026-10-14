#include "class_skills.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "engine/backbuffer_state.hpp"
#include "engine/random.hpp"
#include "engine/sound_defs.hpp"
#include "game_mode.hpp"
#include "inv.h"
#include "levels/gendung.h"
#include "loadsave.h"
#include "missiles.h"
#include "monster.h"
#include "msg.h"
#include "multi.h"
#include "pfile.h"
#include "player.h"
#include "spells.h"
#include "utils/format.hpp"
#include "utils/language.h"
#include "utils/log.hpp"
#include "utils/str_cat.hpp"

namespace devilution {

namespace {

constexpr int8_t N = -1;

// clang-format off
constexpr ClassSkill Skills[] = {
	// Warrior: Combat skills
	/*  0 */ { N_("Bash"),               HeroClass::Warrior, 0, 0, 1, { N, N }, SpellID::SkillBash,         SkillPassive::None,              SpellIcon::BloodBoil },
	/*  1 */ { N_("Stun"),               HeroClass::Warrior, 0, 1, 0, { 0, N }, SpellID::SkillStun,         SkillPassive::None,              SpellIcon::PentaStar },
	/*  2 */ { N_("Cleave"),             HeroClass::Warrior, 0, 2, 2, { 0, N }, SpellID::SkillCleave,       SkillPassive::None,              SpellIcon::FireCloud },
	/*  3 */ { N_("Whirlwind"),          HeroClass::Warrior, 0, 3, 1, { 1, 2 }, SpellID::SkillWhirlwind,    SkillPassive::None,              SpellIcon::DoomSerpents },
	/*  4 */ { N_("Increased Speed"),    HeroClass::Warrior, 0, 4, 0, { 3, N }, SpellID::Invalid,           SkillPassive::IncreasedSpeed,    SpellIcon::Phasing },
	// Warrior: Masteries
	/*  5 */ { N_("Weapon Mastery"),     HeroClass::Warrior, 1, 0, 0, { N, N }, SpellID::Invalid,           SkillPassive::WeaponMastery,     SpellIcon::ItemRepair },
	/*  6 */ { N_("Iron Skin"),          HeroClass::Warrior, 1, 1, 2, { N, N }, SpellID::Invalid,           SkillPassive::IronSkin,          SpellIcon::ManaShield },
	/*  7 */ { N_("Toughness"),          HeroClass::Warrior, 1, 2, 0, { 5, N }, SpellID::Invalid,           SkillPassive::Toughness,         SpellIcon::Healing },
	/*  8 */ { N_("Natural Resistance"), HeroClass::Warrior, 1, 3, 2, { 6, N }, SpellID::Invalid,           SkillPassive::NaturalResistance, SpellIcon::Elemental },
	// Warrior: Warcries
	/*  9 */ { N_("Shout"),              HeroClass::Warrior, 2, 0, 0, { N, N }, SpellID::SkillShout,        SkillPassive::None,              SpellIcon::LongHorn },
	/* 10 */ { N_("War Cry"),            HeroClass::Warrior, 2, 2, 1, { 9, N }, SpellID::SkillWarCry,       SkillPassive::None,              SpellIcon::RedSkull },
	/* 11 */ { N_("Battle Orders"),      HeroClass::Warrior, 2, 3, 0, { 9, N }, SpellID::SkillBattleOrders, SkillPassive::None,              SpellIcon::Invisibility },

	// Rogue: Bow skills
	/* 12 */ { N_("Multishot"),          HeroClass::Rogue, 0, 0, 0, { N, N },   SpellID::SkillMultishot,      SkillPassive::None,           SpellIcon::HolyBolt },
	/* 13 */ { N_("Fire Arrow"),         HeroClass::Rogue, 0, 0, 2, { N, N },   SpellID::SkillFireArrow,      SkillPassive::None,           SpellIcon::Firebolt },
	/* 14 */ { N_("Penetrate"),          HeroClass::Rogue, 0, 1, 1, { N, N },   SpellID::Invalid,             SkillPassive::Penetrate,      SpellIcon::TrapDisarm },
	/* 15 */ { N_("Strafe"),             HeroClass::Rogue, 0, 2, 0, { 12, N },  SpellID::SkillStrafe,         SkillPassive::None,           SpellIcon::ChainLightning },
	/* 16 */ { N_("Lightning Arrow"),    HeroClass::Rogue, 0, 2, 2, { 13, N },  SpellID::SkillLightningArrow, SkillPassive::None,           SpellIcon::ChargedBolt },
	/* 17 */ { N_("Bow Mastery"),        HeroClass::Rogue, 0, 3, 1, { 14, N },  SpellID::Invalid,             SkillPassive::BowMastery,     SpellIcon::Telekinesis },
	// Rogue: Passive and magic
	/* 18 */ { N_("Inner Sight"),        HeroClass::Rogue, 1, 0, 0, { N, N },   SpellID::SkillInnerSight,     SkillPassive::None,           SpellIcon::Infravision },
	/* 19 */ { N_("Critical Strike"),    HeroClass::Rogue, 1, 0, 2, { N, N },   SpellID::Invalid,             SkillPassive::CriticalStrike, SpellIcon::BloodStar },
	/* 20 */ { N_("Dodge"),              HeroClass::Rogue, 1, 1, 1, { N, N },   SpellID::Invalid,             SkillPassive::Dodge,          SpellIcon::Etherealize },
	/* 21 */ { N_("Avoid"),              HeroClass::Rogue, 1, 2, 1, { 20, N },  SpellID::Invalid,             SkillPassive::Avoid,          SpellIcon::BloodRitual },
	/* 22 */ { N_("Phasing"),            HeroClass::Rogue, 1, 3, 0, { 18, N },  SpellID::Phasing,             SkillPassive::None,           SpellIcon::Phasing },
	// Rogue: Traps and tricks
	/* 23 */ { N_("Fire Wall"),          HeroClass::Rogue, 2, 0, 0, { N, N },   SpellID::FireWall,            SkillPassive::None,           SpellIcon::FireWall },
	/* 24 */ { N_("Stone Curse"),        HeroClass::Rogue, 2, 1, 2, { N, N },   SpellID::StoneCurse,          SkillPassive::None,           SpellIcon::StoneCurse },
	/* 25 */ { N_("Guardian"),           HeroClass::Rogue, 2, 2, 0, { 23, N },  SpellID::Guardian,            SkillPassive::None,           SpellIcon::Sentinel },
	/* 26 */ { N_("Trap Mastery"),       HeroClass::Rogue, 2, 3, 1, { 24, N },  SpellID::Invalid,             SkillPassive::TrapMastery,    SpellIcon::Pentagram },

	// Sorcerer: Fire
	/* 27 */ { N_("Firebolt"),           HeroClass::Sorcerer, 0, 0, 1, { N, N },   SpellID::Firebolt,       SkillPassive::None,             SpellIcon::Firebolt },
	/* 28 */ { N_("Inferno"),            HeroClass::Sorcerer, 0, 1, 0, { 27, N },  SpellID::Inferno,        SkillPassive::None,             SpellIcon::Inferno },
	/* 29 */ { N_("Fireball"),           HeroClass::Sorcerer, 0, 2, 2, { 27, N },  SpellID::Fireball,       SkillPassive::None,             SpellIcon::Fireball },
	/* 30 */ { N_("Fire Wall"),          HeroClass::Sorcerer, 0, 3, 0, { 28, N },  SpellID::FireWall,       SkillPassive::None,             SpellIcon::FireWall },
	/* 31 */ { N_("Fire Mastery"),       HeroClass::Sorcerer, 0, 4, 1, { N, N },   SpellID::Invalid,        SkillPassive::FireMastery,      SpellIcon::FlameWave },
	/* 32 */ { N_("Apocalypse"),         HeroClass::Sorcerer, 0, 5, 2, { 29, 30 }, SpellID::Apocalypse,     SkillPassive::None,             SpellIcon::Apocalypse },
	// Sorcerer: Lightning
	/* 33 */ { N_("Charged Bolt"),       HeroClass::Sorcerer, 1, 0, 1, { N, N },   SpellID::ChargedBolt,    SkillPassive::None,             SpellIcon::ChargedBolt },
	/* 34 */ { N_("Lightning"),          HeroClass::Sorcerer, 1, 1, 0, { 33, N },  SpellID::Lightning,      SkillPassive::None,             SpellIcon::Lightning },
	/* 35 */ { N_("Nova"),               HeroClass::Sorcerer, 1, 2, 2, { 33, N },  SpellID::Nova,           SkillPassive::None,             SpellIcon::Nova },
	/* 36 */ { N_("Chain Lightning"),    HeroClass::Sorcerer, 1, 3, 0, { 34, N },  SpellID::ChainLightning, SkillPassive::None,             SpellIcon::ChainLightning },
	/* 37 */ { N_("Lightning Mastery"),  HeroClass::Sorcerer, 1, 4, 1, { N, N },   SpellID::Invalid,        SkillPassive::LightningMastery, SpellIcon::Lightning },
	// Sorcerer: Arcane
	/* 38 */ { N_("Flash"),              HeroClass::Sorcerer, 2, 0, 0, { N, N },   SpellID::Flash,          SkillPassive::None,             SpellIcon::Flash },
	/* 39 */ { N_("Warmth"),             HeroClass::Sorcerer, 2, 0, 2, { N, N },   SpellID::Invalid,        SkillPassive::Warmth,           SpellIcon::Pentagram },
	/* 40 */ { N_("Stone Curse"),        HeroClass::Sorcerer, 2, 1, 0, { 38, N },  SpellID::StoneCurse,     SkillPassive::None,             SpellIcon::StoneCurse },
	/* 41 */ { N_("Teleport"),           HeroClass::Sorcerer, 2, 2, 1, { N, N },   SpellID::Teleport,       SkillPassive::None,             SpellIcon::Teleport },
	/* 42 */ { N_("Mana Shield"),        HeroClass::Sorcerer, 2, 3, 2, { 39, N },  SpellID::ManaShield,     SkillPassive::None,             SpellIcon::ManaShield },
	/* 43 */ { N_("Golem"),              HeroClass::Sorcerer, 2, 4, 0, { 40, N },  SpellID::Golem,          SkillPassive::None,             SpellIcon::Golem },
};
// clang-format on

constexpr int NumSkills = static_cast<int>(std::size(Skills));
static_assert(NumSkills <= MaxClassSkills);

constexpr int TicksPerSecond = 20;

struct HeroSkills {
	std::array<uint8_t, MaxClassSkills> levels {};
	uint16_t points = 0;
	// Active buffs (game ticks left, and the level they were cast at).
	int shoutTicks = 0;
	int shoutLevel = 0;
	int ordersTicks = 0;
	int ordersLevel = 0;
};

std::array<HeroSkills, MAX_PLRS> Heroes;

std::array<uint16_t, MaxMonsters> StunTicks {};
int StunLevelKey = -1;

HeroSkills &StateOf(const Player &player)
{
	return Heroes[std::min<size_t>(player.getId(), MAX_PLRS - 1)];
}

int LevelOf(const Player &player, SkillPassive passive)
{
	const HeroSkills &state = StateOf(player);
	for (int i = 0; i < NumSkills; i++) {
		if (Skills[i].passive == passive && Skills[i].heroClass == player._pClass)
			return state.levels[i];
	}
	return 0;
}

bool IsUtilitySpell(SpellID spell)
{
	switch (spell) {
	case SpellID::Healing:
	case SpellID::Identify:
	case SpellID::TownPortal:
	case SpellID::Infravision:
	case SpellID::Telekinesis:
	case SpellID::Resurrect:
	case SpellID::HealOther:
	case SpellID::ItemRepair:
	case SpellID::StaffRecharge:
	case SpellID::TrapDisarm:
	case SpellID::Search:
		return true;
	default:
		return false;
	}
}

// --- Combat helpers --------------------------------------------------------------------------

Monster *MonsterAt(Point tile)
{
	if (!InDungeonBounds(tile))
		return nullptr;
	const int mi = dMonster[tile.x][tile.y];
	if (mi == 0)
		return nullptr;
	Monster &monster = Monsters[std::abs(mi) - 1];
	if (monster.hasNoLife() || !monster.isPossibleToHit() || monster.isPlayerMinion())
		return nullptr;
	return &monster;
}

void Stun(Monster &monster, int ticks)
{
	const size_t id = monster.getId();
	if (id < StunTicks.size())
		StunTicks[id] = static_cast<uint16_t>(std::max<int>(StunTicks[id], ticks));
}

/** Hits a monster with a non-weapon skill effect (War Cry). */
void SkillDamage(Player &player, Monster &monster, int minDamage, int maxDamage)
{
	if (monster.hasNoLife())
		return;
	const int dam = RandomIntBetween(minDamage, maxDamage) << 6;
	if (&player == MyPlayer)
		ApplyMonsterDamage(DamageType::Physical, monster, Fixed26_6::fromRaw(dam));
	if (monster.hasNoLife())
		M_StartKill(monster, player);
	else
		M_StartHit(monster, player, dam);
}

Point Rotate(Point origin, Point target, float degrees, float minLength)
{
	float dx = static_cast<float>(target.x - origin.x);
	float dy = static_cast<float>(target.y - origin.y);
	float len = std::sqrt(dx * dx + dy * dy);
	if (len < 0.5f) {
		dx = 1;
		dy = 0;
		len = 1;
	}
	const float scale = std::max(1.0f, minLength / len);
	const float r = degrees * 3.14159265f / 180.0f;
	const float rx = (dx * std::cos(r) - dy * std::sin(r)) * scale;
	const float ry = (dx * std::sin(r) + dy * std::cos(r)) * scale;
	return { origin.x + static_cast<int>(std::lround(rx)), origin.y + static_cast<int>(std::lround(ry)) };
}

Direction DirectionTo(Point from, Point to, Direction fallback)
{
	if (from == to)
		return fallback;
	return GetDirection(from, to);
}

bool HoldsBow(const Player &player)
{
	return player.isHoldingItem(ItemType::Bow);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Table access

std::span<const ClassSkill> GetClassSkills()
{
	return { Skills, static_cast<size_t>(NumSkills) };
}

const char *ClassSkillTabName(HeroClass heroClass, int tab)
{
	static constexpr const char *Warrior[] = { N_("Combat"), N_("Masteries"), N_("Warcries") };
	static constexpr const char *Rogue[] = { N_("Bow"), N_("Passive & Magic"), N_("Traps") };
	static constexpr const char *Sorcerer[] = { N_("Fire"), N_("Lightning"), N_("Arcane") };
	tab = std::clamp(tab, 0, 2);
	switch (heroClass) {
	case HeroClass::Warrior:
	case HeroClass::Barbarian:
		return Warrior[tab];
	case HeroClass::Rogue:
	case HeroClass::Bard:
		return Rogue[tab];
	default:
		return Sorcerer[tab];
	}
}

int ClassSkillRequiredLevel(const ClassSkill &skill)
{
	static constexpr int Levels[] = { 1, 6, 12, 18, 24, 30 };
	return Levels[std::min<int>(skill.row, 5)];
}

bool ClassHasSkillTree(HeroClass heroClass)
{
	return IsAnyOf(heroClass, HeroClass::Warrior, HeroClass::Rogue, HeroClass::Sorcerer);
}

// ---------------------------------------------------------------------------------------------
// Per-hero state

int GetClassSkillLevel(const Player &player, int skillIndex)
{
	if (skillIndex < 0 || skillIndex >= NumSkills)
		return 0;
	return StateOf(player).levels[skillIndex];
}

int GetUnspentSkillPoints(const Player &player)
{
	return StateOf(player).points;
}

std::string ClassSkillBlockReason(const Player &player, int skillIndex)
{
	if (skillIndex < 0 || skillIndex >= NumSkills)
		return std::string(_("Unknown skill"));
	const ClassSkill &skill = Skills[skillIndex];
	const HeroSkills &state = StateOf(player);
	if (skill.heroClass != player._pClass)
		return std::string(_("Another class's skill"));
	if (state.levels[skillIndex] >= MaxClassSkillLevel)
		return std::string(_("Maximum level reached"));
	if (player.getCharacterLevel() < ClassSkillRequiredLevel(skill))
		return FormatRuntime(_("Requires character level {:d}"), ClassSkillRequiredLevel(skill));
	for (const int8_t pre : skill.prereq) {
		if (pre >= 0 && state.levels[pre] == 0)
			return FormatRuntime(_("Requires {:s}"), _(Skills[pre].name));
	}
	if (state.points == 0)
		return std::string(_("No skill points left"));
	return {};
}

bool SpendSkillPoint(Player &player, int skillIndex)
{
	if (!ClassSkillBlockReason(player, skillIndex).empty())
		return false;
	HeroSkills &state = StateOf(player);
	state.levels[skillIndex]++;
	state.points--;
	RefreshClassSkillSpells(player);
	CalcPlrInv(player, true);
	return true;
}

void OnClassSkillLevelUp(Player &player)
{
	if (!ClassHasSkillTree(player._pClass))
		return;
	StateOf(player).points++;
}

void ResetClassSkills(Player &player)
{
	HeroSkills &state = StateOf(player);
	state = {};
	state.points = static_cast<uint16_t>(ClassHasSkillTree(player._pClass) ? player.getCharacterLevel() : 0);
}

bool IsCombatSpell(SpellID spell)
{
	if (spell <= SpellID::Null || spell > SpellID::LastClassSkill)
		return false;
	return !IsUtilitySpell(spell);
}

void RefreshClassSkillSpells(Player &player)
{
	if (!ClassHasSkillTree(player._pClass))
		return;
	const HeroSkills &state = StateOf(player);
	for (int s = static_cast<int>(SpellID::Firebolt); s <= static_cast<int>(SpellID::LastClassSkill) && s < 64; s++) {
		const auto spell = static_cast<SpellID>(s);
		if (!IsCombatSpell(spell))
			continue;
		int level = 0;
		for (int i = 0; i < NumSkills; i++) {
			if (Skills[i].spell == spell && Skills[i].heroClass == player._pClass)
				level = std::max<int>(level, state.levels[i]);
		}
		// Diablo's own spells were balanced for level 15 at most.
		if (spell <= SpellID::LAST)
			level = std::min(level, 15);
		const uint8_t old = player._pSplLvl[s];
		player._pSplLvl[s] = static_cast<uint8_t>(level);
		if (level > 0)
			player._pMemSpells |= GetSpellBitmask(spell);
		else
			player._pMemSpells &= ~GetSpellBitmask(spell);
		if (gbIsMultiplayer && &player == MyPlayer && old != level)
			NetSendCmdParam2(true, CMD_CHANGE_SPELL_LEVEL, static_cast<uint16_t>(s), static_cast<uint16_t>(level));
	}
	if (player._pRSplType == SpellType::Spell && IsCombatSpell(player._pRSpell) && player.GetSpellLevel(player._pRSpell) <= 0) {
		player._pRSpell = SpellID::Invalid;
		player._pRSplType = SpellType::Invalid;
	}
}

namespace {
constexpr uint8_t SkillsFileVersion = 1;
}

void SetClassSkillState(Player &player, int points, std::span<const uint8_t> levels)
{
	HeroSkills &state = StateOf(player);
	state = {};
	state.points = static_cast<uint16_t>(std::clamp(points, 0, 0xFFFF));
	for (size_t i = 0; i < levels.size() && i < static_cast<size_t>(NumSkills); i++) {
		if (Skills[i].heroClass == player._pClass)
			state.levels[i] = std::min<uint8_t>(levels[i], MaxClassSkillLevel);
	}
	RefreshClassSkillSpells(player);
	CalcPlrInv(player, true);
}

// ---------------------------------------------------------------------------------------------
// Casting

bool IsMeleeSkill(SpellID spell)
{
	return IsAnyOf(spell, SpellID::SkillBash, SpellID::SkillStun, SpellID::SkillCleave, SpellID::SkillWhirlwind);
}

bool IsWeaponSkill(SpellID spell)
{
	switch (spell) {
	case SpellID::SkillBash:
	case SpellID::SkillStun:
	case SpellID::SkillCleave:
	case SpellID::SkillWhirlwind:
	case SpellID::SkillMultishot:
	case SpellID::SkillFireArrow:
	case SpellID::SkillStrafe:
	case SpellID::SkillLightningArrow:
		return true;
	default:
		return false;
	}
}

bool ClassSkillCanCast(const Player &player, SpellID spell)
{
	switch (spell) {
	case SpellID::SkillMultishot:
	case SpellID::SkillFireArrow:
	case SpellID::SkillStrafe:
	case SpellID::SkillLightningArrow:
		return HoldsBow(player);
	case SpellID::SkillBash:
	case SpellID::SkillStun:
	case SpellID::SkillCleave:
	case SpellID::SkillWhirlwind:
		return !HoldsBow(player);
	default:
		return true;
	}
}

bool CastClassSkill(Player &player, SpellID spell, Point src, Point dst, int level)
{
	level = std::max(level, 1);
	const Point front = src + player._pdir;
	switch (spell) {
	case SpellID::SkillBash: {
		if (Monster *monster = MonsterAt(front); monster != nullptr)
			PlrSkillStrike(player, *monster, 40 + 8 * level, 20 + 4 * level, /*knockback=*/true);
		return true;
	}
	case SpellID::SkillStun: {
		if (Monster *monster = MonsterAt(front); monster != nullptr) {
			if (PlrSkillStrike(player, *monster, 0, 15 + 5 * level, false) && !monster->hasNoLife())
				Stun(*monster, TicksPerSecond + level * TicksPerSecond / 5);
		}
		return true;
	}
	case SpellID::SkillCleave: {
		// The target and the two tiles flanking it.
		for (const Direction d : { player._pdir, Left(player._pdir), Right(player._pdir) }) {
			if (Monster *monster = MonsterAt(src + d); monster != nullptr)
				PlrSkillStrike(player, *monster, 20 + 6 * level, 10, false);
		}
		return true;
	}
	case SpellID::SkillWhirlwind: {
		for (int d = 0; d < 8; d++) {
			if (Monster *monster = MonsterAt(src + static_cast<Direction>(d)); monster != nullptr)
				PlrSkillStrike(player, *monster, 6 * level, 10 + 2 * level, false);
		}
		return true;
	}
	case SpellID::SkillShout: {
		HeroSkills &state = StateOf(player);
		state.shoutTicks = (10 + 2 * level) * TicksPerSecond;
		state.shoutLevel = level;
		CalcPlrInv(player, true);
		return true;
	}
	case SpellID::SkillBattleOrders: {
		HeroSkills &state = StateOf(player);
		const bool fresh = state.ordersTicks == 0;
		state.ordersTicks = (20 + 4 * level) * TicksPerSecond;
		state.ordersLevel = level;
		CalcPlrInv(player, true);
		if (fresh) {
			// The extra life and mana come in full, like Diablo II.
			player._pHitPoints = player._pMaxHP;
			player._pHPBase = player._pMaxHPBase;
		}
		return true;
	}
	case SpellID::SkillWarCry: {
		const int radius = 4;
		const int bonus = player.getCharacterLevel() / 2;
		for (int dy = -radius; dy <= radius; dy++) {
			for (int dx = -radius; dx <= radius; dx++) {
				if (Monster *monster = MonsterAt(src + Displacement { dx, dy }); monster != nullptr) {
					SkillDamage(player, *monster, 5 + 3 * level + bonus, 10 + 5 * level + bonus);
					if (!monster->hasNoLife())
						Stun(*monster, TicksPerSecond / 2 + level * TicksPerSecond / 10);
				}
			}
		}
		return true;
	}
	case SpellID::SkillMultishot: {
		const int arrows = std::min(2 + (level + 1) / 2, 10);
		const float spread = 7.0f;
		for (int i = 0; i < arrows; i++) {
			const float angle = (static_cast<float>(i) - static_cast<float>(arrows - 1) / 2.0f) * spread;
			const Point target = Rotate(src, dst, angle, 8.0f);
			AddMissile(src, target, DirectionTo(src, target, player._pdir), MissileID::Arrow, TARGET_MONSTERS, player, 0, 0);
		}
		return true;
	}
	case SpellID::SkillFireArrow:
		AddMissile(src, dst, player._pdir, MissileID::FireArrow, TARGET_MONSTERS, player, 0, level);
		return true;
	case SpellID::SkillLightningArrow:
		AddMissile(src, dst, player._pdir, MissileID::LightningArrow, TARGET_MONSTERS, player, 0, level);
		return true;
	case SpellID::SkillStrafe: {
		int shots = std::min(3 + level / 2, 10);
		const int range = 10;
		for (int r = 1; r <= range && shots > 0; r++) {
			for (int dy = -r; dy <= r && shots > 0; dy++) {
				for (int dx = -r; dx <= r && shots > 0; dx++) {
					if (std::max(std::abs(dx), std::abs(dy)) != r)
						continue;
					const Point tile = src + Displacement { dx, dy };
					Monster *monster = MonsterAt(tile);
					if (monster == nullptr || !IsTileVisible(tile))
						continue;
					AddMissile(src, tile, DirectionTo(src, tile, player._pdir), MissileID::Arrow, TARGET_MONSTERS, player, 0, 0);
					shots--;
				}
			}
		}
		if (shots == std::min(3 + level / 2, 10))
			AddMissile(src, dst, player._pdir, MissileID::Arrow, TARGET_MONSTERS, player, 0, 0); // nothing in sight: one arrow ahead
		return true;
	}
	case SpellID::SkillInnerSight: {
		const int radius = 7;
		const int reduction = 5 + 3 * level;
		for (int dy = -radius; dy <= radius; dy++) {
			for (int dx = -radius; dx <= radius; dx++) {
				if (Monster *monster = MonsterAt(src + Displacement { dx, dy }); monster != nullptr)
					monster->armorClass = static_cast<uint8_t>(std::max(0, monster->armorClass - reduction));
			}
		}
		// A visible flash marks the cast.
		AddMissile(src, src, player._pdir, MissileID::Infravision, TARGET_MONSTERS, player, 0, level);
		return true;
	}
	default:
		return false;
	}
}

// ---------------------------------------------------------------------------------------------
// Passives and buffs

ClassSkillBonuses GetClassSkillBonuses(const Player &player, int baseArmor)
{
	ClassSkillBonuses b;
	if (!ClassHasSkillTree(player._pClass) || player.getId() >= MAX_PLRS)
		return b;
	const HeroSkills &state = StateOf(player);

	if (const int l = LevelOf(player, SkillPassive::WeaponMastery); l > 0 && !HoldsBow(player)) {
		b.damagePercent += 10 + 5 * l;
		b.toHit += 5 + 3 * l;
	}
	if (const int l = LevelOf(player, SkillPassive::IronSkin); l > 0)
		b.armor += baseArmor * (15 + 10 * l) / 100;
	if (const int l = LevelOf(player, SkillPassive::Toughness); l > 0)
		b.lifePercent += 5 + 3 * l;
	if (const int l = LevelOf(player, SkillPassive::NaturalResistance); l > 0) {
		const int r = std::min(10 + 3 * l, 60);
		b.fireResist += r;
		b.lightningResist += r;
		b.magicResist += r;
	}
	if (const int l = LevelOf(player, SkillPassive::IncreasedSpeed); l > 0)
		b.attackSpeed = l >= 12 ? 3 : (l >= 6 ? 2 : 1);
	if (const int l = LevelOf(player, SkillPassive::BowMastery); l > 0 && HoldsBow(player)) {
		b.damagePercent += 10 + 6 * l;
		b.toHit += 5 + 3 * l;
	}
	if (const int l = LevelOf(player, SkillPassive::Penetrate); l > 0)
		b.toHit += 10 + 5 * l;
	if (const int l = LevelOf(player, SkillPassive::Warmth); l > 0)
		b.manaPercent += 5 + 4 * l;

	if (state.shoutTicks > 0)
		b.armor += (baseArmor + b.armor) * (40 + 8 * state.shoutLevel) / 100;
	if (state.ordersTicks > 0) {
		b.lifePercent += 20 + 3 * state.ordersLevel;
		b.manaPercent += 20 + 3 * state.ordersLevel;
	}
	return b;
}

int ClassSkillCriticalChance(const Player &player)
{
	const int l = LevelOf(player, SkillPassive::CriticalStrike);
	return l > 0 ? std::min(5 + 3 * l, 75) : 0;
}

int ClassSkillDodgeChance(const Player &player)
{
	const int l = LevelOf(player, SkillPassive::Dodge);
	return l > 0 ? std::min(6 + 3 * l, 60) : 0;
}

int ClassSkillAvoidChance(const Player &player)
{
	const int l = LevelOf(player, SkillPassive::Avoid);
	return l > 0 ? std::min(6 + 3 * l, 60) : 0;
}

int ClassSkillElementBonus(const Player &player, DamageType damageType)
{
	int bonus = 0;
	if (damageType == DamageType::Fire) {
		if (const int l = LevelOf(player, SkillPassive::FireMastery); l > 0)
			bonus += 15 + 7 * l;
	}
	if (damageType == DamageType::Lightning) {
		if (const int l = LevelOf(player, SkillPassive::LightningMastery); l > 0)
			bonus += 15 + 7 * l;
	}
	if (IsAnyOf(damageType, DamageType::Fire, DamageType::Magic)) {
		if (const int l = LevelOf(player, SkillPassive::TrapMastery); l > 0)
			bonus += 10 + 6 * l;
	}
	return bonus;
}

bool IsMonsterStunned(const Monster &monster)
{
	const size_t id = monster.getId();
	return id < StunTicks.size() && StunTicks[id] > 0;
}

void UpdateClassSkills()
{
	const int levelKey = (setlevel ? 1000 : 0) + currlevel * 8 + static_cast<int>(leveltype);
	if (levelKey != StunLevelKey) {
		StunTicks.fill(0);
		StunLevelKey = levelKey;
	}
	for (uint16_t &t : StunTicks) {
		if (t > 0)
			t--;
	}
	for (size_t p = 0; p < Players.size() && p < MAX_PLRS; p++) {
		HeroSkills &state = Heroes[p];
		bool changed = false;
		if (state.shoutTicks > 0 && --state.shoutTicks == 0)
			changed = true;
		if (state.ordersTicks > 0 && --state.ordersTicks == 0)
			changed = true;
		if (changed && Players[p].plractive)
			CalcPlrInv(Players[p], false);

		// Diablo II style mana regeneration: a full pool in about 45 seconds.
		Player &player = Players[p];
		if (player.plractive && ClassHasSkillTree(player._pClass) && !player.hasNoLife() && player._pmode != PM_DEATH
		    && HasNoneOf(player._pIFlags, ItemSpecialEffect::NoMana) && player._pMana < player._pMaxMana) {
			const int regen = std::max(player._pMaxMana.raw() / (45 * 20), 1);
			const Fixed26_6 amount = Fixed26_6::fromRaw(std::min(regen, (player._pMaxMana - player._pMana).raw()));
			player._pMana += amount;
			player._pManaBase += amount;
			if (&player == MyPlayer)
				RedrawComponent(PanelDrawComponent::Mana);
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Descriptions

std::vector<std::string> DescribeClassSkill(const ClassSkill &skill, int level)
{
	const auto line = [&skill](int l) -> std::string {
		if (l <= 0)
			return {};
		switch (skill.passive) {
		case SkillPassive::WeaponMastery: return FormatRuntime(_("+{:d}% damage, +{:d}% to hit (melee)"), 10 + 5 * l, 5 + 3 * l);
		case SkillPassive::IronSkin: return FormatRuntime(_("+{:d}% armor"), 15 + 10 * l);
		case SkillPassive::Toughness: return FormatRuntime(_("+{:d}% life"), 5 + 3 * l);
		case SkillPassive::NaturalResistance: return FormatRuntime(_("+{:d}% all resistances"), std::min(10 + 3 * l, 60));
		case SkillPassive::IncreasedSpeed: return l >= 12 ? std::string(_("Fastest attack")) : (l >= 6 ? std::string(_("Faster attack")) : std::string(_("Fast attack")));
		case SkillPassive::BowMastery: return FormatRuntime(_("+{:d}% damage, +{:d}% to hit (bows)"), 10 + 6 * l, 5 + 3 * l);
		case SkillPassive::Penetrate: return FormatRuntime(_("+{:d}% to hit"), 10 + 5 * l);
		case SkillPassive::CriticalStrike: return FormatRuntime(_("{:d}% chance of double damage"), std::min(5 + 3 * l, 75));
		case SkillPassive::Dodge: return FormatRuntime(_("{:d}% chance to dodge melee attacks"), std::min(6 + 3 * l, 60));
		case SkillPassive::Avoid: return FormatRuntime(_("{:d}% chance to avoid missiles"), std::min(6 + 3 * l, 60));
		case SkillPassive::FireMastery: return FormatRuntime(_("+{:d}% fire spell damage"), 15 + 7 * l);
		case SkillPassive::LightningMastery: return FormatRuntime(_("+{:d}% lightning spell damage"), 15 + 7 * l);
		case SkillPassive::Warmth: return FormatRuntime(_("+{:d}% mana"), 5 + 4 * l);
		case SkillPassive::TrapMastery: return FormatRuntime(_("+{:d}% fire and magic damage"), 10 + 6 * l);
		case SkillPassive::None: break;
		}
		switch (skill.spell) {
		case SpellID::SkillBash: return FormatRuntime(_("+{:d}% damage, +{:d}% to hit, knockback"), 40 + 8 * l, 20 + 4 * l);
		case SpellID::SkillStun: return FormatRuntime(_("+{:d}% to hit, stuns for {:d}.{:d}s"), 15 + 5 * l, 1 + l / 5, (l % 5) * 2);
		case SpellID::SkillCleave: return FormatRuntime(_("Hits 3 foes for +{:d}% damage"), 20 + 6 * l);
		case SpellID::SkillWhirlwind: return FormatRuntime(_("Hits all around for +{:d}% damage"), 6 * l);
		case SpellID::SkillShout: return FormatRuntime(_("+{:d}% armor for {:d}s"), 40 + 8 * l, 10 + 2 * l);
		case SpellID::SkillWarCry: return FormatRuntime(_("{:d}-{:d} damage and stun nearby"), 5 + 3 * l, 10 + 5 * l);
		case SpellID::SkillBattleOrders: return FormatRuntime(_("+{:d}% life and mana for {:d}s"), 20 + 3 * l, 20 + 4 * l);
		case SpellID::SkillMultishot: return FormatRuntime(_("Fires {:d} arrows"), std::min(2 + (l + 1) / 2, 10));
		case SpellID::SkillFireArrow: return FormatRuntime(_("Adds {:d}-{:d} fire damage"), 1 + 3 * l, 3 + 5 * l);
		case SpellID::SkillLightningArrow: return FormatRuntime(_("Adds 1-{:d} lightning damage"), 6 + 6 * l);
		case SpellID::SkillStrafe: return FormatRuntime(_("Shoots up to {:d} foes"), std::min(3 + l / 2, 10));
		case SpellID::SkillInnerSight: return FormatRuntime(_("-{:d} armor to nearby foes"), 5 + 3 * l);
		default: {
			const int spellLevel = std::min(l, 15);
			if (MyPlayer != nullptr && skill.spell > SpellID::Null && skill.spell <= SpellID::LAST) {
				const auto [min, max] = GetDamageAmt(skill.spell, spellLevel);
				if (min != -1 && max > 0)
					return FormatRuntime(_("Damage {:d}-{:d} (spell level {:d})"), min, max, spellLevel);
			}
			return FormatRuntime(_("Spell level {:d}"), spellLevel);
		}
		}
	};
	std::vector<std::string> lines;
	if (level > 0)
		lines.push_back(StrCat(_("Current: "), line(level)));
	if (level < MaxClassSkillLevel)
		lines.push_back(StrCat(_("Next level: "), line(level + 1)));
	if (skill.spell != SpellID::Invalid && skill.passive == SkillPassive::None && IsWeaponSkill(skill.spell)) {
		const bool bow = IsAnyOf(skill.spell, SpellID::SkillMultishot, SpellID::SkillFireArrow, SpellID::SkillStrafe, SpellID::SkillLightningArrow);
		lines.push_back(bow ? std::string(_("Requires a bow")) : std::string(_("Requires a melee weapon")));
	}
	return lines;
}

} // namespace devilution
