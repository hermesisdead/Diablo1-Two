/**
 * @file action_mode.cpp
 *
 * Optional action mode: close-up camera over the original isometric art and
 * Souls-like controls.
 *
 * Keyboard & mouse (while no panel is open)
 *   WASD / arrows  move            Space  dodge (costs stamina, brief invulnerability)
 *   Left mouse     light attack    Right mouse  cast the current spell
 *   Q              lock on / off   E      interact (pick up, open, talk)
 *   R              health potion   F      mana potion
 *
 * Controller (while no panel is open)
 *   Left stick  move           Right stick  switch lock-on target (flick)
 *   RB          light attack   RT           cast the current spell
 *   B           dodge          A            interact
 *   X           health potion  Y            mana potion
 *   R3          lock on / off  D-pad L/R    previous / next quick spell
 *   D-pad up    spell list     D-pad down   automap
 *   LB          character      LT           spellbook
 *   Back        inventory      Start        game menu
 *
 * When a panel is open the regular bindings apply, so menus can be used with the
 * mouse, or with the controller's stick-driven cursor and buttons.
 */
#include "action_mode.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include "control/control.hpp"
#include "controls/control_mode.hpp"
#include "controls/controller.h"
#include "controls/controller_motion.h"
#include "controls/game_controls.h"
#include "controls/plrctrls.h"
#include "cursor.h"
#include "diablo.h"
#include "doom.h"
#include "engine/backbuffer_state.hpp"
#include "engine/palette.h"
#include "engine/point.hpp"
#include "engine/render/primitive_render.hpp"
#include "engine/render/scrollrt.h"
#include "engine/render/text_render.hpp"
#include "automap.h"
#include "gmenu.h"
#include "items.h"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "minitext.h"
#include "monster.h"
#include "msg.h"
#include "multi.h"
#include "objects.h"
#include "options.h"
#include "panels/spell_icons.hpp"
#include "player.h"
#include "qol/stash.h"
#include "stores.h"
#include "tables/playerdat.hpp"
#include "tables/spelldat.h"
#include "towners.h"
#include "utils/str_cat.hpp"

namespace devilution {

// Small wrappers exported from diablo.cpp.
bool GameIsRunningForActionMode();
bool NoPanelsOpenForActionMode();
void ActionCycleSpell(bool next);
void ActionDisplaySpells();

namespace {

constexpr float MaxStamina = 100.0f;
constexpr float DodgeCost = 32.0f;
constexpr float AttackCost = 9.0f;
constexpr float StaminaRegenPerSecond = 45.0f;
constexpr uint32_t StaminaRegenDelay = 600;
constexpr int DodgeSteps = 2;
constexpr int LockRange = 10;
constexpr int KeepLockRange = 13;

float Stamina = MaxStamina;
uint32_t LastStaminaSpend = 0;
uint32_t LastUpdateTicks = 0;

int LockedMonster = -1;

int DodgeStepsLeft = 0;
Direction DodgeDirection = Direction::South;
bool DodgeActive = false;
uint32_t DodgeStarted = 0;

bool AttackHeldByMouse = false;
bool MovedByInputLastTick = false;
bool ForcedZoom = false;
bool RightStickArmed = true;

void SpendStamina(float amount)
{
	Stamina = std::max(0.0f, Stamina - amount);
	LastStaminaSpend = SDL_GetTicks();
}

bool IsValidLockTarget(const Monster &monster, int range)
{
	if (monster.isInvalid || monster.hitPoints <= 0 || monster.mode == MonsterMode::Death)
		return false;
	if (monster.isPlayerMinion() || !monster.isPossibleToHit())
		return false;
	if (!InDungeonBounds(monster.position.tile))
		return false;
	if (!IsTileLit(monster.position.tile)) {
#ifdef _DEBUG
		if (std::getenv("DEVILUTIONX_DEV_ACTIONLOCK") == nullptr)
#endif
			return false;
	}
	const Point me = MyPlayer->position.tile;
	const int dist = std::max(std::abs(me.x - monster.position.tile.x), std::abs(me.y - monster.position.tile.y));
	return dist <= range;
}

Monster *LockedTarget()
{
	if (LockedMonster < 0)
		return nullptr;
	Monster &monster = Monsters[LockedMonster];
	if (!IsValidLockTarget(monster, KeepLockRange)) {
		LockedMonster = -1;
		return nullptr;
	}
	return &monster;
}

/** Screen-space vector for a tile delta (x right, y down), in tile-width units. */
void TileDeltaToScreen(int dx, int dy, float &sx, float &sy)
{
	sx = static_cast<float>(dx - dy);
	sy = static_cast<float>(dx + dy) * 0.5f;
}

int PickLockTarget()
{
	const Player &player = *MyPlayer;
	// Mouse users lock what they point at.
	if (ControlMode == ControlTypes::KeyboardAndMouse && pcursmonst >= 0 && IsValidLockTarget(Monsters[pcursmonst], LockRange))
		return pcursmonst;
	float fx, fy;
	const Displacement facing { player._pdir };
	TileDeltaToScreen(facing.deltaX, facing.deltaY, fx, fy);
	const float flen = std::max(0.001f, std::sqrt(fx * fx + fy * fy));
	int best = -1;
	float bestScore = 1e9f;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const int id = static_cast<int>(ActiveMonsters[i]);
		const Monster &monster = Monsters[id];
		if (!IsValidLockTarget(monster, LockRange))
			continue;
		const int dx = monster.position.tile.x - player.position.tile.x;
		const int dy = monster.position.tile.y - player.position.tile.y;
		float sx, sy;
		TileDeltaToScreen(dx, dy, sx, sy);
		const float len = std::max(0.001f, std::sqrt(sx * sx + sy * sy));
		const float cosAngle = (sx * fx + sy * fy) / (len * flen);
		const float score = len + (1.0f - cosAngle) * 3.0f;
		if (score < bestScore) {
			bestScore = score;
			best = id;
		}
	}
	return best;
}

void ToggleLock()
{
	if (LockedTarget() != nullptr) {
		LockedMonster = -1;
		return;
	}
	LockedMonster = PickLockTarget();
}

void SwitchLockTarget(float stickX, float stickY)
{
	const Monster *current = LockedTarget();
	if (current == nullptr)
		return;
	// Stick up is positive Y; screen down is positive.
	const float wantX = stickX, wantY = -stickY;
	const float wantLen = std::max(0.001f, std::sqrt(wantX * wantX + wantY * wantY));
	int best = -1;
	float bestScore = 1e9f;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const int id = static_cast<int>(ActiveMonsters[i]);
		if (id == LockedMonster)
			continue;
		const Monster &monster = Monsters[id];
		if (!IsValidLockTarget(monster, LockRange))
			continue;
		float sx, sy;
		TileDeltaToScreen(monster.position.tile.x - current->position.tile.x, monster.position.tile.y - current->position.tile.y, sx, sy);
		const float len = std::max(0.001f, std::sqrt(sx * sx + sy * sy));
		const float cosAngle = (sx * wantX + sy * wantY) / (len * wantLen);
		if (cosAngle < 0.5f) // within ~60 degrees of the flick
			continue;
		const float score = len * (2.0f - cosAngle);
		if (score < bestScore) {
			bestScore = score;
			best = id;
		}
	}
	if (best >= 0)
		LockedMonster = best;
}

