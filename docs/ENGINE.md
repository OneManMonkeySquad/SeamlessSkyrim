# Engine facts

Skyrim AE 1.7.104, IDs from the Address Library; where SE 1.5.97, AE 1.6.1170 and AE 1.6.353 differ is at
the end. These were expensive to find. Add new ones in the same change that needed them.

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
- **A trip with the loading screen into a cell we hold attached** (Fort Greymoor's trapdoor into
  the prison merged behind another door): the engine makes it the player's cell as it is. Detached
  after that (released on arrival), the frame never ended: the current interior's update (ID
  18889, only for a cell in state Attached) signals frame event 13 (ID 69514), and a job of the
  frame (ID 39068) waits for it (ID 69516) while Main::Update waits for the jobs (ID 35409).
  Release before the trip (`BeforeEngineTrip`).
- **Interior buffer** (`TES+0xC8`): detaching puts a cell at its front, and the one falling off
  the end is unloaded by ID 13721. ID 13721 has about 25 callers, and a cell we still hold must
  not go through any of them: hooked at its entry (`RemoteCell::InstallGridHook`). A held cell
  unloaded the same way crashed in ID 13292.
- **Persistent references** (a town's houses, porches, paving) belong to the worldspace's
  persistent cell. The grid hangs their 3D under the cell they stand in, and takes it off some
  frames after the player goes inside.
  - Loaded at game start, the save has them with 3D.
  - Loaded indoors after the player was outdoors in the same session, those around the door
    have no 3D, so there is no collision under the door either. `RemoteCell::PersistentIn` queues
    them, but not doors. Launched with a save in Tamriel, so do WhiterunWorld's (137 at its gate).
  - **Queuing a reference's 3D** as `TESObjectREFR::Enable` (ID 19800) does: ID 13057 (`*ID 400332`
    the model loader, ref, priority ID 13321 (TES, its cell, 0), false); read on the loader
    threads, set on the main thread. It counts the reference in its cell's
    `loadedData->queuedRefCount` (ID 19151): a cell without loaded data (a reference still filed
    under the persistent cell) crashed there. Both ends of the task (`QueuedReference` vtable
    slot 12, ID 13022, which sets the 3D, and slot 20 → ID 13023) count it off again (ID 19152),
    with 3D or without, so a reference whose model doesn't load doesn't hold the cell's count up.
    `Load3D(true)` is not a request to queue: it is what the loader threads call, and parks the
    3D in thread-local storage. `Load3D(false)` on the main thread: ~6 ms each from disk.
  - 3D loaded while its cell isn't attached (`Load3D`, also `Set3D` again) hangs nowhere, and the
    engine's later attach of the cell leaves it so: it hangs only the 3D it loads itself. ID 19165
    (cell, ref, true, false) hangs one reference's 3D into its cell, as the cell attach does for
    each (ID 18976 → ID 19187); the cell must have its 3D. Also after the engine's own trip with
    its loading screen: a street let go just before it came back without them (Riften's canal
    walk outside Beggar's Row, the player in the canal).
  - ID 19165 reads the cell's portal graph (ID 19167: the cell's own, else its worldspace's, ID
    20513 = `TESWorldSpace::portalGraph`). Loading a save clears the worldspace's graph, then
    moves the save's actors into their cells (ID 19799), hanging the 3D of any that has 3D into a
    cell ID 13308 (TES, cell, false) counts as loaded: kBuilding3D to kAttached. An elk with 3D in
    an unattached Tamriel cell crashed that (`RemoteCell::UnloadDetachedExteriors`).
  - Walking out once, Markarth's ground statics (`MrkDocksideLand01`, temporary references of
    MarkarthMines) became persistent references of a runtime cell `FF000A96 'Wilderness'`, whose 3D
    hangs under `TES::objRoot` outdoors. Why isn't known.
- **Land meshes** are built only when a cell is loaded into the engine's own grid (IDs 13285 and
  13427 call it right after 19063). A remote cell needs it called itself (`RemoteCell::AttachOne`).
- **A cell read from the files and never attached** (`LoadCell`, with or without ID 19111's
  temporary references) crashed the AI update (ID 36031 under 35600 -> 35567), also after
  loading a game. Attached and detached again (IDs 18976, 19063, 19130) it stays kUnattached like
  a cell walked past.
