# Engine facts

Skyrim AE 1.7.104, IDs from the Address Library. These were expensive to find. Add new ones in
the same change that needed them.

## Cells and spaces

- **One current space.** The engine runs either one interior (`TES::interiorCell`) or one exterior
  grid (`TES::gridCells` plus LOD). Rendering, lighting, AI, Havok stepping, sky and weather all
  follow it. Everything here is about keeping a second space alive beside it.
- **Leaving a cell** detaches it, and its actors leave high process. Its references stay in
  memory.
  - The first probe (Dragonsreach → street → Bannered Mare) saw cells drop to `DataLoaded` with
    their 3D freed.
  - Interiors in the interior buffer have since been seen keeping their 3D (state Unattached,
    3D yes), and an attach then has nothing to build.
- **Preloading:** the engine loads the data of interiors linked from nearby doors
  (`bPreloadLinkedInteriors`), so exterior → interior targets are in memory early.
- **Interior → exterior:** the linked door is a persistent reference with no parent cell until
  the exterior loads, so its cell has to be resolved and loaded by us.
- **Attaching a cell that isn't current** (reversed from `TES::SetCurrentCell`):
  - ID 19111 (cell) loads its temporary references.
  - ID 18976 (cell, node) attaches it under any parent node; `SetCurrentCell` passes
    `TES::objRoot`. The cell gets its own `bhkWorld`.
  - ID 19063 (cell, true) finishes: state Attached, actors in high process with 3D. Its gate
    (ID 13308) only checks the cell's state, not whether the cell is current.
  - The attach also installs the cell's room graph in the world light scene
    (`ShadowSceneNode::SetPortalGraph`, slot 0): put the world's back, or the main view culls
    with the other cell's rooms (actors vanish, crashes).
- **Detaching:** take the cell's 3D off our root first, then call ID 19130 (cell, true).
  Detached with the 3D still under our root, it took `objRoot`'s first child instead, the
  temporary node manager (`TES+0x98`). The game then crashed in its update (`NiAVObject::Update`
  from ID 13292).
- **Interior buffer** (`TES+0xC8`): detaching puts a cell at its front, and the one falling off
  the end is unloaded by ID 13721. ID 13721 has about 25 callers, and a cell we still hold must
  not go through any of them: hooked at its entry (`RemoteCell::InstallGridHook`). A held cell
  unloaded the same way crashed in ID 13292.
- **Persistent references** (a town's houses, porches, paving) belong to the worldspace's
  persistent cell. The grid hangs their 3D under the cell they stand in, and takes it off some
  frames after the player goes inside.
  - Loaded at game start, the save has them with 3D.
  - Loaded indoors after the player was outdoors in the same session, those around the door
    have no 3D, so there is no collision under the door either. `RemoteCell::PersistentIn` loads
    them, but not doors.
  - 3D loaded while its cell isn't attached (`Load3D`, also `Set3D` again) hangs nowhere, and the
    engine's later attach of the cell leaves it so: it hangs only the 3D it loads itself. ID 19165
    (cell, ref, true, false) hangs one reference's 3D into its cell, as the cell attach does for
    each (ID 18976 → ID 19187); the cell must have its 3D.
  - Walking out once, Markarth's ground statics (`MrkDocksideLand01`, temporary references of
    MarkarthMines) became persistent references of a runtime cell `FF000A96 'Wilderness'`, whose 3D
    hangs under `TES::objRoot` outdoors. Why isn't known.
- **Land meshes** are built only when a cell is loaded into the engine's own grid (IDs 13285 and
  13427 call it right after 19063). A remote cell needs it called itself (`RemoteCell::AttachOne`).

## Simulation

- **High process:** `Actor::MoveToHigh()` on a remote cell's actors is accepted, and they aren't
  demoted while the cell stays attached.
- **Havok:** the engine steps only the current space's world (`*ID 400440`, in the jobs
  ID 18870 / 18897), with `bhkWorld` vfunc 0x32 (arg 0).
  - Stepping a second world from the main thread, beside that job, corrupted memory: it is
    stepped in the same job, right after the engine's.
  - With Precision loaded, that froze the game; it is stepped at the end of the frame instead.
  - `bhkWorld+0xC701` is the active flag, read only by the step.
- **Collision filter:** `bhkCollisionFilter`'s pair filter is at vtable offset 0x10, its ray
  filter at 0x28. Havok returns `hkBool` through a hidden pointer: a `bool` signature crashed.
- **Line of sight:** ID 37761 (observer, target, mode), called by detection and combat (IDs 39639,
  39640, 41246, among twelve callers). ID 53829 (`HasLineOfSight`) only dispatches to it.
