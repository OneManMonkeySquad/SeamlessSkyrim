# SeamlessSkyrim — architecture

Goal: walk through a load door without a loading screen, and see the other side through the open
doorway before you cross — with that side's own lighting and with its NPCs moving. Interior cells
stay where they are; nothing is merged into the exterior worldspace.

"Door portal" below means our see-through doorway. It is unrelated to the engine's own
`BSPortalGraph` (room/portal occlusion inside interiors), which we will *use* for culling.

## The engine constraint everything follows from

Skyrim runs one "current" space: either one interior (`TES::interiorCell`) or one exterior grid
(`TES::gridCells` + LOD). Rendering, lighting, AI processing, Havok stepping, sky and weather are
all keyed to it. Cells we leave are not necessarily freed: `TES::interiorBuffer` / `exteriorBuffer`
keep recently visited cells resident. The whole design is "keep a second space alive and render it
through a hole", so every subsystem below is about making the engine tolerate a second live space.

## Subsystems

### 1. Door link graph (data, low risk)
At `kDataLoaded`, collect every reference with `ExtraTeleport`, pair it with its linked door, and
precompute the portal transform `T = Linked * Rot180 * Source⁻¹`. Anything seen through door A is
the other side rendered with camera `T * mainCamera`.
- Aperture size from the door's bounds, with a JSON override file for doors whose mesh lies
  (gates, cave mouths, hatches).
- Excluded and left vanilla: trapdoors, ladders, doors leading to a different exterior worldspace
  until subsystem 4 handles two worldspaces (city gates, e.g. Whiterun).

### 2. Residency (spike S1)
Keep the cell behind the nearest relevant door loaded and attached (3D built, Havok world alive)
while the player is near and facing it, and stop the engine from detaching the cell the player just
left. For interior → exterior doors this means keeping the exterior grid around the linked door
(and LOD) resident while inside — the most expensive case.

### 3. Simulation of the other side (spikes S2, S2b)
Target: the other cell's actors stay in **high process** — full AI packages, animation graphs,
character controllers in that cell's own `bhkWorld` (interiors already have one each).
- Hook the engine's process-level promotion/demotion so actors in the portal cell are not demoted
  for being "far" from the player.
- Step the portal cell's `bhkWorld` each frame.
- Detection does not cross the door portal in v1 (no shared Havok world → no line of sight).
  Portal-aware detection is a later feature.
- **Fallback if S2 fails:** "puppets" — actors stay in low process (the engine still moves them
  along their package schedules); we render clones of their 3D and drive locomotion and idle
  animation ourselves. Lower fidelity, far less engine surgery.

### 4. Portal rendering (spikes S3, S4 — the largest part)
Per visible door portal (start with one), each frame:
1. Camera = `T * mainCamera`, oblique near plane on the door plane, viewport limited to the
   aperture's screen rectangle.
2. Cull the other cell's scene root with that frustum (plus the interior's `BSPortalGraph`).
3. Render it through the **normal world pipeline** into an offscreen target, using the other cell's
   lighting: its point lights and shadow casters, its `INTERIOR_DATA` (ambient, directional,
   directional-ambient cube, fog) or lighting template; sky and sun if the other side is exterior.
4. Composite into the main frame through a stencil mask of the aperture, with correct motion vectors
   so TAA/upscalers don't smear it.

Precedent in the engine: the water reflection pass already renders the world a second time from a
different camera into a texture with a clip plane. RenderDoc captures of that pass are the starting
point for finding how to drive a second world render.

**Community Shaders** owns the lighting path (clustered lights, deferred composite, screen-space
effects), so a correct portal pass needs CS's light gathering and composite to run for the portal
view with the portal's light set. Realistic options:
- R1 (start here): offscreen portal render at reduced resolution, screen-space effects off inside
  it, composited as a texture. Isolated and debuggable.
- R2 (later, if R1 is too slow): render into the main G-buffer with a per-pixel scene ID so one
  deferred pass handles both light sets. Requires changes inside CS — as a CS feature/fork (GPL-3)
  or in coordination with its developers.

