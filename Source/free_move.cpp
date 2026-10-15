/**
 * @file free_move.cpp
 *
 * Free movement (see free_move.hpp). Positions are in world tile units with tile centres at
 * integer coordinates; screen x = (x - y) * 32, screen y = (x + y) * 16.
 */
#include "free_move.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

#include <function_ref.hpp>

#include "diablo_msg.hpp"
#include "effects.h"
#include "engine/actor_position.hpp"
#include "engine/path.h"
#include "engine/render/scrollrt.h"
#include "game_mode.hpp"
#include "headless_mode.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "lighting.h"
#include "monster.h"
#include "nthread.h"
#include "options.h"
#include "player.h"
#include "qol/autopickup.h"
#include "utils/language.h"
#include "utils/log.hpp"

namespace devilution {

namespace {

/** How far past the tile edge the body may go before the tile switches (avoids flapping). */
constexpr float SwitchHysteresis = 0.06F;
/** Half size of the body's footprint kept clear of walls. */
constexpr float BodyRadius = 0.22F;
/** Closest two living actors may get (centre to centre) by moving. */
constexpr float MinSeparation = 0.66F;
/** Length of one collision sub-step. */
constexpr float SubStep = 0.1F;

uint32_t Generation = 1;
#ifdef _DEBUG
int DbgStrides = 0;
int DbgChase = 0;
int DbgStrictAfterBlock = 0;
int DbgBlocked = 0;
int DbgBlockedCrowd = 0;
int DbgBlockedTile = 0;
int DbgBlockedWall = 0;
#endif
uint32_t LogicTick = 0;
float CursorX = 0;
float CursorY = 0;

enum class WalkMode : uint8_t {
	None,
	Path,
	Heading,
};

struct PlayerFreeState {
	WalkMode mode = WalkMode::None;
	float headX = 0;
	float headY = 0;
	uint32_t headTick = 0;
	/** Remaining dash distance; negative when not dashing. */
	float dashLeft = -1;
	float dashSpeed = 1;
	bool hasPrecise = false;
	float preciseX = 0;
	float preciseY = 0;
	Point goalTile;
	bool chain = false;
	int8_t chainFrame = 0;
	int8_t chainTick = 0;
	int stuck = 0;
	int strictTicks = 0;
	uint32_t movedTick = 0;
	/** Exact point under the mouse when the last walk request was sent. */
	bool hasPending = false;
	Point pendingTile;
	float pendingX = 0;
	float pendingY = 0;
};

std::array<PlayerFreeState, MAX_PLRS> PlayerStates;

PlayerFreeState &State(const Player &player)
{
	return PlayerStates[player.getId() % MAX_PLRS];
}

Point RoundPoint(float x, float y)
{
	return { static_cast<int>(std::floor(x + 0.5F)), static_cast<int>(std::floor(y + 0.5F)) };
}

int Chebyshev(Point a, Point b)
{
	return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
}

bool IsValid(const ActorPosition &pos)
{
	return pos.freeMove.generation == Generation && pos.freeMove.anchor == pos.tile;
}

/** Makes the continuous position valid for the actor's tile (snapping to its centre if not). */
void Sync(ActorPosition &pos)
{
	FreeMovePosition &f = pos.freeMove;
	if (IsValid(pos))
		return;
	f.x = f.prevX = static_cast<float>(pos.tile.x);
	f.y = f.prevY = static_cast<float>(pos.tile.y);
	f.anchor = pos.tile;
	f.generation = Generation;
	f.hasClaim = false;
	f.blocked = false;
}

/** Current (end of last tick) continuous position, without interpolation. */
void CurrentPosition(const ActorPosition &pos, float &x, float &y)
{
	if (IsValid(pos)) {
		x = pos.freeMove.x;
		y = pos.freeMove.y;
	} else {
		x = static_cast<float>(pos.tile.x);
		y = static_cast<float>(pos.tile.y);
	}
}

/** Depth the body's footprint at (x, y) reaches into walls and solid objects (0 = clear). */
float WallPenetration(float x, float y)
{
	const Point c = RoundPoint(x, y);
	float pen = 0;
	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			const Point t { c.x + dx, c.y + dy };
			if (InDungeonBounds(t) && IsTileWalkable(t))
				continue;
			const float ox = 0.5F + BodyRadius - std::abs(x - static_cast<float>(t.x));
			const float oy = 0.5F + BodyRadius - std::abs(y - static_cast<float>(t.y));
			if (ox > 0 && oy > 0)
				pen = std::max(pen, std::min(ox, oy));
		}
	}
	return pen;
}

/** Another living actor would get too close by moving from (ox, oy) to (nx, ny). */
bool CrowdsSomeone(const void *self, float ox, float oy, float nx, float ny)
{
	const Point c = RoundPoint(nx, ny);
	const auto tooClose = [&](const ActorPosition &other) {
		float qx;
		float qy;
		CurrentPosition(other, qx, qy);
		const float dn = (nx - qx) * (nx - qx) + (ny - qy) * (ny - qy);
		if (dn >= MinSeparation * MinSeparation)
			return false;
		const float dold = (ox - qx) * (ox - qx) + (oy - qy) * (oy - qy);
		return dn < dold - 1e-5F;
	};
	for (int dy = -2; dy <= 2; dy++) {
		for (int dx = -2; dx <= 2; dx++) {
			const Point t { c.x + dx, c.y + dy };
			if (!InDungeonBounds(t))
				continue;
			const int pv = std::abs(static_cast<int>(dPlayer[t.x][t.y]));
			if (pv != 0 && static_cast<size_t>(pv - 1) < Players.size()) {
				const Player &other = Players[pv - 1];
				if (&other != self && !other.hasNoLife() && tooClose(other.position))
					return true;
			}
			if (leveltype == DTYPE_TOWN)
				continue; // towners live in dMonster in town and never move
			const int mv = std::abs(static_cast<int>(dMonster[t.x][t.y]));
			if (mv != 0 && static_cast<size_t>(mv - 1) < MaxMonsters) {
				const Monster &other = Monsters[mv - 1];
				if (&other != self && !other.hasNoLife() && (other.flags & MFLAG_HIDDEN) == 0 && tooClose(other.position))
					return true;
			}
		}
	}
	return false;
}

struct Mover {
	ActorPosition &pos;
	const void *self;
	/** May the actor go from its tile into the neighbouring tile? */
	tl::function_ref<bool(Point, Point)> canEnter;
	/** Performs the switch of the actor's tile (occupancy, lights...). */
	tl::function_ref<void(Point)> onSwitch;
	/** Keep the body clear of walls (off for plain tile-centre steps). */
	bool useWalls;
	/** Keep clear of other living actors. */
	bool useSeparation = true;
	bool dryRun = false;
};

