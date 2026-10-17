#include "custom_maps/menu_console.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "DiabloUI/diabloui.h"
#include "custom_maps/campaign.hpp"
#include "custom_maps/campaign_quests.hpp"
#include "custom_maps/custom_town.hpp"
#include "custom_maps/custom_npcs.hpp"
#include "engine/load_cel.hpp"
#include "engine/load_file.hpp"
#include "engine/render/clx_render.hpp"
#include "tables/itemdat.h"
#include "utils/parse_int.hpp"
#include "custom_maps/custom_map.hpp"
#include "custom_maps/map_editor.hpp"
#include "engine/palette.h"
#include "engine/rectangle.hpp"
#include "engine/render/primitive_render.hpp"
#include "engine/render/text_render.hpp"
#include "engine/sound.h"
#include "engine/surface.hpp"
#include "utils/sdl_compat.h"
#include "utils/str_cat.hpp"
#include "utils/str_split.hpp"

namespace devilution {

namespace {

enum class Screen : uint8_t {
	Campaigns,
	NewCampaign,
	Campaign,
	NewMap,
	Quests,
	Quest,
	Npcs,
	Npc,
};

struct MapRow {
	std::string name;
	dungeon_type style;
	std::string info;
	bool town;
	std::string music;
};

/** A clickable area drawn this frame (immediate mode: rebuilt every frame). */
struct Clickable {
	Rectangle rect;
	std::string label;
	std::function<void(bool right)> action;
};

bool Open = false;
bool WantsEditor = false;
Screen Current = Screen::Campaigns;
std::vector<Clickable> Clickables;

std::vector<Campaign> CampaignList;
std::optional<Campaign> Editing; // the open campaign
std::vector<MapRow> Maps;
int MapScroll = 0;

// Text being typed into a field.
std::string *TypingInto = nullptr;
std::string TypingBackup;
size_t TypingLimit = 40;
bool TypingNameOnly = false; // map names: letters, digits, - and _
std::function<void()> TypingDone;

// New campaign / new map forms.
std::string NewTitle;
std::string NewMapName;
dungeon_type NewMapStyle = DTYPE_TOWN;

std::string Notice;
bool NeedsRefresh = false;

constexpr int RowsShown = 9;

void CreateCampaign();
void CreateMap();
void OpenQuests();
void OpenNpcs();

const std::vector<std::pair<dungeon_type, std::string_view>> &Styles()
{
	static const std::vector<std::pair<dungeon_type, std::string_view>> styles {
		{ DTYPE_TOWN, "Town" },
		{ DTYPE_FOREST, "Wilds" },
		{ DTYPE_CATHEDRAL, "Cathedral" },
		{ DTYPE_CATACOMBS, "Catacombs" },
		{ DTYPE_CAVES, "Caves" },
		{ DTYPE_HELL, "Hell" },
	};
	return styles;
}

std::string_view StyleLabel(dungeon_type style)
{
	for (const auto &[s, label] : Styles()) {
		if (s == style)
			return label;
	}
	return MapStyleName(style);
}

/** Folder-safe name from a title: "The Lost Vale" -> "the-lost-vale". */
std::string NameFromTitle(std::string_view title)
{
	std::string id;
	for (const char c : title) {
		if (std::isalnum(static_cast<unsigned char>(c)) != 0)
			id.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
		else if ((c == ' ' || c == '-' || c == '_') && !id.empty() && id.back() != '-')
			id.push_back('-');
	}
	while (!id.empty() && id.back() == '-')
		id.pop_back();
	if (id.size() > 28)
		id.resize(28);
	return id.empty() ? std::string("campaign") : id;
}

void StopTyping(bool keep)
{
	if (TypingInto == nullptr)
		return;
	if (!keep)
		*TypingInto = TypingBackup;
	TypingInto = nullptr;
	std::function<void()> done = std::move(TypingDone);
	TypingDone = nullptr;
	if (keep && done)
		done();
}

void StartTyping(std::string &text, size_t limit, bool nameOnly, std::function<void()> done = nullptr)
{
	StopTyping(true);
	TypingInto = &text;
	TypingBackup = text;
	TypingLimit = limit;
	TypingNameOnly = nameOnly;
	TypingDone = std::move(done);
}

void RefreshCampaigns()
{
	CampaignList = ListCampaigns();
}

void RefreshMaps()
{
	Maps.clear();
	for (const std::string &name : ListCustomMaps()) {
		std::expected<CustomMap, std::string> map = ReadCustomMap(name);
		if (!map.has_value())
			continue;
		std::string info;
		if (!map->spawners.empty() || !map->bosses.empty())
			info = StrCat(map->spawners.size(), " packs, ", map->bosses.size(), " bosses");
		else if (map->style == DTYPE_TOWN)
			info = StrCat(map->npcs.size(), " townspeople");
		Maps.push_back({ name, map->style, std::move(info), map->style == DTYPE_TOWN && !map->entries.empty(), map->music });
	}
	MapScroll = std::clamp(MapScroll, 0, std::max(0, static_cast<int>(Maps.size()) - RowsShown));
}

/** The song being previewed in the menu, and the menu's own track to go back to. */
bool Previewing = false;
_music_id PreviewReturnTrack = TMUSIC_INTRO;

void PreviewSong(const std::string &song)
{
	if (!Previewing) {
		PreviewReturnTrack = sgnMusicTrack < NUM_MUSIC ? sgnMusicTrack : TMUSIC_INTRO;
		Previewing = true;
	}
	const std::string path = song.empty() ? std::string {} : CampaignMusicDirectory() + song;
	music_crossfade_to(PreviewReturnTrack, path);
}

void EndPreview()
{
	if (!Previewing)
		return;
	Previewing = false;
	music_crossfade_to(PreviewReturnTrack);
}

/** Next (or previous) song for a map: its style's usual music, then each song in the campaign. */
void CycleMapMusic(const std::string &mapName, bool back)
{
	std::expected<CustomMap, std::string> map = ReadCustomMap(mapName);
	if (!map.has_value())
		return;
	std::vector<std::string> choices { "" };
	for (std::string &song : ListCampaignMusic())
		choices.push_back(std::move(song));
	if (choices.size() == 1) {
		Notice = StrCat("No songs yet: put .ogg or .wav files in ", CampaignMusicDirectory());
		return;
	}
	auto it = std::find(choices.begin(), choices.end(), map->music);
	int i = it == choices.end() ? 0 : static_cast<int>(it - choices.begin());
	i = (i + (back ? -1 : 1) + static_cast<int>(choices.size())) % static_cast<int>(choices.size());
	map->music = choices[i];
	if (std::expected<void, std::string> r = WriteCustomMap(*map); !r.has_value()) {
		Notice = r.error();
		return;
	}
	for (MapRow &row : Maps) {
		if (row.name == mapName)
			row.music = map->music;
	}
	PreviewSong(map->music);
	Notice = map->music.empty() ? StrCat(mapName, ": its usual music.") : StrCat(mapName, ": ", map->music, " (loops; playing it now)");
}

void SaveEditing()
{
	if (!Editing)
		return;
	if (std::expected<void, std::string> r = WriteCampaign(*Editing); !r.has_value())
		Notice = r.error();
}

void OpenCampaign(const std::string &id)
{
	Editing = ReadCampaign(id);
	if (!Editing) {
		Notice = "That campaign could not be read.";
		return;
	}
	SetActiveCampaign(id);
	Current = Screen::Campaign;
	Notice.clear();
	RefreshMaps();
}

void UseTownPalette(bool on);

void LaunchEditor(std::string_view map, dungeon_type style, bool play)
{
	UseTownPalette(false);
	Previewing = false; // the game starts its own music
	StopTyping(true);
	QueueMapEditor(map, style, play);
	WantsEditor = true;
}

// --- Drawing helpers ---------------------------------------------------------------------------

/** Palette index nearest a colour: the main menu's palette is not the game's. */
uint8_t Colour(int r, int g, int b)
{
	int best = 0;
	int bestDistance = 1 << 30;
	for (int i = 0; i < 256; i++) {
		const SDL_Color &c = logical_palette[i];
		const int d = (c.r - r) * (c.r - r) * 3 + (c.g - g) * (c.g - g) * 4 + (c.b - b) * (c.b - b) * 2;
		if (d < bestDistance) {
			bestDistance = d;
			best = i;
		}
	}
	return static_cast<uint8_t>(best);
}

struct Colours {
	uint8_t button, buttonHover, buttonOn, field, fieldTyping, border, borderBright, row, rowStart;
};

Colours MenuColours;

void PickColours()
{
	MenuColours = {
		Colour(34, 28, 22), Colour(64, 50, 34), Colour(96, 70, 30),
		Colour(16, 14, 12), Colour(40, 34, 26),
		Colour(120, 90, 40), Colour(210, 170, 90),
		Colour(26, 22, 18), Colour(60, 46, 24),
	};
}

Point Origin()
{
	return GetUIRectangle().position;
}

void Frame(const Surface &out, Rectangle r, uint8_t border)
{
	DrawHorizontalLine(out, r.position, r.size.width, border);
	DrawHorizontalLine(out, r.position + Displacement { 0, r.size.height - 1 }, r.size.width, border);
	DrawVerticalLine(out, r.position, r.size.height, border);
	DrawVerticalLine(out, r.position + Displacement { r.size.width - 1, 0 }, r.size.height, border);
}

/** The NPC form shows the town's palette (for a true preview); the menu's font colours then use the game's. */
bool GamePalette = false;
std::array<SDL_Color, 256> SavedPalette;

void UseTownPalette(bool on)
{
	if (on == GamePalette)
		return;
	if (on) {
		std::array<Color, 256> town;
		LoadFileInMem("levels\\towndata\\town.pal", town);
		SavedPalette = logical_palette;
		for (size_t i = 0; i < town.size(); i++)
			logical_palette[i] = town[i].toSDL();
	} else {
		logical_palette = SavedPalette;
	}
	GamePalette = on;
	UpdateSystemPalette(logical_palette);
}

UiFlags Ink(UiFlags flags)
{
	if (!GamePalette)
		return flags;
	const struct {
		UiFlags menu, game;
	} map[] = { { UiFlags::ColorUiGold, UiFlags::ColorGold }, { UiFlags::ColorUiSilver, UiFlags::ColorWhite }, { UiFlags::ColorUiSilverDark, UiFlags::ColorBlue }, { UiFlags::ColorUiGoldDark, UiFlags::ColorGold } };
	for (const auto &m : map) {
		if (HasAnyOf(flags, m.menu))
			return (flags & ~m.menu) | m.game;
	}
	return flags;
}

void Text(const Surface &out, std::string_view text, Point at, int width, UiFlags flags = UiFlags::ColorUiSilver)
{
	DrawString(out, text, { at, { width, 14 } }, { .flags = Ink(flags) | UiFlags::KerningFitSpacing });
}

void Button(const Surface &out, Rectangle r, std::string label, std::function<void(bool)> action, bool on = false)
{
	const bool hover = r.contains(MousePosition);
	FillRect(out, r.position.x, r.position.y, r.size.width, r.size.height, on ? MenuColours.buttonOn : (hover ? MenuColours.buttonHover : MenuColours.button));
	Frame(out, r, hover || on ? MenuColours.borderBright : MenuColours.border);
	DrawString(out, label, { r.position + Displacement { 0, (r.size.height - 12) / 2 }, { r.size.width, 12 } },
	    { .flags = Ink(on || hover ? UiFlags::ColorUiGold : UiFlags::ColorUiSilver) | UiFlags::AlignCenter | UiFlags::KerningFitSpacing });
	Clickables.push_back({ r, std::move(label), std::move(action) });
}

/** A text box: shows the text, click to type into it. */
void Field(const Surface &out, Rectangle r, std::string &text, std::string_view placeholder, size_t limit, bool nameOnly, std::function<void()> done = nullptr)
{
	const bool typing = TypingInto == &text;
	const bool hover = r.contains(MousePosition);
	FillRect(out, r.position.x, r.position.y, r.size.width, r.size.height, typing ? MenuColours.fieldTyping : MenuColours.field);
	Frame(out, r, typing ? MenuColours.borderBright : MenuColours.border);
	(void)hover;
	const bool blink = (SDL_GetTicks() / 400) % 2 == 0;
	std::string shown = text;
	if (typing) {
		// The end of a long text stays in view while typing.
		while (shown.size() > 1 && GetLineWidth(shown + "_") > r.size.width - 14)
			shown.erase(0, 1);
		if (blink)
			shown += "_";
	} else if (GetLineWidth(shown) > r.size.width - 12) {
		// Long text: its beginning, then "..."
		while (shown.size() > 1 && GetLineWidth(shown + "...") > r.size.width - 12)
			shown.pop_back();
		shown += "...";
	}
	if (shown.empty())
		Text(out, placeholder, r.position + Displacement { 6, (r.size.height - 12) / 2 }, r.size.width - 12, UiFlags::ColorUiSilverDark);
	else
		Text(out, shown, r.position + Displacement { 6, (r.size.height - 12) / 2 }, r.size.width - 12, UiFlags::ColorUiSilver);
	Clickables.push_back({ r, std::string(placeholder), [&text, limit, nameOnly, done](bool) { StartTyping(text, limit, nameOnly, done); } });
}

// --- Screens -----------------------------------------------------------------------------------

void DrawCampaignsScreen(const Surface &out, Point o)
{
	Text(out, "Pick a campaign to work on, or start a new one.", o + Displacement { 40, 76 }, 560, UiFlags::ColorUiSilver);
	Text(out, "Heroes choose a campaign when they are made (Start in...).", o + Displacement { 40, 92 }, 560, UiFlags::ColorUiSilverDark);
	int y = 116;
	if (CampaignList.empty())
		Text(out, "No campaigns yet.", o + Displacement { 40, y + 6 }, 400, UiFlags::ColorUiGold);
	for (size_t i = 0; i < CampaignList.size() && i < 9; i++, y += 30) {
		const Campaign &c = CampaignList[i];
		const std::string where = c.start.empty() ? std::string("starts in Tristram") : StrCat("starts in ", c.start);
		const std::string id = c.id;
		Button(out, { o + Displacement { 40, y }, { 560, 26 } }, StrCat(c.title, "      (", where, ")"), [id](bool) { OpenCampaign(id); });
	}
	Button(out, { o + Displacement { 40, 400 }, { 200, 28 } }, "New Campaign", [](bool) {
		NewTitle.clear();
		Current = Screen::NewCampaign;
		StartTyping(NewTitle, 32, false, [] { CreateCampaign(); });
	});
	Button(out, { o + Displacement { 400, 400 }, { 200, 28 } }, "Close  (`)", [](bool) { StopTyping(true); EndPreview(); Open = false; UseTownPalette(false); });
}

void CreateCampaign()
{
	TypingDone = nullptr; // typing ends here, without running this again
	StopTyping(true);
	std::string title = NewTitle;
	while (!title.empty() && title.back() == ' ')
		title.pop_back();
	if (title.empty()) {
		Notice = "Give the campaign a name first.";
		return;
	}
	std::string id = NameFromTitle(title);
	const std::string base = id;
	for (int n = 2; ReadCampaign(id); n++)
		id = StrCat(base, "-", n);
	if (std::expected<void, std::string> r = WriteCampaign({ id, title, "", "" }); !r.has_value()) {
		Notice = r.error();
		return;
	}
	RefreshCampaigns();
	OpenCampaign(id);
	Notice = "Campaign made. Now make its first map: a town is a good start.";
}

void DrawNewCampaignScreen(const Surface &out, Point o)
{
	Text(out, "New campaign", o + Displacement { 40, 80 }, 560, UiFlags::ColorUiGold);
	Text(out, "Its name, as heroes will see it in Start in...", o + Displacement { 40, 110 }, 560, UiFlags::ColorUiSilver);
	Field(out, { o + Displacement { 40, 130 }, { 560, 26 } }, NewTitle, "e.g. The Lost Vale", 32, false, [] { CreateCampaign(); });
	if (!NewTitle.empty())
		Text(out, StrCat("Saved in: campaigns/", NameFromTitle(NewTitle)), o + Displacement { 40, 162 }, 560, UiFlags::ColorUiSilverDark);
	Button(out, { o + Displacement { 40, 400 }, { 200, 28 } }, "Create", [](bool) { CreateCampaign(); });
	Button(out, { o + Displacement { 400, 400 }, { 200, 28 } }, "Back", [](bool) { StopTyping(false); Current = Screen::Campaigns; });
}

/** Tristram, then each town that has an arrival point. */
std::vector<std::string> StartChoices()
{
	std::vector<std::string> choices { "" };
	for (const MapRow &m : Maps) {
		if (m.town)
			choices.push_back(m.name);
	}
	return choices;
}

void DrawCampaignScreen(const Surface &out, Point o)
{
	if (!Editing)
		return;
	Campaign &c = *Editing;
	Text(out, "Title", o + Displacement { 40, 72 }, 100, UiFlags::ColorUiGold);
	Field(out, { o + Displacement { 170, 66 }, { 430, 22 } }, c.title, "Campaign title", 40, false, [] { SaveEditing(); RefreshCampaigns(); });
	Text(out, "About", o + Displacement { 40, 98 }, 100, UiFlags::ColorUiGold);
	Field(out, { o + Displacement { 170, 92 }, { 430, 22 } }, c.description, "One line about this campaign", 60, false, [] { SaveEditing(); });
	Text(out, "Heroes start in", o + Displacement { 40, 124 }, 128, UiFlags::ColorUiGold);
	Button(out, { o + Displacement { 170, 118 }, { 220, 22 } }, c.start.empty() ? std::string("Tristram") : c.start, [](bool right) {
		const std::vector<std::string> choices = StartChoices();
		auto it = std::find(choices.begin(), choices.end(), Editing->start);
		int i = it == choices.end() ? 0 : static_cast<int>(it - choices.begin());
		i = (i + (right ? -1 : 1) + static_cast<int>(choices.size())) % static_cast<int>(choices.size());
		Editing->start = choices[i];
		SaveEditing();
		RefreshCampaigns();
	});
	Text(out, "(click: next town)", o + Displacement { 400, 124 }, 200, UiFlags::ColorUiSilverDark);

	// Maps.
	Text(out, StrCat("Maps (", Maps.size(), ")"), o + Displacement { 40, 152 }, 200, UiFlags::ColorUiGold);
	int y = 170;
	if (Maps.empty())
		Text(out, "No maps yet. Click New Map.", o + Displacement { 40, y + 6 }, 400, UiFlags::ColorUiSilver);
	for (int i = MapScroll; i < static_cast<int>(Maps.size()) && i < MapScroll + RowsShown; i++, y += 24) {
		const MapRow &m = Maps[i];
		const bool isStart = m.name == c.start;
		FillRect(out, o.x + 40, o.y + y, 220, 22, isStart ? MenuColours.rowStart : MenuColours.row);
		Text(out, isStart ? StrCat(m.name, " (start)") : m.name, o + Displacement { 46, y + 5 }, 140, isStart ? UiFlags::ColorUiGold : UiFlags::ColorUiSilver);
		Text(out, StyleLabel(m.style), o + Displacement { 190, y + 5 }, 70, UiFlags::ColorUiGold);
		const std::string name = m.name;
		const dungeon_type style = m.style;
		Button(out, { o + Displacement { 266, y }, { 174, 22 } }, m.music.empty() ? std::string("Music: usual") : StrCat("Music: ", m.music), [name](bool right) { CycleMapMusic(name, right); });
		Button(out, { o + Displacement { 446, y }, { 74, 22 } }, "Edit", [name, style](bool) { LaunchEditor(name, style, false); });
		Button(out, { o + Displacement { 526, y }, { 74, 22 } }, "Play", [name, style](bool) { LaunchEditor(name, style, true); });
	}
	if (static_cast<int>(Maps.size()) > RowsShown)
		Text(out, "(mouse wheel: more maps)", o + Displacement { 40, 170 + RowsShown * 24 }, 300, UiFlags::ColorUiSilverDark);
	if (!Maps.empty() && Notice.empty())
		Text(out, StrCat("Songs (.ogg or .wav, looped): campaigns/", c.id, "/music"), o + Displacement { 40, 378 }, 560, UiFlags::ColorUiSilverDark);

	Button(out, { o + Displacement { 40, 400 }, { 100, 28 } }, "New Map", [](bool) {
		NewMapName.clear();
		NewMapStyle = Maps.empty() ? DTYPE_TOWN : DTYPE_FOREST;
		Current = Screen::NewMap;
		StartTyping(NewMapName, 32, true);
	});
	Button(out, { o + Displacement { 146, 400 }, { 90, 28 } }, "NPCs", [](bool) { OpenNpcs(); });
	Button(out, { o + Displacement { 242, 400 }, { 90, 28 } }, "Quests", [](bool) { OpenQuests(); });
	Button(out, { o + Displacement { 338, 400 }, { 150, 28 } }, "Portals in Tristram", [](bool) { LaunchEditor(TristramLayerName, DTYPE_TOWN, false); });
	Button(out, { o + Displacement { 494, 400 }, { 106, 28 } }, "Back", [](bool) {
		StopTyping(true);
		EndPreview();
		RefreshCampaigns();
		Current = Screen::Campaigns;
	});
}

void CreateMap()
{
	TypingDone = nullptr;
	StopTyping(true);
	if (!IsValidMapName(NewMapName)) {
		Notice = "Map names use letters, digits, - and _ (up to 32).";
		return;
	}
	if (CustomMapExists(NewMapName)) {
		Notice = "A map with that name exists: opening it.";
		std::expected<CustomMap, std::string> map = ReadCustomMap(NewMapName);
		LaunchEditor(NewMapName, map.has_value() ? map->style : NewMapStyle, false);
		return;
	}
	LaunchEditor(NewMapName, NewMapStyle, false);
}

void DrawNewMapScreen(const Surface &out, Point o)
{
	Text(out, StrCat("New map in ", Editing ? Editing->title : std::string {}), o + Displacement { 40, 80 }, 560, UiFlags::ColorUiGold);
	Text(out, "Name (letters, digits, - and _)", o + Displacement { 40, 110 }, 560, UiFlags::ColorUiSilver);
	Field(out, { o + Displacement { 40, 130 }, { 560, 26 } }, NewMapName, "e.g. riverford", 32, true);
	Text(out, "Style", o + Displacement { 40, 172 }, 560, UiFlags::ColorUiSilver);
	int i = 0;
	for (const auto &[style, label] : Styles()) {
		const dungeon_type s = style;
		Button(out, { o + Displacement { 40 + (i % 3) * 190, 192 + (i / 3) * 34 }, { 180, 28 } }, std::string(label), [s](bool) { NewMapStyle = s; }, NewMapStyle == style);
		i++;
	}
	std::string_view help;
	switch (NewMapStyle) {
	case DTYPE_TOWN: help = "A safe town: paint ground, place buildings and townspeople."; break;
	case DTYPE_FOREST: help = "The land outside a town: town tiles with monster packs and bosses."; break;
	default: help = "A dungeon: paint rooms (walls build themselves), monsters and bosses."; break;
	}
	Text(out, help, o + Displacement { 40, 266 }, 560, UiFlags::ColorUiSilverDark);
	Button(out, { o + Displacement { 40, 400 }, { 200, 28 } }, "Create and edit", [](bool) { CreateMap(); });
	Button(out, { o + Displacement { 400, 400 }, { 200, 28 } }, "Back", [](bool) { StopTyping(false); Current = Screen::Campaign; });
}

// --- Quests ------------------------------------------------------------------------------------

std::vector<CampaignQuest> QuestList;
int QuestIndex = -1;
int QuestScroll = 0;

/** What a quest can point at in this campaign. */
struct QuestChoices {
	std::vector<std::string> people;  // "<town>:<TOWN_TYPE>"
	std::vector<std::string> bosses;  // "<map>:<boss>"
	std::vector<std::string> chests;  // "<map>:<x>,<y>"
	std::vector<std::string> uniques; // unique item names
};
QuestChoices Choices;

void GatherQuestChoices()
{
	Choices = {};
	for (const std::string &name : ListCustomMaps()) {
		std::expected<CustomMap, std::string> map = ReadCustomMap(name);
		if (!map.has_value())
			continue;
		for (const MapNpc &npc : map->npcs)
			Choices.people.push_back(StrCat(name, ":", npc.type));
		for (const MapBoss &boss : map->bosses)
			Choices.bosses.push_back(StrCat(name, ":", boss.name));
		for (const Point chest : MapChests(*map))
			Choices.chests.push_back(StrCat(name, ":", chest.x, ",", chest.y));
	}
	for (const PlaceableTowner &t : PlaceableTowners())
		Choices.people.push_back(StrCat("tristram:", t.type));
	for (const UniqueItem &item : UniqueItems)
		Choices.uniques.push_back(item.UIName);
}

void SaveQuests()
{
	if (std::expected<void, std::string> r = WriteCampaignQuests(QuestList); !r.has_value())
		Notice = r.error();
}

void OpenQuests()
{
	StopTyping(true);
	QuestList = ReadCampaignQuests();
	GatherQuestChoices();
	QuestScroll = 0;
	Current = Screen::Quests;
}

/** Next (or previous) entry of a list after `current` ("" counts as the first when allowed). */
std::string Cycle(const std::vector<std::string> &list, const std::string &current, bool back, bool allowNone = false)
{
	std::vector<std::string> all;
	if (allowNone)
		all.emplace_back();
	all.insert(all.end(), list.begin(), list.end());
	if (all.empty())
		return current;
	auto it = std::find(all.begin(), all.end(), current);
	int i = it == all.end() ? (back ? 0 : -1) : static_cast<int>(it - all.begin());
	i = (i + (back ? -1 : 1) + static_cast<int>(all.size())) % static_cast<int>(all.size());
	return all[i];
}

void DrawQuestsScreen(const Surface &out, Point o)
{
	Text(out, StrCat("Quests (", QuestList.size(), ")"), o + Displacement { 40, 66 }, 300, UiFlags::ColorUiGold);
	Text(out, "Townspeople give these. Place people, bosses and chests in your maps first.", o + Displacement { 40, 84 }, 560, UiFlags::ColorUiSilverDark);
	int y = 106;
	if (QuestList.empty())
		Text(out, "No quests yet. Click New Quest.", o + Displacement { 40, y + 6 }, 400, UiFlags::ColorUiSilver);
	for (int i = QuestScroll; i < static_cast<int>(QuestList.size()) && i < QuestScroll + 9; i++, y += 31) {
		const CampaignQuest &q = QuestList[i];
		FillRect(out, o.x + 40, o.y + y, 400, 29, MenuColours.row);
		Text(out, q.title.empty() ? std::string("(untitled)") : q.title, o + Displacement { 46, y + 2 }, 390, UiFlags::ColorUiGold);
		Text(out, StrCat(QuestGoalLabel(q), "  -  ", QuestRewardLabel(q)), o + Displacement { 46, y + 15 }, 390, UiFlags::ColorUiSilverDark);
		const int index = i;
		Button(out, { o + Displacement { 446, y }, { 74, 29 } }, "Edit", [index](bool) { QuestIndex = index; Current = Screen::Quest; });
		Button(out, { o + Displacement { 526, y }, { 74, 29 } }, "Delete", [index](bool right) {
			if (!right) {
				Notice = "Right-click Delete to remove the quest.";
				return;
			}
			QuestList.erase(QuestList.begin() + index);
			SaveQuests();
		});
	}
	Button(out, { o + Displacement { 40, 400 }, { 200, 28 } }, "New Quest", [](bool) {
		CampaignQuest q;
		q.id = NewQuestId(QuestList);
		q.title = "New quest";
		if (!Choices.people.empty())
			q.giver = Choices.people.front();
		q.target = Choices.bosses.empty() ? std::string {} : Choices.bosses.front();
		QuestList.push_back(std::move(q));
		SaveQuests();
		QuestIndex = static_cast<int>(QuestList.size()) - 1;
		Current = Screen::Quest;
		StartTyping(QuestList.back().title, 40, false, [] { SaveQuests(); });
	});
	Button(out, { o + Displacement { 400, 400 }, { 200, 28 } }, "Back", [](bool) { StopTyping(true); Current = Screen::Campaign; });
}

std::string TargetLabel(const CampaignQuest &q)
{
	if (q.target.empty())
		return "(choose)";
	if (q.goal == QuestGoal::Talk)
		return QuestPersonLabel(q.target);
	const size_t colon = q.target.find(':');
	if (colon == std::string::npos)
		return q.target;
	return q.goal == QuestGoal::Find ? StrCat("Chest at ", q.target.substr(colon + 1), " in ", q.target.substr(0, colon))
	                                 : StrCat(q.target.substr(colon + 1), " in ", q.target.substr(0, colon));
}

void DrawQuestScreen(const Surface &out, Point o)
{
	if (QuestIndex < 0 || QuestIndex >= static_cast<int>(QuestList.size())) {
		Current = Screen::Quests;
		return;
	}
	CampaignQuest &q = QuestList[QuestIndex];
	const auto save = [] { SaveQuests(); };
	int y = 62;
	const auto label = [&](std::string_view text) { Text(out, text, o + Displacement { 40, y + 5 }, 124, UiFlags::ColorUiGold); };
	const Rectangle wide { o + Displacement { 170, y }, { 430, 22 } };
	(void)wide;

	label("Title");
	Field(out, { o + Displacement { 170, y }, { 430, 22 } }, q.title, "Quest title", 40, false, save);
	y += 26;
	label("Given by");
	Button(out, { o + Displacement { 170, y }, { 430, 22 } }, QuestPersonLabel(q.giver), [](bool right) {
		CampaignQuest &q = QuestList[QuestIndex];
		q.giver = Cycle(Choices.people, q.giver, right);
		SaveQuests();
	});
	y += 26;
	label("The task");
	const struct {
		QuestGoal goal;
		std::string_view text;
	} goals[] = { { QuestGoal::Kill, "Kill a boss" }, { QuestGoal::Find, "Find in a chest" }, { QuestGoal::Talk, "Talk to someone" } };
	for (int i = 0; i < 3; i++) {
		const QuestGoal goal = goals[i].goal;
		Button(out, { o + Displacement { 170 + i * 146, y }, { 138, 22 } }, std::string(goals[i].text), [goal](bool) {
			CampaignQuest &q = QuestList[QuestIndex];
			if (q.goal == goal)
				return;
			q.goal = goal;
			const std::vector<std::string> &list = goal == QuestGoal::Kill ? Choices.bosses : (goal == QuestGoal::Find ? Choices.chests : Choices.people);
			q.target = list.empty() ? std::string {} : list.front();
			if (goal == QuestGoal::Talk && q.target == q.giver && list.size() > 1)
				q.target = list[1];
			if (goal != QuestGoal::Kill && q.item.empty())
				q.item = goal == QuestGoal::Find ? "Lost Relic" : "Sealed Letter";
			SaveQuests();
		}, q.goal == goal);
	}
	y += 26;
	label(q.goal == QuestGoal::Kill ? "Boss" : (q.goal == QuestGoal::Find ? "Chest" : "Talk to"));
	const std::vector<std::string> &targets = q.goal == QuestGoal::Kill ? Choices.bosses : (q.goal == QuestGoal::Find ? Choices.chests : Choices.people);
	if (targets.empty()) {
		Text(out, q.goal == QuestGoal::Kill ? "No bosses in your maps yet (map editor: Boss tool)." : (q.goal == QuestGoal::Find ? "No chests in your maps yet (map editor: Chest tool)." : "No townspeople yet."), o + Displacement { 170, y + 5 }, 430, UiFlags::ColorUiSilverDark);
	} else {
		Button(out, { o + Displacement { 170, y }, { 430, 22 } }, TargetLabel(q), [](bool right) {
			CampaignQuest &q = QuestList[QuestIndex];
			const std::vector<std::string> &list = q.goal == QuestGoal::Kill ? Choices.bosses : (q.goal == QuestGoal::Find ? Choices.chests : Choices.people);
			q.target = Cycle(list, q.target, right);
			SaveQuests();
		});
	}
	y += 26;
	if (q.goal != QuestGoal::Kill) {
		label(q.goal == QuestGoal::Find ? "Item in chest" : "Item handed over");
		Field(out, { o + Displacement { 170, y }, { 430, 22 } }, q.item, "e.g. Ancient Tome", 32, false, save);
		y += 26;
	}
	label("Reward");
	const std::string_view rewardNames[] = { "Gold", "Experience", "Magic item", "Unique item" };
	Button(out, { o + Displacement { 170, y }, { 138, 22 } }, std::string(rewardNames[static_cast<int>(q.reward)]), [](bool right) {
		CampaignQuest &q = QuestList[QuestIndex];
		q.reward = static_cast<QuestReward>((static_cast<int>(q.reward) + (right ? 3 : 1)) % 4);
		if (q.reward == QuestReward::UniqueItem && q.uniqueItem.empty() && !Choices.uniques.empty())
			q.uniqueItem = Choices.uniques.front();
		SaveQuests();
	});
	if (q.reward == QuestReward::Gold || q.reward == QuestReward::Experience) {
		static std::string amountText;
		static int amountFor = -1;
		if (TypingInto != &amountText || amountFor != QuestIndex) {
			amountText = StrCat(q.amount);
			amountFor = QuestIndex;
		}
		Field(out, { o + Displacement { 316, y }, { 120, 22 } }, amountText, "amount", 8, true, [] {
			const ParseIntResult<int> v = ParseInt<int>(amountText);
			if (v.has_value() && *v > 0)
				QuestList[QuestIndex].amount = *v;
			SaveQuests();
		});
	} else if (q.reward == QuestReward::UniqueItem) {
		Button(out, { o + Displacement { 316, y }, { 284, 22 } }, q.uniqueItem.empty() ? std::string("(choose)") : q.uniqueItem, [](bool right) {
			CampaignQuest &q = QuestList[QuestIndex];
			q.uniqueItem = Cycle(Choices.uniques, q.uniqueItem, right);
			SaveQuests();
		});
	} else {
		Text(out, "a random good item for the hero's level", o + Displacement { 316, y + 5 }, 284, UiFlags::ColorUiSilverDark);
	}
	y += 26;
	label("Offered after");
	std::string afterLabel = "Any time";
	for (const CampaignQuest &other : QuestList) {
		if (other.id == q.after && !q.after.empty())
			afterLabel = StrCat("\"", other.title, "\" is done");
	}
	Button(out, { o + Displacement { 170, y }, { 430, 22 } }, afterLabel, [](bool right) {
		CampaignQuest &q = QuestList[QuestIndex];
		std::vector<std::string> ids;
		for (const CampaignQuest &other : QuestList) {
			if (other.id != q.id)
				ids.push_back(other.id);
		}
		q.after = Cycle(ids, q.after, right, /*allowNone=*/true);
		SaveQuests();
	});
	y += 30;
	label("They ask");
	Field(out, { o + Displacement { 170, y }, { 430, 22 } }, q.offer, "What the giver says (blank: a plain request)", 200, false, save);
	y += 26;
	label("While waiting");
	Field(out, { o + Displacement { 170, y }, { 430, 22 } }, q.progress, "What they say until it is done (optional)", 200, false, save);
	y += 26;
	label("When done");
	Field(out, { o + Displacement { 170, y }, { 430, 22 } }, q.thanks, "What they say with the reward (optional)", 200, false, save);
	y += 26;
	if (q.goal == QuestGoal::Talk) {
		label("They say");
		Field(out, { o + Displacement { 170, y }, { 430, 22 } }, q.targetText, "What the person you were sent to says (optional)", 200, false, save);
	}
	Button(out, { o + Displacement { 400, 400 }, { 200, 28 } }, "Done", [](bool) { StopTyping(true); Current = Screen::Quests; });
}

// --- NPCs --------------------------------------------------------------------------------------

std::vector<CustomNpc> NpcList;
int NpcIndex = -1;
std::vector<std::string> VoiceFiles;

void SaveNpcs()
{
	if (std::expected<void, std::string> r = WriteCustomNpcs(NpcList); !r.has_value())
		Notice = r.error();
}

void OpenNpcs()
{
	StopTyping(true);
	NpcList = ReadCustomNpcs();
	VoiceFiles = ListCampaignVoices();
	Current = Screen::Npcs;
}

std::string_view LookLabel(std::string_view key)
{
	const NpcLook *look = FindNpcLook(key);
	return look != nullptr ? look->label : key;
}

void DrawNpcsScreen(const Surface &out, Point o)
{
	Text(out, StrCat("Townspeople (", NpcList.size(), ")"), o + Displacement { 40, 66 }, 300, UiFlags::ColorUiGold);
	Text(out, "Place them in a town with the map editor's People tool.", o + Displacement { 40, 84 }, 560, UiFlags::ColorUiSilverDark);
	int y = 106;
	if (NpcList.empty())
		Text(out, "No townspeople yet. Click New NPC.", o + Displacement { 40, y + 6 }, 400, UiFlags::ColorUiSilver);
	for (int i = 0; i < static_cast<int>(NpcList.size()) && i < 9; i++, y += 31) {
		const CustomNpc &npc = NpcList[i];
		FillRect(out, o.x + 40, o.y + y, 400, 29, MenuColours.row);
		Text(out, npc.name, o + Displacement { 46, y + 2 }, 390, UiFlags::ColorUiGold);
		Text(out, StrCat("Looks like: ", LookLabel(npc.look), "  -  ", npc.shop == NpcShop::None ? std::string("no shop") : std::string(NpcShopLabel(npc.shop)), "  -  ", npc.lines.size(), " lines"),
		    o + Displacement { 46, y + 15 }, 390, UiFlags::ColorUiSilverDark);
		const int index = i;
		Button(out, { o + Displacement { 446, y }, { 74, 29 } }, "Edit", [index](bool) { NpcIndex = index; Current = Screen::Npc; });
		Button(out, { o + Displacement { 526, y }, { 74, 29 } }, "Delete", [index](bool right) {
			if (!right) {
				Notice = "Right-click Delete to remove them (and from any town they stand in).";
				return;
			}
			NpcList.erase(NpcList.begin() + index);
			SaveNpcs();
		});
	}
	Button(out, { o + Displacement { 40, 400 }, { 200, 28 } }, "New NPC", [](bool) {
		CustomNpc npc;
		npc.id = NewNpcId(NpcList);
		npc.name = "New townsperson";
		if (!NpcLooks().empty())
			npc.look = std::string(NpcLooks().front().key);
		npc.lines.push_back({ "Well met, traveller.", "" });
		NpcList.push_back(std::move(npc));
		SaveNpcs();
		NpcIndex = static_cast<int>(NpcList.size()) - 1;
		Current = Screen::Npc;
		StartTyping(NpcList.back().name, 32, false, [] { SaveNpcs(); });
	});
	Button(out, { o + Displacement { 400, 400 }, { 200, 28 } }, "Back", [](bool) { StopTyping(true); Current = Screen::Campaign; });
}

/** Sprite of the look being previewed. */
std::string PreviewLook;
std::optional<OwnedClxSpriteList> PreviewSprites;

void DrawNpcPreview(const Surface &out, const CustomNpc &npc, Rectangle box)
{
	FillRect(out, box.position.x, box.position.y, box.size.width, box.size.height, Colour(18, 16, 14));
	Frame(out, box, MenuColours.border);
	const NpcLook *look = FindNpcLook(npc.look);
	if (look == nullptr)
		return;
	if (PreviewLook != look->path || !PreviewSprites) {
		PreviewLook = std::string(look->path);
		PreviewSprites = LoadCel(PreviewLook.c_str(), 96);
	}
	const std::array<uint8_t, 256> trn = BuildNpcTrn(npc, logical_palette);
	const int frame = static_cast<int>((SDL_GetTicks() / static_cast<uint32_t>(std::max(1, look->delay) * 50)) % static_cast<uint32_t>(std::max(1, look->frames)));
	const ClxSprite sprite = (*PreviewSprites)[std::min<int>(frame, static_cast<int>(PreviewSprites->numSprites()) - 1)];
	// Drawn at twice the size: onto a scratch surface, then doubled into the box.
	OwnedSurface scratch(sprite.width(), sprite.height());
	std::memset(scratch.at(0, 0), 0, static_cast<size_t>(scratch.pitch()) * sprite.height());
	RenderClxSpriteWithTRN(scratch, sprite, { 0, 0 }, trn.data());
	const int scale = 2;
	const int w = std::min(sprite.width() * scale, box.size.width - 4);
	const int h = std::min(sprite.height() * scale, box.size.height - 4);
	const Point at { box.position.x + (box.size.width - w) / 2, box.position.y + (box.size.height - h) / 2 };
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const uint8_t c = *scratch.at(x / scale, y / scale);
			if (c != 0)
				out.SetPixel(at + Displacement { x, y }, c);
		}
	}
}