- **Conditions:** `GetInWorldspace` is ID 21475 and compares exactly. Conditions run through the
  script command table (ID 29924 + 0x691 calls the entry), so replacing the entry answers every
  condition.
- **Teleports** onto a point below the land are lifted onto it (ID 37507 + 0xCEA).

## Rendering a second view

- **Frame:** `Main::RenderPlayerView` (ID 36559) → shadow maps → `RenderDepth` (107139) →
  `RenderShadowmasks` (107140) → `RenderWorld` (107142, executes the accumulated draw lists) →
  `RenderFirstPersonView` (107129) → `RenderWaterEffects` (36560) → post.
- **The main camera** is the world-root camera (ID 403513, camera at +0x128). `RenderPlayerView`
  pushes it into every light scene's culling process (ID 415016 count, ID 415022 array) and calls
  ID 107132 (camera).
- **Second views in the engine:** the local map (ID 16335 on), menu and inventory 3D (ID 52727),
  the cubemap camera (ID 107564 on), water reflections (the world a second time, with sky and LOD).
- **The menu-3D recipe** (ID 52727), which the portal pass uses:
  1. Culling context {accumulator, camera, scene}, then `BSCullingProcess::Process`.
  2. Set the render and depth targets: IDs 77453 / 77454, with the RenderTargetManager object
     `&ID 411451` as `this`.
  3. `Renderer::SubmitAccumulator(camera, accumulator, flags)`.
  - The accumulator (`BSShaderAccumulator::Create(0x63)`) must have an `activeShadowSceneNode`,
    or the lighting shader's `GetRenderPasses` crashes while culling.
  - It must be emptied after each submit, as `RenderPlayerView` does (ID 106597
    `ClearItemsList`, ID 106600 (acc, 6) and (acc, 5)), or stale passes crash the batch renderer.
  - ID 77386 (false) afterwards marks the engine's state cache dirty.

## Light scenes

- **Our own light scene:** a `ShadowSceneNode` built like the engine's (constructor ID 106320, on
  the engine heap) and registered in `BSShaderManager::State` slot 3 with ID 105635. Slot 0 is
  the world, 1 menu 3D, 2 and 3 are free. It must be registered: shaders look scenes up by
  index (ID 105637, slot → fog property), and an unregistered one crashed the first frame.
- **A cell's lights:** the attach puts them into the world light scene. They are moved into ours
  (`LightScene::Adopt`). On release, call `RemoveAllLights()` on ours, or walking into the cell
  registers every light twice.
- **Lights are culled and faded against the main camera only.** For another view set
  `BSLight::frustrumCull = 0` and `lodDimmer = 1` on each light before the cull.
- **Directional ambient** is one global, set per frame by `Sky::SetDirectionalAmbientColors`
  (ID 105643). Its raw inputs are at ID 414646 (0x48 bytes), the tint at IDs 390997-390999, the
  fresnel at ID 391000.
- **Sun light and fog:** every `ShadowSceneNode` has its own sun light and fog property.
- **A street in the world scene** while the player is indoors registers one of its lights anew
  every frame. Adopting each into our scene crashed in the world scene's queued removal
  (ID 106335 → 106342).
- **Taking in a light costs ~0.15-0.3 ms.** `AddLight` (ID 106327) only queues it; the scene's
  light update (ID 106335, per queued light ID 106342 → 106401, shadow lights 106400) builds a
  ~200 KB `BSParabolicCullingProcess` for it and walks the scene for the shapes it lights
  (ID 108282, pruned by `worldBound`). It dedupes by `BSLight::light` (the NiLight), not by
  BSLight. The patched Community Shaders still uses the world scene's per-shape lists (without
  them a room came out ~4% darker); our scenes don't need them.
- **Arriving in a cell queues each of its lights twice** in the world scene (Understone Keep: 306
  for 153). The update takes in the first and, finding the second's NiLight active, walks the
  scene for it again. The teleport (ID 40744 + 0x3D2) and every frame (ID 106337 + 0x2B) call the
  update; after a seamless trip `Travel.cpp` lets it take 32 lights at a time, nearest first.
- **`ShadowSceneNode::SetPortalGraph` (ID 106385) with a different graph drops every light**
  (`RemoveAllLights`) and both queues. The engine's cell attach (ID 18976 + 0x140) calls it on
  the world scene with the attached cell's graph (`AttachGraphHook` skips it for our attaches).
  It also frees the old graph's list of occlusion shapes (`BSPortalGraph` + 0x10): before the
  skip, every remote attach left the street without its occlusion planes.
