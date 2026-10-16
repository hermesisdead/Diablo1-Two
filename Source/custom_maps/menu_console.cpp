#include "custom_maps/menu_console.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "DiabloUI/diabloui.h"
#include "custom_maps/custom_map.hpp"
#include "custom_maps/map_editor.hpp"
#include "engine/palette.h"
#include "engine/render/primitive_render.hpp"
#include "engine/render/text_render.hpp"
#include "engine/surface.hpp"
#include "utils/sdl_compat.h"
#include "utils/str_cat.hpp"
#include "utils/str_split.hpp"

namespace devilution {

namespace {

bool Open = false;
bool WantsEditor = false;
std::string Input;
std::vector<std::string> Lines;

void Print(std::string line)
{
	Lines.push_back(std::move(line));
	if (Lines.size() > 60)
		Lines.erase(Lines.begin());
}

void Help()
{
	Print("Commands:");
	Print("  maps\tlist your custom maps");
	Print("  editor <name> [style]\topen or create a map");
	Print("    styles: cathedral, catacombs, caves, hell, town, wilds");
	Print("    (wilds: the land outside a town, with monsters)");
	Print("  editor tristram\tadd portals, doors and arrival points to Tristram");
	Print("  play <name>\tplay a map with the editor's hero");
	Print("  close\tclose this console (or press `)");
	Print(StrCat("Maps are saved in ", CustomMapsDirectory()));
}

void Run(std::string_view line)
{
	std::vector<std::string> args;
	for (std::string_view part : SplitByChar(line, ' ')) {
		if (!part.empty())
			args.emplace_back(part);
	}
	Print(StrCat("> ", line));
	if (args.empty())
		return;
	const std::string &cmd = args[0];
	if (cmd == "help" || cmd == "?") {
		Help();
	} else if (cmd == "maps") {
		const std::vector<std::string> maps = ListCustomMaps();
		if (maps.empty())
			Print("No maps yet. Make one with: editor <name>");
		for (const std::string &name : maps) {
			std::expected<CustomMap, std::string> map = ReadCustomMap(name);
			Print(map.has_value() ? StrCat("  ", name, "\t", MapStyleName(map->style), ", ", map->entries.size(), " arrival points, ", map->portals.size(), " portals/doors", map->spawners.empty() && map->bosses.empty() ? std::string {} : StrCat(", ", map->spawners.size(), " packs, ", map->bosses.size(), " bosses")) : StrCat("  ", name, "   (unreadable)"));
		}
	} else if (cmd == "editor" || cmd == "play") {
		if (args.size() < 2) {
			Print(StrCat("Usage: ", cmd, " <name>", cmd == "editor" ? " [style]" : ""));
			return;
		}
		const bool tristram = args[1] == TristramLayerName;
		if (tristram && cmd == "play") {
			Print("Start a normal game to see Tristram; your portals and doors are there.");
			return;
		}
		if (!tristram && !IsValidMapName(args[1])) {
			Print("Map names use letters, digits, - and _ (up to 32).");
			return;
		}
		dungeon_type style = DTYPE_CATHEDRAL;
		if (args.size() >= 3) {
			const std::optional<dungeon_type> parsed = ParseMapStyle(args[2]);
			if (!parsed || !IsAnyOf(*parsed, DTYPE_CATHEDRAL, DTYPE_CATACOMBS, DTYPE_CAVES, DTYPE_HELL, DTYPE_TOWN, DTYPE_FOREST)) {
				Print("Unknown style. Styles: cathedral, catacombs, caves, hell, town, wilds");
				return;
			}
			style = *parsed;
		}
		if (cmd == "play" && !CustomMapExists(args[1])) {
			Print(StrCat("No map called ", args[1], ". Type maps to see yours."));
			return;
		}
		Print(StrCat(cmd == "play" ? "Playing " : "Opening the editor for ", args[1], " ..."));
		QueueMapEditor(args[1], style, cmd == "play");
		WantsEditor = true;
		Open = false;
	} else if (cmd == "close" || cmd == "exit") {
		Open = false;
	} else {
		Print(StrCat("Unknown command: ", cmd, ". Type help."));
	}
}

} // namespace

bool MenuConsoleHandleEvent(SDL_Event &event)
{
	if (event.type != SDL_EVENT_KEY_DOWN) {
		// While open, the menu behind it gets no input.
		return Open && (event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_TEXT_INPUT);
	}
	const SDL_Scancode sc = SDLC_EventScancode(event);
	if (sc == SDL_SCANCODE_GRAVE) {
		Open = !Open;
		if (Open && Lines.empty()) {
			Print("Diablo1-Two console. Type help for commands.");
		}
		return true;
	}
	if (!Open)
		return false;
	const SDL_Keycode key = SDLC_EventKey(event);
	if (sc == SDL_SCANCODE_ESCAPE) {
		Open = false;
	} else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
		const std::string line = Input;
		Input.clear();
		Run(line);
	} else if (sc == SDL_SCANCODE_BACKSPACE) {
		if (!Input.empty())
			Input.pop_back();
	} else if (Input.size() < 80) {
		const bool shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
		char c = 0;
		if (key >= 'a' && key <= 'z')
			c = static_cast<char>(shift ? key - 'a' + 'A' : key);
		else if (key >= '0' && key <= '9')
			c = static_cast<char>(key);
		else if (key == ' ')
			c = ' ';
		else if (key == '-')
			c = shift ? '_' : '-';
		if (c != 0)
			Input.push_back(c);
	}
	return true;
}

