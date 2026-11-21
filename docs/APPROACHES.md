## Overview

| # | Approach | Status | Where it fell short |
|---|---|---|---|
| 1 | Portal pass: the other space drawn by our own reduced render, put into the doorway | In use for looking out | A reduced render: looking out looks wrong (2) |
| 2 | Crossing without a loading screen | In use, works | (none, kept) |
| 3 | Render-merge: the room drawn by the game's own main pass, masked at the door | In use for looking in | Works only one way: an exterior can't be hung into an interior |
| 4 | Walk into rooms: the game kept outdoors inside the room | Built, off | Visible switches, too many of them, NPCs can't interact |
| 5 | Sectors: interiors moved into the worldspace by a generator | On hold | Houses don't fit their shells; "where is the player" has to be faked everywhere |
| 6 | Open cities by merge | Removed | The edge between the two worlds can't be fixed automatically |
| 7 | Open Cities (the existing mod) as a base | Not taken | A dependency; it loses mods' navmesh edits |
| 8 | A second full render of the outside | Not tried | Cost is acceptable; the engine has switched the outdoors off while you're inside |
| 9 | Renderer stays outside, game goes inside | Proposed | See the end of this file |

## 1. Portal pass (still in use)

From the doorway, the other space is rendered from the main camera carried through the door pair
into our own colour and depth targets, then drawn on a quad in the doorway. It is built on the
recipe the game uses for menu 3D (inventory models), since the main pipeline didn't accept a
second camera.

Why it falls short: it isn't the game's world render.
- **No deferred lighting:** the pass runs with `inWorld == false`, so Community Shaders' deferred
  path and its effects don't run.
- **No sun shadows:** interiors don't draw the sun's shadow maps.
- **No real sky:** a gradient is drawn instead (`DrawSky`).
- **No distant LOD and no fog.**
- **Rain and snow:** experimental, off.
- **Workarounds needed:** freed render passes (a crash, now caught), motion vectors for the
  upscaler, and borrowed engine state that has to be saved and put back.

Each missing feature would need its own re-implementation, and it would never quite match. This
is complaint 2.

## 2. Crossing without a loading screen (in use)

The engine's own door teleport, with the loading screen, its waits and the autosave skipped. The
street is handed over to the engine still attached. About 70 ms per crossing, of which ~25 ms
comes from two crime-gold mods. Kept in every design.

## 3. Render-merge (the default from outside)

The room's 3D is hung into the world scene, moved by the door mapping, and the game's main pass
draws it at full quality. The patched Community Shaders masks the two spaces per pixel at the
door plane and lights the room's shapes with its own light.

Why it falls short: it only works one way. Looking out would mean hanging the exterior into the
interior, and terrain, LOD, sky and water are global systems that can't be moved
(ARCHITECTURE.md, S3). So the view out stayed with the portal pass.

Open: in the agent's test runs (game in the background, Arcadia's Cauldron and
Belethor's), the doorway seen from outside showed a wooden back wall, not the room. The user
reports that looking in works for them. Not resolved.

## 4. Walk into rooms (`[Portals] bWalkIntoRooms`, built, off by default)

The game keeps the player outdoors while they walk into the merged room:
- the room's collision is copied into the outdoor physics world;
- the view out is the game's own outdoors, at full quality;
- the game switches into the interior only once the doorway has been out of view for 3 s, and
  back out when the player heads for the door.

Why it was turned off (note in `PortalManager.cpp`):
- **Every switch showed:** a long frame, the light stepping, the player's own head for a frame.
- **Far too many switches:** 58 in seven minutes of play, once a second on the stairs of the Hall
  of the Dead.
- **The room's people can't see, greet or fight the player** until the switch, because the player
  is in another cell.

Tested again: with the camera inside the room, the room's walls and floor weren't
drawn (its furniture was), and the outdoors showed through. Not investigated.

## 5. Sectors: interiors moved into the worldspace (docs/WORLD-SECTORS.md, on hold)

A generator moves an interior's references, navmesh and lights into the exterior at its door.
At runtime, a collision filter keeps the two spaces apart and per-pixel masking hides each from
the other. Proven on Swindler's Den and the Bannered Mare: people walk out of the inn onto the
street, seams between rooms work, and line of sight works across a seam.

