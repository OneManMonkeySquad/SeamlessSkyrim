#pragma once

// The in-game menu's text, from SeamlessSkyrim_Translations\<language>.json next to the DLL. The
// language is the game's (sLanguage in Skyrim.ini, lower case: german.json); english.json holds
// every key and fills in what the language's file lacks.
namespace UI::Translation
{
	// Once, before the menu is first drawn.
	void Load();

	const char* Text(std::string_view a_key);

	// The text with "##key" appended: ImGui tells widgets apart by their label, and two keys can
	// translate to the same words ("Reset").
	const char* Label(std::string_view a_key);

	template <class... Args>
	std::string Format(std::string_view a_key, const Args&... a_args)
	{
		return std::vformat(Text(a_key), std::make_format_args(a_args...));
	}
}