bool TryStep(Mover &m, float &x, float &y, Point &tile, float sx, float sy)
{
	const float nx = x + sx;
	const float ny = y + sy;
	if (m.useWalls) {
		const float before = WallPenetration(x, y);
		if (WallPenetration(nx, ny) > before + 1e-4F) {
#ifdef _DEBUG
			if (!m.dryRun)
				DbgBlockedWall++;
#endif
			return false;
		}
	}
	Point next = tile;
	if (std::abs(nx - static_cast<float>(tile.x)) > 0.5F + SwitchHysteresis || std::abs(ny - static_cast<float>(tile.y)) > 0.5F + SwitchHysteresis) {
		next = RoundPoint(nx, ny);
		if (next != tile) {
			if (Chebyshev(next, tile) > 1 || !InDungeonBounds(next))
				return false;
			if (!m.canEnter(tile, next)) {
#ifdef _DEBUG
				if (!m.dryRun)
					DbgBlockedTile++;
#endif
				return false;
			}
		}
	}
	if (m.useSeparation && CrowdsSomeone(m.self, x, y, nx, ny)) {
#ifdef _DEBUG
		if (!m.dryRun)
			DbgBlockedCrowd++;
#endif
		return false;
	}
	x = nx;
	y = ny;
	if (next != tile) {
		tile = next;
		if (!m.dryRun)
			m.onSwitch(next);
	}
	return true;
}

/** Moves by (dx, dy), sliding along walls; returns the distance actually covered. */
float Move(Mover &m, float dx, float dy)
{
	const float total = std::hypot(dx, dy);
	if (total < 1e-6F)
		return 0;
	const int steps = std::max(1, static_cast<int>(std::ceil(total / SubStep)));
	const float sx = dx / steps;
	const float sy = dy / steps;
	float x = m.pos.freeMove.x;
	float y = m.pos.freeMove.y;
	Point tile = m.pos.tile;
	float moved = 0;
	for (int i = 0; i < steps; i++) {
		if (TryStep(m, x, y, tile, sx, sy)) {
			moved += std::hypot(sx, sy);
			continue;
		}
		// Slide: keep the component along the obstacle.
		const bool xFirst = std::abs(sx) >= std::abs(sy);
		const float ax = xFirst ? sx : 0;
		const float ay = xFirst ? 0 : sy;
		const float bx = xFirst ? 0 : sx;
		const float by = xFirst ? sy : 0;
		if (std::abs(ax) + std::abs(ay) > 1e-6F && TryStep(m, x, y, tile, ax, ay)) {
			moved += std::abs(ax) + std::abs(ay);
		} else if (std::abs(bx) + std::abs(by) > 1e-6F && TryStep(m, x, y, tile, bx, by)) {
			moved += std::abs(bx) + std::abs(by);
		} else {
			break;
		}
	}
	if (!m.dryRun) {
		m.pos.freeMove.x = x;
		m.pos.freeMove.y = y;
		m.pos.freeMove.anchor = m.pos.tile;
	}
	return moved;
}

// --- Facing ----------------------------------------------------------------------------------

/** Screen angle (radians, y down) of each sprite direction, in Direction order. */
const std::array<float, 8> &DirectionScreenAngles()
{
	static const std::array<float, 8> Angles = [] {
		std::array<float, 8> a {};
		for (int i = 0; i < 8; i++) {
			const Displacement d { static_cast<Direction>(i) };
			const float sx = static_cast<float>(d.deltaX - d.deltaY) * 32.0F;
			const float sy = static_cast<float>(d.deltaX + d.deltaY) * 16.0F;
			a[i] = std::atan2(sy, sx);
		}
		return a;
	}();
	return Angles;
}

float AngleDistance(float a, float b)
{
	float d = std::fmod(std::abs(a - b), 6.2831853F);
	return d > 3.14159265F ? 6.2831853F - d : d;
}

Direction Facing(float wx, float wy, Direction current, bool hysteresis)
{
	const float sx = (wx - wy) * 32.0F;
	const float sy = (wx + wy) * 16.0F;
	if (sx * sx + sy * sy < 1e-8F)
		return current;
	const float angle = std::atan2(sy, sx);
	const auto &angles = DirectionScreenAngles();
	int best = 0;
	float bestDist = 10;
	for (int i = 0; i < 8; i++) {
		const float d = AngleDistance(angle, angles[i]);
		if (d < bestDist) {
			bestDist = d;
			best = i;
		}
	}
	if (hysteresis && current != Direction::NoDirection) {
		const float currentDist = AngleDistance(angle, angles[static_cast<int>(current)]);
		if (currentDist - bestDist < 0.14F)
			return current;
	}
	return static_cast<Direction>(best);
}

// --- Hero ------------------------------------------------------------------------------------

constexpr std::array<Direction, 9> WalkCodeDirection = {
	Direction::NoDirection,
	Direction::NorthEast, // WALK_NE
	Direction::NorthWest, // WALK_NW
	Direction::SouthEast, // WALK_SE
	Direction::SouthWest, // WALK_SW
	Direction::North,     // WALK_N
	Direction::East,      // WALK_E
	Direction::South,     // WALK_S
	Direction::West,      // WALK_W
};

int BuildWaypoints(const Player &player, Point *wp)
{
	Point p = player.position.tile;
	int n = 0;
	for (size_t i = 0; i < MaxPathLengthPlayer; i++) {
		const int code = player.walkpath[i];
		if (code < 1 || code > 8)
			break;
		p = p + WalkCodeDirection[code];
		wp[n++] = p;
	}
	return n;
}

float TilesPerTick(const Player &player)
{
	int8_t frames = 8;
	int8_t ticksPerFrame = 1;
	player.getAnimationFramesAndTicksPerFrame(player_graphic::Walk, frames, ticksPerFrame);
	const int skipped = (leveltype == DTYPE_TOWN && sgGameInitInfo.bRunInTown != 0) ? 3 : -1;
	const int ticks = std::max(2, (frames - 1 - skipped) * std::max<int>(1, ticksPerFrame));
	return 1.0F / static_cast<float>(ticks);
}

bool PlayerCanEnter(const Player &player, Point from, Point to)
{
	if (!PosOkPlayer(player, to))
		return false;
	if (from.x != to.x && from.y != to.y && !CanStep(from, to))
		return false;
	return true;
}

void PlayerSwitchTile(Player &player, Point next)
{
	const Point old = player.position.tile;
	const int id = player.getId() + 1;
	if (std::abs(static_cast<int>(dPlayer[old.x][old.y])) == id)
		dPlayer[old.x][old.y] = 0;
	MarkWalkabilityChanged();
	player.position.tile = next;
	player.position.future = next;
	player.position.temp = next;
	player.position.old = next;
	player.position.freeMove.anchor = next;
	player.occupyTile(next, false);
	if (&player == MyPlayer)
		ViewPosition = next;
	if (leveltype != DTYPE_TOWN) {
		ChangeLightXY(player.lightId, next);
		ChangeVisionXY(player.getId(), next);
	}
}

