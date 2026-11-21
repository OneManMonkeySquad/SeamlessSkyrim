#pragma once

// Development bridge: a localhost-only TCP server (newline-delimited JSON) that the external MCP
// server in tools/skyrim-mcp talks to, so an agent can inspect and drive the game while we work
// on the engine. Not part of the mod's user-facing feature set.
namespace Bridge
{
	inline constexpr std::uint16_t kPort = 47800;

	void Start();
	// Keeps the game running while unfocused. Call once INI settings are loaded (kDataLoaded).
	void ForceAlwaysActive();
}