constexpr Direction AxisToDirection[3][3] = {
	// y: NONE             UP                DOWN
	{ Direction::South, Direction::North, Direction::South },        // x NONE
	{ Direction::West, Direction::NorthWest, Direction::SouthWest }, // x LEFT
	{ Direction::East, Direction::NorthEast, Direction::SouthEast }, // x RIGHT
};

/** Movement input from WASD/arrows and the left stick (the D-pad is used for spells). */
AxisDirection ReadMoveInput()
{
	AxisDirection dir { AxisDirectionX_NONE, AxisDirectionY_NONE };
	const bool *keys = nullptr;
	int numKeys = 0;
	const auto *rawKeys = SDL_GetKeyboardState(&numKeys);
	(void)keys;
	const auto pressed = [&](SDL_Scancode code) { return code < numKeys && rawKeys[code] != 0; };
	if (!ChatFlag) {
		if (pressed(SDL_SCANCODE_A) || pressed(SDL_SCANCODE_LEFT))
			dir.x = AxisDirectionX_LEFT;
		if (pressed(SDL_SCANCODE_D) || pressed(SDL_SCANCODE_RIGHT))
			dir.x = dir.x == AxisDirectionX_LEFT ? AxisDirectionX_NONE : AxisDirectionX_RIGHT;
		if (pressed(SDL_SCANCODE_W) || pressed(SDL_SCANCODE_UP))
			dir.y = AxisDirectionY_UP;
		if (pressed(SDL_SCANCODE_S) || pressed(SDL_SCANCODE_DOWN))
			dir.y = dir.y == AxisDirectionY_UP ? AxisDirectionY_NONE : AxisDirectionY_DOWN;
	}
	if (dir.x == AxisDirectionX_NONE && dir.y == AxisDirectionY_NONE) {
		constexpr float Threshold = 0.4f;
		if (leftStickX <= -Threshold)
			dir.x = AxisDirectionX_LEFT;
		else if (leftStickX >= Threshold)
			dir.x = AxisDirectionX_RIGHT;
		if (leftStickY >= Threshold)
			dir.y = AxisDirectionY_UP;
		else if (leftStickY <= -Threshold)
			dir.y = AxisDirectionY_DOWN;
	}
	return dir;
}