### 5. Threshold crossing (spike S5)
When the camera crosses the aperture, swap roles: the portal cell becomes current and the old one
becomes the portal. Hand the already-attached cell to the engine as the new current cell (no reload,
no `LoadingMenu`), keep camera position/velocity/facing continuous through `T`, and let the engine
swap sky, weather, imagespace, music and ambience.
- Load doors don't open in vanilla; both doors must animate open and close together.
- Collision at the threshold: the player's capsule can't reach the door plane, so the swap fires
  inside a trigger volume in front of the door and places the player at the mirrored point. Per-door
  geometry (doors flush with walls, hollow exterior shells) needs a door-type config.
- Followers and NPCs still use vanilla door teleports; they'll vanish on one side and appear on the
  other, which the portal now shows. Making that continuous is polish.

### 6. Persistence and scripts
Saving with two cells attached, `OnCellAttach`/`OnLoad` firing for the portal cell, and quest
conditions like "player is in location X" all need checking. Probable approach: detach the portal
cell before a save, and keep "player cell" semantics strictly tied to the current cell.

## Spikes, in order of risk

| # | Question | How we answer it | If it fails |
|---|----------|------------------|-------------|
| S1 | Does the engine keep left cells resident, and in what state? | `Probe` log (this build) | We load the portal cell ourselves |
| S2 | Can actors of a non-current cell stay in high process, animated and physical? | Pin a test cell attached, hook process demotion, watch the log + a debug view | Puppet fallback |
| S3 | Can we drive a second world render with our own camera + root into a texture? | RenderDoc of the water reflection pass, then a hook that renders a detached node | Our own minimal renderer for the portal (very costly) |
| S4 | Can that render use a different light set through CS? | Prototype inside a local CS build | Flat-lit portal until resolved with CS |
| S5 | Can an already-attached cell become current without a reload? | Hook the door teleport path | Shortened load screen as fallback |

## Findings

### S1 (Whiterun: Dragonsreach → exterior → Bannered Mare)
- A cell the player leaves drops straight to `DataLoaded`: references stay in memory, **3D is
  freed**, and its actors leave high process with no 3D. The buffer gives us data, not a live scene.
- The engine **preloads the data of interiors linked from nearby doors** (`bPreloadLinkedInteriors`):
  Bannered Mare was already `DataLoaded` before its door was used, as were three other Whiterun
  interiors never visited that session. Exterior → interior targets are therefore in memory early.
- Interior → exterior: the linked door is a persistent ref with no parent cell until the exterior
  loads, so the residency manager has to resolve and load that exterior cell itself.
- Consequence for S2: we must drive `DataLoaded → Building3D → Attached` for a non-current cell
  ourselves, and stop the engine from demoting that cell's actors.

### S2 results (AE 1.7.104) — engine-native simulation of a non-current cell works
Player standing outside in WhiterunPlainsDistrict04, Bannered Mare (0001605E) kept alive:
1. `Actor::MoveToHigh()` on its actors: accepted, not demoted again (S2a).
2. `ID 19111(cell)` loads the cell's temp references; `ID 18976(cell, parentNode)` attaches the
   cell under **any parent node** — `TES::SetCurrentCell` passes `TES::objRoot`, we pass a private
   `NiNode` that is not in the world scene graph. State → Attaching, cell3D parented to our root,
   own `bhkWorld` created (S2b).
3. `ID 19063(cell, true)` finishes the attach: state → Attached, actors in high process with 3D.
   Its gate `ID 13308` only checks the cell state (3..7), not "is current cell" (S2c).
4. The engine steps only the current space's Havok world (`*ID 400440`, via jobs `ID 18870/18897`).
   Calling `bhkWorld` vfunc `0x32` (step, arg 0) on the portal cell's world once per frame from
   the main thread makes NPCs walk at normal speed: Saadia 309 units / 4 s, a miner ~80 units/s,
   1388 frames stepped in 20 s, no crash (S2d). Without stepping: 0–54 units in 10 s.

Other reversed pieces: `ID 19130(cell, bool)` = detach (state → Detaching); `TES+0xC8` interior
buffer, evictions via `ID 13721`; `bhkWorld+0xC701` = "active" flag (only read by the step).