- **A reference's 3D is a copy of a loaded one of the same model, as it is at that moment.** A
  shape hidden with `SetAppCulled` in the first instance of a model (loading a save inside The
  Drunken Huntsman: its own two doors) comes hidden in every instance loaded after (every Whiterun
  dragon door of the street attached next: leaves, handles and backing), through
  `NiObject::CreateClone` (vfunc 0x17). A collision layer too: `bhkRigidBody`'s CreateClone (ID
  78068) builds the copy's Havok body from the original's cinfo (Warmaiden's inside door came
  non-collidable, layer 15, while the inn's door was held walkable). The collision object's flags
  too: Warmaiden's inner doors, loaded while the inn's (same model) were stilled for its merge
  (`kActive` and `kSyncOnUpdate` off), came stilled, their bodies left where the inn's doors stand,
  and couldn't be used. A leaf's turn and a shape drawn two-sided would come the same way (not
  seen). What the portals change in a door's 3D goes
  through `Portal::Pristine`, which makes copies as before the change.

## Doors

- **A door's teleport marker isn't always square to the door.** Solitude's gate, city side:
  48° off for one of the two load-door pairs standing there (00101D9D/00101D9E), 1.4° for the
  other (00037F1B/0001C386). Markarth's Smelter Overseer's House, street side: 17°. Whiterun's
  shops: 4-11°. Most doors: under 1°. The doorway mapping takes each door's facing from its panel
  and only the side from the marker (`Doors::Compute`).
- **A load door's object bounds (OBND) are not its panel.** Its mesh also carries a box behind the
  panel that hides the void behind a shut door (`DoorToBlack*` shapes; textured as wood beams in a
  PBR replacer), handles, sometimes a frame or a piece of wall. Whiterun's `WRShackDoor01` is 51
  units deep in its bounds for an 8 unit panel, Belethor's dragon door 39 for 7. The two doors of
  a pair carry the box on opposite sides, so a mapping centred on the bounds put doorways 20-30
  units off (hand-made "further in" corrections of about -20 were making up for it). The panel is
  measured from the mesh (`Doors::Measure`): the leaves are the targets of the root's
  `NiMultiTargetTransformController`; vertices come from `BSGraphics::TriShape::rawVertexData` /
  `rawIndexData` (kept in memory for these meshes; position first, 4 floats, stride
  `(vertexDesc & 0xF) * 4`); along the thin axis the faces are the outermost 1-unit slices
  holding at least a quarter of the most surface facing that way, so handles hardly count.
- **A door's model isn't always upright.** Castle Dour's doors (`SDoorJail02Load.nif`, 00025199
  and 000223B2) are modelled lying down, y up; their references are turned 90° about x to stand
  them up. The door code works in the model turned so its z is up (`Upright` in `DoorPair.cpp`).
  Bridge `ref_3d` prints each node's local rotation.
- **Doors lying in a floor or ceiling** (trapdoors, hatches, floor grates, Dwemer lift platforms)
  are load doors like any other: the College of Winterhold's yard trapdoor (0002BCE6) into the
  Midden. Their base object's bounds tell them (the extent along the axis the reference's angles
  turn upwards under half the narrower one across; `Doors::LiesFlat`), loaded or not:
  `WinterholdTrapdoor01` 96 x 96 x 33, `ShipTrapdoor01` modelled standing and laid down. The
  ladders below them (`TrapdoorLadder01`, 114 x 118 x 397) look like standing doors.
- **A reference's rotation from its angles** is CommonLib's `NiMatrix3(angles)`
  (`SetEulerAnglesXYZ`, = Rx(-x) Ry(-y) Rz(-z)), checked against the 3D's on Ilinalta's Deep's
  tilted trapdoor (00015E11, -30.7, -66.1, 308.4), which `EulerAnglesToAxesZXY` stood upright.
- **A door's mesh can hold the wall around its panel.** Riften's `RiftenRWDoorLoad01` (base
  0006F267, e.g. Elgrim's Elixirs) carries the stone frame filling the room's arch, wider than the
  panel; the room's own wall has an arch-shaped hole. Hiding a door means hiding its leaves (the
  animated nodes), not its whole 3D: hidden whole, the doorway was a hole into the void.
- **What closes a doorway** (a panel behind the door hiding the void: Belethor's, from a mod; the
  cap inside Solitude's Hall of the Dead's arch, `SMdDEntSol01.nif`) can be one shape with the
  frame and reveals round it, so culling shapes takes the frame too. Nothing culls them: beyond the
  door plane and seen through the doorway, the doorway mask already leaves them out per pixel.
- **The doorway mask's plane** lies `Render::Doorway::kMargin` (5 units) beyond the door's
  middle, so the two spaces' frames don't z-fight. A panel shut on the plane is drawn by
  neither space: the world's shut door was cut away (the sky showed), the merged space's door
  near shut vanished. Shut, the mask is sent from behind the panel; nearly shut with the merged
  space's door swinging, from in front of it (`UpdateMerged`).
- **City gates are animated doors.** Their meshes have two leaves turned by a
  `NiMultiTargetTransformController`, like a house door: Whiterun's `WRDoorMainGate01.nif`
  (`FrontGateDoor06`/`08`), Riften's `RTMainGate01.nif` (`DoorL`/`DoorR`), Solitude's
  `SGateDoor.nif` (`SGateDoorLeft`/`Right`).
- **Behind a city gate the player's own worldspace has a stand-in** for the other side, seen
  through the gateway once the gate is gone: Tamriel holds simple copies of Whiterun's and
  Riften's houses behind their gates, and Whiterun's worldspace its land beyond. Solitude's
  gatehouse is walled up behind its gate on both sides (`SMainGate.nif` in Tamriel).