Why it is on hold:
- **Houses don't fit their shells.** Rooms are larger than the building around them, so there is
  overlap that has to be layered, and rooms with several entrances need seams.
- **"Where is the player" is wrong everywhere.** The player's cell is the street, not the inn:
  quest conditions, location, the interior flag, ownership and trespassing, and music all need a
  faked answer. Every query that gets missed is a silent bug.
- **Size:** connecting every house of a city puts all their references and navmesh into the
  outdoor cells.

Decided: no merges. The code (`tools/places`, `src/World/Sectors.*`) and the generated
places (`Places 7`-`10.esp`, `sectors.json`) were removed.

## 6. Open cities by merge (built, tested, removed)

A generator moved each city worldspace into Tamriel, and a runtime hook answered the
`GetInWorldspace` condition for the city. Whiterun worked end to end.

Why it was dropped: the city worldspace and Tamriel were each built to look right only with their
own surroundings. Along the walls, the two worlds' rocks, terrain pieces, light cards and water
planes met, and the user found a lot of broken geometry. Open Cities fixed that edge by hand. On
top of that, compatibility needed a growing list of runtime answers. Details and findings worth
keeping are in WORLD-SECTORS.md, under "Dropped".

## 7. Open Cities as a base (considered, not taken)

The idea: build on Arthmoor's hand-made edge and let our patcher only forward other mods' changes
onto it. Not taken because it is a dependency, and mods' edits to a city's navmesh are lost under
it.

## 8. A second full render of the outside (not tried)

Run the game's own render a second time for the doorway: the outside, from the camera carried
through the door, into our own targets.

Estimated cost, not measured:
- **3–6 ms per frame, only while a doorway is in view.** The doorway sees a quarter to a third of
  a normal view, so far fewer draw calls.
- **Shadows can be cut down:** the sun's shadow maps only need to cover that view, with fewer
  levels or redrawn every other frame.
- **Screen effects stay small:** they only touch the doorway's pixels, at reduced resolution.
- **A precedent exists:** the game already renders the world a second time every frame for water
  reflections, including sky and LOD.

The blocker is not cost. Once the player is inside, the game has switched the outdoors off: sky
and weather, the sun and its shadows, distant LOD, outdoor lights. The second render would have to
bring all of that back for a few milliseconds and put it away again, every frame. Community
Shaders also takes its per-frame data from the first pass of a frame.

## 9. Proposed: the renderer stays outside while the game goes inside

The idea in one line: keep render-merge (3), but separate "which space the game is in" from
"which space the renderer draws as the world".

- **Crossing:** the player crosses at the door exactly as now (2). The game, AI, physics and quest
  conditions have the player in the room, natively.
- **Near the door:** the renderer keeps drawing the outside as the world, with the outside's sky,
  sun and shadows, weather and LOD. The room is merged in, as when looking in, which already works
  at full quality. The view out is then the game's own outdoors, not our picture.
- **Away from the door:** once the doorway has been out of view for a while, the renderer switches
  to plain interior mode.
- **Cost:** about the same as standing outdoors at that spot, because it's one render, not two.

How it differs from what failed:
- **Unlike 1,** nothing is re-implemented; the outdoors is drawn by the game itself.
- **Unlike 3's dead end,** the exterior is never moved: the room is moved, as it already is when
  looking in.
- **Unlike 4,**
  - there are no switches in the middle of a room on the game's side, and no copied collision;
  - the room's people share a cell with the player, so they can see, greet and fight them.
- **Unlike 5 and 6,** no data is merged, so there are no compatibility answers to fake.

What it doesn't solve:
- **The renderer's own switch back to interior mode has to be invisible.** The merged room must
  be lit exactly like the real interior: the same lights, ambient, directional light, fog and
  image space. The "light stepping" in 4 was this mismatch. It's the same shaders with the same
  inputs, so it's solvable, but it must be measured.
- **Complaint 1** (door geometry) is untouched.
- **Complaint 3** (NPCs looking through a doorway) is only solved for the player's own side. A
  room's people looking out at the street are still in another cell than the street's people.

