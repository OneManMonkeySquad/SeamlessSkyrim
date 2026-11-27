# Known issues

Open bugs and things not tried. A fixed one comes out.

## Crashes and hangs

- SE 1.5.97: `coc MarkarthUnderstoneKeep` from Markarth's street, with the keep merged into the
  street (walked in and out just before), hung the game during the load (once; log ends at the
  `coc`). Not tried on AE.
- AE: `place_go` (`coc RiftenBeggarsRow`) from Riften's street just after walking out of Beggar's
  Row hung the game during the load: no CPU, every thread waiting; the log ends with the room's
  door opened again (`Doors::Open`) after the release. Again 2026-11-26 (Bottled Shaders fork):
  `place_go bannered-mare-door-out` (`coc WhiterunBanneredMare`) a few seconds after walking out
  of the Bannered Mare; the log ends with the release and the `coc`. The thread that ran them waits
  (stale frames near ID 76561, culling); the frame thread waits in HDT-SMP's end-of-frame hook
  (`Hooks::MainHooks::Unk_sub`, under `EndOfFrameHook`); every other thread idle. Twice now.
  Also on SE 1.5.97 (no HDT-SMP), twice on 2026-11-26: `place_go bannered-mare-door-out` a few
  seconds after walking out of the Bannered Mare, 0% CPU. From the Tamriel exterior it loads.
  Again 2026-11-27 (Open Shaders fork, AE): the frame thread waits for the frame's jobs (ID 35409
  under `EndOfFrameHook`), one job thread waits in ID 39068 (on ID 69516, event 0xD).
- A hang right after walking out of Arcadia's (2026-11-16, agent game, once in ~17 exits): the main thread waiting for the frame's jobs (ID 35409), one job thread (an audio job, ID 39080 -> 91863) spinning in EngineFixes' `ScrapHeap::Allocate` replacement (ID 68173 -> 68144). Not reproduced; not known whether SeamlessSkyrim plays a part.
- Pointers overwritten with floats (2026-11-27, AE 1.7.104): three crashes read a pointer that
  holds two floats (0x3F039F20'3F36DF34 and the like). Loading a save right after
  `whiterun-gate-run-back`, in the Papyrus VM's load (SkyrimSE.exe+148EF80, `mov eax,[rdi+28h]`;
  also at 11:33 the same day); walking out through Whiterun's gate, a refcount on a freed object
  in a scene walk (ID 70520+0x50, under 0EE33DE; also at 08:48); and the Winterhold `coc` crash
  below. Something writes floats (a transform?) into memory freed and reused. Not found yet.
- Walking into Understone Keep in the suite (`understone-keep-walk-in-out`, right after the smelter
  house's route): the game was gone once, with no crash log and the plugin log lost to the
  relaunch. Not again in 4 runs of the same two routes. `run_suite` now keeps the log.
- Crash in the portal pass (2026-11-23, after walking back and forth through Riften's south gate):
  `BSEffectShader` setup (SkyrimSE.exe+155757A, ID 107527+0x4EA) reads a light of the pass whose
  `NiLight` is gone (`BSLight::light` null), drawing the street behind the gate on the first frame
  it was ready. Not reproduced in about 30 trips since.
- Saving releases the spaces behind doors from the save's own thread (`kSaveGame` arrives off the
  main thread: `released: saving` logged by another thread than the frame's), while the frame's
  culling jobs may still walk them. The game crashed once that way (user, Windhelm's Palace of the
  Kings, 0.2 s after the upstairs behind its door was attached, its references still loading): in
  a culling job, `BSCullingProcess::Process` (ID 106921) reading a null object (ID 76561 + 0x56).
  Not reproduced by saving 0.05-0.4 s after an attach of the same door.
- Crash in an animation job (2026-11-27, Open Shaders 2.23.1): a `BShkbAnimationGraph` updated
  with its vtable null (SkyrimSE.exe+0BCF258, `call [rax+140h]`, 1.7.104), a few ms after
  `released: a closer door` let go of WindhelmBloodworks (10 actors, 9 with 3D) merged into the
  Palace of the Kings and the upstairs was attached in its place (`place_go
  windhelm-palace-upstairs-door` from Whiterun). Not again on the next run of the same steps.
- A user's game (0.7.0, AE 1.6.1170, stock Community Shaders 1.9.1, CS Water Mod with its
  WaterWind and WaterWade plugins, 139 plugins), "often, when entering different buildings":
  crashes attaching a street's water. The one log sent (2026-11-26) is from after he added
  `WaterReflectionsEngineFix.dll` 0.9.2-beta: in the material lookup (ID 107719 + 0x21D, from
  `AddWater` + 0x3D0), a table entry holding a freed material. That plugin deletes a reflection's
  material itself at its last reference, which leaves the entry (read from its source, not
  reproduced); it isn't needed beside SeamlessSkyrim. His crashes before it: no log. A reflection
  left with a freed material was found and fixed after this (`WaterReflections`); not known
  whether that was his crash.
- AE 1.7.104, once in three runs of `coc` between Winterhold's street and its houses (2026-11-27,
  on `coc WinterholdTheFrozenHearth` from the street, the inn merged): crash on a Havok job thread
  in Precision's `ActiveActor` destructor (`PrecisionHandler::Clear`), destroying a `BSFadeNode`
  whose extra data array (ID 70520 + 0x50) holds floats for a pointer. Not reproduced in 40 more
  such trips.