- **A door's swing is its NIF's `Open` and `Close` sequences** (on the 3D root's
  `NiControllerManager`). An interpolator can be sampled at any key time with
  `NiInterpolator::Update1` into an `NiQuatTransform` (translate, quaternion, scale; CommonLib
  only declares it). Whiterun's house doors: 0.6 s each; `Open` stands still for the first
  0.15 s, then eases in and out; `Close` eases in and out. Portal doors replay them
  (`Doors::SwingAmount`), turned by the pair's own angle and towards the side they open into.
- **A door's open and close sounds** are its base form's `TESObjectDOOR::openSound` and
  `closeSound`; `BSAudioManager::GetSoundHandle`, then `SetPosition`/`SetObjectToFollow`, `Play`.
- **A door's prompt:** `TESObjectDOOR::GetActivateText` (vfunc 0x4C, ID 17923) gets the door
  reference as its second argument (CommonLib calls it the activator). It writes
  `"<verb>\n<name>"`, with `"\n<lock word><level>"` after it for a locked door; the name is the
  destination's for a load door, the reference's display name for any other. The HUD asks again
  only when the crosshair update (ID 40620) sees the target change and sets
  `playerFlags.shouldUpdateCrosshair`; setting that flag yourself makes it ask the next frame.
- **Automatic load doors** (base door flag 2, `AutoLoadDoor01` 00031897: cave mouths, 307 of the
  2052 load doors in Skyrim.esm and the DLCs): nobody uses them. Loading the door's 3D (ID 17934)
  puts a `BSPlayerDistanceCheckController` on it with the callback ID 17933: with the player
  ~350 units from the 3D it opens `Mist Menu` and `Fader Menu`, the fade's callback activates the
  door for the player, and the arrival closes both menus (no loading screen, 0.5 s).
  - With the activation swallowed the Mist menu stays open: the HUD and every other menu are no
    longer drawn (only the cursor), until a game is loaded.
  - The distance is to the 3D where it stands: the door of a space merged behind a doorway fires
    from the player's side of it.
  - Moved into range (`move_to`) the player isn't sent through; walking in from outside is.

## Crossing: the engine's teleport

- **The teleport's cost** (ID 40438, loading screen and waits skipped): 15-40 ms into an interior,
  25-60 ms out to a street seen before, 60-70 ms out of Belethor's the first time after a load
  (170 ms before `RemoteCell::Prewarm`).
- **The change into an exterior** (ID 13317) sets the grid up around the arrival cell
  (13288 -> 13428 -> 13430) and calls ID 13427 for *every* grid cell: `TESWorldSpace::LoadCell`
  (from the plugin files if not in memory), the attach if the cell isn't in the grid yet, then
  `GetLand()` and `LoadVertices(true, true)` on its land, which reads (ID 18748) and builds
  (ID 18750) a land not loaded yet and leaves a built one alone. A city worldspace's cells take
  their land from the parent's: `cellLand` is null until `GetLand()` makes it (flags 0x400 with
  the record found, 0x40F once loaded). It also demands the world map's clouds (ID 53142 + 0x272,
  `BSModelDB::Demand` into a global emptied indoors; the path from ID 20495, by default
  `Meshes\Sky\SkyrimWorldMapCloudBank.nif`): ~25 ms from disk.
  - Coming from an interior, it re-uses the grid buffered when the player went in
    (`TES::saveGridX/Y`, +0xE0) only if every grid cell is kUnattached (ID 15951, called at
    13317+0xF4); then it attaches the kUnattached ones and leaves attached ones alone. Otherwise it
    sets the worldspace up anew and sleeps until the task pool's loads are done (ID 75708, 50 ms
    at a time). Street cells handed over still attached failed that check and made the walk out
    slower than detaching them (`GridReadyHook` accepts them).
  - Coming from an interior it first detaches it (+0x82: ID 19130 (cell, true)), then, the cell
    found in the interior buffer, demotes its actors (ID 19046), or else unloads it (ID 13721).
    With `bPreemptivelyUnloadCells:General` set (off by default) and more than
    `iLargeIntRefCount:General` (1000) references in the interior, it then purges every buffered
    cell and sets the worldspace up anew (+0x392: `PurgeBufferedCells`, `SetWorldSpace`).
- **The change into an interior** (`TES::SetCurrentCell`, ID 13316), coming from outdoors:
  - It detaches every attached cell of the grid (+0x50E: ID 19130 (cell, false), uGridsToLoad
    squared) and the worldspace's sky cell (+0x58E, (cell, true)): 18 ms and up to 26 ms walking
    into Belethor's. The cells stay loaded (kUnattached); walking out, ID 13317 attaches them
    again, the sky cell too unless it is attached (+0x480).
  - Then it waits for the task pool (+0x6CB, +0x700: ID 75708). Usually nothing is queued: a
    cell's detach cancels its loads (ID 13059). With the grid kept attached the wait was 66 ms.
  - Skipped for a seamless walk in (`RemoteCell::keepGridOnEntry`, `GridDetachHook`): the street
    behind the door takes the cells as they are, and hands them back attached on the way out.