/** "-" and "+" around a value: click the value for +, right-click for -. */
void Stepper(const Surface &out, Rectangle r, std::string label, std::function<void(int)> change)
{
	Button(out, r, std::move(label), [change](bool right) { change(right ? -1 : 1); });
}

void DrawNpcScreen(const Surface &out, Point o)
{
	if (NpcIndex < 0 || NpcIndex >= static_cast<int>(NpcList.size())) {
		Current = Screen::Npcs;
		return;
	}
	CustomNpc &npc = NpcList[NpcIndex];
	const auto save = [] { SaveNpcs(); };
	int y = 62;
	const auto label = [&](std::string_view text) { Text(out, text, o + Displacement { 40, y + 5 }, 90, UiFlags::ColorUiGold); };

	label("Name");
	Field(out, { o + Displacement { 130, y }, { 290, 22 } }, npc.name, "Their name", 32, false, save);
	y += 26;
	label("Looks like");
	Button(out, { o + Displacement { 130, y }, { 290, 22 } }, std::string(LookLabel(npc.look)), [](bool right) {
		CustomNpc &npc = NpcList[NpcIndex];
		std::vector<std::string> keys;
		for (const NpcLook &look : NpcLooks())
			keys.emplace_back(look.key);
		npc.look = Cycle(keys, npc.look, right);
		SaveNpcs();
	});
	y += 26;
	for (int slot = 0; slot < 2; slot++) {
		label(slot == 0 ? "Colours" : "and");
		const NpcRecolor &r = npc.recolor[slot];
		Button(out, { o + Displacement { 130, y }, { 96, 22 } }, std::string(NpcColorSources()[r.source]), [slot](bool right) {
			NpcRecolor &r = NpcList[NpcIndex].recolor[slot];
			const int n = static_cast<int>(NpcColorSources().size());
			r.source = (r.source + (right ? n - 1 : 1)) % n;
			SaveNpcs();
		});
		if (r.source != 0) {
			Stepper(out, { o + Displacement { 230, y }, { 64, 22 } }, r.source == 1 ? StrCat("turn ", r.hue) : StrCat("to ", r.hue), [slot](int d) {
				NpcRecolor &r = NpcList[NpcIndex].recolor[slot];
				r.hue = (r.hue + d * 20 + 360) % 360;
				SaveNpcs();
			});
			Stepper(out, { o + Displacement { 298, y }, { 60, 22 } }, StrCat("vivid ", r.saturation), [slot](int d) {
				NpcRecolor &r = NpcList[NpcIndex].recolor[slot];
				r.saturation = std::clamp(r.saturation + d * 20, 0, 200);
				SaveNpcs();
			});
			Stepper(out, { o + Displacement { 362, y }, { 58, 22 } }, StrCat("light ", r.brightness), [slot](int d) {
				NpcRecolor &r = NpcList[NpcIndex].recolor[slot];
				r.brightness = std::clamp(r.brightness + d * 10, 30, 200);
				SaveNpcs();
			});
		}
		y += 26;
	}
	label("Shop");
	Button(out, { o + Displacement { 130, y }, { 290, 22 } }, std::string(NpcShopLabel(npc.shop)), [](bool right) {
		CustomNpc &npc = NpcList[NpcIndex];
		npc.shop = static_cast<NpcShop>((static_cast<int>(npc.shop) + (right ? NpcShopCount - 1 : 1)) % NpcShopCount);
		SaveNpcs();
	});
	y += 30;

	// What they say: each line can have a recording.
	Text(out, "What they say (one line each time; each can have a recording)", o + Displacement { 40, y }, 380, UiFlags::ColorUiGold);
	y += 18;
	for (int i = 0; i < static_cast<int>(npc.lines.size()) && i < 5; i++, y += 25) {
		NpcLine &line = npc.lines[i];
		Field(out, { o + Displacement { 40, y }, { 196, 22 } }, line.text, "A line they say", 160, false, save);
		const int index = i;
		Button(out, { o + Displacement { 240, y }, { 92, 22 } }, line.voice.empty() ? std::string("No voice") : line.voice, [index](bool right) {
			NpcLine &line = NpcList[NpcIndex].lines[index];
			if (VoiceFiles.empty()) {
				Notice = StrCat("No recordings yet: put .wav or .ogg files in ", CampaignVoiceDirectory());
				return;
			}
			line.voice = Cycle(VoiceFiles, line.voice, right, /*allowNone=*/true);
			SaveNpcs();
			PlayNpcVoice(line.voice);
		});
		Button(out, { o + Displacement { 336, y }, { 42, 22 } }, "Play", [index](bool) { PlayNpcVoice(NpcList[NpcIndex].lines[index].voice); });
		Button(out, { o + Displacement { 382, y }, { 38, 22 } }, "Cut", [index](bool) {
			StopTyping(true);
			NpcList[NpcIndex].lines.erase(NpcList[NpcIndex].lines.begin() + index);
			SaveNpcs();
		});
	}
	if (npc.lines.size() < 5) {
		Button(out, { o + Displacement { 40, y }, { 140, 22 } }, "Add a line", [](bool) {
			StopTyping(true);
			NpcList[NpcIndex].lines.push_back({ "", "" });
			SaveNpcs();
			StartTyping(NpcList[NpcIndex].lines.back().text, 160, false, [] { SaveNpcs(); });
		});
	}

	DrawNpcPreview(out, npc, { o + Displacement { 430, 62 }, { 170, 300 } });
	Text(out, "Preview", o + Displacement { 430, 366 }, 170, UiFlags::ColorUiSilverDark | UiFlags::AlignCenter);
	Button(out, { o + Displacement { 400, 400 }, { 200, 28 } }, "Done", [](bool) { StopTyping(true); Current = Screen::Npcs; });
}