bool HasMoveInput(AxisDirection dir)
{
	return dir.x != AxisDirectionX_NONE || dir.y != AxisDirectionY_NONE;
}

Direction ToDirection(AxisDirection dir)
{
	return AxisToDirection[static_cast<size_t>(dir.x)][static_cast<size_t>(dir.y)];
}

bool IsDiagonalBlocked(Point position, Direction dir)
{
	if (IsNoneOf(dir, Direction::North, Direction::East, Direction::South, Direction::West))
		return false;
	const Point leftStep = position + Left(dir);
	const Point rightStep = position + Right(dir);
	if (IsTileNotSolid(leftStep) && IsTileNotSolid(rightStep))
		return false;
	return !PosOkPlayer(*MyPlayer, leftStep) && !PosOkPlayer(*MyPlayer, rightStep);
}

void StepTowards(Player &player, Direction dir)
{
	const Point target = player.position.future + dir;
	if (!player.isWalking() && player.CanChangeAction())
		player._pdir = dir;
	if (PosOkPlayer(player, target) && IsDiagonalBlocked(player.position.future, dir)) {
		if (player._pmode == PM_STAND)
			StartStand(player, dir);
		return;
	}
	NetSendCmdLoc(player.getId(), true, CMD_WALKXY, target);
}

void StartDodge()
{
	Player &player = *MyPlayer;
	if (DodgeActive || Stamina < DodgeCost || leveltype == DTYPE_TOWN && false)
		return;
	if (player._pmode != PM_STAND && !player.isWalking())
		return; // can't roll out of an attack or a spell
	const AxisDirection input = ReadMoveInput();
	if (HasMoveInput(input)) {
		DodgeDirection = ToDirection(input);
	} else {
		DodgeDirection = Opposite(player._pdir); // backstep
	}
	SpendStamina(DodgeCost);
	DodgeStepsLeft = DodgeSteps;
	DodgeActive = true;
	DodgeStarted = SDL_GetTicks();
}

void UpdateDodge()
{
	if (!DodgeActive)
		return;
	Player &player = *MyPlayer;
	if (SDL_GetTicks() - DodgeStarted > 1500) {
		DodgeActive = false;
		DodgeStepsLeft = 0;
		return;
	}
	if (player._pmode == PM_STAND) {
		if (DodgeStepsLeft <= 0) {
			DodgeActive = false;
			return;
		}
		const Point target = player.position.future + DodgeDirection;
		if (!PosOkPlayer(player, target) || IsDiagonalBlocked(player.position.future, DodgeDirection)) {
			DodgeStepsLeft = 0;
			DodgeActive = false;
			return;
		}
		player._pdir = DodgeDirection;
		NetSendCmdLoc(player.getId(), true, CMD_WALKXY, target);
		DodgeStepsLeft--;
	}
}

bool IsAdjacent(Point a, Point b)
{
	return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y)) <= 1;
}

/** Light attack: hits the locked/pointed target or swings in place, without chasing. */
bool DoAttack(bool fromMouse)
{
	Player &player = *MyPlayer;
	if (leveltype == DTYPE_TOWN)
		return false;
	if (player._pmode != PM_STAND && !player.CanChangeAction() && !player.isWalking())
		return true;
	Monster *target = LockedTarget();
	if (target == nullptr && pcursmonst >= 0 && IsValidLockTarget(Monsters[pcursmonst], LockRange))
		target = &Monsters[pcursmonst];
	const bool ranged = player.UsesRangedWeapon();
	SpendStamina(AttackCost);
	if (target != nullptr) {
		if (ranged) {
			LastPlayerAction = PlayerActionType::AttackMonsterTarget;
			NetSendCmdParam1(true, CMD_RATTACKID, static_cast<uint16_t>(target->getId()));
			return true;
		}
		if (IsAdjacent(player.position.future, target->position.tile) || CanTalkToMonst(*target)) {
			LastPlayerAction = PlayerActionType::AttackMonsterTarget;
			NetSendCmdParam1(true, CMD_ATTACKID, static_cast<uint16_t>(target->getId()));
			return true;
		}
		const Direction dir = GetDirection(player.position.future, target->position.tile);
		LastPlayerAction = PlayerActionType::Attack;
		NetSendCmdLoc(MyPlayerId, true, CMD_SATTACKXY, player.position.future + dir);
		return true;
	}
	Direction dir = player._pdir;
	Point aim = player.position.future + dir;
	if (fromMouse && cursPosition != player.position.future) {
		aim = cursPosition;
		dir = GetDirection(player.position.future, aim);
	}
	LastPlayerAction = PlayerActionType::Attack;
	if (ranged)
		NetSendCmdLoc(MyPlayerId, true, CMD_RATTACKXY, aim);
	else
		NetSendCmdLoc(MyPlayerId, true, CMD_SATTACKXY, player.position.future + dir);
	return true;
}