void DrawMenuConsole()
{
	if (!Open)
		return;
	const Surface out(DiabloUiSurface());
	const int w = out.w(), h = std::min(out.h(), 260);
	DrawHalfTransparentRectTo(out, 0, 0, w, h);
	DrawHalfTransparentRectTo(out, 0, 0, w, h);
	DrawHalfTransparentRectTo(out, 0, 0, w, h);
	DrawHorizontalLine(out, { 0, h - 1 }, w, PAL16_YELLOW + 8);
	const int lineHeight = 14;
	const int visible = (h - 30) / lineHeight;
	const int first = std::max(0, static_cast<int>(Lines.size()) - visible);
	int y = 6;
	for (int i = first; i < static_cast<int>(Lines.size()); i++, y += lineHeight) {
		const UiFlags color = Lines[i].starts_with(">") ? UiFlags::ColorGold : UiFlags::ColorWhite;
		const std::string_view line = Lines[i];
		const size_t tab = line.find('\t');
		if (tab == std::string_view::npos) {
			DrawString(out, line, { { 8, y }, { w - 16, 12 } }, { .flags = color });
			continue;
		}
		// "left\tright": the right part starts in a second column.
		DrawString(out, line.substr(0, tab), { { 8, y }, { 200, 12 } }, { .flags = UiFlags::ColorWhitegold });
		DrawString(out, line.substr(tab + 1), { { 212, y }, { w - 220, 12 } }, { .flags = color });
	}
	const bool blink = (SDL_GetTicks() / 400) % 2 == 0;
	DrawString(out, StrCat("> ", Input, blink ? "_" : ""), { { 8, h - 20 }, { w - 16, 12 } }, { .flags = UiFlags::ColorWhitegold });
}

#ifdef _DEBUG
void MenuConsoleDevScript()
{
	// Test hook: DEVILUTIONX_DEV_MENUTYPE = text typed into the console, one key per frame
	// ('`' toggles it, '|' is Enter); DEVILUTIONX_DEV_MENUSHOT = file for a BMP of the menu once typed.
	static size_t pos = 0;
	static int frames = 0;
	const char *text = std::getenv("DEVILUTIONX_DEV_MENUTYPE");
	if (text == nullptr)
		return;
	if (++frames < 30)
		return;
	const std::string_view script = text;
	if (pos < script.size()) {
		const char c = script[pos++];
		SDL_Event event {};
		event.type = SDL_EVENT_KEY_DOWN;
		SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
		SDL_Keycode key = static_cast<SDL_Keycode>(c);
		if (c == '`') sc = SDL_SCANCODE_GRAVE;
		else if (c == '|') { sc = SDL_SCANCODE_RETURN; key = SDLK_RETURN; }
		else if (c >= 'a' && c <= 'z') sc = static_cast<SDL_Scancode>(SDL_SCANCODE_A + (c - 'a'));
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
}

} // namespace devilution