- **Detaching or attaching a cell tells every mod's sink about each reference** (ID 19188 or
  19187 -> ID 19362, the cell attach/detach event). In the test load order DynDOLOD's sink is
  most of a cell's attach: 10 of 14 profile samples under ID 19362 on a first walk out.
- **Walking out with every grid cell already attached, nothing installs the world scene's room
  graph**: ID 13317 + 0x78 takes the interior's out, and the next engine attach of a cell would
  install the worldspace's (ID 20513, `TESWorldSpace` + 0x130; a cell's own if it has one, ID
  18976 + 0xFE). The first frame's actor cull then read a null graph (a crash; see Light scenes).
  `GridReadyHook` installs it, before the cells' lights are queued (+0x590).
- **Where a persistent reference's 3D hangs follows the cells' attach state.** With the street's
  cells detached on going indoors, it ends up in one node of the persistent cell's, off the
  scene. Kept attached, each stays under the cell it hangs in, which isn't always the cell it
  stands in (192 of Whiterun's stood in the nine cells round the Bannered Mare's door and hung
  in cells beyond them).
- **The exterior cell buffer** (`TES::exteriorBuffer`, +0xD0, `uExterior Cell Buffer` slots) lists
  the loaded exterior cells, attached ones first, packed (ID 13368, on a detach, stops at the
  first empty slot). ID 13434 puts a cell at its front and unloads the one pushed off the end.
  ID 13721 doesn't take the cell it unloads out of the buffer; the engine empties the slot
  itself. A cell made at runtime (Markarth's 'Wilderness') is freed by the unload: left in its
  slot, the next load unloaded it again (ID 19130 + 0x891 on freed memory).
- **Two worldspaces can share a sky cell** (Windhelm's and Solitude's). Changing between them,
  `TES::SetWorldSpace` (ID 13315 + 0x1EB) gives it the new worldspace's room graph (ID 19168)
  without looking whether it is loaded: unloaded before, a crash.
- **A detached interior loses its references' 3D after a while**, its cell node staying (3D=yes):
  Arcadia's Cauldron, let go on walking into Belethor's and attached again 20 s later, took 4.7 s
  to load. Belethor's, detached by the teleport and attached again in the same frame, was whole.
- **Teleports** onto a point below the land are lifted onto it (ID 37507 + 0xCEA).
- **A mod's hook in a profile isn't necessarily the mod's work.** Walking in or out of Whiterun's
  shops (2026-11-16), ~25 ms of the teleport sat under `po3_LightPlacer` (ID 19411+0x257 ->
  Light Placer -> ID 40466 -> ID 20039): 20039 dispatches the cell change to every mod's sink.
  There, ZeroBountyHostilityFix called `PlayerPayCrimeGold` 78 times per cell change, and
  PlayerPayCrimeGoldTweak re-read its INI and wrote two log lines on each call. Look below 20039
  before blaming Light Placer.
- **Each trip through Riften's south gate leaks about 2.5 GB of GPU textures** in the test load
  order (private bytes 15 GB after loading, 31 GB after ten trips; crashes after many trips came
  with the machine's memory nearly used up). Not SeamlessSkyrim's: the same with the engine's own
  trip and its loading screen, the portal hooks not installed, and with Community Shaders, Faster
  Textures, Engine Fixes' texture patch and 88 of the 91 SKSE plugins removed (left:
  SeamlessSkyrim for the bridge, trainwreck, HDT-SMP; without HDT-SMP this load order crashes on
  faces at load, ID 69192). Bridge `gpu_live` shows textures the engine still holds through
  materials after leaving for an empty cell and `pcb`, and ones whose `NiSourceTexture` and
  renderer texture are freed but whose D3D texture and view stay alive with no outside reference,
  also after `ClearState` and `Flush`. Not tried: the game without SKSE.

## Simulation

- **SMP hair and cloth** (hdtSMP64 / FSMP) keep their last positions after a teleport and are
  drawn stretched from the old place to the new one; SMP resets on the loading menu closing.
  `ResetClothPhysics` sends a loading-menu open and close to SMP's own sinks only (the sink's
  vtable inside `hdtsmp64.dll`): sent to every sink, FasterTextures did its loading-screen work
  on each trip.
- **An actor's 3D is hung back into its cell every frame:** the 3D position update (ID 37995, for
  every actor in high process, on the job threads; +0x279) calls ID 19165 (cell, actor, true, ..),
  which, when the 3D's parent isn't the cell's node for actors (ID 19339), queues the move there
  (ID 19415, a per-thread queue run by ID 19416 → ID 19414 `AttachChild`).
