#pragma once

// ENB (enbdev.com) runs on the game's own shaders, so with it the native backend does the work
// (Render/Backend.h). ENB sits in the game folder as d3d11.dll, a proxy for the real one, and
// exports its SDK from there; this finds it and reads what is useful for diagnostics.
//
// What ENB does on its own and nothing here changes: it grades interiors and exteriors with
// separate settings and switches when the game's current cell changes, which with seamless doors
// is the moment the player crosses the doorway (a visible step in colour and brightness, as large
// as the preset makes the two apart). Its screen-space effects read the depth buffer, which the
// doorway mask writes correctly (cut pixels leave no depth).
namespace Render::Enb
{
	void Detect();
	[[nodiscard]] bool Present();
	[[nodiscard]] json Describe();
}
