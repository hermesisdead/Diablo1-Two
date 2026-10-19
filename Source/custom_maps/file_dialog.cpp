#include "custom_maps/file_dialog.hpp"

#include <cstdio>
#include <cstdlib>
#include <cwchar>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h first
#include <commdlg.h>
#ifdef _MSC_VER
#pragma comment(lib, "comdlg32.lib")
#endif
#ifdef USE_SDL3
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>
#else
#include <SDL.h>
#include <SDL_syswm.h>
#endif
#include "utils/display.h"
#endif

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include "utils/log.hpp"
#include "utils/str_cat.hpp"

namespace devilution {

namespace {

#ifdef _WIN32
std::wstring Widen(std::string_view s)
{
	if (s.empty())
		return {};
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring w(static_cast<size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
	return w;
}

std::string Narrow(std::wstring_view w)
{
	if (w.empty())
		return {};
	const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
	std::string s(static_cast<size_t>(n), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
	return s;
}

HWND GameWindow()
{
	if (ghMainWnd == nullptr)
		return nullptr;
#ifdef USE_SDL3
	return static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(ghMainWnd), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
#else
	SDL_SysWMinfo info;
	SDL_VERSION(&info.version);
	if (SDL_GetWindowWMInfo(ghMainWnd, &info) != SDL_TRUE)
		return nullptr;
	return info.info.win.window;
#endif
}
#else
/** Runs a command and returns what it printed (nullopt when it could not run or was cancelled). */
std::optional<std::string> RunAndRead(const std::string &command)
{
	FILE *p = popen(command.c_str(), "r");
	if (p == nullptr)
		return std::nullopt;
	std::string out;
	char buffer[4096];
	size_t got;
	while ((got = std::fread(buffer, 1, sizeof(buffer), p)) > 0)
		out.append(buffer, got);
	const int status = pclose(p);
	if (status != 0)
		return std::nullopt;
	return out;
}

std::vector<std::string> SplitPaths(const std::string &text, char separator)
{
	std::vector<std::string> paths;
	std::string current;
	for (const char c : text) {
		if (c == separator || c == '\n' || c == '\r') {
			if (!current.empty())
				paths.push_back(current);
			current.clear();
		} else {
			current.push_back(c);
		}
	}
	if (!current.empty())
		paths.push_back(current);
	return paths;
}

/** Puts text in single quotes for the shell. */
std::string Quote(std::string_view s)
{
	std::string out = "'";
	for (const char c : s) {
		if (c == '\'')
			out += "'\\''";
		else
			out.push_back(c);
	}
	out += "'";
	return out;
}
#endif

} // namespace

std::optional<std::vector<std::string>> ChooseFilesToOpen(std::string_view title, const std::vector<std::string> &extensions)
{
#ifdef _WIN32
	// Filter: "Music (*.ogg;*.mp3)\0*.ogg;*.mp3\0All files\0*.*\0\0"
	std::wstring patterns;
	for (const std::string &ext : extensions) {
		if (!patterns.empty())
			patterns += L";";
		patterns += L"*." + Widen(ext);
	}
	std::wstring filter = L"Sound files (" + patterns + L")";
	filter.push_back(L'\0');
	filter += patterns;
	filter.push_back(L'\0');
	filter += L"All files";
	filter.push_back(L'\0');
	filter += L"*.*";
	filter.push_back(L'\0');
	filter.push_back(L'\0');
	const std::wstring wtitle = Widen(title);
	std::vector<wchar_t> buffer(1 << 16, L'\0');
	OPENFILENAMEW ofn {};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = GameWindow();
	ofn.lpstrFilter = filter.c_str();
	ofn.lpstrFile = buffer.data();
	ofn.nMaxFile = static_cast<DWORD>(buffer.size());
	ofn.lpstrTitle = wtitle.c_str();
	ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
	if (GetOpenFileNameW(&ofn) == 0) {
		if (const DWORD error = CommDlgExtendedError(); error != 0)
			LogError("File dialog error {}", static_cast<unsigned>(error));
		return std::vector<std::string> {};
	}
	// One file: its full path. Several: the folder, then each name, each ending in \0, then \0.
	std::vector<std::wstring> parts;
	for (const wchar_t *p = buffer.data(); *p != L'\0'; p += wcslen(p) + 1)
		parts.emplace_back(p);
	std::vector<std::string> files;
	if (parts.size() == 1) {
		files.push_back(Narrow(parts[0]));
	} else {
		for (size_t i = 1; i < parts.size(); i++)
			files.push_back(Narrow(parts[0] + L"\\" + parts[i]));
	}
	return files;
#elif defined(__APPLE__) && TARGET_OS_OSX
	std::string types;
	for (const std::string &ext : extensions)
		types += StrCat(types.empty() ? "" : ", ", "\"", ext, "\"");
	const std::string script = StrCat(
	    "set fs to choose file with prompt \"", title, "\" of type {", types, "} with multiple selections allowed\n",
	    "set out to \"\"\n",
	    "repeat with f in fs\n",
	    "set out to out & POSIX path of f & linefeed\n",
	    "end repeat\n",
	    "return out");
	const std::optional<std::string> out = RunAndRead(StrCat("osascript -e ", Quote(script), " 2>/dev/null"));
	if (!out)
		return std::vector<std::string> {}; // cancelled
	return SplitPaths(*out, '\n');
#elif defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
	std::string globs;
	for (const std::string &ext : extensions)
		globs += StrCat(globs.empty() ? "" : " ", "*.", ext);
	if (std::system("command -v zenity >/dev/null 2>&1") == 0) {
		const std::optional<std::string> out = RunAndRead(StrCat("zenity --file-selection --multiple --separator='|' --title=", Quote(title),
		    " --file-filter=", Quote(StrCat("Music | ", globs)), " 2>/dev/null"));
		return out ? SplitPaths(*out, '|') : std::vector<std::string> {};
	}
	if (std::system("command -v kdialog >/dev/null 2>&1") == 0) {
		const std::optional<std::string> out = RunAndRead(StrCat("kdialog --getopenfilename --multiple --separate-output --title ", Quote(title),
		    " ~ ", Quote(StrCat(globs, "|Music")), " 2>/dev/null"));
		return out ? SplitPaths(*out, '\n') : std::vector<std::string> {};
	}
	return std::nullopt;
#else
	(void)title;
	(void)extensions;
	return std::nullopt;
#endif
}

} // namespace devilution
