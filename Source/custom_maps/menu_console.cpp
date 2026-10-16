#include "custom_maps/menu_console.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "DiabloUI/diabloui.h"
#include "custom_maps/campaign.hpp"
#include "custom_maps/custom_map.hpp"
#include "custom_maps/map_editor.hpp"
#include "engine/palette.h"
#include "engine/rectangle.hpp"
#include "engine/render/primitive_render.hpp"
#include "engine/render/text_render.hpp"
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
};

struct MapRow {
	std::string name;
	dungeon_type style;
	std::string info;
	bool town;
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
		Maps.push_back({ name, map->style, std::move(info), map->style == DTYPE_TOWN && !map->entries.empty() });
	}
	MapScroll = std::clamp(MapScroll, 0, std::max(0, static_cast<int>(Maps.size()) - RowsShown));
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

void LaunchEditor(std::string_view map, dungeon_type style, bool play)
{
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

void Text(const Surface &out, std::string_view text, Point at, int width, UiFlags flags = UiFlags::ColorUiSilver)
{
	DrawString(out, text, { at, { width, 14 } }, { .flags = flags | UiFlags::KerningFitSpacing });
}

void Button(const Surface &out, Rectangle r, std::string label, std::function<void(bool)> action, bool on = false)
{
	const bool hover = r.contains(MousePosition);
	FillRect(out, r.position.x, r.position.y, r.size.width, r.size.height, on ? MenuColours.buttonOn : (hover ? MenuColours.buttonHover : MenuColours.button));
	Frame(out, r, hover || on ? MenuColours.borderBright : MenuColours.border);
	DrawString(out, label, { r.position + Displacement { 0, (r.size.height - 12) / 2 }, { r.size.width, 12 } },
	    { .flags = (on || hover ? UiFlags::ColorUiGold : UiFlags::ColorUiSilver) | UiFlags::AlignCenter | UiFlags::KerningFitSpacing });
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
	if (typing && blink)
		shown += "_";
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
	Button(out, { o + Displacement { 400, 400 }, { 200, 28 } }, "Close  (`)", [](bool) { StopTyping(true); Open = false; });
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
		FillRect(out, o.x + 40, o.y + y, 400, 22, isStart ? MenuColours.rowStart : MenuColours.row);
		Text(out, m.name, o + Displacement { 46, y + 5 }, 170, isStart ? UiFlags::ColorUiGold : UiFlags::ColorUiSilver);
		Text(out, StyleLabel(m.style), o + Displacement { 220, y + 5 }, 80, UiFlags::ColorUiGold);
		Text(out, isStart ? std::string("start") : m.info, o + Displacement { 300, y + 5 }, 140, UiFlags::ColorUiSilverDark);
		const std::string name = m.name;
		const dungeon_type style = m.style;
		Button(out, { o + Displacement { 446, y }, { 74, 22 } }, "Edit", [name, style](bool) { LaunchEditor(name, style, false); });
		Button(out, { o + Displacement { 526, y }, { 74, 22 } }, "Play", [name, style](bool) { LaunchEditor(name, style, true); });
	}
	if (static_cast<int>(Maps.size()) > RowsShown)
		Text(out, "(mouse wheel: more maps)", o + Displacement { 40, 170 + RowsShown * 24 }, 300, UiFlags::ColorUiSilverDark);

	Button(out, { o + Displacement { 40, 400 }, { 160, 28 } }, "New Map", [](bool) {
		NewMapName.clear();
		NewMapStyle = Maps.empty() ? DTYPE_TOWN : DTYPE_FOREST;
		Current = Screen::NewMap;
		StartTyping(NewMapName, 32, true);
	});
	Button(out, { o + Displacement { 210, 400 }, { 200, 28 } }, "Portals in Tristram", [](bool) { LaunchEditor(TristramLayerName, DTYPE_TOWN, false); });
	Button(out, { o + Displacement { 420, 400 }, { 180, 28 } }, "Back", [](bool) {
		StopTyping(true);
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
		case Screen::Campaigns: Open = false; break;
		case Screen::NewCampaign: Current = Screen::Campaigns; break;
		case Screen::Campaign:
			RefreshCampaigns();
			Current = Screen::Campaigns;
			break;
		case Screen::NewMap: Current = Screen::Campaign; break;
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
	DrawString(out, "Campaign Editor", { o + Displacement { 0, 30 }, { 640, 30 } }, { .flags = UiFlags::FontSize24 | UiFlags::ColorUiGold | UiFlags::AlignCenter });
	switch (Current) {
	case Screen::Campaigns: DrawCampaignsScreen(out, o); break;
	case Screen::NewCampaign: DrawNewCampaignScreen(out, o); break;
	case Screen::Campaign: DrawCampaignScreen(out, o); break;
	case Screen::NewMap: DrawNewMapScreen(out, o); break;
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