float MovePlayer(Player &player, float dx, float dy, bool useWalls, bool &switched, bool dryRun = false)
{
	switched = false;
	const auto canEnter = [&player](Point from, Point to) { return PlayerCanEnter(player, from, to); };
	const auto onSwitch = [&player, &switched](Point next) {
		PlayerSwitchTile(player, next);
		switched = true;
	};
	Mover m { player.position, &player, canEnter, onSwitch, useWalls, true, dryRun };
	return Move(m, dx, dy);
}

bool SegmentClear(const Player &player, float x0, float y0, float x1, float y1)
{
	const float len = std::hypot(x1 - x0, y1 - y0);
	const int steps = std::max(1, static_cast<int>(std::ceil(len / SubStep)));
	Point prev = player.position.tile;
	for (int i = 1; i <= steps; i++) {
		const float t = static_cast<float>(i) / static_cast<float>(steps);
		const float x = x0 + (x1 - x0) * t;
		const float y = y0 + (y1 - y0) * t;
		if (WallPenetration(x, y) > 0)
			return false;
		const Point tile = RoundPoint(x, y);
		if (tile != prev) {
			if (Chebyshev(tile, prev) > 1 || !InDungeonBounds(tile) || !PlayerCanEnter(player, prev, tile))
				return false;
			prev = tile;
		}
	}
	return true;
}

void FinalPoint(const PlayerFreeState &st, Point tile, float &x, float &y)
{
	if (st.hasPrecise && RoundPoint(st.preciseX, st.preciseY) == tile) {
		x = st.preciseX;
		y = st.preciseY;
	} else {
		x = static_cast<float>(tile.x);
		y = static_cast<float>(tile.y);
	}
}

/** Picks where to head: the farthest waypoint in straight, clear sight. */
void PathSteer(const Player &player, PlayerFreeState &st, const Point *wp, int n, float &tx, float &ty, bool &useBody)
{
	const FreeMovePosition &f = player.position.freeMove;
	useBody = true;
	if (n == 0) {
		FinalPoint(st, player.position.tile, tx, ty);
		return;
	}
	if (st.strictTicks > 0) {
		tx = static_cast<float>(wp[0].x);
		ty = static_cast<float>(wp[0].y);
		useBody = false;
		return;
	}
	const int limit = std::min(n, 12);
	for (int k = limit - 1; k >= 0; k--) {
		float x;
		float y;
		if (k == n - 1) {
			FinalPoint(st, wp[k], x, y);
		} else {
			x = static_cast<float>(wp[k].x);
			y = static_cast<float>(wp[k].y);
		}
		if (SegmentClear(player, f.x, f.y, x, y)) {
			tx = x;
			ty = y;
			return;
		}
	}
	tx = static_cast<float>(wp[0].x);
	ty = static_cast<float>(wp[0].y);
	useBody = false;
}

/** After the tile changed mid-walk: make walkpath start from the new tile again. */
void ResyncWalkpath(Player &player, const PlayerFreeState &st, const Point *wp, int n)
{
	const Point tile = player.position.tile;
	for (int j = 0; j < n; j++) {
		if (wp[j] != tile)
			continue;
		const int shift = j + 1;
		for (size_t i = 0; i < MaxPathLengthPlayer; i++) {
			const size_t from = i + static_cast<size_t>(shift);
			player.walkpath[i] = from < MaxPathLengthPlayer ? player.walkpath[from] : WALK_NONE;
		}
		return;
	}
	const Point goal = st.goalTile;
	if (goal == tile) {
		ClrPlrPath(player);
		return;
	}
	int8_t path[MaxPathLengthPlayer];
	const int len = FindPath(
	    CanStep, [&player](Point p) { return PosOkPlayer(player, p); }, tile, goal, path, MaxPathLengthPlayer);
	ClrPlrPath(player);
	if (len <= 0)
		return;
	for (int i = 0; i < len && i < static_cast<int>(MaxPathLengthPlayer); i++)
		player.walkpath[i] = path[i];
}

void SetWalkFacing(Player &player, Direction dir)
{
	if (player._pdir == dir)
		return;
	player._pdir = dir;
	player.tempDirection = dir;
	if (!HeadlessMode && player.AnimInfo.sprites)
		player.AnimInfo.sprites = player.AnimationData[static_cast<size_t>(player_graphic::Walk)].spritesForDirection(dir);
}

void UpdatePlayerLight(Player &player)
{
	if (leveltype == DTYPE_TOWN || player.lightId == NO_LIGHT)
		return;
	ChangeLightOffset(player.lightId, GetPlayerLightOffset(player));
}

void PlayWalkSound(const Player &player)
{
	if (!*GetOptions().Audio.walkingSound || (leveltype == DTYPE_TOWN && sgGameInitInfo.bRunInTown != 0))
		return;
	if (player.AnimInfo.tickCounterOfCurrentFrame == 0 && (player.AnimInfo.currentFrame == 0 || player.AnimInfo.currentFrame == 4))
		PlaySfxLoc(SfxID::Walk, player.position.tile);
}

void BeginWalk(Player &player, Direction dir)
{
	PlayerFreeState &st = State(player);
	Sync(player.position);
	NewPlrAnim(player, player_graphic::Walk, dir);
	if (st.chain && st.chainFrame >= 0 && st.chainFrame < player.AnimInfo.numberOfFrames) {
		player.AnimInfo.currentFrame = st.chainFrame;
		player.AnimInfo.tickCounterOfCurrentFrame = st.chainTick;
	}
	st.chain = false;
	player._pmode = PM_WALK_NORTHWARDS;
	player._pdir = dir;
	player.tempDirection = dir;
	player.position.future = player.position.tile;
	player.position.temp = player.position.tile;
	SetPlayerOld(player);
	player.position.freeMove.walking = true;
	st.stuck = 0;
}

/** Ends the walk where the hero is (keeps the exact position). */
void StopWalk(Player &player)
{
	PlayerFreeState &st = State(player);
	st.mode = WalkMode::None;
	st.dashLeft = -1;
	st.strictTicks = 0;
	player.position.freeMove.walking = false;
	StartStand(player, player._pdir);
	UpdatePlayerLight(player);
}

/** Stands for one moment at a tile change so pending actions (attack, pick up...) get their chance. */
void ChainStand(Player &player)
{
	PlayerFreeState &st = State(player);
	st.chain = true;
	st.chainFrame = player.AnimInfo.currentFrame;
	st.chainTick = player.AnimInfo.tickCounterOfCurrentFrame;
	player.position.freeMove.walking = false;
	StartStand(player, player._pdir);
}

bool CanHeadThere(Player &player, float wx, float wy)
{
	bool switched = false;
	const float step = TilesPerTick(player);
	return MovePlayer(player, wx * step, wy * step, true, switched, true) > step * 0.2F;
}

// --- Monsters --------------------------------------------------------------------------------