- A user's game (0.5.0, Maids II) hung minutes after an automatic door fired from indoors and
  left the Mist menu open. The menu is fixed; the hang wasn't reproduced.

## Looks wrong

- The skylight (the indoor stand-in for the sun) in The Drunken Huntsman turns off when seen from outside (jarring).
- Walking out of the Bannered Mare at dusk (`bannered-mare-walk-out`, 19:30): the sky and the
  distance turn bluer in the frame of the crossing (sky ~(122,93,98) to (144,124,165) in sRGB, the
  houses' blue ~44 to ~90); the street's surfaces match since the street gets IBL indoors. The tone
  blend is continuous there (64% to 57% indoor weight) and turning IBL off doesn't change it.
  Suspected: the sky's fog. Indoors `Sky::fogNear/Far/Power/Clamp` hold the interior's
  (27853, 163840, 1, 1), outdoors the weather's (0, 150000, 0.615, 0.92); not tried.
- With a doorway in view the view jerks back and forth every ~100 ms, "like network lag" (user;
  also in a user's video of an older version on the native backend, at Belethor's from the
  street). In the video, standing still, the screen cycles through the same three images for over
  a second, each a different camera position, as if the swap chain's buffers were presented
  without the world drawn into them. Not reproduced: the November 22 build on SE 1.5.97 (native
  backend) after several walks through Belethor's and Arcadia's and the city gate, nor the
  current build on AE with the fork, at 60 fps.
- The street's particles (Riften's falling leaves) drawn inside an interior it is merged into, in front of the doorway too (Mistveil Keep's hall, looking out). Cause not looked into.
- The street merged into Riften's jail right after loading its save: the frame of the merge is drawn nearly black, the room included (seen with and without the persistent references' pass). Cause not looked into.
- While a doorway's picture is being captured (up to ~3 s before a room is merged), every capture frame shows the previous frame's picture in the doorway: the view lags a frame while it lasts. Drawing the live pass as well on those frames would fix it, at about double the portal cost while capturing.
- Whiterun's distant objects and trees gone right after walking out of Belethor's (the user had been in and out of it and the Bannered Mare a few times): every LOD node's object and tree layer state 2 (none), the land's normal, the mountains bare low LOD land until a game is loaded. Not reproduced in ~30 walk-outs of either of its doors (front and back), quick or after waiting inside.
- Looking out of an interior built of rooms (Mistveil Keep at Riften), the street's distant tree
  LOD is hidden: there it came into the sun's shadow map twice now and then, the batch list looped
  and the game hung (ENGINE.md, Culling). What adds it twice isn't known.
- Walking out of the Smelter Overseer's House (Markarth's mines), first time after loading the
  save: a flat grey wall, like distant LOD, drawn over the pipe and the city behind the house's
  corner. In each of 22 full suite runs (22:05–23:03), then in none of 7 runs after; cause not
  looked into.