void Activate(const Clickable &c, bool right)
{
	Notice.clear();
	const std::function<void(bool)> action = c.action; // the list is rebuilt by the action's screen change
	action(right);
}

} // namespace

bool MenuConsoleHandleEvent(SDL_Event &event)
{
	const bool keyDown = event.type == SDL_EVENT_KEY_DOWN;
	if (keyDown && SDLC_EventScancode(event) == SDL_SCANCODE_GRAVE) {
		StopTyping(true);
		Open = !Open;
		if (!Open) {
			EndPreview();
			UseTownPalette(false);
		}
		if (Open) {
			RefreshCampaigns();
			if (Current == Screen::Campaign || Current == Screen::NewMap) {
				if (Editing)
					OpenCampaign(Editing->id);
				else
					Current = Screen::Campaigns;
			}
		}
		return true;
	}
	if (!Open)
		return false;
	switch (event.type) {
	case SDL_EVENT_MOUSE_MOTION:
		return false; // the menu keeps the mouse position up to date
	case SDL_EVENT_MOUSE_BUTTON_DOWN: {
		const bool right = event.button.button == SDL_BUTTON_RIGHT;
		const std::vector<Clickable> clickables = Clickables;
		bool onField = false;
		for (const Clickable &c : clickables) {
			if (c.rect.contains(MousePosition)) {
				onField = c.label.empty() ? false : true;
				Activate(c, right);
				return true;
			}
		}
		if (!onField)
			StopTyping(true);
		return true;
	}
	case SDL_EVENT_MOUSE_WHEEL:
		if (Current == Screen::Campaign)
			MapScroll = std::clamp(MapScroll + (SDLC_EventWheelIntY(event) > 0 ? -1 : 1), 0, std::max(0, static_cast<int>(Maps.size()) - RowsShown));
		return true;
	case SDL_EVENT_KEY_DOWN:
		break;
	default:
		return event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_TEXT_INPUT;
	}

	const SDL_Scancode sc = SDLC_EventScancode(event);
	const SDL_Keycode key = SDLC_EventKey(event);
	if (TypingInto != nullptr) {
		if (sc == SDL_SCANCODE_ESCAPE) {
			StopTyping(false);
		} else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
			StopTyping(true);
		} else if (sc == SDL_SCANCODE_BACKSPACE) {
			if (!TypingInto->empty())
				TypingInto->pop_back();
		} else if (TypingInto->size() < TypingLimit) {
			const bool shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
			char c = 0;
			if (key >= 'a' && key <= 'z')
				c = static_cast<char>(shift && !TypingNameOnly ? key - 'a' + 'A' : key);
			else if (key >= '0' && key <= '9')
				c = shift && key == '1' && !TypingNameOnly ? '!' : static_cast<char>(key);
			else if (key == '-')
				c = shift ? '_' : '-';
			else if (key == ' ')
				c = TypingNameOnly ? '-' : ' ';
			else if (!TypingNameOnly && (key == '.' || key == ',' || key == '\''))
				c = static_cast<char>(key);
			else if (!TypingNameOnly && key == ';')
				c = shift ? ':' : ';';
			else if (!TypingNameOnly && key == '/')
				c = shift ? '?' : '/';
			if (c != 0)
				TypingInto->push_back(c);
		}
		return true;
	}
	if (sc == SDL_SCANCODE_ESCAPE) {
		switch (Current) {
		case Screen::Campaigns:
			Open = false;
			UseTownPalette(false);
			break;
		case Screen::NewCampaign: Current = Screen::Campaigns; break;
		case Screen::Campaign:
			EndPreview();
			RefreshCampaigns();
			Current = Screen::Campaigns;
			break;
		case Screen::NewMap: Current = Screen::Campaign; break;
		case Screen::Quests: Current = Screen::Campaign; break;
		case Screen::Quest: Current = Screen::Quests; break;
		case Screen::Npcs: Current = Screen::Campaign; break;
		case Screen::Npc: Current = Screen::Npcs; break;
		}
	} else if ((sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) && Current == Screen::NewMap) {
		CreateMap();
	}
	return true;
}