void DoSpell()
{
	ActionModePrepareSpellTarget();
	PerformSpellAction();
}

/** Interact with the nearest item, object, NPC or talkable monster around the player. */
void DoInteract()
{
	const Player &player = *MyPlayer;
	const Point me = player.position.future;
	const Point ahead = me + player._pdir;
	int bestScore = 1 << 30;
	enum class Kind { None, Item, Object, Towner, Monster } kind = Kind::None;
	int bestId = -1;
	Point bestPos {};
	const auto consider = [&](Kind k, int id, Point pos, int priority) {
		const int dist = std::max(std::abs(pos.x - me.x), std::abs(pos.y - me.y));
		if (dist > 2)
			return;
		const int score = dist * 10 + (pos == ahead ? -5 : 0) + priority;
		if (score < bestScore) {
			bestScore = score;
			kind = k;
			bestId = id;
			bestPos = pos;
		}
	};
	for (int i = 0; i < ActiveItemCount; i++) {
		const int id = ActiveItems[i];
		consider(Kind::Item, id, Items[id].position, 0);
	}
	for (int i = 0; i < ActiveObjectCount; i++) {
		const int id = ActiveObjects[i];
		const Object &object = Objects[id];
		if (!object.canInteractWith() || object.IsDisabled())
			continue;
		consider(Kind::Object, id, object.position, 2);
	}
	if (leveltype == DTYPE_TOWN) {
		for (size_t i = 0; i < Towners.size(); i++)
			consider(Kind::Towner, static_cast<int>(i), Towners[i].position, 1);
	} else {
		for (size_t i = 0; i < ActiveMonsterCount; i++) {
			const int id = static_cast<int>(ActiveMonsters[i]);
			if (CanTalkToMonst(Monsters[id]))
				consider(Kind::Monster, id, Monsters[id].position.tile, 1);
		}
	}
	switch (kind) {
	case Kind::Item:
		NetSendCmdLocParam1(true, invflag ? CMD_GOTOGETITEM : CMD_GOTOAGETITEM, bestPos, static_cast<uint16_t>(bestId));
		break;
	case Kind::Object:
		LastPlayerAction = PlayerActionType::OperateObject;
		NetSendCmdLoc(MyPlayerId, true, CMD_OPOBJXY, bestPos);
		break;
	case Kind::Towner:
		NetSendCmdLocParam1(true, CMD_TALKXY, bestPos, static_cast<uint16_t>(bestId));
		break;
	case Kind::Monster:
		NetSendCmdParam1(true, CMD_ATTACKID, static_cast<uint16_t>(bestId));
		break;
	default:
		break;
	}
}

void FaceLockTarget()
{
	Player &player = *MyPlayer;
	const Monster *target = LockedTarget();
	if (target == nullptr || player._pmode != PM_STAND || DodgeActive)
		return;
	const Direction dir = GetDirection(player.position.tile, target->position.tile);
	if (dir != player._pdir && player.position.tile != target->position.tile)
		StartStand(player, dir);
}

void EnsureZoom()
{
	auto &zoom = GetOptions().Graphics.zoom;
	if (IsActionMode()) {
		if (!*zoom) {
			zoom.SetValue(true);
			ForcedZoom = true;
			CalculatePanelAreas();
			CalcViewportGeometry();
			RedrawEverything();
		}
	} else if (ForcedZoom) {
		ForcedZoom = false;
		if (*zoom) {
			zoom.SetValue(false);
			CalculatePanelAreas();
			CalcViewportGeometry();
			RedrawEverything();
		}
	}
}

} // namespace

bool IsActionMode()
{
	return *GetOptions().Gameplay.actionMode && MyPlayer != nullptr && GameIsRunningForActionMode();
}

bool IsActionModeFree()
{
	if (!IsActionMode())
		return false;
	return NoPanelsOpenForActionMode() && !IsPlayerInStore() && !ChatFlag && !gmenu_is_active() && !DropGoldFlag
	    && !IsWithdrawGoldOpen && !DoomFlag && !PadMenuNavigatorActive && !PadHotspellMenuActive && !MyPlayerIsDead
	    && !qtextflag;
}

bool IsActionHudEnabled()
{
	return *GetOptions().Gameplay.actionMode && *GetOptions().Gameplay.actionHud;
}

bool IsActionHudShown()
{
	return IsActionMode() && *GetOptions().Gameplay.actionHud && IsActionModeFree();
}

