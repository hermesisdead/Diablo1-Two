#include "panels/skill_tree.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "class_skills.hpp"
#include "control/control.hpp"
#include "controls/control_mode.hpp"
#include "engine/backbuffer_state.hpp"
#include "engine/palette.h"
#include "engine/rectangle.hpp"
#include "engine/render/primitive_render.hpp"
#include "engine/render/text_render.hpp"
#include "inv.h"
#include "panels/spell_icons.hpp"
#include "panels/ui_panels.hpp"
#include "player.h"
#include "qol/stash.h"
#include "qol/visual_store.h"
#include "spells.h"
#include "stores.h"
#include "utils/format.hpp"
#include "utils/language.h"
#include "utils/str_cat.hpp"

namespace devilution {

bool SkillTreeOpen = false;
int SkillTreeTab = 0;
bool SkillButtonDown = false;

namespace {

// Layout inside the 320x352 side panel.
constexpr int IconWidth = 37;
constexpr int IconHeight = 38;
constexpr int GridTop = 44;
constexpr int RowSpacing = 45;
constexpr int ColumnCenters[3] = { 62, 160, 258 };
constexpr int TabTop = 318;
constexpr int TabHeight = 26;
constexpr int TabLeft = 8;
constexpr int TabWidth = 101;

constexpr uint8_t BackgroundColor = PAL16_BEIGE + 15;
constexpr uint8_t FrameColor = PAL16_YELLOW + 6;
constexpr uint8_t LineColor = PAL16_YELLOW + 9;
constexpr uint8_t LineLitColor = PAL16_YELLOW + 2;

/** Skill button: mirrors the "Level Up" button on the right side of the main panel. */
const Rectangle SkillButtonRect = { { 640 - 40 - 61, -39 }, { 61, 22 } };

/** Top-left corner of a skill icon, panel-relative. */
Point IconTopLeft(const ClassSkill &skill)
{
	return { ColumnCenters[skill.column] - IconWidth / 2, GridTop + skill.row * RowSpacing };
}

Rectangle IconRect(const ClassSkill &skill)
{
	return { GetPanelPosition(UiPanels::Spell, IconTopLeft(skill)), Size { IconWidth, IconHeight } };
}

int SkillUnderCursor(Point mousePosition)
{
	const Player &player = *InspectPlayer;
	const auto skills = GetClassSkills();
	for (size_t i = 0; i < skills.size(); i++) {
		const ClassSkill &skill = skills[i];
		if (skill.heroClass != player._pClass || skill.tab != SkillTreeTab)
			continue;
		if (IconRect(skill).contains(mousePosition))
			return static_cast<int>(i);
	}
	return -1;
}

void DrawPanelFrame(const Surface &out)
{
	const Point origin = GetPanelPosition(UiPanels::Spell, { 0, 0 });
	FillRect(out, origin.x, origin.y, SidePanelSize.width, SidePanelSize.height, BackgroundColor);
	UnsafeDrawBorder2px(out, Rectangle { origin, SidePanelSize }, FrameColor);
	// Inner hairline.
	DrawHorizontalLine(out, origin + Displacement { 5, 5 }, SidePanelSize.width - 10, PAL16_YELLOW + 11);
	DrawHorizontalLine(out, origin + Displacement { 5, SidePanelSize.height - 6 }, SidePanelSize.width - 10, PAL16_YELLOW + 11);
	DrawVerticalLine(out, origin + Displacement { 5, 5 }, SidePanelSize.height - 10, PAL16_YELLOW + 11);
	DrawVerticalLine(out, origin + Displacement { SidePanelSize.width - 6, 5 }, SidePanelSize.height - 10, PAL16_YELLOW + 11);
}

/** Draws the connector from a prerequisite to the skill that needs it (vertical, or an L-shape across columns). */
void DrawConnector(const Surface &out, const ClassSkill &from, const ClassSkill &to, bool lit)
{
	const uint8_t color = lit ? LineLitColor : LineColor;
	const Point a = GetPanelPosition(UiPanels::Spell, IconTopLeft(from) + Displacement { IconWidth / 2, IconHeight });
	const Point b = GetPanelPosition(UiPanels::Spell, IconTopLeft(to) + Displacement { IconWidth / 2, 0 });
	if (a.x == b.x) {
		for (int dx = -1; dx <= 0; dx++)
			DrawVerticalLine(out, { a.x + dx, a.y }, b.y - a.y, color);
		return;
	}
	const int midY = b.y - 4;
	for (int dx = -1; dx <= 0; dx++) {
		DrawVerticalLine(out, { a.x + dx, a.y }, midY - a.y, color);
		DrawVerticalLine(out, { b.x + dx, midY }, b.y - midY, color);
	}
	const int left = std::min(a.x, b.x) - 1;
	const int width = std::abs(a.x - b.x) + 2;
	DrawHorizontalLine(out, { left, midY - 1 }, width, color);
	DrawHorizontalLine(out, { left, midY }, width, color);
}

void DrawTooltip(const Surface &out, const Player &player, int skillIndex)
{
	const ClassSkill &skill = GetClassSkills()[skillIndex];
	const int level = GetClassSkillLevel(player, skillIndex);

	struct Line {
		std::string text;
		UiFlags color;
	};
	std::vector<Line> lines;
	lines.push_back({ std::string(_(skill.name)), UiFlags::ColorWhitegold });
	if (skill.spell != SpellID::Invalid && skill.passive == SkillPassive::None) {
		std::string kind = std::string(_("Active skill"));
		if (level > 0)
			kind = StrCat(kind, " - ", FormatRuntime(_("Mana: {:d}"), GetManaAmount(player, skill.spell).whole()));
		lines.push_back({ kind, UiFlags::ColorBlue });
	} else {
		lines.push_back({ std::string(_("Passive skill")), UiFlags::ColorBlue });
	}
	lines.push_back({ FormatRuntime(_("Skill level: {:d} / {:d}"), level, MaxClassSkillLevel), UiFlags::ColorWhite });
	for (std::string &text : DescribeClassSkill(skill, level))
		lines.push_back({ std::move(text), UiFlags::ColorWhite });
	lines.push_back({ FormatRuntime(_("Required level: {:d}"), ClassSkillRequiredLevel(skill)), UiFlags::ColorWhite });
	if (!IsInspectingPlayer()) {
		const std::string reason = ClassSkillBlockReason(player, skillIndex);
		if (reason.empty())
			lines.push_back({ std::string(_("Left click: add a point")), UiFlags::ColorGold });
		else
			lines.push_back({ reason, UiFlags::ColorRed });
		if (level > 0 && skill.passive == SkillPassive::None)
			lines.push_back({ std::string(_("Right click: ready this skill")), UiFlags::ColorGold });
	}

	constexpr int LineHeight = 14;
	int width = 0;
	for (const Line &line : lines)
		width = std::max(width, GetLineWidth(line.text, GameFont12, 1));
	width += 16;
	const int height = static_cast<int>(lines.size()) * LineHeight + 10;

	// To the left of the panel, level with the cursor, so the tree stays visible.
	const Point panel = GetPanelPosition(UiPanels::Spell, { 0, 0 });
	Point pos { panel.x - width - 6, MousePosition.y - height / 2 };
	pos.x = std::max(pos.x, 0);
	pos.y = std::clamp(pos.y, 0, std::max(GetMainPanel().position.y - height, 0));

	DrawHalfTransparentRectTo(out, pos.x, pos.y, width, height);
	DrawHalfTransparentRectTo(out, pos.x, pos.y, width, height);
	UnsafeDrawBorder2px(out, Rectangle { pos, Size { width, height } }, PAL16_GRAY + 6);
	int y = pos.y + 5;
	for (const Line &line : lines) {
		DrawString(out, line.text, { { pos.x, y }, { width, LineHeight } }, { .flags = line.color | UiFlags::AlignCenter });
		y += LineHeight;
	}
}

bool IsSkillButtonVisible()
{
	if (!SkillTreeAvailable() || GetUnspentSkillPoints(*MyPlayer) == 0)
		return false;
	if (SpellSelectFlag || (SpellbookFlag && SkillTreeOpen) || ControlMode == ControlTypes::VirtualGamepad)
		return false;
	if (IsPlayerInStore() || IsStashOpen || IsVisualStoreOpen)
		return false;
	return true;
}

Rectangle SkillButtonScreenRect()
{
	const Rectangle &mainPanel = GetMainPanel();
	return { mainPanel.position + Displacement { SkillButtonRect.position.x, SkillButtonRect.position.y }, SkillButtonRect.size };
}

} // namespace

bool SkillTreeAvailable()
{
	return MyPlayer != nullptr && ClassHasSkillTree(MyPlayer->_pClass);
}

void ToggleSkillTree()
{
	if (!SkillTreeAvailable() || IsPlayerInStore())
		return;
	if (SpellbookFlag && SkillTreeOpen) {
		SpellbookFlag = false;
		SkillTreeOpen = false;
		return;
	}
	CloseInventory();
	CloseVisualStore();
	SpellbookFlag = true;
	SkillTreeOpen = true;
	RedrawEverything();
}

void DrawSkillTree(const Surface &out)
{
	const Player &player = *InspectPlayer;
	DrawPanelFrame(out);

	const int points = GetUnspentSkillPoints(player);
	DrawString(out, _("Skill Tree"), { GetPanelPosition(UiPanels::Spell, { 0, 12 }), { SidePanelSize.width, 0 } },
	    { .flags = UiFlags::ColorWhitegold | UiFlags::AlignCenter });
	DrawString(out, FormatRuntime(_("Points: {:d}"), points), { GetPanelPosition(UiPanels::Spell, { 14, 12 }), { 120, 0 } },
	    { .flags = (points > 0 ? UiFlags::ColorGold : UiFlags::ColorWhite) });
	DrawString(out, FormatRuntime(_("Level {:d}"), player.getCharacterLevel()), { GetPanelPosition(UiPanels::Spell, { SidePanelSize.width - 134, 12 }), { 120, 0 } },
	    { .flags = UiFlags::ColorWhite | UiFlags::AlignRight });

	// Connectors first so icons sit on top of them.
	const auto skills = GetClassSkills();
	for (const ClassSkill &skill : skills) {
		if (skill.heroClass != player._pClass || skill.tab != SkillTreeTab)
			continue;
		for (const int8_t pre : skill.prereq) {
			if (pre < 0)
				continue;
			const bool lit = GetClassSkillLevel(player, pre) > 0;
			DrawConnector(out, skills[pre], skill, lit);
		}
	}

	int hovered = -1;
	for (size_t i = 0; i < skills.size(); i++) {
		const ClassSkill &skill = skills[i];
		if (skill.heroClass != player._pClass || skill.tab != SkillTreeTab)
			continue;
		const int index = static_cast<int>(i);
		const int level = GetClassSkillLevel(player, index);
		const bool learnable = ClassSkillBlockReason(player, index).empty();
		const bool reachable = player.getCharacterLevel() >= ClassSkillRequiredLevel(skill);

		SpellType trans = SpellType::Invalid;
		if (level > 0)
			trans = SpellType::Skill;
		else if (learnable || reachable)
			trans = SpellType::Spell;
		SetSpellTrans(trans);

		const Rectangle rect = IconRect(skill);
		const Point bottomLeft = rect.position + Displacement { 0, IconHeight - 1 };
		DrawSmallSpellIconFrame(out, bottomLeft, skill.icon);
		if (learnable && !IsInspectingPlayer()) {
			SetSpellTrans(SpellType::Skill);
			DrawSmallSpellIconBorder(out, bottomLeft);
		}
		if (skill.spell != SpellID::Invalid && skill.passive == SkillPassive::None && player._pRSpell == skill.spell && level > 0 && !IsInspectingPlayer())
			UnsafeDrawBorder2px(out, rect, PAL16_BLUE + 4);

		// Level box at the bottom right of the icon, like Diablo II.
		const Point box = rect.position + Displacement { IconWidth - 6, IconHeight - 12 };
		FillRect(out, box.x, box.y, 18, 14, 0);
		UnsafeDrawBorder2px(out, Rectangle { box + Displacement { -1, -1 }, Size { 20, 16 } }, PAL16_YELLOW + 8);
		DrawString(out, StrCat(level), { box + Displacement { 0, 1 }, { 18, 12 } },
		    { .flags = (level > 0 ? UiFlags::ColorWhite : UiFlags::ColorBlack) | UiFlags::AlignCenter, .spacing = 0 });

		if (rect.contains(MousePosition))
			hovered = index;
	}

	// Tabs.
	for (int tab = 0; tab < 3; tab++) {
		const Point tabPos = GetPanelPosition(UiPanels::Spell, { TabLeft + tab * (TabWidth + 1), TabTop });
		const bool active = tab == SkillTreeTab;
		FillRect(out, tabPos.x, tabPos.y, TabWidth, TabHeight, active ? PAL16_YELLOW + 12 : 0);
		UnsafeDrawBorder2px(out, Rectangle { tabPos, Size { TabWidth, TabHeight } }, active ? PAL16_YELLOW + 3 : PAL16_YELLOW + 10);
		int tabPoints = 0;
		for (size_t i = 0; i < skills.size(); i++) {
			if (skills[i].heroClass == player._pClass && skills[i].tab == tab)
				tabPoints += GetClassSkillLevel(player, static_cast<int>(i));
		}
		DrawString(out, _(ClassSkillTabName(player._pClass, tab)), { tabPos + Displacement { 0, 3 }, { TabWidth, 12 } },
		    { .flags = (active ? UiFlags::ColorWhitegold : UiFlags::ColorWhite) | UiFlags::AlignCenter | UiFlags::KerningFitSpacing });
		DrawString(out, StrCat(tabPoints), { tabPos + Displacement { 0, 13 }, { TabWidth, 12 } },
		    { .flags = UiFlags::ColorGold | UiFlags::AlignCenter });
	}

	if (hovered >= 0)
		DrawTooltip(out, player, hovered);
}

void CheckSkillTreeClick(Point mousePosition)
{
	for (int tab = 0; tab < 3; tab++) {
		const Rectangle tabRect { GetPanelPosition(UiPanels::Spell, { TabLeft + tab * (TabWidth + 1), TabTop }), Size { TabWidth, TabHeight } };
		if (tabRect.contains(mousePosition)) {
			SkillTreeTab = tab;
			return;
		}
	}
	if (IsInspectingPlayer())
		return;
	const int skillIndex = SkillUnderCursor(mousePosition);
	if (skillIndex < 0)
		return;
	Player &player = *MyPlayer;
	if (!SpendSkillPoint(player, skillIndex))
		return;
	const ClassSkill &skill = GetClassSkills()[skillIndex];
	// First point in an active skill with nothing readied: ready it straight away.
	if (skill.passive == SkillPassive::None && skill.spell != SpellID::Invalid
	    && (player._pRSpell == SpellID::Invalid || player._pRSplType == SpellType::Invalid)) {
		player._pRSpell = skill.spell;
		player._pRSplType = SpellType::Spell;
	}
	RedrawEverything();
}

bool CheckSkillTreeRightClick(Point mousePosition)
{
	if (IsInspectingPlayer())
		return false;
	const int skillIndex = SkillUnderCursor(mousePosition);
	if (skillIndex < 0)
		return false;
	Player &player = *MyPlayer;
	const ClassSkill &skill = GetClassSkills()[skillIndex];
	if (skill.passive != SkillPassive::None || skill.spell == SpellID::Invalid || GetClassSkillLevel(player, skillIndex) == 0)
		return true;
	player._pRSpell = skill.spell;
	player._pRSplType = SpellType::Spell;
	RedrawEverything();
	return true;
}

void DrawSkillButton(const Surface &out)
{
	if (!IsSkillButtonVisible())
		return;
	const Rectangle rect = SkillButtonScreenRect();
	DrawString(out, _("New Skill"), { { rect.position.x - 30, rect.position.y - 16 }, { rect.size.width + 60, 0 } },
	    { .flags = UiFlags::ColorWhite | UiFlags::AlignCenter | UiFlags::KerningFitSpacing });
	FillRect(out, rect.position.x, rect.position.y, rect.size.width, rect.size.height, SkillButtonDown ? PAL16_YELLOW + 12 : PAL16_BEIGE + 14);
	UnsafeDrawBorder2px(out, rect, SkillButtonDown ? PAL16_YELLOW + 8 : PAL16_YELLOW + 4);
	DrawString(out, StrCat("+", GetUnspentSkillPoints(*MyPlayer)), { rect.position + Displacement { 0, 4 }, { rect.size.width, 14 } },
	    { .flags = UiFlags::ColorGold | UiFlags::AlignCenter });
}

void CheckSkillButton()
{
	if (!IsSkillButtonVisible())
		return;
	if (!SkillButtonDown && SkillButtonScreenRect().contains(MousePosition))
		SkillButtonDown = true;
}

void CheckSkillButtonUp()
{
	if (SkillButtonScreenRect().contains(MousePosition) && !(SpellbookFlag && SkillTreeOpen))
		ToggleSkillTree();
	SkillButtonDown = false;
}

} // namespace devilution