Unknowns, cheapest check first:
1. **Do sky and LOD survive entering an interior?** Or does the game tear them down, and can we
   keep them, the way the street is already kept attached (`bHandOverStreet`)?
2. **Can the sky be held in outdoor mode** (`Sky::mode`, weather, sun) while the current cell is
   an interior?
3. **Does the door masking work** with the camera in the room? It is needed both ways, and is
   currently broken in the agent's test runs (see 3 and 4).

First test: walk into Belethor's and check in memory what of the outdoors still exists (sky
state, LOD nodes, the street's cells and lights), without changing any rendering yet. The answer
decides between this and 8.

### Prototype state (`[Portals] bMergeOutdoors`, on by default)

The first test (bridge `dump_world` logs an "outdoors probe"), standing in Belethor's:
- **The distant LOD survives indoors.** Tamriel's terrain manager stays active with its LOD loaded;
  the land, object and tree LOD nodes are only culled (`SetAppCulled`).
- **The sky stays in the world scene,** in dome-only mode (`Sky::mode` 2). Seen through the open
  doorway it renders with clouds.
- **The street stays attached** (9 grid cells) and is attached by the portal as before.

Built so far (the street merged into the interior instead of the portal pass):
- **The street is merged:** the street's cells are merged into the interior's scene with
  `RemoteCell::Merge` (the room code, unchanged).
- **The LOD comes along:** the LOD root is moved by the same transform, and its land, object and
  tree nodes are shown.
- **Masking and lighting:** the doorway mask is sent as for a room. The street's shapes are lit by
  the outdoor snapshot (`MergedLighting` for an exterior cell).
- **Doors:** both door panels are hidden.
- **Doorway blockers:** the interior's shapes standing just beyond the door plane are culled. In
  Belethor's, the doorway's wall piece holds a backing panel that the portal quad used to cover.
- **Crossing out** is the portal's, and the merged street is handed over attached.

Found on the way:
- **A light leak that crashed the game:** with the street in the world scene, the engine registers
  one of its lights anew every frame. Adopting each into the portal scene crashed in the world
  scene's queued light removal (ID 106335 -> 106342). Merged streets are no longer adopted from,
  and their lights aren't updated.

**Blocker (open): the interior's room graph culls the street away.** With the doorway mask off,
nothing of the street's solid geometry is drawn; only the sky and some of its glow effects are.
- **Taking the graph out of the world scene** (`SetPortalGraph(nullptr)`) crashed the engine's
  per-actor cull (ID 40335 -> 107507 -> 106433, which reads the graph unchecked).
- **Adding the street's cells to the graph's `alwaysRenderChildren`:** the street's effect shapes
  appear, its solid geometry still doesn't.
- **Adding the LOD root to that list too** hung the game on the first frame.

Next options:
- **Our own cull:** cull the street and the LOD into the main view's accumulator ourselves, as the
  seam view culls `ObjectLODRoot` and `LODRoot` for its own pass. This needs the main view's
  culling in interior mode (ID 107132).
- **A room in the graph:** give the street a room in the interior's graph, with a portal at the
  door, so the engine's own room culling sees it through the doorway.

Tried since, none of which drew the street's solid geometry:
- **Hung under the interior's own 3D** (the graph's only cell node; Belethor's has no rooms).
- **Multibound shapes moved with the merge.** They keep world coordinates; now done in
  `RemoteCell::MoveBounds`, also for rooms merged into the street.
- **`kFixedBound` taken off** the merged cells' nodes, so their bounds follow the move.
- **The distance fade left out** (`kIgnoreFade`), and the doorway mask off (`iMergeDebug` 1).
- **Culled into the main view by us,** right after the engine culls the player's body
  (`MainViewCullHook`, call to ID 36563 in RenderPlayerView, main camera ID 415007, culling
  process ID 414946). The street's shapes then get normal render passes (counted at
  `BSLightingShaderProperty` vfunc 0x2A), but still aren't drawn: that point is probably after
  the main view's cull has been finished and sorted.