void ReleaseClaim(Monster &monster)
{
	FreeMovePosition &f = monster.position.freeMove;
	if (!f.hasClaim)
		return;
	const Point c = f.claim;
	if (InDungeonBounds(c) && dMonster[c.x][c.y] == -static_cast<int>(monster.getId() + 1)) {
		dMonster[c.x][c.y] = 0;
		MarkWalkabilityChanged();
	}
	f.hasClaim = false;
}

void MonsterSwitchTile(Monster &monster, Point next)
{
	ActorPosition &pos = monster.position;
	FreeMovePosition &f = pos.freeMove;
	const Point old = pos.tile;
	const int id = static_cast<int>(monster.getId()) + 1;
	if (dMonster[old.x][old.y] == id)
		dMonster[old.x][old.y] = 0;
	pos.tile = next;
	pos.old = next;
	f.anchor = next;
	monster.occupyTile(next, false);
	if (f.hasClaim && f.claim == next)
		f.hasClaim = false;
	if (f.hasClaim)
		pos.future = f.claim;
	else
		pos.future = next;
	ChangeLightXY(monster.lightId, next);
}

void EnemyPosition(const Monster &monster, float &x, float &y)
{
	if ((monster.flags & MFLAG_TARGETS_MONSTER) != 0) {
		CurrentPosition(Monsters[monster.enemy].position, x, y);
	} else {
		CurrentPosition(Players[monster.enemy].position, x, y);
	}
}

} // namespace

// --- Mode ------------------------------------------------------------------------------------

bool IsFreeMovement()
{
	return *GetOptions().Gameplay.freeMovement && !gbIsMultiplayer;
}

void ToggleFreeMovement()
{
	if (gbIsMultiplayer) {
		InitDiabloMsg(_("Free movement is single player only"));
		return;
	}
	auto &opt = GetOptions().Gameplay.freeMovement;
	opt.SetValue(!*opt);
	if (!*opt) {
		for (Player &player : Players) {
			if (player.plractive && IsPlayerFreeWalking(player))
				StopWalk(player);
		}
		for (size_t i = 0; i < ActiveMonsterCount; i++) {
			Monster &monster = Monsters[ActiveMonsters[i]];
			if (!IsMonsterFreeWalking(monster))
				continue;
			ReleaseClaim(monster);
			monster.position.freeMove.walking = false;
			M_StartStand(monster, monster.direction);
		}
		for (Player &player : Players) {
			if (player.plractive && player.lightId != NO_LIGHT && leveltype != DTYPE_TOWN)
				ChangeLightOffset(player.lightId, { 0, 0 });
		}
		Generation++;
	}
	PlayerStates = {};
	InitDiabloMsg(*opt ? _("Free movement: on") : _("Free movement: off (tile grid)"));
}

void FreeMoveBeginTick()
{
	LogicTick++;
	for (Player &player : Players) {
		FreeMovePosition &f = player.position.freeMove;
		f.prevX = f.x;
		f.prevY = f.y;
	}
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		FreeMovePosition &f = Monsters[ActiveMonsters[i]].position.freeMove;
		f.prevX = f.x;
		f.prevY = f.y;
	}
	for (PlayerFreeState &st : PlayerStates)
		st.chain = false;
}

void FreeMoveResetLevel()
{
	Generation++;
	PlayerStates = {};
}

// --- Rendering -------------------------------------------------------------------------------

void GetActorWorldPosition(const ActorPosition &position, float &x, float &y)
{
	if (!IsValid(position)) {
		x = static_cast<float>(position.tile.x);
		y = static_cast<float>(position.tile.y);
		return;
	}
	const FreeMovePosition &f = position.freeMove;
	const float t = static_cast<float>(ProgressToNextGameTick) / static_cast<float>(AnimationInfo::baseValueFraction);
	x = f.prevX + (f.x - f.prevX) * t;
	y = f.prevY + (f.y - f.prevY) * t;
}

namespace {

Displacement FreeScreenOffset(const ActorPosition &position)
{
	if (!IsValid(position))
		return {};
	float x;
	float y;
	GetActorWorldPosition(position, x, y);
	const float dx = x - static_cast<float>(position.tile.x);
	const float dy = y - static_cast<float>(position.tile.y);
	return { static_cast<int>(std::lround((dx - dy) * 32.0F)), static_cast<int>(std::lround((dx + dy) * 16.0F)) };
}

DisplacementOf<int8_t> FreeLightOffset(const ActorPosition &position)
{
	if (!IsValid(position))
		return {};
	const float dx = position.freeMove.x - static_cast<float>(position.tile.x);
	const float dy = position.freeMove.y - static_cast<float>(position.tile.y);
	return { static_cast<int8_t>(std::lround(dx * 8.0F)), static_cast<int8_t>(std::lround(dy * 8.0F)) };
}

} // namespace

Displacement FreeMoveCurrentScreenOffset(const ActorPosition &position)
{
	if (!IsValid(position))
		return {};
	const float dx = position.freeMove.x - static_cast<float>(position.tile.x);
	const float dy = position.freeMove.y - static_cast<float>(position.tile.y);
	return { static_cast<int>(std::lround((dx - dy) * 32.0F)), static_cast<int>(std::lround((dx + dy) * 16.0F)) };
}

Displacement GetPlayerDrawOffset(const Player &player, bool cameraMode)
{
	Displacement offset = FreeScreenOffset(player.position);
	if (player.isWalking() && !player.position.freeMove.walking)
		offset += GetOffsetForWalking(player.AnimInfo, player._pdir);
	return cameraMode ? -offset : offset;
}

Displacement GetMonsterDrawOffset(const Monster &monster, bool cameraMode)
{
	Displacement offset = FreeScreenOffset(monster.position);
	if (monster.isWalking() && !monster.position.freeMove.walking)
		offset += GetOffsetForWalking(monster.animInfo, monster.direction);
	return cameraMode ? -offset : offset;
}

DisplacementOf<int8_t> GetPlayerLightOffset(const Player &player)
{
	if (player.isWalking() && !player.position.freeMove.walking) {
		const WorldTileDisplacement offset = player.position.CalculateWalkingOffset(player._pdir, player.AnimInfo);
		return offset.screenToLight();
	}
	return FreeLightOffset(player.position);
}

DisplacementOf<int8_t> GetMonsterLightOffset(const Monster &monster)
{
	if (monster.isWalking() && !monster.position.freeMove.walking) {
		const WorldTileDisplacement offset = monster.position.CalculateWalkingOffset(monster.direction, monster.animInfo);
		return offset.screenToLight();
	}
	return FreeLightOffset(monster.position);
}

// --- Hero ------------------------------------------------------------------------------------

bool IsPlayerFreeWalking(const Player &player)
{
	return player.position.freeMove.walking && player.isWalking();
}

