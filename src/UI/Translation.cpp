#include "UI/Translation.h"

#include "Paths.h"

#include <fstream>

namespace
{
	struct Entry
	{
		std::string key;
		std::string text;
		std::string label;
	};

	// About fifty keys.
	std::vector<Entry> entries;

	Entry& Find(std::string_view a_key)
	{
		const auto it = std::ranges::find(entries, a_key, &Entry::key);
		SEAMLESS_ASSERT(it != entries.end());
		return *it;
	}

	std::filesystem::path FileOf(std::string_view a_language)
	{
		std::string name(a_language);
		std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return Paths::PluginDirectory() / "SeamlessSkyrim_Translations" / (name + ".json");
	}

	// A text with more {} than the code passes makes std::vformat throw while the menu is drawn.
	std::size_t Placeholders(std::string_view a_text)
	{
		return std::ranges::count(a_text, '{');
	}
}

namespace UI::Translation
{
	void Load()
	{
		// Ships with the DLL; without it the menu has no text at all.
		std::ifstream english(FileOf("english"));
		SEAMLESS_ASSERT(english);
		for (const auto& [key, value] : json::parse(english).items()) {
			entries.push_back({ key, value.get<std::string>(), {} });
		}

		const std::string language = RE::GetINISetting("sLanguage:General")->GetString();
		const auto        path = FileOf(language);
		// A translation is someone else's file, possibly older than this version: what is wrong in
		// it is logged and left in English, rather than costing the player their game.
		if (path != FileOf("english") && std::filesystem::exists(path)) {
			std::ifstream file(path);
			const json    translated = json::parse(file, nullptr, false);
			if (translated.is_discarded() || !translated.is_object()) {
				logger::error("translation: {} isn't a JSON object; the menu stays in English", path.string());
			} else {
				int used = 0;
				for (const auto& [key, value] : translated.items()) {
					const auto it = std::ranges::find(entries, key, &Entry::key);
					if (it == entries.end()) {
						logger::warn("translation: {} has \"{}\", which english.json doesn't", path.filename().string(), key);
					} else if (!value.is_string() || Placeholders(value.get<std::string>()) != Placeholders(it->text)) {
						logger::warn("translation: \"{}\" in {} needs the {{}} of english.json; kept in English", key, path.filename().string());
					} else {
						it->text = value.get<std::string>();
						++used;
					}
				}
				logger::info("translation: {} of {} texts from {}", used, entries.size(), path.filename().string());
			}
		}

		for (auto& entry : entries) {
			entry.label = entry.text + "##" + entry.key;
		}
	}

	const char* Text(std::string_view a_key)
	{
		return Find(a_key).text.c_str();
	}

	const char* Label(std::string_view a_key)
	{
		return Find(a_key).label.c_str();
	}
}