- **`Occlusion.esp` off** (the user's suggestion): no change for the street.

What the counters showed:
- **Shadow lights do reach the street:** shadow passes for its shapes are asked for (render
  modes 12 to 15), so the shadow culls traverse it.
- **The main view's cull does not reach it.** The street's particle effects (glows, smoke) are
  drawn, but they come from the light scene's own particle list, not from the cull.

Later the same day (merge on by default, `bMergeOutdoors=1`):
- **The ID 415022 loop is not the cull.** ID 76557 only clears each culling process's collected
  geometry.
- **The cull in RenderDepth is not the world's.** The `Process` call at ID 107139 + 0x710 culls
  the player's skeleton.
- **`ShadowSceneNode` vfunc 0x34 (OnVisible) is never called** for the world scene indoors.
- **Hung under the interior's statics branch**, the street's shapes do get normal render passes
  (render mode 0, counted at vfunc 0x2A), and still are not drawn. With the doorway mask off,
  with the sky hidden, and with the interior's occluders taken out (Belethor's graph has 0 rooms,
  0 portals, 0 occluders), it is the same.
- **Hung directly under the world's light scene,** not even the effects are culled.

What the shapes look like (probe in `dump_world`):
- **A street door and the same model inside differ only by the cast-shadows flag,** which the merge
  takes off on purpose. Both have fade 1.0.

Open question: what drops shapes that have passes. Candidates are the depth pre-pass (the
lighting pass tests EQUAL against it) and registration with the world's light scene (a cell
attached by the engine registers its geometry with the scene it is attached under; merged shapes
are registered with the portal's).

A graphics-debugger capture of one frame (RenderDoc) inside Belethor's would settle it: whether
draw calls for the street's shapes exist, and which test drops their pixels.

**Solved (RenderDoc capture): the doorway mask was another door's.**
- **Pixel history** at the doorway showed the street's draws (its land, Whiterun's scaffolding)
  in the depth pre-pass, discarded by the shader. With no depth written, the lighting pass (depth
  test EQUAL) drew nothing there.
- **The shader's constants** said why. The street's shapes were flagged as the merged space, but
  the doorway it was masked against was at (25915, -6863), across Whiterun: the Bannered Mare's
  door.
- **The cause:** `World::Sectors::UpdateDoorway` sent its nearest place door to Community Shaders
  every frame, however far away, after the portal manager had sent its own. Community Shaders
  holds only one doorway mask.
- **The fix:** a space merged in by a portal now owns the mask, and a place's door is sent only
  within 3000 units.
- **Result, tested:** the street draws through Belethor's doorway at full quality (the game's own
  renderer, night lighting, lit windows). Looking in from outside shows the room again.
- **Probably the same cause:** "looking in" failing in the agent's runs earlier the same day.

Later the same evening (the view out made to look like the outside):
- **Daylight:** the street was lit from a snapshot taken the last time the player was out; loaded
  indoors, or half a game hour inside, there was none and the street was as dark as the room.
  Its sun and ambient are now worked out from the weather the way the sky does it (equal to the
  engine's to 1e-7), times the outdoor image space's sunlight scale (1.7 on a clear day: without
  it the street was a dull grey).
- **Persistent references** (a town's houses, porches, paving) hang in the worldspace's persistent
  cell's node, not in the street cells. After walking out and back in they weren't moved with the
  street, and the distant land showed where the paving should be. Merged with it now.
- **Sun and shadows:** Community Shaders' Interior Sun is switched on while the street is merged
  (`SeamlessPortal_SetInteriorSun`), the engine's sun shadow light gets a directional light of its
  own pointed at the outdoor sun, the street casts shadows again, and the shader applies them to
  the street only (the room keeps its own, unshadowed directional light). Tested in Belethor's at
  13:00: the porch's shadow and the sunlit plaza look like the outside.

Tools for next time:
- **Analysis scripts:** they run in RenderDoc's own Python (`qrenderdoc --python script.py`)
  are in `tools/renderdoc/` (capture path in `RDC_CAPTURE`, output folder in `RDC_OUT`). Pixel history (`PixelHistory`) names the draw and the
  test that dropped a pixel; `GetCBufferVariableContents` shows the mask constants of a draw.
- **Captures:** Community Shaders writes them to `Data/SKSE/Plugins/CommunityShaders/Captures`
  and they are deleted when RenderDoc closes them. Copy them out first.
