#include "custom_maps/custom_npcs.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <deque>

#include "custom_maps/campaign.hpp"
#include "engine/assets.hpp"
#include "engine/palette.h"
#include "engine/render/clx_render.hpp"
#include "engine/sound.h"
#include "tables/townerdat.hpp"
#include "towners.h"
#include "utils/file_util.h"
#include "utils/log.hpp"
#include "utils/parse_int.hpp"
#include "utils/str_cat.hpp"
#include "utils/str_split.hpp"

namespace devilution {

namespace {

const std::vector<NpcLook> AllLooks {
	{ "TOWN_SMITH", "Blacksmith", "towners\\smith\\smithn", 16, 3 },
	{ "TOWN_HEALER", "Healer", "towners\\healer\\healer", 20, 6 },
	{ "TOWN_TAVERN", "Innkeeper", "towners\\twnf\\twnfn", 16, 3 },
	{ "TOWN_STORY", "Elder", "towners\\strytell\\strytell", 25, 3 },
	{ "TOWN_DRUNK", "Drunkard", "towners\\drunk\\twndrunk", 18, 3 },
	{ "TOWN_WITCH", "Witch", "towners\\townwmn1\\witch", 19, 6 },
	{ "TOWN_BMAID", "Barmaid", "towners\\townwmn1\\wmnn", 18, 6 },
	{ "TOWN_PEGBOY", "Boy", "towners\\townboy\\pegkid1", 20, 6 },
	{ "TOWN_GAMBLER", "Priest", "towners\\priest\\priest8", 33, 4 },
	{ "TOWN_DEADGUY", "Wounded man", "towners\\butch\\deadguy", 8, 6 },
	{ "TOWN_FARMER", "Farmer", "towners\\farmer\\farmrn2", 15, 3 },
	{ "TOWN_COWFARM", "Farmer in a cow suit", "towners\\farmer\\cfrmrn2", 15, 3 },
	{ "TOWN_GIRL", "Girl", "towners\\girl\\girlw1", 20, 6 },
};

std::string NpcsPath()
{
	const std::string dir = ActiveCampaignDirectory();
	return dir.empty() ? std::string {} : dir + "npcs.ini";
}

std::string OneLine(std::string_view text)
{
	std::string out(text);
	for (char &c : out) {
		if (c == '\n' || c == '\r' || c == '|')
			c = ' ';
	}
	return out;
}

std::string_view ShopName(NpcShop shop)
{
	switch (shop) {
	case NpcShop::Smith: return "smith";
	case NpcShop::Healer: return "healer";
	case NpcShop::Witch: return "witch";
	case NpcShop::Boy: return "boy";
	case NpcShop::Gambler: return "gambler";
	case NpcShop::Storyteller: return "storyteller";
	default: return "none";
	}
}

NpcShop ParseShop(std::string_view s)
{
	for (int i = 0; i < NpcShopCount; i++) {
		if (ShopName(static_cast<NpcShop>(i)) == s)
			return static_cast<NpcShop>(i);
	}
	return NpcShop::None;
}

int ToInt(std::string_view s, int fallback)
{
	const ParseIntResult<int> v = ParseInt<int>(s);
	return v.has_value() ? *v : fallback;
}

NpcRecolor ParseRecolor(std::string_view value)
{
	NpcRecolor r;
	std::vector<std::string_view> f;
	for (std::string_view part : SplitByChar(value, ','))
		f.push_back(part);
	if (f.empty())
		return r;
	const std::vector<std::string_view> &sources = NpcColorSources();
	for (size_t i = 0; i < sources.size(); i++) {
		std::string lower(sources[i]);
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (lower == f[0])
			r.source = static_cast<int>(i);
	}
	if (f.size() >= 4) {
		r.hue = ToInt(f[1], 0);
		r.saturation = std::clamp(ToInt(f[2], 100), 0, 300);
		r.brightness = std::clamp(ToInt(f[3], 100), 20, 300);
	}
	return r;
}

std::string RecolorText(const NpcRecolor &r)
{
	std::string source(NpcColorSources()[std::clamp(r.source, 0, static_cast<int>(NpcColorSources().size()) - 1)]);
	std::transform(source.begin(), source.end(), source.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return StrCat(source, ",", r.hue, ",", r.saturation, ",", r.brightness);
}

// --- Cache of the open campaign's NPCs --------------------------------------------------------

std::string CachedCampaign = "\x01";
std::vector<CustomNpc> Cached;

const std::vector<CustomNpc> &CachedNpcs()
{
	if (CachedCampaign != ActiveCampaign()) {
		CachedCampaign = ActiveCampaign();
		Cached = ReadCustomNpcs();
	}
	return Cached;
}

/** Custom NPCs standing in town now, by tile. */
std::vector<std::pair<Point, std::string>> Placed;
/** Names the townsperson structs point at (kept for the game's lifetime). */
std::deque<std::string> NameStore;

// --- Colour ----------------------------------------------------------------------------------

struct Hsv {
	float h, s, v;
};

Hsv ToHsv(const SDL_Color &c)
{
	const float r = c.r / 255.0F, g = c.g / 255.0F, b = c.b / 255.0F;
	const float mx = std::max({ r, g, b }), mn = std::min({ r, g, b });
	const float d = mx - mn;
	float h = 0;
	if (d > 1e-5F) {
		if (mx == r)
			h = 60.0F * std::fmod((g - b) / d, 6.0F);
		else if (mx == g)
			h = 60.0F * ((b - r) / d + 2.0F);
		else
			h = 60.0F * ((r - g) / d + 4.0F);
	}
	if (h < 0)
		h += 360.0F;
	return { h, mx <= 0 ? 0 : d / mx, mx };
}

SDL_Color FromHsv(Hsv x)
{
	x.h = std::fmod(std::fmod(x.h, 360.0F) + 360.0F, 360.0F);
	x.s = std::clamp(x.s, 0.0F, 1.0F);
	x.v = std::clamp(x.v, 0.0F, 1.0F);
	const float c = x.v * x.s;
	const float hp = x.h / 60.0F;
	const float xx = c * (1 - std::fabs(std::fmod(hp, 2.0F) - 1));
	float r = 0, g = 0, b = 0;
	if (hp < 1) r = c, g = xx;
	else if (hp < 2) r = xx, g = c;
	else if (hp < 3) g = c, b = xx;
	else if (hp < 4) g = xx, b = c;
	else if (hp < 5) r = xx, b = c;
	else r = c, b = xx;
	const float m = x.v - c;
	SDL_Color out;
	out.r = static_cast<uint8_t>(std::lround((r + m) * 255));
	out.g = static_cast<uint8_t>(std::lround((g + m) * 255));
	out.b = static_cast<uint8_t>(std::lround((b + m) * 255));
	out.a = 255;
	return out;
}

float HueDistance(float a, float b)
{
	const float d = std::fabs(a - b);
	return std::min(d, 360.0F - d);
}

/** Does a colour belong to a colour group (NpcColorSources)? */
bool InGroup(int source, const Hsv &c)
{
	constexpr float centres[] = { 0, 0, 0, 30, 55, 120, 220, 285 };
	switch (source) {
	case 0: return false;
	case 1: return true;                         // all colours
	case 8: return c.s < 0.16F && c.v > 0.06F;    // grays
	default:
		if (c.s < 0.16F || c.v < 0.06F)
			return false;
		return HueDistance(c.h, centres[source]) <= (source == 6 ? 40.0F : 22.0F);
	}
}

} // namespace

const std::vector<NpcLook> &NpcLooks()
{
	static std::vector<NpcLook> looks;
	if (looks.empty()) {
		for (const NpcLook &look : AllLooks) {
			if (FindAsset(StrCat(look.path, ".cel").c_str()).ok())
				looks.push_back(look);
		}
	}
	return looks;
}

const NpcLook *FindNpcLook(std::string_view key)
{
	for (const NpcLook &look : NpcLooks()) {
		if (look.key == key)
			return &look;
	}
	return NpcLooks().empty() ? nullptr : &NpcLooks().front();
}

const std::vector<std::string_view> &NpcColorSources()
{
	static const std::vector<std::string_view> sources { "No change", "All colours", "Reds", "Oranges", "Yellows", "Greens", "Blues", "Purples", "Grays" };
	return sources;
}

std::string_view NpcShopLabel(NpcShop shop)
{
	switch (shop) {
	case NpcShop::Smith: return "Weapons and armour (like Griswold)";
	case NpcShop::Healer: return "Potions (like Pepin)";
	case NpcShop::Witch: return "Magic (like Adria)";
	case NpcShop::Boy: return "One rare item (like Wirt)";
	case NpcShop::Gambler: return "Gambling (like Tremain)";
	case NpcShop::Storyteller: return "Identifies items (like Cain)";
	default: return "No shop";
	}
}

std::vector<CustomNpc> ReadCustomNpcs()
{
	std::vector<CustomNpc> npcs;
	const std::string path = NpcsPath();
	if (path.empty())
		return npcs;
	FILE *file = OpenFile(path.c_str(), "rb");
	if (file == nullptr)
		return npcs;
	std::string text;
	char buffer[4096];
	size_t got;
	while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
		text.append(buffer, got);
	std::fclose(file);
	for (std::string_view line : SplitByChar(text, '\n')) {
		if (!line.empty() && line.back() == '\r')
			line.remove_suffix(1);
		if (line.empty() || line[0] == '#')
			continue;
		if (line.front() == '[' && line.back() == ']') {
			npcs.emplace_back();
			npcs.back().id = std::string(line.substr(1, line.size() - 2));
			continue;
		}
		if (npcs.empty())
			continue;
		const size_t eq = line.find('=');
		if (eq == std::string_view::npos)
			continue;
		const std::string_view key = line.substr(0, eq);
		const std::string_view value = line.substr(eq + 1);
		CustomNpc &npc = npcs.back();
		if (key == "name") npc.name = std::string(value);
		else if (key == "look") npc.look = std::string(value);
		else if (key == "recolor1") npc.recolor[0] = ParseRecolor(value);
		else if (key == "recolor2") npc.recolor[1] = ParseRecolor(value);
		else if (key == "shop") npc.shop = ParseShop(value);
		else if (key == "line") {
			const size_t bar = value.find('|');
			npc.lines.push_back({ std::string(value.substr(0, bar)), bar == std::string_view::npos ? std::string {} : std::string(value.substr(bar + 1)) });
		}
	}
	return npcs;
}

std::expected<void, std::string> WriteCustomNpcs(const std::vector<CustomNpc> &npcs)
{
	const std::string path = NpcsPath();
	if (path.empty())
		return std::unexpected("No campaign is open.");
	std::string text = "# Campaign townspeople (Campaign Editor, NPCs). line=what they say|voice file\n";
	for (const CustomNpc &npc : npcs) {
		text += StrCat("\n[", npc.id, "]\n");
		text += StrCat("name=", OneLine(npc.name), "\n");
		text += StrCat("look=", npc.look, "\n");
		text += StrCat("recolor1=", RecolorText(npc.recolor[0]), "\n");
		text += StrCat("recolor2=", RecolorText(npc.recolor[1]), "\n");
		text += StrCat("shop=", ShopName(npc.shop), "\n");
		for (const NpcLine &line : npc.lines)
			text += StrCat("line=", OneLine(line.text), "|", line.voice, "\n");
	}
	FILE *file = OpenFile(path.c_str(), "wb");
	if (file == nullptr)
		return std::unexpected(StrCat("Cannot write ", path));
	const bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
	std::fclose(file);
	if (!ok)
		return std::unexpected(StrCat("Cannot write ", path));
	CachedCampaign = ActiveCampaign();
	Cached = npcs;
	return {};
}

std::string NewNpcId(const std::vector<CustomNpc> &npcs)
{
	for (int n = 1;; n++) {
		std::string id = StrCat("n", n);
		if (std::none_of(npcs.begin(), npcs.end(), [&](const CustomNpc &npc) { return npc.id == id; }))
			return id;
	}
}

const CustomNpc *FindCustomNpc(std::string_view id)
{
	for (const CustomNpc &npc : CachedNpcs()) {
		if (npc.id == id)
			return &npc;
	}
	return nullptr;
}

std::string CustomNpcType(std::string_view id)
{
	return StrCat("custom:", id);
}

std::string_view CustomNpcIdOf(std::string_view type)
{
	constexpr std::string_view Prefix = "custom:";
	return type.starts_with(Prefix) ? type.substr(Prefix.size()) : std::string_view {};
}

std::string CampaignVoiceDirectory()
{
	const std::string dir = ActiveCampaignDirectory();
	return dir.empty() ? std::string {} : StrCat(dir, "voice" DIRECTORY_SEPARATOR_STR);
}

std::vector<std::string> ListCampaignVoices()
{
	std::vector<std::string> files;
	const std::string dir = CampaignVoiceDirectory();
	if (dir.empty() || !DirectoryExists(dir.c_str()))
		return files;
	for (std::string &name : ListFiles(dir.c_str())) {
		std::string lower = name;
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (lower.ends_with(".ogg") || lower.ends_with(".wav"))
			files.push_back(std::move(name));
	}
	std::sort(files.begin(), files.end());
	return files;
}

void PlayNpcVoice(std::string_view voiceFile)
{
	if (voiceFile.empty() || voiceFile.find_first_of("/\\") != std::string_view::npos)
		return;
	const std::string path = CampaignVoiceDirectory() + std::string(voiceFile);
	if (!path.empty() && FileExists(path.c_str()))
		PlayVoiceFile(path);
}

std::array<uint8_t, 256> BuildNpcTrn(const CustomNpc &npc, const std::array<SDL_Color, 256> &palette)
{
	std::array<uint8_t, 256> trn;
	for (int i = 0; i < 256; i++)
		trn[i] = static_cast<uint8_t>(i);
	if (npc.recolor[0].source == 0 && npc.recolor[1].source == 0)
		return trn;
	// Colours the town palette has, to map the changed colours onto.
	for (int i = 1; i < 256; i++) {
		Hsv c = ToHsv(palette[i]);
		bool changed = false;
		for (const NpcRecolor &r : npc.recolor) {
			if (!InGroup(r.source, c))
				continue;
			if (r.source == 1)
				c.h += static_cast<float>(r.hue);
			else if (r.source == 8)
				c.h = static_cast<float>(r.hue), c.s = 0.35F;
			else
				c.h = static_cast<float>(r.hue);
			c.s *= r.saturation / 100.0F;
			c.v *= r.brightness / 100.0F;
			changed = true;
			break;
		}
		if (!changed)
			continue;
		const SDL_Color want = FromHsv(c);
		int best = i;
		int bestDistance = 1 << 30;
		for (int j = 1; j < 256; j++) {
			const SDL_Color &p = palette[j];
			const int dr = p.r - want.r, dg = p.g - want.g, db = p.b - want.b;
			const int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
			if (d < bestDistance) {
				bestDistance = d;
				best = j;
			}
		}
		trn[i] = static_cast<uint8_t>(best);
	}
	return trn;
}

bool MakeCustomNpcEntry(std::string_view id, Point position, int direction, TownerDataEntry &entry)
{
	const CustomNpc *npc = FindCustomNpc(id);
	if (npc == nullptr)
		return false;
	const NpcLook *look = FindNpcLook(npc->look);
	if (look == nullptr)
		return false;
	entry = {};
	entry.type = TOWN_CUSTOM;
	entry.name = npc->name;
	entry.position = position;
	entry.direction = static_cast<Direction>(direction);
	entry.animWidth = 96;
	entry.animPath = std::string(look->path);
	entry.animFrames = static_cast<uint8_t>(look->frames);
	entry.animDelay = static_cast<int16_t>(look->delay);
	std::erase_if(Placed, [&](const auto &p) { return p.first == position; });
	Placed.emplace_back(position, std::string(id));
	return true;
}

void InitCustomNpcTowner(Towner &towner)
{
	for (const auto &[position, id] : Placed) {
		if (position != towner.position)
			continue;
		const CustomNpc *npc = FindCustomNpc(id);
		if (npc == nullptr)
			return;
		NameStore.push_back(npc->name);
		towner.name = NameStore.back();
		if (towner.ownedAnim) {
			const std::array<uint8_t, 256> trn = BuildNpcTrn(*npc, logical_palette);
			ClxApplyTrans(*towner.ownedAnim, trn.data());
		}
		return;
	}
}

const CustomNpc *CustomNpcOf(const Towner &towner)
{
	if (towner._ttype != TOWN_CUSTOM)
		return nullptr;
	for (const auto &[position, id] : Placed) {
		if (position == towner.position)
			return FindCustomNpc(id);
	}
	return nullptr;
}

void TalkToCustomNpc(Player & /*player*/, Towner & /*towner*/)
{
	// The campaign conversation (campaign_quests.cpp) handles custom townspeople; this is only
	// reached when no campaign is open, where they cannot be placed.
}

} // namespace devilution
