#include "custom_maps/map_monsters.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>

#include <magic_enum/magic_enum.hpp>

#include "custom_maps/custom_map.hpp"
#include "custom_maps/map_editor.hpp"
#include "engine/direction.hpp"
#include "engine/random.hpp"
#include "game_mode.hpp"
#include "levels/forest.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "monster.h"
#include "player.h"
#include "tables/monstdat.h"
#include "utils/log.hpp"
#include "utils/status_macros.hpp"
#include "utils/str_cat.hpp"

namespace devilution {

namespace {

bool Usable(const MonsterData &data)
{
	if (data.availability == MonsterAvailability::Never)
		return false;
	if (gbIsSpawn && data.availability == MonsterAvailability::Retail)
		return false;
	return data.minDunLvl > 0;
}

std::string KeyOf(size_t type)
{
	if (type < static_cast<size_t>(NUM_DEFAULT_MTYPES)) {
		const std::string_view name = magic_enum::enum_name(static_cast<_monster_id>(type));
		if (!name.empty())
			return std::string(name);
	}
	return MonstersData[type].name;
}

std::optional<_monster_id> ResolveMonster(std::string_view key)
{
	if (const std::optional<_monster_id> id = magic_enum::enum_cast<_monster_id>(key); id && static_cast<size_t>(*id) < MonstersData.size())
		return id;
	for (size_t i = 0; i < MonstersData.size(); i++) {
		if (MonstersData[i].name == key)
			return static_cast<_monster_id>(i);
	}
	return std::nullopt;
}

std::optional<size_t> ResolveBoss(std::string_view name)
{
	for (size_t i = 0; i < UniqueMonstersData.size(); i++) {
		if (UniqueMonstersData[i].mName == name)
			return i;
	}
	return std::nullopt;
}

bool IsPlaceableBoss(size_t index)
{
	const UniqueMonsterData &data = UniqueMonstersData[index];
	if (static_cast<size_t>(data.mtype) >= MonstersData.size())
		return false;
	if (static_cast<UniqueMonsterType>(index) == UniqueMonsterType::NaKrul)
		return false; // tied to Hellfire's last level
	return MonstersData[data.mtype].availability != MonsterAvailability::Never && !(gbIsSpawn && MonstersData[data.mtype].availability == MonsterAvailability::Retail);
}

/** Types chosen for "auto" spawners on this visit: fitting the hero's level, the same for the same map and level. */
std::vector<_monster_id> AutoTypes;

uint32_t Hash(std::string_view text, uint32_t seed)
{
	uint32_t h = 2166136261u ^ seed;
	for (const char c : text)
		h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
	return h;
}

void ChooseAutoTypes(const CustomMap &map)
{
	AutoTypes.clear();
	const Player &player = *MyPlayer;
	const int tier = ForestTierForCharacterLevel(player.getCharacterLevel());
	std::vector<_monster_id> candidates;
	for (size_t i = 0; i < MonstersData.size(); i++) {
		const MonsterData &data = MonstersData[i];
		if (!Usable(data) || static_cast<_monster_id>(i) == MT_GOLEM)
			continue;
		if (tier >= data.minDunLvl && tier <= data.maxDunLvl)
			candidates.push_back(static_cast<_monster_id>(i));
	}
	uint32_t h = Hash(map.name, static_cast<uint32_t>(tier));
	while (!candidates.empty() && AutoTypes.size() < 3) {
		h = h * 1103515245u + 12345u;
		const size_t k = (h >> 8) % candidates.size();
		AutoTypes.push_back(candidates[k]);
		candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(k));
	}
}

std::optional<_monster_id> SpawnerType(const MapSpawner &spawner)
{
	if (spawner.monster == AutoMonster) {
		if (AutoTypes.empty())
			return std::nullopt;
		const uint32_t h = static_cast<uint32_t>(spawner.tile.x) * 73856093u ^ static_cast<uint32_t>(spawner.tile.y) * 19349663u;
		return AutoTypes[(h >> 4) % AutoTypes.size()];
	}
	return ResolveMonster(spawner.monster);
}

bool Active()
{
	return setlevel && setlvlnum == SL_CUSTOM && ActiveCustomMap() != nullptr && !IsMapEditing() && MyPlayer != nullptr;
}

/** Adds a level monster type if the level has room for its graphics. */
std::optional<size_t> AddType(_monster_id type)
{
	const size_t existing = LevelMonsterTypeIndex(type);
	if (existing < LevelMonsterTypeCount)
		return existing;
	if (LevelMonsterTypeCount >= MaxLvlMTypes || MonstersData[type].image > 4000 - monstimgtot) {
		LogWarn("Custom map: no room for monster type {}", MonstersData[type].name);
		return std::nullopt;
	}
	const std::expected<size_t, std::string> index = AddMonsterType(type, PLACE_SPECIAL);
	if (!index.has_value()) {
		LogWarn("Custom map: monster type {}: {}", MonstersData[type].name, index.error());
		return std::nullopt;
	}
	return *index;
}

bool FreeForMonster(Point p)
{
	return InDungeonBounds(p) && !IsTileSolid(p) && dMonster[p.x][p.y] == 0 && dPlayer[p.x][p.y] == 0 && dObject[p.x][p.y] == 0;
}

/** Free tiles around a spot, nearest first. */
std::vector<Point> FreeTilesAround(Point centre, int radius, size_t wanted)
{
	std::vector<Point> tiles;
	for (int r = 0; r <= radius && tiles.size() < wanted; r++) {
		for (int dy = -r; dy <= r && tiles.size() < wanted; dy++) {
			for (int dx = -r; dx <= r && tiles.size() < wanted; dx++) {
				if (std::max(std::abs(dx), std::abs(dy)) != r)
					continue;
				const Point p = centre + Displacement { dx, dy };
				if (FreeForMonster(p))
					tiles.push_back(p);
			}
		}
	}
	return tiles;
}

/** Bosses whose AI follows a quest's script fight like their kind instead (no speeches, no waiting). */
void MakeFighter(Monster &monster)
{
	if (IsAnyOf(monster.ai, MonsterAIID::Gharbad, MonsterAIID::Zhar, MonsterAIID::Snotspill, MonsterAIID::Lazarus, MonsterAIID::LazarusSuccubus, MonsterAIID::Lachdanan, MonsterAIID::Warlord)) {
		const MonsterAIID base = MonstersData[monster.type().type].ai;
		monster.ai = IsAnyOf(base, MonsterAIID::Gharbad, MonsterAIID::Zhar, MonsterAIID::Snotspill, MonsterAIID::Lazarus, MonsterAIID::LazarusSuccubus, MonsterAIID::Lachdanan, MonsterAIID::Warlord)
		    ? MonsterAIID::Fallen
		    : base;
	}
	monster.talkMsg = TEXT_NONE;
	monster.goal = MonsterGoal::Normal;
}

} // namespace