void FreeMoveStartWalk(Player &player)
{
	PlayerFreeState &st = State(player);
	Sync(player.position);
	if (st.mode != WalkMode::Heading)
		st.mode = WalkMode::Path;
	Direction dir = player._pdir;
	Point wp[MaxPathLengthPlayer];
	const int n = BuildWaypoints(player, wp);
	float tx;
	float ty;
	bool useBody;
	PathSteer(player, st, wp, n, tx, ty, useBody);
	dir = Facing(tx - player.position.freeMove.x, ty - player.position.freeMove.y, dir, false);
	BeginWalk(player, dir);
}

bool FreeMoveDoWalk(Player &player)
{
	PlayerFreeState &st = State(player);
	FreeMovePosition &f = player.position.freeMove;
	Sync(player.position);
	if (!IsFreeMovement()) {
		StopWalk(player);
		return true;
	}
	// A walk that ends and restarts within one tick must not move twice.
	if (st.movedTick == LogicTick)
		return false;
	st.movedTick = LogicTick;
	PlayWalkSound(player);

	const float speed = TilesPerTick(player);
	float dx = 0;
	float dy = 0;
	bool useBody = true;
	Point wp[MaxPathLengthPlayer];
	int n = 0;

	if (st.mode == WalkMode::Heading) {
		const bool dashing = st.dashLeft >= 0;
		if (!dashing && (player.walkpath[0] != WALK_NONE || player.destAction != ACTION_NONE || LogicTick - st.headTick > 1)) {
			StopWalk(player);
			return true;
		}
		const float mul = dashing ? st.dashSpeed : 1.0F;
		float step = speed * mul / std::max(std::abs(st.headX), std::abs(st.headY));
		if (dashing)
			step = std::min(step, st.dashLeft);
		dx = st.headX * step;
		dy = st.headY * step;
	} else {
		st.mode = WalkMode::Path;
		n = BuildWaypoints(player, wp);
		if (n > 0)
			st.goalTile = wp[n - 1];
		else
			st.goalTile = player.position.tile;
		float tx;
		float ty;
		PathSteer(player, st, wp, n, tx, ty, useBody);
		const float ddx = tx - f.x;
		const float ddy = ty - f.y;
		const float dist = std::hypot(ddx, ddy);
		if (n == 0 && dist < 0.02F) {
			ClrPlrPath(player);
			st.hasPrecise = false;
			StopWalk(player);
			return true;
		}
		if (dist > 1e-5F) {
			const float ux = ddx / dist;
			const float uy = ddy / dist;
			const float step = std::min(dist, speed / std::max(std::abs(ux), std::abs(uy)));
			dx = ux * step;
			dy = uy * step;
		}
	}

	if (dx * dx + dy * dy > 1e-10F)
		SetWalkFacing(player, Facing(dx, dy, player._pdir, true));

	bool switched = false;
	const float want = std::hypot(dx, dy);
	const float moved = MovePlayer(player, dx, dy, useBody, switched);
	if (want > 1e-4F && moved < want * 0.3F)
		st.stuck++;
	else
		st.stuck = 0;
	UpdatePlayerLight(player);

	if (st.mode == WalkMode::Heading && st.dashLeft >= 0) {
		st.dashLeft -= moved;
		if (st.dashLeft <= 1e-3F || st.stuck > 0) {
			StopWalk(player);
			return true;
		}
	}

	if (switched)
		AutoPickup(player);

	if (st.mode == WalkMode::Heading) {
		if (st.stuck >= 2) {
			StopWalk(player);
			return true;
		}
		return false;
	}

	if (n == 0 && st.stuck > 0) {
		// Final approach inside the destination tile: this is as close as the hero can get.
		ClrPlrPath(player);
		st.hasPrecise = false;
		StopWalk(player);
		return true;
	}
	if (st.strictTicks > 0)
		st.strictTicks--;
	if (st.stuck == 2)
		st.strictTicks = 16; // the straight line keeps failing: follow the plain tile path
	if (st.stuck >= 6) {
#ifdef _DEBUG
		LogVerbose("FREEMOVE hero stuck at {:.2f},{:.2f}, walk abandoned", f.x, f.y);
#endif
		ClrPlrPath(player);
		st.hasPrecise = false;
		player.destAction = ACTION_NONE;
		StopWalk(player);
		return true;
	}

	if (switched) {
		ResyncWalkpath(player, st, wp, n);
		if (player.walkpath[0] != WALK_NONE || player.destAction != ACTION_NONE) {
			ChainStand(player);
			return true;
		}
	}
	return false;
}

void FreeMoveOnWalkCommand(Player &player, Point target)
{
	if (!IsFreeMovement())
		return;
	PlayerFreeState &st = State(player);
	st.dashLeft = -1;
	st.stuck = 0;
	st.strictTicks = 0;
	st.hasPrecise = false;
	if (&player == MyPlayer) {
		if (st.hasPending && st.pendingTile == target) {
			st.hasPrecise = true;
			st.preciseX = st.pendingX;
			st.preciseY = st.pendingY;
		} else if (RoundPoint(CursorX, CursorY) == target) {
			st.hasPrecise = true;
			st.preciseX = CursorX;
			st.preciseY = CursorY;
		}
	}
	st.hasPending = false;
	st.goalTile = target;
	if (st.mode == WalkMode::Heading)
		st.mode = WalkMode::Path;
	// A click inside the hero's own tile: walk to the exact point.
	if (st.hasPrecise && player._pmode == PM_STAND && player.walkpath[0] == WALK_NONE && target == player.position.tile) {
		st.mode = WalkMode::Path;
		FreeMoveStartWalk(player);
	}
}

void FreeMoveUpdateHeldTarget(Player &player)
{
	if (!IsFreeMovement() || &player != MyPlayer)
		return;
	PlayerFreeState &st = State(player);
	if (st.mode != WalkMode::Path || !IsPlayerFreeWalking(player))
		return;
	if (RoundPoint(CursorX, CursorY) == st.goalTile) {
		st.hasPrecise = true;
		st.preciseX = CursorX;
		st.preciseY = CursorY;
	}
}

void FreeMoveSetHeading(Player &player, float worldX, float worldY)
{
	if (!IsFreeMovement())
		return;
	const float len = std::hypot(worldX, worldY);
	if (len < 1e-4F)
		return;
	PlayerFreeState &st = State(player);
	if (st.dashLeft >= 0 && IsPlayerFreeWalking(player))
		return;
	st.headX = worldX / len;
	st.headY = worldY / len;
	st.headTick = LogicTick;
	if (IsPlayerFreeWalking(player)) {
		if (st.mode != WalkMode::Heading) {
			ClrPlrPath(player);
			st.hasPrecise = false;
			st.mode = WalkMode::Heading;
		}
		return;
	}
	if (player._pmode != PM_STAND || player.destAction != ACTION_NONE || player.walkpath[0] != WALK_NONE)
		return;
	Sync(player.position);
	const Direction dir = Facing(st.headX, st.headY, player._pdir, false);
	if (!CanHeadThere(player, st.headX, st.headY)) {
		// Facing the wall without walking into it.
		if (dir != player._pdir)
			StartStand(player, dir);
		return;
	}
	st.mode = WalkMode::Heading;
	st.hasPrecise = false;
	st.dashLeft = -1;
	BeginWalk(player, dir);
}

