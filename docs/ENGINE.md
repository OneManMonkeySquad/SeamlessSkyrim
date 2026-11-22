# Engine facts

Skyrim AE 1.7.104, IDs from the Address Library. These were expensive to find. Add new ones in
the same change that needed them.

## SE 1.5.97 against AE 1.7.104

Found porting the plugin; each was checked in the code of both.

- Mostly the same code at different offsets. Where the compiler inlined differently:
  `BGSTerrainManager::Update` (SE 31017 / AE 31802) calls the node placement (31041 / 31828) once,
  where AE calls it three times; SE has the placement's body (a call to 31048 / 31835 with flags
  7 or 0x27, then three vcalls) inlined at the other two places.
- `Renderer::SetRenderTarget` AE 77453 is SE 75646, a wrapper that passes the Renderer object
  (ID 524907, AE 411393) and jumps to 75654. CommonLib pairs 77453 with 75654, which takes `this`
  in rcx; AE's 77453 ignores rcx and loads 411393 itself. Either way the object a caller passes is
  unused.
- Player layout: the actor cull's drawn bits at +0x274 (AE +0x27C); the teleport's queue at
  +0x640 and its valid flag at +0x682 (AE +0x650, +0x692). TES's worldspace at +0x140 (AE +0x148).
- Same in both: fog property fields (+0x38..+0x88), task pool (vfunc 0x80, +0xE0, +0xF0), the
  grid array (+0x10 size, +0x18 cells), ShadowSceneNode allocation 0x308, the Havok step call
  (`call [rax+0x190]` at +0x18 of SE 18440 / 18466), the actor cull's first 19 bytes, culling
  process vtable slots.
- ID 19384 (SE 18972), the reference fade update, takes the reference's 3D
  (`TESObjectREFR::Get3D2`, vfunc 0x70, at every call site), not the reference.

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
- **A door's teleport marker isn't always square to the door.** Solitude's gate, city side:
  48° off for one of the two load-door pairs standing there (00101D9D/00101D9E), 1.4° for the
  other (00037F1B/0001C386). Markarth's Smelter Overseer's House, street side: 17°. Whiterun's
  shops: 4-11°. Most doors: under 1°. The doorway mapping takes each door's facing from its panel
  and only the side from the marker (`Doors::Compute`).
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
  - A street's world (seen from indoors) is stepped at the end of the frame in either mode; in
    the job it was never tried.
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
- **The main view's depth prepass** (`RenderDepth`) draws most statics with the Utility shader's
  shadow map technique (vertex descriptor 0xC000, RenderShadowmap | Clamped), skinned ones with
  RenderDepth (0x2004). The technique doesn't tell a shadow map from the main view; the depth
  target does (`RendererShadowState::depthStencil`, `kSHADOWMAPS` and the like).
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
- **Two-sided at runtime:** setting `kTwoSided` on a shape's `BSShaderProperty` takes effect at its
  next draw. Community Shaders' lighting shader turns the normals of faces seen from behind (fading
  out past ~10000 units), so they are lit as their own side. Load doors' leaves are often modelled
  from the front only (Markarth's Dwemer doors), since their back faced the other cell.

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
- **Tree LOD in an interior's room:** the street's distant tree LOD (`BSMultiStreamInstanceTriShape`,
  shader type 9) hung in a room of the interior came into the sun's shadow map (Interior Sun)
  twice now and then: that batch's pass list then led from its last pass back to its first, and
  `BSBatchRenderer::RenderBatches` (ID 107642, from ID 107133) drew it forever. Its shapes never
  pass through a process's AppendVirtual; keeping the LOD out of the shadow maps' culls, or
  `kCastShadows` off on their shader properties, didn't stop it.
- **The main view's culling process** is a `BSGeometryListCullingProcess` (vtable ID 196460, the
  process in ID 415022), not a `BSCullingProcess` (vtable ID 238422): a hook on one vtable misses
  the other.
- **`BSCullingProcess::AppendVirtual`** (vtable slot 0x18) is hooked by SkyrimPrecombines (Faster
  Objects); the original is ID 76556.

## The outdoors seen from indoors

- **Distant LOD survives indoors:** Tamriel's terrain manager stays active with its LOD loaded.
  Its land, object and tree nodes are only culled (`SetAppCulled`).
- **The sky stays in the world scene**, but drawn only in an interior that shows the sky: there
  `Sky::mode` is 2 (dome only; the Bannered Mare), elsewhere 1 (interior, the sky's nodes culled;
  Understone Keep in the base game). `Sky::SetMode` (ID 26222, SE 25679; `(Sky*, mode)`) sets the
  mode and culls or shows the nodes for it; the engine calls it on entering a cell. A mod can give
  an interior the sky (Understone Keep had it in the AE test load order).
- **The street can stay attached** when the player goes in (the grid's nine cells).
- **Animated references are run only in the player's cells.** TES (ID 13325) does it each frame
  for the interior the player is in, or else for the grid's cells. A cell's animated references
  are `LOADED_CELL_DATA` +0x40 (added on 3D load by ID 19419 for references whose 3D has the BSX
  "animated" bit, removed by ID 19420), filled in any attached cell. Per frame the engine runs
  their animation graphs (ID 18900, `UpdateAnimation`) and, through ID 18891 / 18875, ID 18894:
  the fade check, a visible-room check, then `UpdateSelectedDownwardPass` of the 3D with the
  frame's time (ID 400333) and flags 2, which runs the controllers and so the particle systems.
  ID 18894 skips the references of a room merged into the street; the selective update on its
  own runs them (`RemoteCell.cpp`, `AnimateReferences`).
- **`ShadowSceneNode::particleNodes`** (+0x1C8) is only a queue: the light update (ID 106335)
  pairs each new particle node with lights and empties it every frame. It doesn't animate.
- **`BGSTerrainManager` layout:**
  - `updateNodes` are the level-32 nodes.
  - A node's `children` (+0x30) points to its 4 children inline, 0x50 apart, not to pointers.
  - The low 12 bits of `nodeState` hold one nibble per layer: terrain 0x00F, objects 0x0F0,
    trees 0xF00. The values are 2 none, 4 shown, 8 split into the children.
  - Object layers live on the level 16, 8 and 4 nodes, so a level-32 node without one is normal.
