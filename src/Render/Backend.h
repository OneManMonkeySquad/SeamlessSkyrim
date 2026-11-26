#pragma once

// Who draws the parts of seamless doors that need changes inside the shaders: the doorway mask
// (two spaces drawn in one pass, each only where the other isn't), the far door's clip in the
// portal pass, and the light of a room merged into the world.
//
// - Community Shaders, the SeamlessSkyrim fork (docs/COMMUNITY-SHADERS-PATCH.md): it replaces the
//   game's shaders and does all of it inside them, reached through its SeamlessPortal_* exports.
// - Native: the game's own shaders, unchanged (also what ENB runs on). The mask is cut by the
//   rasterizer from clip distances a geometry shader of ours adds behind the engine's vertex
//   shader (Render/NativeMask.cpp); the light by changing what the engine hands its lighting
//   shader per draw (Render/NativeLighting.cpp).
// - None: a stock Community Shaders without the fork's exports. It replaces the game's shaders
//   and ignores what the native backend sets up, so seamless rendering is off; crossing doors
//   without a loading screen still works.
//
// Chosen once at kDataLoaded, when every SKSE plugin is loaded. [Render] sBackend in
// SeamlessSkyrim.ini overrides the choice (auto, cs, native, none).
namespace Render::Backend
{
	enum class Kind
	{
		kNone,
		kCommunityShaders,
		kNative,
	};

	void Install();

	[[nodiscard]] Kind        Current();
	[[nodiscard]] const char* Name();
	// The game's own shaders draw (no Community Shaders loaded, the fork or not).
	[[nodiscard]] bool GameShaders();

	// A room can be drawn into the world (render-merge): something masks the two spaces.
	[[nodiscard]] bool CanMerge();
	// How many rooms can be merged at once, each masked at its own doorway and lit by its own
	// light: 4 with the fork's per-room exports, else 1. The native backend cuts the world into
	// convex pieces per doorway, and a shape near two doorways would multiply them: one room.
	inline constexpr int kMaxMergedRooms = 4;
	[[nodiscard]] int MergedRooms();

	// Render-merge, every frame while merged; nullptr when nothing is merged. a_door: doorway
	// centre (3), unit normal into the merged space (3), unit horizontal axis (3), half width, half
	// height, 1 with the camera in the merged space.
	// a_room: 0 the room the player can walk into (the only one with the camera inside), 1..3
	// rooms only seen from the street (below MergedRooms()).
	void SetMergedDoor(int a_room, const float* a_door);
	// The merged space's own light: directional colour (3), direction towards the light (3),
	// directional ambient as three rows of a 3x4 matrix (12), scale for its lights and ambient.
	// nullptr: lit like the world.
	void SetMergedLighting(int a_room, const float* a_lighting);
	// The street merged into an interior: its fog (near, far, power, most it covers, near colour,
	// far colour).
	void SetMergedFog(const float* a_fog);
	// Rooms merged into the street, seen from it: the street's fog (as SetMergedFog) for the air in
	// front of their doorways; nullptr: none. Community Shaders only.
	void SetFrontFog(const float* a_fog);
	// A room merged into the street, seen from it: the street's light coming in through its
	// doorway, as the radiance of the opening (rgb); nullptr: none. Community Shaders only.
	void SetMergedDaylight(int a_room, const float* a_daylight);
	// Room 0 merged into the interior the player is in: its light coming out through its doorway into
	// the player's space, as the radiance of the opening (rgb); nullptr: none. Community Shaders only.
	void SetHostDaylight(const float* a_daylight);
	// A city gate seen through as a merged door in room 0 (the portal pass draws the other side):
	// the world's own shapes are masked through it only up to a_depth beyond its plane, where its far
	// gate stands in the world; 0 when room 0 isn't a gate. Only when CanMaskGate().
	[[nodiscard]] bool CanMaskGate();
	void               SetGatePassage(float a_depth);
	// The street merged into an interior gets the sun's shadows. Community Shaders only.
	void SetInteriorSun(bool a_on);
	// The player walked out of a doorway: lighting that builds up over time is settled at once.
	void SettleSkylighting();

	// Around the portal pass. a_sun: the light scene's sun, already carrying the other space's
	// light. The clip plane (unit normal, offset; pixels with dot(P, n) < offset are cut) is set
	// before Begin and cleared (nullptr) after End.
	void SetPortalClip(const float* a_plane);
	// The sun's shadows of an outdoor space the portal pass draws, rendered with its own shadow light
	// into shadow map arrays made like the engine's (Community Shaders fork only); set before Begin,
	// cleared (nullptr) after End.
	bool CanShadowPortal();
	// The outdoor worldspace the portal pass draws, or nullptr: the fork keeps its terrain's shadows
	// (Terrain Shadows) for it while the player's worldspace (a city's) has none of its own.
	void SetPortalWorldspace(RE::TESWorldSpace* a_worldspace);
	void SetPortalShadows(RE::BSShadowDirectionalLight* a_light, void* a_shadowMaps, void* a_volumetricShadowMaps);
	// a_camera: the camera the pass draws with; the fork turns the pass's depth back into distance
	// with its near and far (a city gate's portal camera has its near plane at the far gate).
	void BeginPortalPass(RE::NiDirectionalLight* a_sun, bool a_interior, const RE::NiCamera* a_camera);
	void EndPortalPass();

	// Community Shaders' Sky Sync: the sun's direction for an hour and its place on the sky.
	bool OutdoorSunDirection(float a_hour, const float* a_position, float* a_towardSun);

	// Whether shapes drawn with these shaders are cut at the far door in the portal pass. Community
	// Shaders cuts only in its Lighting and Utility shaders; the native mask in every shader it hooks.
	[[nodiscard]] bool CutsPortalShader(RE::BSShader::Type a_type);
	// Whether the weather's rain and snow are masked at a merged doorway (the fork's particle
	// shader): without it, rain let into an interior with the street merged fell in the room too.
	[[nodiscard]] bool MasksPrecipitation();

	[[nodiscard]] json Describe();
}