5. Soak: 5 minutes / 18,673 stepped frames, no crash (S2e).
6. Release: take cell3D off the private root **first**, then `ID 19130(cell, true)` → state
   Unattached, actors demoted to MiddleHigh; afterwards entering the cell normally works (engine
   re-attaches it under objRoot, NPCs fine). Calling 19130 with cell3D still under our root
   crashed ~3 s later in `NiAVObject::Update` of `TES+0x98` (S2f).
7. Side effect to undo: the attach calls `ShadowSceneNode::SetPortalGraph` with the portal cell's
   room graph, overwriting the current space's (seen in a debug view: the cell's NPCs rendered
   but its room geometry was culled).

Open: save behaviour with an extra attached cell, scripts / `OnCellAttach` events firing for
the portal cell, detection across the portal.

### S3 reconnaissance (rendering)
Decision: **one unified approach for both directions** — render the other space from the main
camera mirrored through the door and composite it into the door's aperture. (Placing the other
scene graph behind the door was considered: single pass and CS-friendly, but it can't work for
inside → outside because terrain/LOD/sky/water are global systems that can't be re-parented.)

Frame structure (names from Community Shaders' FrameAnnotations): `Main::RenderPlayerView`
(36559) → shadow maps → `RenderDepth` (107139) → `RenderShadowmasks` (107140) → `RenderWorld`
(107142, executes the accumulated draw lists) → `RenderFirstPersonView` (107129) →
`RenderWaterEffects` (36560) → post. The main view's camera comes from the global world-root
camera (`ID 403513`, camera at +0x128); RenderPlayerView pushes it into every ShadowSceneNode's
culling process (`ID 415016` count / `ID 415022` array) and calls `ID 107132(camera)`.
Existing second-view precedents: local map (`ID 16335..`, LocalMapCamera), menu/inventory 3D
(`ID 52727`, separate scene graph + camera), cubemap camera (`ID 107564..`), water reflections.

### S3a results — first portal pixels via the menu-3D recipe
`ID 52727` reduces to: `CullingContext{accumulator, camera, scene}` → `BSCullingProcess::Process`
→ set render/depth targets → `Renderer::SubmitAccumulator(camera, accumulator, flags)` (Submit =
camera data + accumulator `StartAccumulating` + `FinishAccumulatingDispatch` + post-resolve).
Our version (`src/PortalRender.cpp`), run from the Present hook after the frame is complete:
- camera: `Main::WorldRootCamera()->Clone()`, world = `T · mainCamera.world` with
  `T = W_linkedDoor · Rz(180°) · W_sourceDoor⁻¹` from the doors' 3D world transforms;
- accumulator: `BSShaderAccumulator::Create(0x63)` held by a ref-counted owner; it **must** get an
  `activeShadowSceneNode` (borrowed from the main accumulator for now) or the lighting shader's
  `GetRenderPasses` crashes while culling;
- targets: `SetRenderTarget`/`SetCurrentDepthStencilTarget` (`ID 77453/77454`) take the
  RenderTargetManager object `&ID 411451` as `this`; colour `kMENUBG`, depth `kMAIN_COPY`;
- `SaveRenderTargetToFile(kMENUBG)` → the Bannered Mare interior, rendered while the player stands
  in the street, from a camera 350 units behind the inside door (mirroring the player 350 units in
  front of the outside door). Geometry, textures, candles and fire all render.
Seen in the image, all expected: geometry between camera and door (the inside door itself) is not
clipped yet; lit by the exterior light scene, so the interior is dark; full-screen pass.

### S3b results — live portal composited into the doorway
Every frame, in the Present hook: render as above (near plane pushed to the linked door, linked
door app-culled for the pass), then project the source door's OBND corners with the main camera
and copy that screen rectangle from the portal target onto the back buffer with DirectXTK
`SpriteBatch` (portal and main view share projection and viewport, so pixels map 1:1), then
`ID 77386(false)` to mark the engine's state cache dirty. The accumulator must be emptied after
each submit (`ID 106597` ClearItemsList, `ID 106600(acc, 6)` and `(acc, 5)`, as RenderPlayerView
does) or stale passes crash the batch renderer.
Result: the Bannered Mare's interior (floor, rugs, fireplace, pillars, chandelier) shown live in
the doorway while standing in the street.
Known crash: as soon as the player moves, a pass dereferences garbage in
`BSBatchRenderer::SetupAndDrawPass`. The inn geometry's cached render passes reference lights
from the *borrowed exterior light scene*, which changes when the player moves. Next step: a
dedicated ShadowSceneNode (light scene) for the portal with the cell's own lights, ambient and
directional colours — which is also what "proper interior lighting" needs. Also visible: the
exterior sun's screen-space shadow mask is applied to interior pixels.

### S4 results — the portal's own light scene
- The private root is now a `ShadowSceneNode` built like the engine's (ctor `ID 106320` on the
  engine heap, capacities 0x100/4/4/0x4000/0x80/0x10) and registered in `BSShaderManager::State`
  slot 3 with `ID 105635` (slots: 0 world, 1 menu-3D, 2/3 free). Registration is required:
  shaders look the active light scene up by index (`ID 105637` = slot → fog property) and an
  unregistered index crashed the first frame.