- **Moving a cell's 3D moves its bodies too:** `NiAVObject::Update` of the cell (ID 70539 →
  the collision object's ID 78910 → ID 78178) sets its bodies' transforms in Havok, and the
  broadphase update (IDs 61802, 64613) is most of moving Riften's street out of the jail on
  walking out (70-270 ms). `NiUpdateData::kDisableCollision` doesn't skip it; ID 78910 returns early
  when a thread-local state (TLS + 0x9E8) is 1, set by whom isn't known.
- **An update with a loose item's collision active reads its place back from its body** (ID 70539
  → 78177 → 78382 → ID 19826). If the body is outside the item's cell (in an interior, below its
  floor), ID 19826 treats the item as fallen out and moves it to another cell (ID 19799). A
  created item (FF id) has its form ID cleared there (ID 36459) and isn't saved. Put a moved cell
  back with its loose items stilled, and give them their flags back afterwards (`Release`).
- **High process:** `Actor::MoveToHigh()` on a remote cell's actors is accepted, and they aren't
  demoted while the cell stays attached.
- **An NPC's graph `Speed` is pulled, not set:** each graph update reads it from the actor's
  movement through `BSTAnimationGraphDataChannel<Actor, float, ActorSpeedChannel>` (vtable AE
  207549 / SE 260606, slot 1 `PollChannelUpdateImpl` stores it at +0x18; the actor is at +0x28),
  so `SetGraphVariableFloat("Speed")` is overwritten; an actor moved by `SetPosition` walks in place
  with its legs still. `TurnDelta`'s channel is ActorTurnDeltaChannel (207559). Writes inside
  `Actor::UpdateAnimation` (ID 37361, after its ID 37364) never ran for NPCs.
- **AI after being moved:** an NPC held on a path of our own for a second (`SetPosition` every
  frame) stood 1-2 s at the end before its AI walked on; `EvaluatePackage(true, false)` then gets
  it going within ~0.1 s, except now and then the first one after a load.