std::expected<void, std::string> AddCustomMapMonsterTypes()
{
	if (!Active())
		return {};
	const CustomMap &map = *ActiveCustomMap();
	ChooseAutoTypes(map);
	for (const MapSpawner &spawner : map.spawners) {
		if (const std::optional<_monster_id> type = SpawnerType(spawner); type)
			AddType(*type);
		else
			LogWarn("Custom map: unknown spawner monster {}", spawner.monster);
	}
	for (const MapBoss &boss : map.bosses) {
		if (const std::optional<size_t> index = ResolveBoss(boss.name); index && IsPlaceableBoss(*index))
			AddType(UniqueMonstersData[*index].mtype);
		else
			LogWarn("Custom map: unknown boss {}", boss.name);
	}
	return {};
}

std::expected<void, std::string> PlaceCustomMapMonsters()
{
	if (!Active())
		return {};
	const CustomMap &map = *ActiveCustomMap();
	int placed = 0;
	for (const MapBoss &boss : map.bosses) {
		const std::optional<size_t> index = ResolveBoss(boss.name);
		if (!index || !IsPlaceableBoss(*index) || LevelMonsterTypeIndex(UniqueMonstersData[*index].mtype) >= LevelMonsterTypeCount)
			continue;
		const std::vector<Point> spot = FreeTilesAround(boss.tile, 4, 1);
		if (spot.empty())
			continue;
		const int minions = UniqueMonstersData[*index].monsterPack == UniqueMonsterPack::None ? 0 : boss.minions;
		ASSIGN_OR_RETURN(Monster *const monster, PlaceUniqueMonsterAt(*index, spot.front(), minions));
		if (monster != nullptr) {
			MakeFighter(*monster);
			placed++;
		}
	}
	for (const MapSpawner &spawner : map.spawners) {
		const std::optional<_monster_id> type = SpawnerType(spawner);
		if (!type)
			continue;
		const size_t typeIndex = LevelMonsterTypeIndex(*type);
		if (typeIndex >= LevelMonsterTypeCount)
			continue;
		for (const Point p : FreeTilesAround(spawner.tile, 4, static_cast<size_t>(spawner.count))) {
			if (ActiveMonsterCount >= MaxMonsters - 10)
				break;
			if (PlaceMonsterAt(typeIndex, p) != nullptr)
				placed++;
		}
	}
	LogVerbose("Custom map {}: {} monsters from {} spawners and {} bosses", map.name, placed, map.spawners.size(), map.bosses.size());
	return {};
}