- The cell attach puts the portal cell's lights into the **world** light scene (all 75 active
  lights there were the inn's while standing outside). `adopt_lights` moves them (BSLight whose
  NiLight is under cell3D) into the portal scene; they arrive as queued adds and are processed when
  the portal scene is culled. Detach must `RemoveAllLights()` on the portal scene, or re-entering
  the cell registers every light twice.
- Result: no more crash when the player moves (walk away / wait / walk back), and the exterior
  sun's shadow mask no longer shows on interior pixels.
- Lighting is still ambient-only: the engine culls lights against the main camera only
  (`BSLight::frustrumCull` 255 for all portal lights, 0 for visible lights when inside); marking
  them visible is not enough — 2213 of 2507 lighting passes in the portal render carry no point
  light (numLights 1), 154 carry one, 140 two.
- Why it matters: our pass runs outside `Main::RenderWorld`, so Community Shaders treats it like
  a menu (forward, "strict" per-pass lights; LLF only builds its clustered light buffer from light
  scene slot 0 with the main camera, and only inside RenderWorld is CS's deferred path active).
  Matching the inside look needs the portal pass to go through CS's world path with the portal
  scene's lights — a CS integration, not engine RE.
- Open leak: light→geometry pairs keep growing (17k after 3 s vs 8k inside).

### S4b results — the inn lit by its own lights
Two fixes together:
1. **Plugin**, before each portal cull: for every light of the portal light scene set
   `frustrumCull = 0` *and* `lodDimmer = 1`. The engine computes both from the main camera; seen
   from the street the inn's lights were culled and faded to 0.
2. **Community Shaders patch** (fork of v1.9.1 at `E:\projects\CommunityShaders-Seamless`, branch
   `seamless-portal`; installed DLL replaces the Vortex one, original backed up in
   `original-1.9.1/`): `LightLimitFix::SetupPortalStrictLights`, called from
   `BSLightingShader_SetupGeometry_After` for every lighting pass. If the pass's light scene is
   State slot 3 it fills LLF's strict light list (max 15) with the portal scene's lights whose
   radius reaches the object, nearest first, converted exactly like CS's own strict path (ISL
   aware, positions relative to the current camera, no shadow flags). It can't live in
   `GeometrySetupConstantPointLights`: the engine only calls that (`ID 107300+0xB30`) for passes
   it already gave point lights. The LLF pixel shader loops over the strict list regardless of
   the engine's light count.
Result: warm fire/candle lighting in the portal view, stable while walking around, composited
live into the door. Still different from being inside: ambient/directional/fog come from the
exterior (needs the cell's lighting template), no CS screen-space effects in the portal pass.

### S4c results — the cell's own ambient and directional light
- Directional ambient is one global, set per frame by `Sky::SetDirectionalAmbientColors`
  (`ID 105643`: colors[3][2] + specular tint + fresnel; CS hooks it). It stores its raw inputs at
  `ID 414646` (0x48 bytes), tint at `ID 390997..390999`, fresnel at `ID 391000`. The portal pass
  saves those, calls 105643 with the cell's DALC (from the lighting template when the cell
  inherits ambient) and calls it again with the saved values afterwards; the exterior is
  unaffected.