void DrawMenuConsole()
{
	if (!Open)
		return;
	UseTownPalette(Current == Screen::Npc);
	if (NeedsRefresh) {
		// Back from the editor: the campaign may have new maps.
		NeedsRefresh = false;
		RefreshCampaigns();
		if (Editing && Current != Screen::Campaigns) {
			const std::string notice = Notice;
			OpenCampaign(Editing->id);
			Notice = notice;
		}
	}
	Clickables.clear();
	PickColours();
	const Surface out(DiabloUiSurface());
	FillRect(out, 0, 0, out.w(), out.h(), Colour(0, 0, 0));
	const Point o = Origin();
	const Rectangle panel { o + Displacement { 20, 20 }, { 600, 440 } };
	DrawHalfTransparentRectTo(out, panel.position.x, panel.position.y, panel.size.width, panel.size.height);
	Frame(out, panel, MenuColours.border);
	DrawString(out, "Campaign Editor", { o + Displacement { 0, 30 }, { 640, 30 } }, { .flags = UiFlags::FontSize24 | Ink(UiFlags::ColorUiGold) | UiFlags::AlignCenter });
	switch (Current) {
	case Screen::Campaigns: DrawCampaignsScreen(out, o); break;
	case Screen::NewCampaign: DrawNewCampaignScreen(out, o); break;
	case Screen::Campaign: DrawCampaignScreen(out, o); break;
	case Screen::NewMap: DrawNewMapScreen(out, o); break;
	case Screen::Quests: DrawQuestsScreen(out, o); break;
	case Screen::Quest: DrawQuestScreen(out, o); break;
	case Screen::Npcs: DrawNpcsScreen(out, o); break;
	case Screen::Npc: DrawNpcScreen(out, o); break;
	}
	if (!Notice.empty())
		Text(out, Notice, o + Displacement { 40, 378 }, 560, UiFlags::ColorUiGold);
	Text(out, "`: close    Esc: back    Enter: done typing", o + Displacement { 40, 438 }, 560, UiFlags::ColorUiSilverDark | UiFlags::AlignCenter);
#ifdef _DEBUG
	// Test hook: DEVILUTIONX_DEV_MENUSHOTAFTER = BMP of the menu once back from the editor.
	static int backFrames = -1;
	if (backFrames < 0 && WantsEditor)
		backFrames = 0;
	if (const char *shot = std::getenv("DEVILUTIONX_DEV_MENUSHOTAFTER"); shot != nullptr && backFrames >= 0 && !WantsEditor && ++backFrames == 60)
		SDL_SaveBMP(DiabloUiSurface(), shot);
#endif
}

