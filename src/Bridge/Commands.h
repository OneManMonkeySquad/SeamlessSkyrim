#pragma once

namespace Bridge
{
	struct Command
	{
		json (*handler)(const json& a_args);
		// Almost everything touches engine state that is only safe on the main thread. Commands
		// that wait for something else (e.g. a frame to be presented) opt out so they don't block it.
		bool mainThread = true;
	};

	const std::unordered_map<std::string, Command>& GetCommands();
}