void FreeMoveStopHeading(Player &player)
{
	PlayerFreeState &st = State(player);
	if (st.mode != WalkMode::Heading || st.dashLeft >= 0)
		return;
	if (IsPlayerFreeWalking(player))
		StopWalk(player);
	st.mode = WalkMode::None;
}

bool FreeMoveDash(Player &player, float worldX, float worldY, float distance, float speedMultiplier)
{
	if (!IsFreeMovement())
		return false;
	const float len = std::hypot(worldX, worldY);
	if (len < 1e-4F)
		return false;
	if (player._pmode != PM_STAND && !IsPlayerFreeWalking(player))
		return false;
	PlayerFreeState &st = State(player);
	Sync(player.position);
	st.headX = worldX / len;
	st.headY = worldY / len;
	st.headTick = LogicTick;
	if (!CanHeadThere(player, st.headX, st.headY))
		return false;
	ClrPlrPath(player);
	player.destAction = ACTION_NONE;
	st.hasPrecise = false;
	st.dashLeft = distance;
	st.dashSpeed = speedMultiplier;
	const Direction dir = Facing(st.headX, st.headY, player._pdir, false);
	if (IsPlayerFreeWalking(player))
		SetWalkFacing(player, dir);
	else
		BeginWalk(player, dir);
	st.mode = WalkMode::Heading;
	return true;
}

bool FreeMoveIsDashing(const Player &player)
{
	return State(player).dashLeft >= 0 && IsPlayerFreeWalking(player);
}

void SetFreeMoveCursorPoint(float x, float y)
{
	CursorX = x;
	CursorY = y;
}

void FreeMoveNoteWalkRequest(Point target)
{
	if (!IsFreeMovement() || MyPlayer == nullptr)
		return;
	PlayerFreeState &st = State(*MyPlayer);
	st.hasPending = RoundPoint(CursorX, CursorY) == target;
	st.pendingTile = target;
	st.pendingX = CursorX;
	st.pendingY = CursorY;
}

// --- Monsters --------------------------------------------------------------------------------

bool IsMonsterFreeWalking(const Monster &monster)
{
	return monster.position.freeMove.walking && monster.isWalking();
}

Direction FreeMoveStartMonsterStride(Monster &monster, Direction md)
{
	ActorPosition &pos = monster.position;
	FreeMovePosition &f = pos.freeMove;
	Sync(pos);
	const Point tile = pos.tile;
	const Point stepTile = tile + md;
	float ax = static_cast<float>(stepTile.x);
	float ay = static_cast<float>(stepTile.y);
	const bool strict = f.blocked;
#ifdef _DEBUG
	DbgStrides++;
	if (strict)
		DbgStrictAfterBlock++;
#endif
	f.blocked = false;

	if (!strict && (monster.flags & MFLAG_NO_ENEMY) == 0) {
		// Chasing: head straight at the enemy when the AI's step points roughly that way.
		float ex;
		float ey;
		EnemyPosition(monster, ex, ey);
		const float vx = ex - f.x;
		const float vy = ey - f.y;
		const float dist = std::hypot(vx, vy);
		const Displacement mdv { md };
		const float mdLen = std::hypot(static_cast<float>(mdv.deltaX), static_cast<float>(mdv.deltaY));
		if (dist > 1.15F && mdLen > 0) {
			const float ux = vx / dist;
			const float uy = vy / dist;
			const float cosine = (ux * static_cast<float>(mdv.deltaX) + uy * static_cast<float>(mdv.deltaY)) / mdLen;
			if (cosine > 0.64F) {
				const float cheb = 1.0F / std::max(std::abs(ux), std::abs(uy));
				const float reach = std::min(cheb, dist - 0.9F);
				const float cx = f.x + ux * reach;
				const float cy = f.y + uy * reach;
				const Point ct = RoundPoint(cx, cy);
				bool ok = false;
				if (ct == stepTile || ct == tile)
					ok = true;
				else if (Chebyshev(ct, tile) == 1)
					ok = DirOK(monster, GetDirection(tile, ct));
				if (ok && WallPenetration(cx, cy) == 0) {
#ifdef _DEBUG
					DbgChase++;
#endif
					ax = cx;
					ay = cy;
				}
			}
		}
	}

	const Point aimTile = RoundPoint(ax, ay);
	f.hasClaim = false;
	if (aimTile != tile) {
		monster.occupyTile(aimTile, true);
		f.claim = aimTile;
		f.hasClaim = true;
	}
	pos.future = aimTile;
	pos.old = tile;
	f.startX = f.x;
	f.startY = f.y;
	f.aimX = ax;
	f.aimY = ay;
	f.strideTick = 0;
	f.strideTicks = 1;
	f.walking = true;
	f.strict = strict || (ax == static_cast<float>(stepTile.x) && ay == static_cast<float>(stepTile.y) && f.x == static_cast<float>(tile.x) && f.y == static_cast<float>(tile.y));
	monster.var1 = 0;
	monster.var2 = 0;
	const Direction face = Facing(ax - f.x, ay - f.y, md, false);
	monster.var3 = static_cast<int8_t>(face);
	return face;
}

void FreeMoveSetMonsterStrideTicks(Monster &monster)
{
	const int ticks = std::max(1, monster.animInfo.numberOfFrames * std::max<int>(1, monster.animInfo.ticksPerFrame));
	monster.position.freeMove.strideTicks = static_cast<int16_t>(ticks + 1);
}

bool FreeMoveMonsterWalk(Monster &monster)
{
	ActorPosition &pos = monster.position;
	FreeMovePosition &f = pos.freeMove;
	Sync(pos);
	const bool strict = f.strict;
	const bool animationEnd = monster.animInfo.isLastFrame();
	f.strideTick++;
	if (f.blocked && !animationEnd)
		return false; // blocked: wait for the stride to end (ending early could loop the AI forever)
	const float t = animationEnd ? 1.0F : std::min(1.0F, static_cast<float>(f.strideTick) / static_cast<float>(f.strideTicks));
	const float gx = f.startX + (f.aimX - f.startX) * t;
	const float gy = f.startY + (f.aimY - f.startY) * t;
	const float want = std::hypot(gx - f.x, gy - f.y);
	bool blocked = false;
	if (want > 1e-4F) {
		const auto canEnter = [&monster](Point from, Point to) {
			const FreeMovePosition &fm = monster.position.freeMove;
			if (fm.hasClaim && to == fm.claim)
				return true;
			if (!IsTileAvailable(monster, to))
				return false;
			return from.x == to.x || from.y == to.y || CanStep(from, to);
		};
		const auto onSwitch = [&monster](Point next) { MonsterSwitchTile(monster, next); };
		Mover m { pos, &monster, canEnter, onSwitch, !strict };
		const float moved = Move(m, gx - f.x, gy - f.y);
		if (moved < want * 0.5F && want > 0.03F)
			blocked = true;
	}
	if (blocked) {
		f.blocked = true;
#ifdef _DEBUG
		DbgBlocked++;
#endif
	}
	if (!animationEnd)
		return false;
	ReleaseClaim(monster);
	f.walking = false;
	pos.future = pos.tile;
	return true;
}