#ifdef _DEBUG
void MenuConsoleDevScript()
{
	// Test hook: DEVILUTIONX_DEV_MENUTYPE = keys, one per frame: '`' toggles the menu, '|' is Enter,
	// '[label]' clicks the button or field with that label. DEVILUTIONX_DEV_MENUSHOT = BMP of the menu once done.
	static size_t pos = 0;
	static int frames = 0;
	const char *text = std::getenv("DEVILUTIONX_DEV_MENUTYPE");
	if (text == nullptr)
		return;
	if (++frames < 30 || frames % 3 != 0)
		return;
	const std::string_view script = text;
	if (pos < script.size()) {
		if (script[pos] == '[') {
			const size_t end = script.find(']', pos);
			const std::string_view label = script.substr(pos + 1, end - pos - 1);
			pos = end == std::string_view::npos ? script.size() : end + 1;
			for (const Clickable &c : std::vector<Clickable>(Clickables)) {
				if (c.label.starts_with(label)) {
					Activate(c, false);
					break;
				}
			}
			return;
		}
		const char c = script[pos++];
		SDL_Event event {};
		event.type = SDL_EVENT_KEY_DOWN;
		SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
		SDL_Keycode key = static_cast<SDL_Keycode>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
		if (c == '`') sc = SDL_SCANCODE_GRAVE;
		else if (c == '|') { sc = SDL_SCANCODE_RETURN; key = SDLK_RETURN; }
		else if (c >= 'a' && c <= 'z') sc = static_cast<SDL_Scancode>(SDL_SCANCODE_A + (c - 'a'));
		else if (c >= 'A' && c <= 'Z') sc = static_cast<SDL_Scancode>(SDL_SCANCODE_A + (c - 'A'));
		else if (c >= '0' && c <= '9') sc = c == '0' ? SDL_SCANCODE_0 : static_cast<SDL_Scancode>(SDL_SCANCODE_1 + (c - '1'));
		else if (c == ' ') sc = SDL_SCANCODE_SPACE;
		else if (c == '-') sc = SDL_SCANCODE_MINUS;
#ifdef USE_SDL3
		event.key.scancode = sc;
		event.key.key = key;
#else
		event.key.keysym.scancode = sc;
		event.key.keysym.sym = key;
#endif
		MenuConsoleHandleEvent(event);
		return;
	}
	if (const char *shot = std::getenv("DEVILUTIONX_DEV_MENUSHOT"); shot != nullptr && pos == script.size()) {
		pos++;
		SDL_SaveBMP(DiabloUiSurface(), shot);
	}
}
#endif

bool MenuConsoleWantsMapEditor()
{
	return WantsEditor;
}

void MenuConsoleClearRequest()
{
	WantsEditor = false;
	Open = true; // back to the campaign once the editor closes
	NeedsRefresh = true;
}

} // namespace devilution
