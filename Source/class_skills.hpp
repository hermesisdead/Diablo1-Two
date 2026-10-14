/**
 * @file class_skills.hpp
 *
 * Diablo II style class skills: every class has three skill tabs; heroes earn one skill
 * point per level and spend it on skills whose level and prerequisites they meet. Active
 * skills are spells (existing Diablo spells for the Sorcerer, new SpellIDs for the Warrior's
 * and Rogue's combat skills); passive skills feed into the hero's stats and damage.
 */
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "engine/point.hpp"
#include "panels/spell_icons.hpp"
#include "tables/playerdat.hpp"
#include "tables/spelldat.h"

namespace devilution {

struct Player;
struct Monster;
enum class DamageType : uint8_t;

enum class SkillPassive : uint8_t {
	None,
	WeaponMastery,
	IronSkin,
	Toughness,
	NaturalResistance,
	IncreasedSpeed,
	BowMastery,
	Penetrate,
	CriticalStrike,
	Dodge,
	Avoid,
	FireMastery,
	LightningMastery,
	Warmth,
	TrapMastery,
};

struct ClassSkill {
	const char *name;
	HeroClass heroClass;
	uint8_t tab;    // 0..2
	uint8_t row;    // 0..5: required level 1, 6, 12, 18, 24, 30
	uint8_t column; // 0..2
	int8_t prereq[2]; // indices into GetClassSkills(), -1 for none
	SpellID spell;    // SpellID::Invalid for passives
	SkillPassive passive;
	SpellIcon icon;
};

constexpr int MaxClassSkillLevel = 20;
constexpr int MaxClassSkills = 64;

/** @brief All class skills, every class. Indices are stable (they are saved). */
std::span<const ClassSkill> GetClassSkills();

/** @brief Tab names for a class. */
const char *ClassSkillTabName(HeroClass heroClass, int tab);

/** @brief The level a skill tier (row) requires. */
int ClassSkillRequiredLevel(const ClassSkill &skill);

/** @brief Classes that have a skill tree in this build. */
bool ClassHasSkillTree(HeroClass heroClass);

// --- Per-hero state -----------------------------------------------------------------------

int GetClassSkillLevel(const Player &player, int skillIndex);
int GetUnspentSkillPoints(const Player &player);

/** @brief Why a point cannot be spent, or empty when it can. */
std::string ClassSkillBlockReason(const Player &player, int skillIndex);

/** @brief Spends one point on a skill (local hero). Returns false when not allowed. */
bool SpendSkillPoint(Player &player, int skillIndex);

/** @brief Called on level up: grants a skill point. */
void OnClassSkillLevelUp(Player &player);

/** @brief Reset a hero to no skills and one point per character level (new hero, or old save without skills). */
void ResetClassSkills(Player &player);

/** @brief Re-derive spell levels/known spells from skills (and drop combat spells from books). */
void RefreshClassSkillSpells(Player &player);

/** @brief Restores saved skills (save format lives in loadsave.cpp: LoadClassSkills/SaveClassSkills). */
void SetClassSkillState(Player &player, int points, std::span<const uint8_t> levels);

// --- Gameplay hooks -------------------------------------------------------------------------

/** @brief True for spells that belong to class skill trees (books no longer teach them). */
bool IsCombatSpell(SpellID spell);

/** @brief True for the Warrior/Rogue weapon skills that use the attack animation. */
bool IsWeaponSkill(SpellID spell);

/** @brief Weapon skills that need the hero next to the target (walks there first, like an attack). */
bool IsMeleeSkill(SpellID spell);

/** @brief Casts a class skill spell (new SpellIDs). Returns false if it fizzled. */
bool CastClassSkill(Player &player, SpellID spell, Point src, Point dst, int level);

/** @brief Mana cost override for class skills (D2 style flat costs). */
bool ClassSkillCanCast(const Player &player, SpellID spell);

struct ClassSkillBonuses {
	int damagePercent = 0;
	int toHit = 0;
	int armor = 0;
	int fireResist = 0;
	int lightningResist = 0;
	int magicResist = 0;
	int lifePercent = 0;
	int manaPercent = 0;
	int attackSpeed = 0; // 0..3: quick, fast, faster, fastest
};

/** @brief Stat bonuses from passives and active buffs (warcries). */
ClassSkillBonuses GetClassSkillBonuses(const Player &player, int baseArmor);

/** @brief Chance (percent) of a critical (double damage) physical hit. */
int ClassSkillCriticalChance(const Player &player);
/** @brief Chance (percent) to dodge a melee hit / avoid a missile. */
int ClassSkillDodgeChance(const Player &player);
int ClassSkillAvoidChance(const Player &player);
/** @brief Bonus percent to the hero's spell damage of this element. */
int ClassSkillElementBonus(const Player &player, DamageType damageType);

/** @brief Monster stun from Stun / War Cry. */
bool IsMonsterStunned(const Monster &monster);

/** @brief Per game tick: buff timers, stun timers. */
void UpdateClassSkills();

/** @brief Text lines describing a skill at a level (current level and next). */
std::vector<std::string> DescribeClassSkill(const ClassSkill &skill, int level);

} // namespace devilution