void ToggleActionMode()
{
	auto &opt = GetOptions().Gameplay.actionMode;
	opt.SetValue(!*opt);
	LockedMonster = -1;
	DodgeActive = false;
	DodgeStepsLeft = 0;
	Stamina = MaxStamina;
	EnsureZoom();
	CalculatePanelAreas();
	CalcViewportGeometry();
	RedrawEverything();
}

void ActionModeUpdate()
{
	EnsureZoom();
	const uint32_t now = SDL_GetTicks();
	const float dt = std::min(0.1f, (now - LastUpdateTicks) / 1000.0f);
	LastUpdateTicks = now;
	if (!IsActionMode())
		return;
	RedrawEverything();

	if (now - LastStaminaSpend > StaminaRegenDelay)
		Stamina = std::min(MaxStamina, Stamina + StaminaRegenPerSecond * dt);

	UpdateDodge();
#ifdef _DEBUG
	if (std::getenv("DEVILUTIONX_DEV_ACTIONLOCK") != nullptr && LockedTarget() == nullptr)
		LockedMonster = PickLockTarget();
#endif
	if (!IsActionModeFree()) {
		AttackHeldByMouse = false;
		return;
	}
	FaceLockTarget();

	// Holding the attack button keeps swinging.
	if (AttackHeldByMouse && (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) == 0)
		AttackHeldByMouse = false;
	const bool held = AttackHeldByMouse || IsControllerButtonPressed(ControllerButton_BUTTON_RIGHTSHOULDER);
	Player &player = *MyPlayer;
	if (held && player._pmode == PM_STAND && player.destAction == ACTION_NONE && !DodgeActive)
		DoAttack(AttackHeldByMouse);
}

void ActionModeAfterCursor()
{
	if (!IsActionModeFree())
		return;
	const Monster *target = LockedTarget();
	if (target == nullptr)
		return;
	pcursmonst = static_cast<int>(target->getId());
	cursPosition = target->position.tile;
}

void ActionModePrepareSpellTarget()
{
	if (!IsActionModeFree())
		return;
	const Monster *target = LockedTarget();
	if (target == nullptr)
		return;
	pcursmonst = static_cast<int>(target->getId());
	cursPosition = target->position.tile;
}

bool ActionModeHandleKey(SDL_Keycode key)
{
	if (!IsActionModeFree())
		return false;
	if (key >= 'a' && key <= 'z')
		key = static_cast<SDL_Keycode>(key - 'a' + 'A');
	switch (key) {
	case 'W':
	case 'A':
	case 'S':
	case 'D':
		return true; // movement is polled every tick
	case SDLK_SPACE:
		StartDodge();
		return true;
	case 'Q':
		ToggleLock();
		return true;
	case 'E':
		DoInteract();
		return true;
	case 'R':
		UseBeltItem(BeltItemType::Healing);
		return true;
	case 'F':
		UseBeltItem(BeltItemType::Mana);
		return true;
	default:
		return false;
	}
}

bool ActionModeHandleLeftMouse(bool isShiftHeld)
{
	(void)isShiftHeld;
	if (!IsActionModeFree() || pcurs != CURSOR_HAND || !MyPlayer->HoldItem.isEmpty())
		return false;
	const bool overHudArea = GetMainPanel().contains(MousePosition);
	if (overHudArea && !IsActionHudShown())
		return false; // the real panel is visible there
	if (leveltype == DTYPE_TOWN)
		return false; // talking, shopping and picking up work as usual
	// Clicking an item or a non-breakable object still interacts with it.
	if (!overHudArea && LockedTarget() == nullptr) {
		if (pcursitem != -1)
			return false;
		if (ObjectUnderCursor != nullptr && ObjectUnderCursor->_oBreak != 1)
			return false;
	}
	AttackHeldByMouse = true;
	DoAttack(true);
	return true;
}

bool ActionModeHandleButton(ControllerButton button)
{
	if (!IsActionMode())
		return false;
	if (button == ControllerButton_BUTTON_BACK && (invflag || IsActionModeFree())) {
		ProcessGameAction(GameAction { GameActionType_TOGGLE_INVENTORY });
		return true;
	}
	if (!IsActionModeFree())
		return false;
	switch (button) {
	case ControllerButton_BUTTON_RIGHTSHOULDER:
		DoAttack(false);
		return true;
	case ControllerButton_AXIS_TRIGGERRIGHT:
		DoSpell();
		return true;
	case ControllerButton_BUTTON_B:
		StartDodge();
		return true;
	case ControllerButton_BUTTON_A:
		DoInteract();
		return true;
	case ControllerButton_BUTTON_X:
		UseBeltItem(BeltItemType::Healing);
		return true;
	case ControllerButton_BUTTON_Y:
		UseBeltItem(BeltItemType::Mana);
		return true;
	case ControllerButton_BUTTON_RIGHTSTICK:
		ToggleLock();
		return true;
	case ControllerButton_BUTTON_DPAD_LEFT:
		ActionCycleSpell(false);
		return true;
	case ControllerButton_BUTTON_DPAD_RIGHT:
		ActionCycleSpell(true);
		return true;
	case ControllerButton_BUTTON_DPAD_UP:
		ActionDisplaySpells();
		return true;
	case ControllerButton_BUTTON_DPAD_DOWN:
		DoAutoMap();
		return true;
	case ControllerButton_BUTTON_LEFTSHOULDER:
		ProcessGameAction(GameAction { GameActionType_TOGGLE_CHARACTER_INFO });
		return true;
	case ControllerButton_AXIS_TRIGGERLEFT:
		ProcessGameAction(GameAction { GameActionType_TOGGLE_SPELL_BOOK });
		return true;
	default:
		return false;
	}
}

