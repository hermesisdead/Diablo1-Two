/**
 * @file panels/skill_tree.hpp
 *
 * Diablo II style skill tree panel (opens in the right side panel slot, key T).
 */
#pragma once

#include "engine/point.hpp"
#include "engine/surface.hpp"

namespace devilution {

/** @brief True while the right side panel shows the skill tree instead of the spellbook. */
extern bool SkillTreeOpen;
extern int SkillTreeTab;
extern bool SkillButtonDown;

/** @brief The local hero's class has a skill tree. */
bool SkillTreeAvailable();

/** @brief T key: toggle the skill tree. */
void ToggleSkillTree();

void DrawSkillTree(const Surface &out);
/** @brief Left click inside the panel: spend points, switch tabs. */
void CheckSkillTreeClick(Point mousePosition);
/** @brief Right click inside the panel: ready the skill under the cursor. Returns true when handled. */
bool CheckSkillTreeRightClick(Point mousePosition);

/** @brief "New Skill" button above the main panel when skill points are unspent. */
void DrawSkillButton(const Surface &out);
void CheckSkillButton();
void CheckSkillButtonUp();

} // namespace devilution
