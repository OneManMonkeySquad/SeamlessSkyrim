#pragma once

// A remote space's own light scene: a ShadowSceneNode that is also the private scene-graph root the
// space is attached under. Culling it from the portal camera assigns *its* lights to the space's
// geometry, and the CS patch reads the one registered in BSShaderManager::State slot 3.
//
// Every space kept attached has a scene of its own (interiors share coordinates: two rooms in one
// scene would light each other), and only the shown one is registered.
namespace Portal::LightScene
{
	inline constexpr std::uint32_t kSlot = 3;

	// A new scene to attach a space under, not registered (not drawn) until Show.
	[[nodiscard]] RE::NiPointer<RE::ShadowSceneNode> Create();

	// The scene registered in slot 3: the shown space's, or an empty one while none is shown.
	RE::ShadowSceneNode* Get();
	// a_scene registered in slot 3. nullptr: the empty one.
	void Show(RE::ShadowSceneNode* a_scene);
	[[nodiscard]] bool Shown(const RE::ShadowSceneNode* a_scene);

	// a_scene's space was released: its lights and object lists emptied, unregistered if shown,
	// and kept alive, never freed. Render passes cached on the released shapes keep pointing at
	// its sun light; freed after eight releases, a portal pass crashed in the lighting shader's
	// setup reading that light (BSLightingShader::SetupGeometry, ID 107300 + 0x595). Costs one light
	// scene per space attached.
	void Retire(RE::NiPointer<RE::ShadowSceneNode> a_scene);

	// a_scene's lights and object lists emptied.
	void Clear(RE::ShadowSceneNode* a_scene);

	// Any thread: a_top (the top of a scene graph) is one of ours, shown or not.
	[[nodiscard]] bool IsOurs(const RE::NiAVObject* a_top);

	// Moves the lights under a_cell3D (the remote cell) from the engine's light scenes into a_scene.
	// Cheap enough to run every frame, which also catches lights created after the attach.
	// a_coming: lights the scene is about to be given; another for the same NiLight isn't taken.
	int Adopt(RE::ShadowSceneNode* a_scene, const RE::NiAVObject* a_cell3D, const std::vector<RE::NiPointer<RE::BSLight>>& a_coming = {});

	// Lights changing scene as they are. A light taken into a scene the engine's way (AddLight) is
	// given a 200 KB culling process, built entry by entry (ID 76551), and a walk of the scene for
	// the shapes it lights (ID 106401): ~0.2 ms each, 12-17 ms of a crossing for lights that were
	// lit a frame before in the other scene. The shapes a light is paired with are kept by the light
	// and by each shape's shader property (ID 108283), not by the scene, and the street's shapes
	// are the same ones on either side of the door: moved from one scene's list of active lights
	// into the other's, a light goes on lighting them with nothing built.
	// Take: the lights of a_scene that a_which accepts, out of its active list (not its shadow
	// casters, sun or cloud light). Give: those of a_lights that a_which accepts into a_scene's,
	// unless the scene has one for the same NiLight already; returns how many, and leaves the rest
	// in a_lights. Dispose: lights no scene will take, removed the engine's way (their pairs undone).
	using LightFilter = std::function<bool(const RE::BSLight*)>;
	[[nodiscard]] std::vector<RE::NiPointer<RE::BSLight>> Take(RE::ShadowSceneNode* a_scene, const LightFilter& a_which);
	int  Give(RE::ShadowSceneNode* a_scene, std::vector<RE::NiPointer<RE::BSLight>>& a_lights, const LightFilter& a_which);
	void Dispose(std::vector<RE::NiPointer<RE::BSLight>>& a_lights);
	// a_light hangs under a_ancestor.
	[[nodiscard]] bool LightUnder(const RE::BSLight* a_light, const RE::NiAVObject* a_ancestor);
	// Given lights whose pairs may lack shapes loaded while the light was in another scene: a_count
	// of them per call walk the scene again (the scene's update queue), until all have.
	void Refresh(RE::ShadowSceneNode* a_scene, std::vector<RE::NiPointer<RE::BSLight>>& a_lights, std::size_t a_count);

	// The world scene's lights right now.
	[[nodiscard]] std::vector<RE::NiPointer<RE::BSLight>> WorldLights();

	// The engine culls and fades lights against the main camera only; for the portal view, and for a
	// room merged into the world (lit from this scene by the patched Community Shaders), every light
	// of the shown scene must count as visible and unfaded.
	void PrepareForView();
	// The same for a scene that isn't registered (a room merged into the world beside the shown
	// one, lit from its own scene by the patched Community Shaders), and what the engine's light
	// update does for registered scenes only: the lights adopted into it wait in its add queue
	// until that update takes them in. Main thread, right before the player's view.
	void PrepareUnshownForView(RE::ShadowSceneNode* a_scene);

	json Describe(RE::ShadowSceneNode* a_scene, const RE::NiAVObject* a_cell3D);
	json DescribeAll(const RE::NiAVObject* a_cell3D);
}