- **NPCs through a load door:** one following close behind another is moved through without
  `MovementMessageActivateDoor`'s idle call (the door idle hook doesn't see it), sometimes from
  more than 200 units in front of the door. After the move the AI's path goes on from the door's
  marker.
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
- **`bhkWorld::PickObject`** with a char-controller filter still hits bodies on the non-collidable
  layer (15): a door made walkable stops the ray, not the player. A ray started on a surface
  misses it. A house's own collision can close its doorway 2 units behind the door's back face
  (Sadri's Used Wares, Windhelm).
- **Line of sight:** ID 37761 (observer, target, mode), called by detection and combat (IDs 39639,
  39640, 41246, among twelve callers). ID 53829 (`HasLineOfSight`) only dispatches to it.
- **Conditions:** `GetInWorldspace` is ID 21475 and compares exactly. Conditions run through the
  script command table (ID 29924 + 0x691 calls the entry), so replacing the entry answers every
  condition.
- **Water:** `TESWaterSystem` (ID 400450) lists water objects (+0x20, each: +0x20 shape, +0x40
  reflection) and reflections (+0x38, each: +0x14 plane, +0x40 the material of the water it was
  made for, a plain pointer set by the constructor, ID 32158, called only from `AddWater` + 0x770).
  A water joining a reflection reads that material (`AddWater`, ID 32179 + 0x685: its +0x38
  texture is copied into the new water's material). The reflection outlives its first water
  whenever others joined: the material is freed and the next join reads freed memory. Seen with
  a street's water joining a room's reflection, and across a loading screen (launch, load in the
  Hall of the Elements, `coc WinterholdExterior05`: the sea's reflection, 93 waters on it, its
  material freed; not tried without the plugin). `WaterReflections` gives each reflection a
  reference on its material. `RemoveWater`
  (ID 32182) does nothing while the system is disabled (+0xB0). Bridge `water` lists both, and
  says `FREED` for such a material.
- **Shader materials:** the manager (ID 403555, lock ID 415284) keeps every shared material in a
  table (+0x14 capacity, +0x30 entries of key, material, next). `BSShaderProperty::SetMaterial`
  (ID 105544) looks the new one up (ID 107719: a slot with the same key is compared through the
  stored material's vtable +0x18) and releases the old (ID 107720: at zero references the entry
  with the material's key, +0x2C, is taken out and the material deleted). A material deleted any
  other way stays in the table, and the next lookup with its key calls into freed memory. A
  reference is added under the lock. Community Shaders' PBR materials have their vtable in its
  DLL. Bridge `materials` lists entries whose material is gone (AE only).

## Rendering a second view

- **Frame:** `Main::RenderPlayerView` (ID 36559) → shadow maps → `RenderDepth` (107139) →
  `RenderShadowmasks` (107140) → `RenderWorld` (107142, executes the accumulated draw lists) →
  `RenderFirstPersonView` (107129) → `RenderWaterEffects` (36560) → post.
- **The main view's depth prepass** (`RenderDepth`) draws most statics with the Utility shader's
  shadow map technique (vertex descriptor 0xC000, RenderShadowmap | Clamped), skinned ones with
  RenderDepth (0x2004). The technique doesn't tell a shadow map from the main view; the depth
  target does (`RendererShadowState::depthStencil`, `kSHADOWMAPS` and the like).
- **Sun shadows indoors:** the frame setup (ID 36561, SE 35562) sets a "no sun" flag in the frame's
  render data (+0x51 of what ID 107121 returns) when `TES::interiorCell` is set (jne at +0x37B),
  or when `TES::GetWorldSpace` (ID 13314, SE 13169) has flag 0x40 or ID 20481 says so (calls at
  +0x37D and +0x392). With it clear indoors the engine draws the sun's shadow maps and mask. The
  lighting shader reads the mask only with `kShadowDir` (bit 13) and `kDefShadow` (bit 14) in its
  technique: outdoors every shape has both; indoors shapes have at most `kDefShadow`.
- **The technique being drawn:** `SetupAndDrawPass` (ID 107644) stores the technique it is given
  in ID 415272 (with the shader in ID 415273) before the shader's `SetupGeometry`; a hook that
  changes that argument changes it there, not in the pass's `passEnum`.
- **Matrices for a mask of one's own:** `RendererShadowState::cameraData` (`ViewData`) after a submit
  holds that submit's camera; `viewProjMat` maps camera-relative positions (`posAdjust`) as
  `v * M` (inverted, the same way back from depth). A `BSShadowDirectionalLight` cascade's
  `shadowmapDescriptors[i].lightTransform` maps absolute world positions as `v * M` to the map's uv
  (xy, 0..1) and depth (z); `endSplitDistances` / `startSplitDistances` are view distances along
  the camera's forward. The main view's depth prepass accumulator is ID 415008 (SE 528063).
- **Alpha test in the game's own lighting shader:** only the prepass cuts alpha-tested shapes; the
  lighting pass draws them with depth test EQUAL and a technique without `kDoAlphaTest` (bit 20),
  although `SetupAndDrawPass` gets `alphaTest` true and `RendererShadowState::alphaTestEnabled`
  and `alphaTestRef` are set. A pass without a prepass (the portal pass) draws leaves and grass as
  solid cards unless bit 20 is added to the technique; the game's shader package has those
  variants (1.5.97 checked). Community Shaders' lighting shader tests alpha anyway.
- **Camera data in the frame:** at the start of `RenderPlayerView` the NiCamera has this frame's
  transform but the renderer's camera data (`RendererShadowState::cameraData`) is still the last
  frame's; by `Main::RenderWorld` both are this frame's.
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
  - Render flags (`BSShaderAccumulator::RENDER_FLAG`): 0x1 jitters the projection like the main
    view's (`State::SetCameraData`, ID 77503: the cache entry is per camera and jitter); 0x400 calls
    `Renderer::UpdateViewPort(0, 0, true)` (ID 77240), the whole target, without dynamic
    resolution. Binding a target with "update viewport" (ID 77453) applies the render scale
    (`BSGraphics::State` ratios), as for the main view, which is drawn into the target's top-left
    part. The main view submits with 0x41.
- **Scissor:** `RendererShadowState::rasterStateScissorMode` (+0xA4) = 1 with
  `DIRTY_RASTER_CULL_MODE` (0x20) makes the state apply pick rasterizer states with the scissor on;
  the rectangle is whatever `RSSetScissorRects` last set on the immediate context (nothing in
  `SubmitAccumulator` sets one).
- **A culling camera narrows what is drawn, not where:** a shape the doorway-narrowed cull keeps is
  shaded across the whole target. At Solitude's gate the portal pass shaded 24 times the doorway's
  pixels (2.4 of its 3.5 ms GPU) until scissored to the doorway.
- **Draw submission costs ~1.4 µs per render pass** on the main thread (engine state, Community
  Shaders' hooks, D3D11), on the one immediate context: it can't move to another thread.
- **The sun's shadow maps** (`ShadowSceneNode::sunShadowDirLight`, a `BSShadowDirectionalLight`;
  every scene made with ID 106320 has one, sharing the scene's sun `NiDirectionalLight`):
  1. `UpdateCamera(camera)` (vtable +0x80, ID 108496) fits the cascades to the camera (ID 107132
     passes the world camera): split between the camera frustum's near and min(shadow distance,
     its far) by the practical split scheme (linear and logarithmic blended), so a near plane
     pushed out moves every split.
  2. `DirShadowLightCulling` (ID 108492, SE 101498; Community Shaders detours it for Interior Sun)
     culls job arrays (one `BSTArray` of top nodes per culling job, one job per entry of
     `fullFrustumCullingProcessArray`; the world's come from ID 106323) into the cascades'
     accumulators, then a last node list.
  3. `Accumulate(count, 0, nullptr)` (vtable +0x48; ID 107137).
  4. `Render(index)` (vtable +0x50; ID 107133) draws each cascade (ID 107604: binds the depth target
     slice, submits the cascade's accumulator with its own camera) into
     `kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM` when ID 391108 is 2 (it is in this game), then into
     `kSHADOWMAPS_ESRAM`, and writes the cascades' `lightTransform`s.
  Community Shaders builds its soft copy (Volumetric Shadows' VSM) from the shadow map bound at
  PS t4 when the main view's shadow mask is drawn, and reads `lightTransform` for t98.
  - So each cascade is drawn twice: 2 cascades at 2048 were ~27M pixels shaded, 0.8-0.9 ms GPU, for
    the portal pass. Its maps drawn at half size came out wrong (the ground beyond Solitude's gate
    all in shadow): the cascades are drawn for the engine's resolution. Fitted to the doorway's
    narrowed camera instead of the portal camera, the shadows cost 0.3 ms less main thread but
    0.25 ms more GPU (more pixels per caster).
- **Vanilla specular has no N·L:** `pow(N·H, shininess)` times the light, so a surface lit at a
  grazing angle (a low sun on tree needles) glints at full strength. In the world the shadow maps
  hide most of it; drawn without them, it shows.
- **Rain and snow** are one particle shape per weather (`Sky::precip->currentPrecip` /
  `lastPrecip`, under the world scene's "Weather" node), a cube that follows the camera.
  - The precipitation update (ID 26179, SE 25638; from `Sky::Update` and ID 26222) gives the
    emitter (`BSParticleShaderCubeEmitter`, the shader property's `particleEmitter`) density 0 and
    culls the shape unless `Sky::mode` is full: indoors nothing falls, shown or not.
  - The occlusion map (ID 26175, from `RenderPlayerView` after the shadow masks) is drawn every
    frame indoors too, from the main view's cull roots, as long as the weather has rain or snow.
    ID 26176 switches it (and clears it once).
  - Rain writes no depth: a picture put over the main view later covers the rain in front of it.
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
- **No lasting eye adaptation.** The tone map multiplies by `avg.y / avg.x` (ISHDR blend), two
  copies of the frame's average luminance adapted at different speeds: once settled the exposure
  is 1. Interiors and the street are each authored to look right as drawn, so a room seen from a
  sunlit street is as bright as from inside it unless something dims it (`RoomLightScale`).
- **A street in the world scene** while the player is indoors registers one of its lights anew
  every frame. Adopting each into our scene crashed in the world scene's queued removal
  (ID 106335 → 106342).
- **A light's pairs with the shapes it lights belong to the light and the shapes, not the scene**
  (`BSLight::geomList` and each shape's shader property, ID 108283), and so does its culling
  process (`BSLight` + 0x128, 0x30230 bytes; ID 106401 makes one only for a light without, and
  its constructor, ID 76551, builds a 4096-entry free list with an interlocked exchange per entry:
  about half of taking a light in). A `BSLight` moved from one scene's `activeLights` into
  another's goes on as it is, with no update of either scene (`LightScene::Take`, `Give`).
  - Reparenting a shape's cell out of a scene and into another undoes its pairs; hung into a
    scene again, the scene's update pairs the new geometry with its active lights (ID 106350-
    106352). So a light must be in the active list before that update to cost nothing.
  - A reference that has its light already gets no new one on its cell's attach (ID 19678 looks at
    its `ExtraLight`); a cell's detach with the unload flag takes the lights away. The world scene
    gets a `BSLight` for a reference's `NiLight` when it has none, within the teleport out.
  - `RemoveLight` (ID 106332) only queues, and does nothing while the scene's byte at +0x300 is 0.
    The update's removal goes by the `NiLight`: a second `BSLight` for the same one, removed from
    the world scene, took the scene's own with it (18 of Whiterun's 53 street lights gone).
  - The update (ID 106335): the add queue (+0x160; a queued light whose `NiLight` is active already
    only walks again), a queue of lights to walk again (+0x190), the remove queue (+0x178), then
    the new geometry.
- **Taking in a light costs ~0.15-0.3 ms.** `AddLight` (ID 106327) only queues it; the scene's
  light update (ID 106335, per queued light ID 106342 → 106401, shadow lights 106400) builds a
  ~200 KB `BSParabolicCullingProcess` for it and walks the scene for the shapes it lights
  (ID 108282, pruned by `worldBound`). It dedupes by `BSLight::light` (the NiLight), not by
  BSLight. The patched Community Shaders still uses the world scene's per-shape lists (without
  them a room came out ~4% darker); our scenes don't need them.
- **Arriving in a cell queues each of its lights twice** in the world scene (Understone Keep: 306
  for 153). The update takes in the first and, finding the second's NiLight active, walks the
  scene for it again. The teleport (ID 40744 + 0x3D2) and every frame (ID 106337 + 0x2B) call the
  update; after a seamless trip `Travel.cpp` lets it take 32 lights at a time, nearest first, in
  the world scene and in ours (the space walked out of, attached again with its ~150 lights).
- **`ShadowSceneNode::SetPortalGraph` (ID 106385) with a different graph drops every light**
  (`RemoveAllLights`) and both queues. The engine's cell attach (ID 18976 + 0x140) calls it on
  the world scene with the attached cell's graph (`AttachGraphHook` skips it for our attaches).
  It also frees the old graph's list of occlusion shapes (`BSPortalGraph` + 0x10): before the
  skip, every remote attach left the street without its occlusion planes.
- **The world scene's graph must never be null in a frame:** the per-actor cull (ID 40335 ->
  ID 106433 + 0x28) reads it unchecked for every actor in the view frustum. It is set to null by
  `SetCurrentCell` + 0x37C (until the attach installs the new cell's), ID 13317 + 0x78 (walking
  out, the interior's), ID 19170 (an interior's loaded data freed while its graph is installed)
  and ID 20515 (the worldspace's graph released while installed). The attach installs null for a
  cell with no graph, but every interior has one, even with no rooms.

## Culling

- **What straddles an interior's portals hangs in a node of its own** (the parent of each
  portal's `BSPortalSharedNode`, under the cell's sixth child). The main view leaves that node
  out of a cell it culls; the rooms cull it through their portals. A merged interior hung in
  another interior's room is culled as a plain subtree, which walks into that node, and its merged
  rooms cull the shared nodes too (`RoomCullHook`): the shapes went into the main view's batches
  twice, a batch's pass list ran in a circle and the game hung in `BSBatchRenderer::RenderBatches`
  (ID 107642). While hung there the node is app-culled (`RemoteCell::Merge`).
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
- **A switch node's hidden child** (`NiSwitchNode`, a coin bag or mushroom picked or not) is never
  updated: its world transform stays at the origin, and the switch node's bound takes it in.

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
  frame's time (ID 400333) and flags 2, which runs the controllers. The particle systems are not
  simulated there: the update returns flag 0x100 for a 3D with some, and ID 18894 then queues it
  (ID 19366, SE 18952; at most 200 a frame) for jobs that simulate them (ID 19387 per object).
  ID 18894 skips the references of a room merged into the street; the selective update plus that
  queue run them (`RemoteCell.cpp`, `AnimateReferences`). Without the queue a merged room's fire
  had no flames: the particles stayed where they were at the merge, and their bounds there (in the
  interior's coordinates) left them out of the native backend's doorway cut.
- **`ShadowSceneNode::particleNodes`** (+0x1C8) is only a queue: the light update (ID 106335)
  pairs each new particle node with lights and empties it every frame. It doesn't animate.
- **`BGSTerrainManager` layout:**
  - `updateNodes` are the level-32 nodes.
  - A node's `children` (+0x30) points to its 4 children inline, 0x50 apart, not to pointers.
  - The low 12 bits of `nodeState` hold one nibble per layer: terrain 0x00F, objects 0x0F0,
    trees 0xF00. The values are 2 none, 4 shown, 8 split into the children.
  - Object layers live on the level 16, 8 and 4 nodes, so a level-32 node without one is normal.

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

## AE 1.6.1170 against AE 1.7.104

- Same IDs (all of the plugin's exist in 1.6.1170's table); 5 of 33 hook sites at other offsets
  (`Hooks::Offset`), the rest identical, and so are the byte patterns the plugin checks.
- From 1.7.99 the player's runtime data sits 8 bytes later (CommonLib's
  `RUNTIME_SSE_1_7_99`): the teleport's queue at +0x648 and valid flag at +0x68A on 1.6.1170.

## AE 1.6.353 against AE 1.6.1170

- Same IDs; 5 of 36 hook sites at other offsets (the terrain update's placement calls, Main::Update's
  end-of-frame call), the rest and the byte patterns the plugin checks identical.
- Before 1.6.629 references are 8 bytes shorter, so the player's offsets are SE's: drawn bits at
  +0x274, teleport queue +0x640, valid flag +0x682.
- TES's worldspace is at +0x140 before 1.6.1130 (CommonLib's TES split), +0x148 after.
- PlayerCharacter's `BSTEventSource<BGSActorCellEvent>` is at +0x2D0 as on SE (0x58 each, then
  death +0x328, position +0x380); CommonLib's `AsBGSActorCellEventSource` says +0x2D8 for every AE
  before 1.7.99. Through it, `AddEventSink` spun forever on a lock word that wasn't one: the game
  hung before the main menu.
- `BSGraphics::State`'s runtime data starts at +0x60 as on 1.6.1170 (Halton points at +0xC4,
  dynamic resolution ratios at +0x104); CommonLib's `GetRuntimeData` takes SE's +0x58 before
  1.6.629 and reads a Halton point (5/9) as the width ratio.
- SKSE 2.1.5 refuses a declaration with `kVersionIndependent_StructsPost629`; SKSE 2.2 refuses one
  without it on 1.6.629+ unless it says no struct use (`kVersionIndependentEx_NoStructUse`). One DLL
  for both declares no struct use, as Community Shaders does.
