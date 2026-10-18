/**
 * @file towners.h
 *
 * Interface of functionality for loading and spawning towners.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/clx_sprite.hpp"
#include "engine/render/hd_sprite.hpp"
#include "items.h"
#include "levels/dun_tile.hpp"
#include "player.h"
#include "quests.h"
#include "utils/attributes.h"

namespace devilution {

enum _talker_id : uint8_t {
	TOWN_SMITH,
	TOWN_HEALER,
	TOWN_DEADGUY,
	TOWN_TAVERN,
	TOWN_STORY,
	TOWN_DRUNK,
	TOWN_WITCH,
	TOWN_BMAID,
	TOWN_PEGBOY,
	TOWN_COW,
	TOWN_FARMER,
	TOWN_GIRL,
	TOWN_COWFARM,
	/** Tremain, the cut priest from Diablo's data, now running a gambling table. */
	TOWN_GAMBLER,
	/** Brother Kalim, Sand Tristram's priest: sells magic items. */
	TOWN_PRIEST,
	/** Jamal's caravan, between Tristram and Sand Tristram. */
	TOWN_CARAVAN,
	/** The caravan's cow, loaded with bags. */
	TOWN_PACKCOW,
	/** A townsperson made in the Campaign Editor (any number of them; see custom_maps/custom_npcs.hpp). */
	TOWN_CUSTOM,
	// Note: Enum values are parsed from TSV using magic_enum
	// The actual count is determined dynamically from TSV data
};

// Runtime mappings built from TSV data
extern DVL_API_FOR_TEST std::unordered_map<_talker_id, std::string> TownerLongNames; // Maps towner type enum to display name
extern const std::unordered_map<_talker_id, const char *> TownerShortNames;          // Maps towner type enum to Lua/mod short name

struct Towner {
	OptionalOwnedClxSpriteList ownedAnim;
	/** The new-texture version of ownedAnim (same frames), when there is one. */
	OptionalOwnedClxSpriteList ownedNewAnim;
	/** The original-art animation (ownedAnim, or a shared sheet such as the cows'). */
	OptionalClxSpriteList classicAnim;
	/** The full-colour version of the new sprites (HD textures), when there is one. */
	std::shared_ptr<const HdSprite> hd;
	/** ownedAnim itself is new sprites (a look that has no original art). */
	bool classicIsNew = false;
	/** The new sprites are the ones shown now. */
	bool showingNew = false;

	// Strolling about near where he stands (Griswold, with the new textures, single player).
	/** Walk sprites: 8 directions (Direction order) of TownerWalkFrames frames; none: he stays put. */
	OptionalOwnedClxSpriteList ownedWalk;
	std::shared_ptr<const HdSprite> walkHd;
	/** Where he works (where the map puts him) and where he is going. */
	Point home;
	Point goal;
	Point stepFrom;
	Direction walkDir = Direction::South;
	/** 0 at work (his usual animation), 1 walking, 2 stopped for a moment. */
	uint8_t walkState = 0;
	uint8_t strolls = 0;      // stops made on this walk
	int16_t walkTimer = 0;    // ticks to wait
	int16_t stepTick = 0;     // ticks into the current step (0: not stepping)
	int16_t walkAnimTick = 0; // walk animation clock
	int16_t stuckTicks = 0;

	/** The walk sprites are shown now (out of his usual spot or on the way). */
	[[nodiscard]] bool usingWalkSprite() const
	{
		return walkState != 0 && ownedWalk && showingNew;
	}
	/** Frame of the walk sprites (direction * TownerWalkFrames + frame). */
	[[nodiscard]] int walkSpriteFrame() const;
	/** Drawn over the towner each frame (the pack cow's load). */
	OptionalOwnedClxSpriteList ownedOverlay;
	OptionalClxSpriteList anim;
	/** Specifies the animation frame sequence. */
	std::span<const uint8_t> animOrder;
	void (*talk)(Player &player, Towner &towner);

	std::string_view name;

	/** Tile position of NPC */
	Point position;
	/** Randomly chosen topic for discussion (picked when loading into town) */
	_speech_id gossip;
	uint16_t _tAnimWidth;
	/** Tick length of each frame in the current animation */
	int16_t _tAnimDelay;
	/** Increases by one each game tick, counting how close we are to _pAnimDelay */
	int16_t _tAnimCnt;
	/** Number of frames in current animation */
	uint8_t _tAnimLen;
	/** Current frame of animation. */
	uint8_t _tAnimFrame;
	uint8_t _tAnimFrameCnt;
	_talker_id _ttype;

	[[nodiscard]] ClxSprite currentSprite() const
	{
		if (usingWalkSprite())
			return (*ownedWalk)[static_cast<size_t>(walkSpriteFrame()) % ownedWalk->numSprites()];
		return (*anim)[_tAnimFrame % anim->numSprites()];
	}
	/** While stepping between tiles: where he is drawn from his tile. */
	[[nodiscard]] Displacement stepOffset() const;
	[[nodiscard]] Displacement getRenderingOffset() const
	{
		return Displacement { -CalculateSpriteTileCenterX(_tAnimWidth), 0 } + stepOffset();
	}
};

/** Frames per direction of the walk sprites. */
constexpr int TownerWalkFrames = 10;

extern std::vector<Towner> Towners;

/** @brief Shows the new or the original art on every townsperson, as the Textures option says. */
void ApplyTownerTextures();
/** @brief Cycles Vanilla, HD and HD Redux textures (the backslash key). */
void ToggleNewTextures();
/** @brief The new-texture sprites standing in for an original towner animation (e.g. "towners\\smith\\smithn"), if any. */
[[nodiscard]] const char *NewTownerTexturePath(std::string_view classicPath);
/** @brief Whether a towner look (original path without extension, or "towners\\new\\<name>") can be shown. */
[[nodiscard]] bool TownerLookExists(std::string_view path);
/** @brief A towner look's sprites as the New Textures option shows them. */
[[nodiscard]] OwnedClxSpriteList LoadTownerLookSprites(std::string_view path, uint16_t width);

/**
 * @brief Returns the number of unique towner types found in TSV data.
 * This is dynamically determined from the loaded towner data.
 */
size_t GetNumTownerTypes();

/**
 * @brief Returns the number of towner instances (actual spawned towners).
 * This is dynamically determined from the loaded towner data.
 */
size_t GetNumTowners();

bool IsTownerPresent(_talker_id npc);
/**
 * @brief Maps from a _talker_id value to a pointer to the Towner object, if they have been initialised
 * @param type enum constant identifying the towner
 * @return Pointer to the Towner or nullptr if they are not available
 */
Towner *GetTowner(_talker_id type);

void InitTowners();
void FreeTownerGFX();
void ProcessTowners();
void TalkToTowner(Player &player, int t);

void UpdateGirlAnimAfterQuestComplete();
void UpdateCowFarmerAnimAfterQuestComplete();

#ifdef _DEBUG
bool DebugTalkToTowner(_talker_id type);
#endif

} // namespace devilution