bool ActionModeHandleRightStick(float x, float y)
{
	if (!IsActionModeFree())
		return false;
	const float mag = std::sqrt(x * x + y * y);
	if (mag < 0.35f)
		RightStickArmed = true;
	else if (mag > 0.75f && RightStickArmed) {
		RightStickArmed = false;
		if (LockedTarget() != nullptr)
			SwitchLockTarget(x, y);
	}
	return true; // no free cursor while playing; it returns as soon as a panel opens
}

bool ActionModeMovement(Player &player)
{
	if (!IsActionMode())
		return false;
	if (!IsActionModeFree())
		return true;
	if (DodgeActive)
		return true;
	const AxisDirection input = ReadMoveInput();
	if (!HasMoveInput(input)) {
		if (MovedByInputLastTick && player.walkpath[0] != WALK_NONE && player.destAction == ACTION_NONE)
			NetSendCmdLoc(player.getId(), true, CMD_WALKXY, player.position.future); // stop
		MovedByInputLastTick = false;
		return true;
	}
	MovedByInputLastTick = true;
	StepTowards(player, ToDirection(input));
	return true;
}

bool IsPlayerDodging(const Player &player)
{
	return DodgeActive && &player == MyPlayer && !gbIsMultiplayer;
}

int ActionModeWalkSkipFrames(const Player &player)
{
	if (DodgeActive && &player == MyPlayer)
		return 6;
	return 0;
}

// ---------------------------------------------------------------------------------------------
// HUD