- Loading the Smelter Overseer's House's save again 0.4 s after the street had been attached
  (a launch with the save, then the route's own load): 41 of the merged street's shapes drawn at
  the street's own place, not the room's. Once; not in 5 loads after. Cause not looked into.
- Seen from indoors without Sky Sync, the street's sun direction and colours are those of the
  moment the player came in, for half a game hour; then they jump to the computed ones
  (`PortalRender.cpp`, `RecentOutdoors`).
- Through Solitude's gate from the city: the snowy mountains behind the fort, seen once outside,
  aren't there; the city's worldspace's own distance shows where the portal pass drew sky.
- Walking out of Belethor's (`belethors-walk-out`): one frame just after the crossing drawn almost
  white, as if overexposed. Once in two runs. Cause not looked into.
- A room seen from across a sunlit street gets the street's sky light through its doorway but no
  patch of sunlight on its floor (the sun's shadow at the doorway isn't known to the room's shapes).
  With the native backend (no Community Shaders) it gets neither that light nor the street's fog in
  front of the doorway; only its dimming.
- A room seen from the street is dimmed by its doorway picture's brightness against the view's; a
  room merged beside the shown one without a picture of its own takes the other pictures' mean.
- A door pair placed at two sizes is evened out only for the street seen from inside (drawn
  larger, `PlacementOf`). Between two interiors (now merged, `bMergeInteriors`) not looked at
  again; with the portal pass the far side's wall showed in a band inside the larger doorway.
- Walking out of Understone Keep (`markarth-understone-walk-out`): in the last frame before the
  crossing the guard beside the door was drawn without his head, and the banners on the pillars
  were missing from inside throughout. Not compared with a build before the street was scaled.
- Through Solitude's gate near sunset (18:24 to 18:36) the tops of the trees beside the cliff and
  behind the ruined fort catch the sun in the gateway and not outside (their lower parts, the
  ground and the cliff tree's needles are shaded in both). Not known what shades them outside: a
  caster beyond the portal's nine cells, distant LOD (not in the portal pass), or the gateway's
  shadows being the soft copy (Volumetric Shadows' VSM, 512 texels a cascade) where the world has
  its crisp shadow mask. The portal's own shadow pass costs about 1 ms of the main thread.
- The street merged into an interior: the land of some of its cells is left at the street's own
  place, not moved to the doorway (`scene_check`): loaded inside The Drunken Huntsman
  (`SeamlessTestDrunkenHuntsmanInside`, 3 of 9 cells), walked into Warmaiden's
  (`warmaidens-back-door-walk-in`). Not looked at on screen.

## Doors and crossing

- `place_go` out of the Smelter Overseer's House with the street merged into it: `coc Wilderness`
  left the player in the house's cell, then moved to the street's coordinates and falling. Once.
- Outdoors with few doors around, a fourth space coming within reach halves the reach
  (`sparse`), and every kept room beyond the shorter one is let go in the same frame, as is a
  city gate's view beyond it.
- Walking into Whiterun through its gate from outside (load `SeamlessTestWhiterunGateOutside`,
  walk to (19900, -7424); also right after walking out), the player was carried straight back out
  about 3 s later: the arrival point is 40 units past the city-side gate's plane, and once the
  view back out is attached, `Approach` crosses at depth >= 0 without the walk being towards the
  doorway. Seen on Nov 24; that condition is still there. Possibly gone: in
  `whiterun-gate-in-out-se` (SE, Nov 25) the player stayed inside 6 s and then walked out.
- A door's 3D rebuilt while held open is noticed at the next `Doors::Open` on it; the door the
  player came out of is opened once and waits for another opener.
- Sven and Hilde's house door in Riverwood (01CBB0): its hand turn (-1.6°) was made against the
  door's teleport marker and not converted when the doorway took the door's panel instead (the
  door was locked in every save at hand). The others were.
- Standing at Solitude's gate in the city (the Battle for Solitude save), both gates lost their 3D
  once, 25 s after walking up, and the portal stopped drawing ("a door has no 3D"); the city behind
  the gate showed. Once; not after a reload. Cause not looked into.
- `coc` out of Solitude's Hall of the Dead with the street merged in: the portal was let go, then
  attached to the Hall's door again before the load, and stayed active and merged in Belethor's
  until a second `coc`. Once.
- Whiterun, after walking into Arcadia's Cauldron and out, then saving at night ~600 units away:
  Arcadia's and Belethor's doors stood open on the house's backing panel, with no portal activated
  again, even at the door; seven other doors stuck as open stand-ins since the walk out. Not
  reproduced with a console save after the same walk; how the user saved isn't known yet.
- Riften's door to the docks (000422AD, portal switched off): walking back in from the docks, the
  player stops 31 units short of the doorway at 000FE264, a reference without a model in
  RiftenCityWest that `RemoteCell::ClearPassage` leaves in the passage (it goes by the 3D's bound;
  not confirmed to be why).
- Riften's north gate: with the small door beside it (00078EDA, portal switched off) on as well,
  that door keeps the portal and the gate stays shut, even standing at the gate (both lead into
  the same cell; `kSwitchMargin`).
- Shutting a door by using it while standing in its doorway swings it shut onto the player.
- Beggar's Row's street door in Riften, opened and shut quickly many times (the user): then the
  doorway couldn't be walked into, three tries. Not reproduced by the user afterwards, nor in
  ~10 tries of 8–10 uses 0.05–0.6 s apart, from 100 units off or standing in the doorway.
- A door opened for the first time, the space behind it loaded: the user saw it swing open twice.
  Not seen in frame captures at Beggar's Row's street door, opened before its room was there
  (stand-in, merged 4 s later) or after; nor at Elgrim's Elixirs' door.
- A locked door opened with its key: the engine's own trip with its loading screen (only a picked
  lock is caught: the door then opens). Not tried.
- Through a city gate the view back takes about 5 s (the purge after the trip, then the load):
  walking back through it sooner, the player is held in the gateway until then.
- College of Winterhold, Hall of the Elements: with the door up to the Arcanaeum (000175DD) used and
  open, the walk in (`winterhold-arcanaeum-walk-in`) stops at x 320 instead of going up the stairs;
  the route predates doors opening only when used and doesn't use the door.
- Automatic load doors (cave mouths; ENGINE.md, Doors) aren't seamless: the engine takes the
  player through with its fade, as without the plugin (`gromms-pass-autoload-walk-in`).
- An automatic door of a space shown behind a doorway is kept from firing
  (`helgen-keep-street-autodoor`); the engine counts the player as near it all the same, so one
  walked through that doorway already within ~350 units of such a door isn't sent through until
  they step away and back. Not tried.

## People, quests and loose things

- Markarth: the market attack at the start of The Forsworn Conspiracy played again, not every
  time (user). Not reproduced: no save from before the quest. Possibly the same lost-item bug fixed with
  `belethors-dropped-item-out-and-in`, or the purge of the city's cells after a gate trip
  (`Travel.cpp`, `PurgeBufferedCells`).
- Physics objects and arrows are not carried through a doorway.
- Physics objects can break.
- Whiterun's hanging shop signs: torn off and fallen on walking out of Belethor's, twice in ~30 walk-outs (`belethors-walk-out`; once right after loading the save coming from Riften); cause not found.
- NPCs opening a shut door (`DoorUse`, from NpcTravel's door idle hook) seen in the log only
  (Whiterun's market with every door forced shut: they open, NPCs walk through), not watched.
- NPCs walked through the Bannered Mare's doorway from the street (`bannered-mare-npcs-evening`
  with capture): one vanishes a step past the door plane while the one beside it, not yet past,
  is still drawn (`MergedOverrides` hands it to the room there). Seen before the walk changes too.
- An NPC following close behind another through a door can be moved in by the engine from more
  than 200 units out (`kDoorRange`): not carried, it vanishes from the steps and appears at the
  marker inside. Once (Brenuin) the engine moved it straight back out, where it turned round and
  walked in again by the door. Seen once each.
- The first NPC walked through a door after a load can still stand 1.5 s on the marker before
  walking on; the others go on within 0.25 s.

## Slow frames

- Understone Keep seen from Markarth's street: frame ~13.7 ms, ~9.6 ms with it hidden. Left: the game drawing it, the fork's light setup per shape with `LightOcclusion::Visibility` (~1.5 ms), our cull (~0.5 ms).
- Walking into Understone Keep: the first frame takes ~110 ms (the teleport ~37 ms, releasing the keep's merge ~15 ms, attaching the street ~16 ms, merging it ~5 ms), the next ~50 ms. Walking out: ~115 ms (the teleport ~25 ms, attaching the keep ~16 ms, the player view ~40 ms), then for a second every other frame ~55 ms (the light-occlusion rays catching up).
- Walking out of Riften's jail into the street, the first time after loading the save: the first frame outside takes 95-185 ms (the teleport 70-100 ms), then a few frames of 50-180 ms. Before the grid was prewarmed, moving the street back out of the jail was 50-270 ms of it, nearly all of it Havok moving the street's bodies (ENGINE.md, "Moving a cell's 3D moves its bodies too"); not split up again since.
- Walking out of Belethor's the first time after loading the save: the first frame ~80 ms (the teleport ~60 ms: the grid cells' attach, and ~20 ms under po3_LightPlacer's hook at ID 19411; not checked whether that is Light Placer's own work or the cell change event's other sinks below it, see ENGINE.md, "A mod's hook in a profile").
- Walking out of Arcadia's Cauldron (2026-11-16 build, 8 exits): in 2, the third frame after the crossing took 360-480 ms; profiled once (229 ms), 80% of it waiting in `Present` (graphics card or frame generation), not in the game's code. Not looked at since.
- Persistent references without 3D whose cell has no loaded data (still filed under the worldspace's persistent cell) are loaded on the main thread, not queued (`RemoteCell::PersistentIn`): 71 of them around Riften's jail, ~30 ms of a 50 ms attach.
- In the Bannered Mare with the street merged, the world scene's light update takes ~4 ms every frame (`perf`, `lights: world`). Not looked into.
- The street merged into an interior stays merged until it is let go, also far from the door
  (2400 units in an interior with few doors): the cost `closeBy` in `Manager::Update` is there
  to avoid (2.6 ms a frame in Honningbrew Meadery's cellar) is only avoided coming from afar.
  Nothing takes a merge back short of letting the space go.

## ENB, the native backend and other game versions

- Open Shaders 2.23.1, walking out of the Bannered Mare with the street merged in (13:00, clear):
  the first frame outside (~200 ms long) is drawn without the sun, the street dull and dark, then
  lit. Every walk-out, also the second in a session; not Skylighting (off: the same), not Sky Sync
  (off: the same). Stock Open Shaders (no merge, the engine's crossing) doesn't show it. Cause not
  found.

- SE 1.5.97 with ENB 0.505, effects on: the street seen from inside a building (Warmaiden's at
  16:30, route `warmaidens-street-sun-1630-se`) lies in full sun where outside it is in the
  houses' shade. With ENB's effects off it is shaded as outside. At 17:30 both match. The same
  before and after the portal pass's t37 swap (commit fa4646d). Measured on the street's
  lighting draws: ENB's shader indoors doesn't read the sun's mask (t14 swapped for black: no
  change); the engine's mask there is right (dumped: the near street in shadow). ENB's own t37
  scales the ambient light (its x at 0 turned the shaded porch posts black while the cobbles kept
  their sun). Tried and dropped: t37's x times the mask's x — shade without any ambient, black.
  The sun's direct light indoors under ENB has no per-pixel input we can set; left open.
- With ENB's effects on, the white textures put over ENB's screen-sized slots for the portal
  pass's lighting draws (`Neutralized`) go through hooks on the context's draw calls, which the
  runtime rewrites under them: a few draws a frame likely keep ENB's t37, faint main-view shapes
  in the picture's shadows for a frame. Not tried since the cut moved off the draw calls; moving
  this too needs a way to bind it that ENB doesn't undo at the draw.
- SE 1.5.97 with ENB 0.505 (effects on, `EnableTerrainBlending=true`): after a seamless trip into
  Whiterun and back out, the land outside the gate is a flat grey-brown, untextured (route
  `whiterun-gate-roundtrip-terrain-se`). Only when an interior was visited just before the game
  was loaded (`coc` into Warmaiden's, with portals on or off, street-sun hooks installed or not).
  Not with the engine's own trip through the gate, not with the portal pass skipped
  (`portal_switches skipPortalPass`), not with terrain blending off, not with ENB's effects
  toggled off. Present before commit fa4646d too. Cause not found. Measured:
  - Right after the crossing out, at the gate, the ground is textured; it is flat after the
    route's `move` to the stables (one land quad, `Block (0, 0)` of cell 4,-2, the one stood on).
  - It lasts with the portals switched off, and ends when the grid shifts by one cell (same land,
    still loaded).
  - The quad's lighting draw has the same shaders, textures (also ENB's t37/t39/t40, dumped),
    targets, states and game constants flat and textured. Flat, it writes depth but no colour
    (a marked pixel of the colour target survives every lighting draw); textured, it writes
    both. So the difference is inside ENB.
  - Not it: the land's material and textures, its fade-in (`BSLightingShaderProperty` +0x108, 1
    on all 100 quads; forced to 1 from the arrival on: still flat), the distant land (`tll`),
    HighDetailRange (right), and the object fade ENB keeps in its constants (cb6 +1132: the fade
    of the reference faded last, 0.03 when flat by coincidence of the same `move`; brought to
    0.99, still flat).
- SE 1.5.97 with the Community Shaders fork: not tried (the fork builds for SE; Community Shaders
  isn't installed in the 1.5.97 test game).
- Without Community Shaders (the native backend) water isn't masked at a merged room's doorway:
  the street's canal shows in the Ratway's doorway over the steps down. `NativeMask` hooks the
  lighting, utility and effect shaders only; the water shader isn't tried.
- Two interiors merged (`bMergeInteriors`) with the native backend (no Community Shaders, SE
  1.5.97): not tried.
- The automatic doors' hook (`AutoDoorHook`) installs on SE 1.5.97 without error but hasn't been
  tried with an automatic door there or on AE 1.6.1170.
- AE 1.6.353, once: walking from the market into the Bannered Mare, the player stopped at the
  open doorway (25737,-7629) and didn't cross; the room had been merged half a second before. The
  same walk crossed every later time. Not reproduced.
- AE 1.6.353 is checked only as far as the commits that brought it in: CommonLib reads two
  structs wrongly there (ENGINE.md), and other accessors it gates at 1.6.629 may be wrong too.
- Bottled Shaders other than 2.0.3 (a Community Shaders fork, same `CommunityShaders.dll`, no
  `SeamlessPortal_*` exports) gets the "None" backend: no room merged into the street, the portal
  pass unclipped. Each Bottled Shaders release needs the port moved onto it.
- Community Shaders 2.0 switches itself off when it finds a DLL on its incompatible list (new
  there: `NMN_DeformableTerrain.dll`, Dynamic Terrain Deformation): it logs "Incompatible DLL ...
  detected" and installs nothing. Its DLL still exports the fork's functions, so SeamlessSkyrim
  picks the Community Shaders backend and the doorways are drawn by no one. It should fall back to
  the native backend then.
- Community Shaders 2.0, rain: in the storm save (`bannered-mare-rain-look-out`, `sw 0010A241`) no
  rain showed, through the Bannered Mare's doorway nor outside, also with the portals off and Rain
  Refraction off. The precipitation shape is there (density 8, not culled), its emitter reports 0
  particles. Not compared with stock 2.0 rc.2. With Bottled Shaders 2.0.3 the same route shows the
  rain falling in the street through the doorway.
- Bottled Shaders 2.0.3, the street seen from inside a building (Bannered Mare):
  - Snow Cover isn't on it: indoors Snow Cover takes the interior's config (none). Not seen in
    Whiterun (no snow in its streets); in Windhelm the street through a doorway would be bare.
    Taking the street's config indoors would also need the snow's world positions moved back out
    of the interior.
  - The outdoor haze (View Atmosphere's medium) and the volumetric clouds aren't the street's: the
    medium resolves from the interior, and the clouds are traced from the interior's coordinates,
    turned with the door pair. Through the doorway the air is clear, the sky bluer and the clouds
    elsewhere; outside the street is hazier and greyer.
  - The street's ambient falls back to the outdoor IBL of the moment the player walked in
    (`IBL::outdoorIBLTexture`); waiting indoors for hours leaves it at that hour's light.
- Bottled Shaders 2.0.3: the game crashed once in the sun's shadow map rendering
  (`BSShadowDirectionalLight` cascade, a batch pass through `SetupAndDrawPass`, SkyrimSE.exe+15603CD),
  standing in the Bannered Mare's doorway after walking in and out several times at 19:30.
  Not reproduced.
- Community Shaders 2.0, the street's Physical Sky seen from indoors: no lightning (Physical Sky
  keeps it off in interiors); the street's own light is the weather's colour, not Physical Sky's sun
  as outdoors; the medium's height fog and scene settings resolve from the interior's layers.

## Other

- Settings changed in the in-game menu are written into the shipped `SeamlessSkyrim.ini`
  (`Menu.cpp`, `Save`): an update replaces them with the new defaults. The door tunings had the
  same problem and now have their own file (`SeamlessSkyrim_Doors_User.ini`).
