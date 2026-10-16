#include "custom_maps/campaign.hpp"

#include <algorithm>
#include <cstdio>

#include "custom_maps/custom_map.hpp"
#include "utils/file_util.h"
#include "utils/log.hpp"
#include "utils/paths.h"
#include "utils/str_cat.hpp"
#include "utils/str_split.hpp"

namespace devilution {

namespace {

std::string Active;
std::string NewHero;

std::string CustomMapsRoot()
{
	return StrCat(paths::BasePath(), "mods" DIRECTORY_SEPARATOR_STR "custommaps" DIRECTORY_SEPARATOR_STR);
}

std::string CampaignFolder(std::string_view id)
{
	return StrCat(CampaignsDirectory(), id, DIRECTORY_SEPARATOR_STR);
}

std::optional<std::string> ReadText(const std::string &path)
{
	FILE *file = OpenFile(path.c_str(), "rb");
	if (file == nullptr)
		return std::nullopt;
	std::string data;
	char buffer[2048];
	size_t got;
	while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
		data.append(buffer, got);
	std::fclose(file);
	return data;
}

/** One line of text for campaign.ini (no line breaks). */
std::string OneLine(std::string_view text)
{
	std::string out(text);
	std::replace(out.begin(), out.end(), '\n', ' ');
	std::replace(out.begin(), out.end(), '\r', ' ');
	return out;
}

/** Maps made before campaigns existed (mods/custommaps/maps, tristram.ini) move into "My Maps". */
void MigrateLooseMaps()
{
	static bool done = false;
	if (done)
		return;
	done = true;
	const std::string looseMaps = CustomMapsRoot() + "maps" DIRECTORY_SEPARATOR_STR;
	const std::string looseLayer = CustomMapsRoot() + "tristram.ini";
	std::vector<std::string> maps;
	if (DirectoryExists(looseMaps.c_str())) {
		for (std::string &name : ListDirectories(looseMaps.c_str())) {
			if (IsValidMapName(name) && FileExists((looseMaps + name + DIRECTORY_SEPARATOR_STR "map.dun").c_str()))
				maps.push_back(std::move(name));
		}
	}
	const bool layer = FileExists(looseLayer.c_str());
	if (maps.empty() && !layer)
		return;
	std::string id = "my-maps";
	for (int n = 2; DirectoryExists(CampaignFolder(id).c_str()); n++)
		id = StrCat("my-maps-", n);
	const Campaign campaign { id, "My Maps", "Maps made before campaigns.", "" };
	if (!WriteCampaign(campaign).has_value())
		return;
	const std::string target = CampaignFolder(id) + "maps" DIRECTORY_SEPARATOR_STR;
	RecursivelyCreateDir(target.c_str());
	for (const std::string &name : maps)
		RenameFile((looseMaps + name).c_str(), (target + name).c_str());
	if (layer)
		RenameFile(looseLayer.c_str(), (CampaignFolder(id) + "tristram.ini").c_str());
	LogInfo("Moved {} loose maps into the campaign \"My Maps\" ({})", maps.size(), id);
}

} // namespace

bool IsValidCampaignId(std::string_view id)
{
	return IsValidMapName(id);
}

std::string CampaignsDirectory()
{
	return CustomMapsRoot() + "campaigns" DIRECTORY_SEPARATOR_STR;
}

std::vector<Campaign> ListCampaigns()
{
	MigrateLooseMaps();
	std::vector<Campaign> list;
	const std::string dir = CampaignsDirectory();
	if (!DirectoryExists(dir.c_str()))
		return list;
	for (const std::string &id : ListDirectories(dir.c_str())) {
		if (std::optional<Campaign> campaign = ReadCampaign(id); campaign)
			list.push_back(std::move(*campaign));
	}
	std::sort(list.begin(), list.end(), [](const Campaign &a, const Campaign &b) { return a.title < b.title; });
	return list;
}

std::optional<Campaign> ReadCampaign(std::string_view id)
{
	if (!IsValidCampaignId(id))
		return std::nullopt;
	const std::optional<std::string> text = ReadText(CampaignFolder(id) + "campaign.ini");
	if (!text)
		return std::nullopt;
	Campaign campaign;
	campaign.id = std::string(id);
	campaign.title = std::string(id);
	for (std::string_view line : SplitByChar(*text, '\n')) {
		if (!line.empty() && line.back() == '\r')
			line.remove_suffix(1);
		if (line.empty() || line[0] == '#')
			continue;
		const size_t eq = line.find('=');
		if (eq == std::string_view::npos)
			continue;
		const std::string_view key = line.substr(0, eq);
		const std::string_view value = line.substr(eq + 1);
		if (key == "title" && !value.empty())
			campaign.title = std::string(value);
		else if (key == "description")
			campaign.description = std::string(value);
		else if (key == "start")
			campaign.start = IsValidMapName(value) ? std::string(value) : std::string {};
	}
	return campaign;
}

std::expected<void, std::string> WriteCampaign(const Campaign &campaign)
{
	if (!IsValidCampaignId(campaign.id))
		return std::unexpected(StrCat("Not a valid campaign name: ", campaign.id));
	const std::string folder = CampaignFolder(campaign.id);
	RecursivelyCreateDir((folder + "maps").c_str());
	const std::string text = StrCat("# Campaign (map editor). Share this whole folder to share the campaign.\n",
	    "title=", OneLine(campaign.title), "\n",
	    "description=", OneLine(campaign.description), "\n",
	    "# Town map new heroes start in (empty: Tristram).\n",
	    "start=", campaign.start, "\n");
	FILE *file = OpenFile((folder + "campaign.ini").c_str(), "wb");
	if (file == nullptr)
		return std::unexpected(StrCat("Cannot write ", folder, "campaign.ini"));
	const bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
	std::fclose(file);
	if (!ok)
		return std::unexpected(StrCat("Cannot write ", folder, "campaign.ini"));
	return {};
}

const std::string &ActiveCampaign()
{
	return Active;
}

void SetActiveCampaign(std::string id)
{
	if (!id.empty() && !ReadCampaign(id)) {
		LogWarn("Campaign {} is not installed; playing Diablo: Original", id);
		id.clear();
	}
	Active = std::move(id);
}

std::string ActiveCampaignDirectory()
{
	return Active.empty() ? std::string {} : CampaignFolder(Active);
}

std::string ActiveCampaignTitle()
{
	if (Active.empty())
		return std::string(OriginalCampaignTitle);
	const std::optional<Campaign> campaign = ReadCampaign(Active);
	return campaign ? campaign->title : Active;
}

void SetNewHeroCampaign(std::string id)
{
	NewHero = std::move(id);
}

const std::string &NewHeroCampaign()
{
	return NewHero;
}

std::string CampaignStartTown(std::string_view id)
{
	const std::optional<Campaign> campaign = ReadCampaign(id);
	if (!campaign || campaign->start.empty())
		return {};
	const std::string dun = StrCat(CampaignFolder(id), "maps" DIRECTORY_SEPARATOR_STR, campaign->start, DIRECTORY_SEPARATOR_STR "map.dun");
	return FileExists(dun.c_str()) ? campaign->start : std::string {};
}

} // namespace devilution