- Every ShadowSceneNode has its own sun light and fog property (world, menu-3D and portal all
  differ), so the portal scene's sun `NiDirectionalLight` gets the cell's directional colour
  (template-aware). Direction and fade not mapped yet; fog not applied yet (BSFogProperty colour
  fields unmapped in CommonLib).
- With all lights visible and not faded, the engine assigns 4–16 lights per pass on its own; the
  light→geometry count is stable (~16.7k over 30 s), so not a leak, just higher than inside (~8k).
Result: the portal view of the Bannered Mare is close to the inside look (warm candles and fire,
cooler walls from the directional light). Remaining visual differences: fog, directional light
direction, CS screen-space effects (AO/GI) not running for the portal pass.

### S2 decision
Engine-side simulation (option 1) is the target. If it proves impossible, fall back to the
engine still running AI and pathing at a lower process level while we render behavior-graph-driven
copies (Skyrim Together-style replication), which keeps the engine in charge of positions and
so preserves continuity at the threshold.

## The mod loop — `src/Portal/`
- `LightScene`: the portal's ShadowSceneNode (= private root), registered in State slot 3;
  adopts the remote cell's lights from the engine's scenes every frame (lights appear
  asynchronously after the attach); `PrepareForView` marks them visible/unfaded; `Renew` starts a
  fresh scene per remote cell (reusing one after a release crashed on leftover geometry lists).
- `RemoteCell`: attach (19111 → 18976 under our root → 19063 → MoveToHigh → adopt lights),
  per-frame update (adopt + Havok step), `Healthy` (attached under our root), `FullyLoaded`
  (`refsFullyLoaded`, no queued refs — drawing a cell whose 3D was still streaming in crashed),
  release (cell3D off our root, lights cleared, 19130; if the engine took the cell over, only
  forget it).
- `PortalManager` (per frame from the PlayerCharacter::Update hook): nearest load door within
  1200 units whose other side is an interior → attach; release beyond 1700 units, for a door at
  least 200 units closer, when the player goes indoors, before saves/loads (SKSE kSaveGame /
  kPreLoadGame) and when the player activates the door; walking into the doorway (within 90 units
  of the door centre, facing it) releases and activates the door, i.e. the normal transition with
  its loading screen; a 3 s hold-off afterwards so the cell isn't re-grabbed mid-transition.
- `PortalRender`: draws the active portal once both doors have 3D and the cell is fully loaded;
  fresh accumulator per remote cell.
- Switch: `Data/SKSE/Plugins/SeamlessSkyrim.ini` `[Portals] bEnabled` (default 0); the MCP's
  `portals_enable` toggles it for a session.
Verified in one run: activate at the inn, switch to another house 2200 units away and back,
walk in through the doorway, come back out, autosave release/re-attach — no crash.
Not yet: no loading-screen-free crossing (S5), outside ← inside portals, fog, CS screen-space
effects, exact clip plane.

## Test setup
A small test plugin made in the Creation Kit (with Creation Kit Platform Extended): one exterior
house door, one interior with a few lights (one shadowed) and two NPCs on simple sandbox/patrol
packages, plus one interior ↔ interior door. Every spike runs there first, then on vanilla
locations.

## Tools
- Visual Studio 2026 (MSVC, bundled CMake, Ninja, vcpkg), CommonLibSSE-NG (alandtse fork, pinned
  submodule), SKSE 2.3 / Address Library v5 for 1.7.104.
- RenderDoc for every rendering spike; Community Shaders source for its render hooks and lighting.
- Ghidra or IDA plus x64dbg for reverse engineering; Address Library IDs map functions across
  runtime versions.
- Crash Logger for crash dumps, with the PDB the build deploys next to the DLL.
- Creation Kit + Creation Kit Platform Extended for test cells; xEdit for bulk inspection.