namespace {

uint8_t NearestIndex(int r, int g, int b, int lo = 128)
{
	int best = lo;
	int bestDist = 1 << 30;
	for (int i = lo; i < 256; i++) {
		const SDL_Color &c = logical_palette[i];
		const int dr = c.r - r, dg = c.g - g, db = c.b - b;
		const int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
		if (d < bestDist) {
			bestDist = d;
			best = i;
		}
	}
	return static_cast<uint8_t>(best);
}

struct HudColors {
	uint8_t gold, goldDark, goldLight, black, back;
	uint8_t red, redDark, redLight;
	uint8_t blue, blueDark, blueLight;
	uint8_t green, greenDark, greenLight;
	uint8_t white;
};

HudColors BuildHudColors()
{
	HudColors c;
	c.gold = NearestIndex(196, 150, 60);
	c.goldDark = NearestIndex(104, 76, 30);
	c.goldLight = NearestIndex(244, 214, 128);
	c.black = NearestIndex(6, 4, 4);
	c.back = NearestIndex(28, 18, 16);
	c.red = NearestIndex(176, 24, 24);
	c.redDark = NearestIndex(84, 10, 10);
	c.redLight = NearestIndex(232, 84, 72);
	c.blue = NearestIndex(40, 70, 190);
	c.blueDark = NearestIndex(14, 24, 84);
	c.blueLight = NearestIndex(120, 150, 240);
	// The palette has no real green; stamina uses a bright straw yellow instead.
	c.green = NearestIndex(226, 196, 70);
	c.greenDark = NearestIndex(120, 96, 24);
	c.greenLight = NearestIndex(250, 240, 170);
	c.white = NearestIndex(236, 230, 220);
	return c;
}

void DrawBar(const Surface &out, const HudColors &c, Point pos, int width, int height, float frac, uint8_t fill, uint8_t fillDark, uint8_t fillLight)
{
	frac = std::clamp(frac, 0.0f, 1.0f);
	FillRect(out, pos.x - 2, pos.y - 2, width + 4, height + 4, c.goldDark);
	FillRect(out, pos.x - 1, pos.y - 1, width + 2, height + 2, c.black);
	FillRect(out, pos.x, pos.y, width, height, c.back);
	const int w = static_cast<int>(width * frac);
	if (w > 0) {
		FillRect(out, pos.x, pos.y, w, height, fillDark);
		FillRect(out, pos.x, pos.y, w, std::max(1, height * 2 / 3), fill);
		FillRect(out, pos.x, pos.y, w, 1, fillLight);
	}
	DrawHorizontalLine(out, { pos.x - 2, pos.y - 2 }, width + 4, c.gold);
	// End caps.
	FillRect(out, pos.x + width + 2, pos.y - 3, 2, height + 6, c.gold);
	FillRect(out, pos.x - 4, pos.y - 3, 2, height + 6, c.gold);
}

/** Screen position of a monster's feet (centre of its tile), accounting for zoom. */
std::optional<Point> MonsterScreenFeet(const Monster &monster)
{
	Point p = GetScreenPosition(monster.position.tile);
	if (monster.isWalking())
		p += GetOffsetForWalking(monster.animInfo, monster.direction);
	p.x += TILE_WIDTH / 2 - 8;
	p.y -= TILE_HEIGHT / 4;
	if (*GetOptions().Graphics.zoom) {
		p.x *= 2;
		p.y *= 2;
	}
	if (p.x < 0 || p.y < 0 || p.x >= gnScreenWidth || p.y >= gnScreenHeight)
		return std::nullopt;
	return p;
}

void DrawLockMarker(const Surface &out, const HudColors &c)
{
#ifdef _DEBUG
	if (std::getenv("DEVILUTIONX_DEV_MARKPLAYER") != nullptr) {
		Point p = GetScreenPosition(MyPlayer->position.tile);
		p.x += TILE_WIDTH / 2;
		p.y -= TILE_HEIGHT / 4;
		if (*GetOptions().Graphics.zoom) {
			p.x *= 2;
			p.y *= 2;
		}
		const int h = MyPlayer->AnimInfo.currentSprite().height() * (*GetOptions().Graphics.zoom ? 2 : 1);
		FillRect(out, p.x - 3, p.y - h * 2 / 5 - 3, 7, 7, c.white);
		FillRect(out, p.x - 20, p.y, 41, 1, c.red);
	}
#endif
	const Monster *target = LockedTarget();
	if (target == nullptr || !target->animInfo.sprites)
		return;
	const std::optional<Point> feet = MonsterScreenFeet(*target);
	if (!feet)
		return;
	const int scale = *GetOptions().Graphics.zoom ? 2 : 1;
	const int spriteH = target->animInfo.currentSprite().height() * scale;
	const Point chest { feet->x, feet->y - spriteH * 2 / 5 };
	// Souls-style lock dot: white core, dark ring.
	FillRect(out, chest.x - 3, chest.y - 2, 7, 5, c.black);
	FillRect(out, chest.x - 2, chest.y - 3, 5, 7, c.black);
	FillRect(out, chest.x - 2, chest.y - 1, 5, 3, c.white);
	FillRect(out, chest.x - 1, chest.y - 2, 3, 5, c.white);
	if (!target->isUnique()) {
		const int barW = 44;
		const float frac = target->maxHitPoints > 0 ? static_cast<float>(target->hitPoints.raw()) / static_cast<float>(target->maxHitPoints.raw()) : 0.0f;
		const Point barPos { feet->x - barW / 2, std::max(4, feet->y - spriteH * 7 / 8 - 6) };
		FillRect(out, barPos.x - 1, barPos.y - 1, barW + 2, 5, c.black);
		FillRect(out, barPos.x, barPos.y, static_cast<int>(barW * frac), 3, c.red);
	}
}

void DrawBossBar(const Surface &out, const HudColors &c)
{
	const Monster *target = LockedTarget();
	if (target == nullptr || !target->isUnique())
		return;
	const int width = std::min(out.w() - 80, 420);
	const int x = (out.w() - width) / 2;
	const int y = out.h() - 46;
	const float frac = target->maxHitPoints > 0 ? static_cast<float>(target->hitPoints.raw()) / static_cast<float>(target->maxHitPoints.raw()) : 0.0f;
	DrawString(out, target->name(), Rectangle { Point { x, y - 18 }, Size { width, 14 } }, { .flags = UiFlags::ColorGold | UiFlags::AlignCenter | UiFlags::VerticalCenter });
	DrawBar(out, c, { x, y }, width, 6, frac, c.red, c.redDark, c.redLight);
}

void DrawStatusBars(const Surface &out, const HudColors &c)
{
	const Player &p = *MyPlayer;
	const int maxHp = std::max(1, p._pMaxHP.whole());
	const int hp = std::clamp(p._pHitPoints.whole(), 0, maxHp);
	const int maxMana = std::max(1, p._pMaxMana.whole());
	const int mana = std::clamp(p._pMana.whole(), 0, maxMana);
	const bool noMana = p.hasNoMana() || HasAnyOf(p._pIFlags, ItemSpecialEffect::NoMana);
	// Bar length grows with the maximum, like the Souls games.
	const auto lengthFor = [&](int maxValue) { return std::clamp(70 + maxValue, 90, std::min(320, out.w() / 2)); };
	const int x = 18;
	DrawBar(out, c, { x, 14 }, lengthFor(maxHp), 7, static_cast<float>(hp) / maxHp, c.red, c.redDark, c.redLight);
	if (*GetOptions().Gameplay.showHealthValues)
		DrawString(out, StrCat(hp, "/", maxHp), Rectangle { Point { x + lengthFor(maxHp) + 8, 9 }, Size { 80, 16 } }, { .flags = UiFlags::ColorWhitegold | UiFlags::VerticalCenter });
	int y = 27;
	if (!noMana) {
		DrawBar(out, c, { x, y }, lengthFor(maxMana), 5, static_cast<float>(mana) / maxMana, c.blue, c.blueDark, c.blueLight);
		if (*GetOptions().Gameplay.showManaValues)
			DrawString(out, StrCat(mana, "/", maxMana), Rectangle { Point { x + lengthFor(maxMana) + 8, y - 5 }, Size { 80, 16 } }, { .flags = UiFlags::ColorWhitegold | UiFlags::VerticalCenter });
		y += 11;
	}
	DrawBar(out, c, { x, y }, 140, 4, Stamina / MaxStamina, c.green, c.greenDark, c.greenLight);

	// Current spell, bottom left.
	const SpellID spell = p._pRSpell;
	if (spell != SpellID::Invalid) {
		DrawSmallSpellIcon(out, { 18, out.h() - 14 }, spell);
		DrawString(out, GetSpellData(spell).sNameText, Rectangle { Point { 62, out.h() - 36 }, Size { 160, 16 } }, { .flags = UiFlags::ColorWhitegold | UiFlags::VerticalCenter });
	}

	// Belt, bottom right; experience above it like a soul counter.
	const int slotX = out.w() - 18 - 8 * 20;
	for (int i = 0; i < 8; i++) {
		const Item &it = p.SpdList[i];
		const int sx = slotX + i * 20;
		FillRect(out, sx, out.h() - 28, 17, 17, c.goldDark);
		FillRect(out, sx + 1, out.h() - 27, 15, 15, c.black);
		if (!it.isEmpty()) {
			uint8_t col = c.red;
			if (it._itype == ItemType::Gold)
				col = c.gold;
			else if (IsAnyOf(it._iMiscId, IMISC_MANA, IMISC_FULLMANA))
				col = c.blue;
			else if (IsAnyOf(it._iMiscId, IMISC_REJUV, IMISC_FULLREJUV))
				col = c.redLight;
			else if (it._itype == ItemType::Misc && !IsAnyOf(it._iMiscId, IMISC_HEAL, IMISC_FULLHEAL))
				col = c.goldLight;
			FillRect(out, sx + 4, out.h() - 24, 9, 9, col);
		}
		DrawString(out, StrCat(i + 1), Rectangle { Point { sx, out.h() - 42 }, Size { 17, 12 } }, { .flags = UiFlags::ColorGold | UiFlags::AlignCenter | UiFlags::VerticalCenter });
	}
	const uint32_t next = p.getNextExperienceThreshold();
	DrawString(out, StrCat(p._pExperience, " / ", next), Rectangle { Point { out.w() - 220, out.h() - 60 }, Size { 202, 14 } }, { .flags = UiFlags::ColorWhitegold | UiFlags::AlignRight | UiFlags::VerticalCenter });
}

void DrawInteractPrompt(const Surface &out)
{
	std::string text;
	UiFlags color = UiFlags::ColorWhite;
	if (pcursitem != -1) {
		const Item &item = Items[pcursitem];
		text = std::string(item.getName());
		color = item.getTextColor();
	} else if (ObjectUnderCursor != nullptr) {
		text = std::string(ObjectUnderCursor->name().str());
	} else if (leveltype == DTYPE_TOWN && pcursmonst >= 0 && static_cast<size_t>(pcursmonst) < Towners.size()) {
		text = std::string(Towners[pcursmonst].name);
	}
	if (text.empty())
		return;
	const Rectangle rect { Point { out.w() / 2 - 160, out.h() - 84 }, Size { 320, 16 } };
	DrawString(out, text, rect, { .flags = color | UiFlags::AlignCenter | UiFlags::VerticalCenter });
}

} // namespace

void DrawActionHud(const Surface &out)
{
	const HudColors c = BuildHudColors();
	DrawStatusBars(out, c);
	DrawBossBar(out, c);
	DrawInteractPrompt(out);
}

void DrawActionWorldOverlay(const Surface &out)
{
	if (!IsActionMode() || AutomapActive)
		return;
	const HudColors c = BuildHudColors();
	DrawLockMarker(out, c);
}

} // namespace devilution