- **A switch node's hidden child** (`NiSwitchNode`, a coin bag or mushroom picked or not) is never
  updated: its world transform stays at the origin, and the switch node's bound takes it in.

## Culling

- **The main view's cull** (ID 36643) doesn't walk the scene. It hands each loaded cell's 3D to
  jobs slot by slot, leaving out:
  - slots 0, 1 and 4;
  - the first child of slot 6, an interior's "Shared Portal Geometry", culled through the portal
    graph only.

  `NiNode` and `BSMultiBoundNode` OnVisible aren't called for those nodes. `BSMultiBoundRoom`'s
  (ID 75886) is.
- **Rooms and shared nodes:** a `BSMultiBoundRoom` (ID 75886) or `BSPortalSharedNode` (ID 76448)
  culls its children only when the process's portal graph lists it or the cull mode is kAllPass /
  kIgnoreMultiBounds.
- **A portal's shared geometry:** `BSPortal::portalSharedNode`; the portals are in
  `BSMultiBoundRoom::portalList`, the rooms in the cell's `loadedData->portalGraph->rooms`.
- **A portal's rectangle:** `BSOcclusionPlane::corners` is all zeros in a loaded cell. The
  rectangle is `translation + rotation * (±size.x, 0, ±size.y)`, in the cell's own coordinates
  (Understone Keep's and Mistveil Keep's portals; size taken as half extents, which is at worst
  too big). `entranceRoom` / `exitRoom` are the rooms on either side; rooms have no names.
- **A room's `worldBound`** can be huge (Understone Keep: radius ~93000, centred half way to the
  origin); its multibound (`QPointWithin`) is the one to test points with.
- **Objects outside the rooms** (`BSPortalGraph::alwaysRenderChildren`) hang nowhere in the scene.
  The scene's cull with the graph (ID 106322) culls them one by one.
  - 3D finished after the cell's attach waits in `pendingAlwaysRenderAdds`. It is taken in only
    while the graph is the scene's, so in a cell attached behind a door it never is: most of
    Riften's jail, Mistveil Keep's dais steps.
- **In an interior with a portal graph,** the main view tests what lies outside the rooms against
  the view through the rooms' portals (the process's compound frustum; kIgnoreMultiBounds skips
  that test). A street hung outside the rooms isn't drawn.
  - Listed as always-render with the graph, it is drawn, but then it is also in the sun's shadow
    map, which crashed (EngineFixes' alpha test fix, a pass without a shader).
  - Hung under a room, it is culled with that room's view.
- **Dead ends found while looking for the interior's cull:**
  - The ID 415022 loop isn't a cull: ID 76557 only clears each process's collected geometry.
  - The cull in `RenderDepth` (ID 107139 + 0x710) is the player's skeleton.
  - `ShadowSceneNode` vfunc 0x34 (OnVisible) is never called for the world scene indoors.
- **Occlusion:** the per-object cull (ID 76553, a culling process's vtable slot 0x16) tests an
  object against the process's compound frustum (ID 76821), which holds the occlusion planes and
  boxes of the world scene's graph, unless the cull mode is kAllPass or kIgnoreMultiBounds; then
  only against the view (ID 76564). A room merged behind a street's cliff was hidden by them
  through its doorway (`MergedCull` culls merged cells ignoring multibounds).
- **The main view's culling process** is a `BSGeometryListCullingProcess` (vtable ID 196460, the
  process in ID 415022), not a `BSCullingProcess` (vtable ID 238422): a hook on one vtable misses
  the other.
- **`BSCullingProcess::AppendVirtual`** (vtable slot 0x18) is hooked by SkyrimPrecombines (Faster
  Objects); the original is ID 76556.

## The outdoors seen from indoors

- **Distant LOD survives indoors:** Tamriel's terrain manager stays active with its LOD loaded.
  Its land, object and tree nodes are only culled (`SetAppCulled`).
- **The sky stays in the world scene** in dome-only mode (`Sky::mode` 2).
- **The street can stay attached** when the player goes in (the grid's nine cells).
- **`BGSTerrainManager` layout:**
  - `updateNodes` are the level-32 nodes.
  - A node's `children` (+0x30) points to its 4 children inline, 0x50 apart, not to pointers.
  - The low 12 bits of `nodeState` hold one nibble per layer: terrain 0x00F, objects 0x0F0,
    trees 0xF00. The values are 2 none, 4 shown, 8 split into the children.
  - Object layers live on the level 16, 8 and 4 nodes, so a level-32 node without one is normal.