const std::vector<SpawnableMonster> &SpawnableMonsters()
{
	static std::vector<SpawnableMonster> list;
	if (!list.empty())
		return list;
	list.push_back({ std::string(AutoMonster), "Auto (fits the hero's level)" });
	std::vector<size_t> types;
	for (size_t i = 0; i < MonstersData.size(); i++) {
		if (Usable(MonstersData[i]) && static_cast<_monster_id>(i) != MT_GOLEM)
			types.push_back(i);
	}
	std::stable_sort(types.begin(), types.end(), [](size_t a, size_t b) { return MonstersData[a].minDunLvl < MonstersData[b].minDunLvl; });
	for (const size_t i : types) {
		std::string label = MonstersData[i].name;
		// Kinds that share a name (Dark One with spear or sword, ...) are told apart by their weapon.
		const bool shared = std::count_if(types.begin(), types.end(), [&](size_t j) { return MonstersData[j].name == MonstersData[i].name; }) > 1;
		if (shared) {
			const std::string key = KeyOf(i);
			const std::string_view tail = key.size() >= 2 ? std::string_view(key).substr(key.size() - 2) : std::string_view {};
			if (tail == "SP") label += ", spear";
			else if (tail == "SW" || tail == "SD") label += ", sword";
			else if (tail == "AX") label += ", axe";
			else if (tail == "BW") label += ", bow";
			else if (tail == "MC") label += ", mace";
			else label += StrCat(", ", key);
		}
		list.push_back({ KeyOf(i), StrCat(label, " (", static_cast<int>(MonstersData[i].minDunLvl), ")") });
	}
	return list;
}

std::string SpawnerLabel(std::string_view key)
{
	if (key == AutoMonster)
		return "Auto";
	if (const std::optional<_monster_id> type = ResolveMonster(key); type)
		return MonstersData[*type].name;
	return std::string(key);
}

const std::vector<std::string> &PlaceableBosses()
{
	static std::vector<std::string> list;
	if (!list.empty())
		return list;
	std::vector<size_t> bosses;
	for (size_t i = 0; i < UniqueMonstersData.size(); i++) {
		if (IsPlaceableBoss(i))
			bosses.push_back(i);
	}
	std::stable_sort(bosses.begin(), bosses.end(), [](size_t a, size_t b) { return UniqueMonstersData[a].mlevel < UniqueMonstersData[b].mlevel; });
	for (const size_t i : bosses)
		list.push_back(UniqueMonstersData[i].mName);
	return list;
}

} // namespace devilution