// --- Helpers ---------------------------------------------------------------------------------

void ScreenToWorldVector(float screenX, float screenY, float &worldX, float &worldY)
{
	const float wx = screenX / 64.0F + screenY / 32.0F;
	const float wy = screenY / 32.0F - screenX / 64.0F;
	const float len = std::hypot(wx, wy);
	if (len < 1e-6F) {
		worldX = worldY = 0;
		return;
	}
	worldX = wx / len;
	worldY = wy / len;
}

Direction FacingForWorldVector(float worldX, float worldY)
{
	return Facing(worldX, worldY, Direction::South, false);
}

} // namespace devilution

#ifdef _DEBUG
// --- Test harness (DEVILUTIONX_DEV_FREECHECK / DEVILUTIONX_DEV_FREEWALK) ----------------------

#include <cstdio>
#include <string_view>

#include "debug.h"
#include "msg.h"
#include "utils/log.hpp"
#include "utils/str_cat.hpp"
#include "utils/str_split.hpp"

namespace devilution {

namespace {

struct FreeCheckStats {
	int ticks = 0;
	int violations = 0;
	int tileChanges = 0;
	float maxOffset = 0;
	float maxPenetration = 0;
	float minSeparation = 99;
	int sameTile = 0;
	Point lastTile;
	int commands = 0;
	int arrivals = 0;
	float maxArrivalError = 0;
	bool waitingArrival = false;
	float targetX = 0;
	float targetY = 0;
};
FreeCheckStats Stats;

void Violation(std::string_view what, int id, Point tile, float x, float y)
{
	Stats.violations++;
	if (Stats.violations <= 40)
		LogInfo("FREECHECK violation {} actor {} tile {},{} pos {:.2f},{:.2f}", what, id, tile.x, tile.y, x, y);
}

void CheckActor(const ActorPosition &pos, int id, bool isPlayer)
{
	const Point tile = pos.tile;
	if (!InDungeonBounds(tile)) {
		Violation("out of bounds", id, tile, 0, 0);
		return;
	}
	if (IsTileSolid(tile))
		Violation("on solid tile", id, tile, 0, 0);
	if (!IsValid(pos))
		return;
	const float ox = std::abs(pos.freeMove.x - static_cast<float>(tile.x));
	const float oy = std::abs(pos.freeMove.y - static_cast<float>(tile.y));
	Stats.maxOffset = std::max({ Stats.maxOffset, ox, oy });
	if (ox > 0.5F + SwitchHysteresis + 0.01F || oy > 0.5F + SwitchHysteresis + 0.01F)
		Violation("offset beyond tile", id, tile, pos.freeMove.x, pos.freeMove.y);
	const float pen = WallPenetration(pos.freeMove.x, pos.freeMove.y);
	Stats.maxPenetration = std::max(Stats.maxPenetration, pen);
	if (pen > 0.45F)
		Violation(isPlayer ? "player deep in wall" : "monster deep in wall", id, tile, pos.freeMove.x, pos.freeMove.y);
}

} // namespace

void FreeMoveDebugTick()
{
	static const bool check = std::getenv("DEVILUTIONX_DEV_FREECHECK") != nullptr;
	static const char *walk = std::getenv("DEVILUTIONX_DEV_FREEWALK");
	if (!check && walk == nullptr)
		return;
	if (MyPlayer == nullptr || !MyPlayer->plractive)
		return;
	Player &player = *MyPlayer;
	Stats.ticks++;

	if (check) {
		const int pid = player.getId() + 1;
		if (std::abs(static_cast<int>(dPlayer[player.position.tile.x][player.position.tile.y])) != pid)
			Violation("player not in dPlayer", pid, player.position.tile, 0, 0);
		CheckActor(player.position, 0, true);
		if (player.position.tile != Stats.lastTile) {
			Stats.tileChanges++;
			Stats.lastTile = player.position.tile;
		}
		std::vector<std::pair<float, float>> living;
		std::vector<int> livingIds;
		livingIds.push_back(0);
		float px;
		float py;
		CurrentPosition(player.position, px, py);
		living.emplace_back(px, py);
		for (size_t i = 0; i < ActiveMonsterCount; i++) {
			const Monster &monster = Monsters[ActiveMonsters[i]];
			if (monster.hasNoLife() || monster.isInvalid || (monster.position.tile.x <= 1 && monster.position.tile.y == 0) || (monster.flags & MFLAG_HIDDEN) != 0)
				continue;
			const int mid = static_cast<int>(monster.getId()) + 1;
			const Point t = monster.position.tile;
			if (InDungeonBounds(t) && dMonster[t.x][t.y] != mid)
				Violation("monster not in dMonster", mid, t, 0, 0);
			const FreeMovePosition &f = monster.position.freeMove;
			if (IsMonsterFreeWalking(monster) && f.hasClaim && dMonster[f.claim.x][f.claim.y] != -mid)
				Violation("claim lost", mid, f.claim, 0, 0);
			if (InDungeonBounds(t) && dPlayer[t.x][t.y] != 0)
				Violation("monster on player tile", mid, t, 0, 0);
			CheckActor(monster.position, mid, false);
			float mx;
			float my;
			CurrentPosition(monster.position, mx, my);
			living.emplace_back(mx, my);
			livingIds.push_back(mid);
		}
		for (size_t a = 0; a < living.size(); a++) {
			for (size_t b = a + 1; b < living.size(); b++) {
				const float d = std::hypot(living[a].first - living[b].first, living[a].second - living[b].second);
				Stats.minSeparation = std::min(Stats.minSeparation, d);
				static int closeLogs = 0;
				if (d < 0.45F && closeLogs++ < 12) {
					const auto describe = [](int id) -> std::string {
						if (id == 0)
							return StrCat("hero mode ", static_cast<int>(MyPlayer->_pmode));
						const Monster &m = Monsters[id - 1];
						return StrCat("monster ", id, " mode ", static_cast<int>(m.mode), " free ", m.position.freeMove.walking ? 1 : 0, " strict ", m.position.freeMove.strict ? 1 : 0);
					};
					LogInfo("FREECHECK close {:.2f}: {} at {:.2f},{:.2f} / {} at {:.2f},{:.2f}", d, describe(livingIds[a]), living[a].first, living[a].second, describe(livingIds[b]), living[b].first, living[b].second);
				}
			}
		}
	}

	if (const char *toggles = std::getenv("DEVILUTIONX_DEV_FREETOGGLE"); toggles != nullptr) {
		// Comma separated ticks at which free movement is toggled (like pressing F).
		for (const std::string_view entry : SplitByChar(toggles, ',')) {
			if (std::atoi(std::string(entry).c_str()) == Stats.ticks) {
				ToggleFreeMovement();
				LogInfo("FREECHECK toggled at tick {}: now {}", Stats.ticks, IsFreeMovement() ? "free" : "grid");
			}
		}
	}

	if (const char *cast = std::getenv("DEVILUTIONX_DEV_FREECAST"); cast != nullptr) {
		player._pRSpell = static_cast<SpellID>(std::atoi(cast));
		player._pRSplType = SpellType::Spell;
		player._pMana = player._pMaxMana;
	}

	if (walk != nullptr) {
		DebugGodMode = true;
		const std::string_view mode = walk;
		if (mode == "spin") {
			// Walk in a slowly turning direction: runs along walls, into corners and through doors.
			const float angle = static_cast<float>(Stats.ticks) * 0.035F;
			FreeMoveSetHeading(player, std::cos(angle), std::sin(angle));
		} else if (mode == "paths") {
			if (Stats.waitingArrival && player._pmode == PM_STAND && player.walkpath[0] == WALK_NONE) {
				Stats.waitingArrival = false;
				const float err = std::hypot(player.position.freeMove.x - Stats.targetX, player.position.freeMove.y - Stats.targetY);
				Stats.arrivals++;
				if (err < 0.3F)
					Stats.maxArrivalError = std::max(Stats.maxArrivalError, err);
				LogInfo("FREEWALK arrived at {:.2f},{:.2f} target {:.2f},{:.2f} err {:.2f}", player.position.freeMove.x, player.position.freeMove.y, Stats.targetX, Stats.targetY, err);
			}
			if (Stats.ticks % 70 == 5) {
				for (int attempt = 0; attempt < 50; attempt++) {
					const Point t = player.position.tile + Displacement { std::rand() % 17 - 8, std::rand() % 17 - 8 };
					if (!InDungeonBounds(t) || !PosOkPlayer(player, t))
						continue;
					const float fx = static_cast<float>(t.x) + static_cast<float>(std::rand() % 80 - 40) / 100.0F;
					const float fy = static_cast<float>(t.y) + static_cast<float>(std::rand() % 80 - 40) / 100.0F;
					SetFreeMoveCursorPoint(fx, fy);
					Stats.targetX = fx;
					Stats.targetY = fy;
					Stats.waitingArrival = true;
					Stats.commands++;
					NetSendCmdLoc(MyPlayerId, true, CMD_WALKXY, t);
					break;
				}
			}
		} else if (mode == "hunt") {
			// Walk up to the nearest monster and fight it.
			static int kills = 0;
			static int lastAlive = -1;
			int alive = 0;
			for (size_t i = 0; i < ActiveMonsterCount; i++)
				alive += Monsters[ActiveMonsters[i]].hasNoLife() ? 0 : 1;
			if (lastAlive >= 0 && alive < lastAlive)
				kills += lastAlive - alive;
			lastAlive = alive;
			Stats.arrivals = kills;
			if (player._pmode == PM_STAND && player.destAction == ACTION_NONE && Stats.ticks % 10 == 0) {
				int best = -1;
				int bestDist = 14;
				for (size_t i = 0; i < ActiveMonsterCount; i++) {
					const Monster &monster = Monsters[ActiveMonsters[i]];
					if (monster.hasNoLife() || (monster.flags & MFLAG_HIDDEN) != 0 || monster.isInvalid || monster.position.tile.x <= 1)
						continue;
					const int dist = player.position.tile.WalkingDistance(monster.position.tile);
					if (dist < bestDist) {
						bestDist = dist;
						best = static_cast<int>(ActiveMonsters[i]);
					}
				}
				if (best >= 0) {
					Stats.commands++;
					NetSendCmdParam1(true, CMD_ATTACKID, static_cast<uint16_t>(best));
				}
			}
		} else if (mode == "dash") {
			// Dodge-style dashes in random directions.
			if (Stats.ticks % 25 == 0) {
				const float angle = static_cast<float>(std::rand() % 628) / 100.0F;
				const bool ok = FreeMoveDash(player, std::cos(angle), std::sin(angle), 2.0F, 3.0F);
				Stats.commands++;
				if (ok)
					Stats.arrivals++;
			}
		} else if (mode == "fight") {
			// Stand still and swing at anything adjacent.
			if (player._pmode == PM_STAND) {
				for (size_t i = 0; i < ActiveMonsterCount; i++) {
					const Monster &monster = Monsters[ActiveMonsters[i]];
					if (!monster.hasNoLife() && player.position.tile.WalkingDistance(monster.position.tile) <= 1) {
						NetSendCmdParam1(true, CMD_ATTACKID, static_cast<uint16_t>(monster.getId()));
						break;
					}
				}
			}
		}
	}

	if (std::getenv("DEVILUTIONX_DEV_FREETRACE") != nullptr) {
		for (size_t i = 0; i < ActiveMonsterCount; i++) {
			const Monster &monster = Monsters[ActiveMonsters[i]];
			if (monster.hasNoLife() || !IsValid(monster.position))
				continue;
			LogInfo("FREETRACE {} m{} {:.3f} {:.3f} tile {} {} mode {} dir {} walk {} strict {}", Stats.ticks, monster.getId(), monster.position.freeMove.x, monster.position.freeMove.y, monster.position.tile.x, monster.position.tile.y, static_cast<int>(monster.mode), static_cast<int>(monster.direction), IsMonsterFreeWalking(monster) ? 1 : 0, monster.position.freeMove.strict ? 1 : 0);
		}
		LogInfo("FREETRACE {} hero {:.3f} {:.3f}", Stats.ticks, player.position.freeMove.x, player.position.freeMove.y);
	}

	if (Stats.ticks % 100 == 0)
		LogInfo("FREESTRIDES {} chase {} strictAfterBlock {} blocked {} (wall {} tile {} crowd {})", DbgStrides, DbgChase, DbgStrictAfterBlock, DbgBlocked, DbgBlockedWall, DbgBlockedTile, DbgBlockedCrowd);
	if (Stats.ticks % 100 == 0)
		LogInfo("FREECHECK tick {} violations {} tiles {} maxOffset {:.2f} maxPen {:.2f} minSep {:.2f} cmds {} arrivals {} pos {:.2f},{:.2f} mode {}", Stats.ticks, Stats.violations, Stats.tileChanges, Stats.maxOffset, Stats.maxPenetration, Stats.minSeparation, Stats.commands, Stats.arrivals, player.position.freeMove.x, player.position.freeMove.y, static_cast<int>(player._pmode));
}

} // namespace devilution
#endif
