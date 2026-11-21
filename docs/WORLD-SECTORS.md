# SeamlessSkyrim — interiors as part of the world ("sectors")

Status: dropped; its code (`tools/places`, `src/World/Sectors.*`) was removed. Decided: **portals for every door, no merges** (neither
cities nor places). Every space stays where the game put it; the portal shows
the other side and the crossing has no loading screen. The open problems of that design are in
KNOWN-ISSUES.md. This document keeps the sector design and what was learnt building it.

## Dropped: open cities by merge (built, tested and removed)

A generator (`Places city-survey` / `city-generate`) moved a city worldspace into Tamriel: city
references copied in (persistent ones moved), Tamriel's stand-in city disabled inside the city's
footprint, both gate doors disabled (an open gateway), every city navmesh merged into Tamriel's
navmesh of its cell and joined at the gate, Markarth's own land written into Tamriel's. At
runtime the `GetInWorldspace` condition answered the city inside its footprint. Whiterun worked
end to end (walking through the gate, people pathing both ways, save/load, quest conditions); the
other four cities were generated and only looked at.

Why it was dropped:
- The edge between the two worlds. The city worldspace and Tamriel's surroundings were each
  built to look right only with their own counterpart. Rules got the inside right, but along the
  walls both worlds' rocks, terrain pieces, light cards and water planes met (the user's first
  walk around Whiterun: a lot of broken geometry). Open Cities fixed that edge by hand; a patcher
  can't promise it, least of all for Solitude (no closed wall) and Markarth (its own land).
- Compatibility needed a growing list of runtime answers (`GetInWorldspace` done; Papyrus
  `GetWorldSpace`, music, regions, no-grass still open), and copied references got new FormIDs
  that can shift between runs of the patcher (a save's changes to them then land on other
  objects).
- Open Cities itself as a base was considered (its edge is done by hand; the patcher would only
  forward mods' changes onto it). Not taken: it is a dependency, and mods' changes to a city's
  navmesh are lost under it.

Worth knowing if it ever comes back:
- City worldspaces share Tamriel's coordinates; WhiterunWorld also holds 4000+ DynDOLOD copies of
  Tamriel's land and objects for the view from inside.
- Twins: the same piece in both worlds (same base within 64 units) must be kept once, not
  disabled as stand-in (that took Whiterun's outer walls away); a variant in the same spot (a
  `_LOD` building, walls without high collision) keeps the city's version.
- A gate has no opening animation: as an ordinary door it stays shut.
- People only path between the city and Tamriel once the city's navmeshes are triangles of
  Tamriel's own navmesh of each cell; links between separate meshes in the middle of a cell were
  never taken. The originals left in the city worldspace must lose their door triangles, or the
  AI's pathing crashes (`PathingCell`, `NavMeshInfoMap`) on a gate that leads nowhere.
- `GetInWorldspace` (condition function, ID 21475) compares exactly; conditions run through the
  script command table (`TESConditionItem::IsTrue`, ID 29924 + 0x691 calls the entry), so
  replacing the table entry's function answers every condition.
- Console commands on a reference through the bridge need the ID quoted (`"01B1F1".disable`).

## Main compatibility item (for sectors): where the game thinks the player is

The game asks "is the player in the Bannered Mare", "is the player indoors", "is the player in
Whiterun" all over: quest and dialogue conditions (GetInCell, GetInCurrentLocation,
GetInWorldspace, IsInInterior, GetIsCurrentLocation...), Papyrus (GetParentCell, GetWorldSpace,
IsInInterior, GetCurrentLocation), trespassing and crime (cell ownership, the public flag),
music, acoustics, weather, fast travel, and AI packages with location or cell targets.

Where the vanilla answer is already right and nothing is needed:
- Portal doors: after crossing, the player really is in the interior cell. While looking through
  a doorway from outside, they really are outside.

Where it is wrong unless we answer it:
- Sectors: the player's cell is an exterior cell, not the Bannered Mare (or Swindler's Den):
  cell, interior flag, ownership/public, music, location all need the sector's answer.

Two ways to answer, to be decided by a spike (S4 below):
- Hook the engine's queries (the condition functions and the Papyrus natives above, the crime
  and music checks) to answer with the city or sector the player is in. One place to maintain,
  works for every mod's conditions, but each query that is missed is a silent bug.
- Rewrite the data in the generator (conditions comparing against WhiterunWorld become
  "in Tamriel and in Whiterun's location", etc.). Works for conditions in records, not for
  compiled Papyrus scripts.
Likely both: the generator reports every record and script referring to a city worldspace or a
placed interior's cell, so the size of the problem is known per load order.

## Why the portal design can't be the end state (written before the scope decision)

The portal design keeps every interior a separate engine space and makes the other side *look*
present. NPCs can't see, chase or fight the player through a doorway, because the engine never
puts the two in one space: separate coordinates, separate Havok worlds, separate cells. Every
fix for that is a per-system translation layer (detection, combat, pathing, projectiles, sound,
dialogue range, crime), and each missed case is a silent gameplay bug. Every space change is
also a visible event (lighting, image space, eye adaptation, a long frame).

## The design

Interiors become real content of their worldspace, placed where their entrance door is, and the
engine runs them natively: one coordinate system, one Havok world, one navmesh graph, one AI
process list. No cell change happens at the entrance. What used to come from "being in an
interior" (lighting, image space, fog, music, acoustics, location, no sky or weather) comes from a
**sector**: a volume of the world with its own environment, blended at its entrances.

Two parts:

### 1. The place generator (offline, load-order aware)

A Synthesis/Mutagen patcher (C#, runs on the user's load order like other generated patches)
that writes `SeamlessSkyrim - Places.esp` plus `SKSE/Plugins/SeamlessSkyrim/sectors.json`.
For each selected interior (a "place") it:

1. Computes the placement transform from the entrance door pair: the interior's door onto the
   exterior's door (`T = Exterior * Rot180 * Interior⁻¹`, as `Doors::Compute`).
2. **Moves** the interior's references (not copies) into the exterior worldspace cells their new
   positions fall in: same FormIDs, so quests, aliases, scripts, location ref types, keyword links
   and linked refs keep pointing at the right objects. Persistent refs go to the worldspace's
   persistent cell as usual.
3. Converts the interior's navmeshes into exterior navmeshes (vertices transformed, split per
   exterior cell, edge links between the pieces), and links them to the outdoor navmesh at the
   entrance (replacing the door's teleport link with ordinary edges/door links).
4. Turns the entrance load doors into ordinary doors (teleport removed) on both sides.
5. Writes the sector: bounds (oriented boxes, possibly several per place), entrance doorways,
   and the environment taken from the interior cell (lighting/template, image space, fog,
   music type, acoustic space, location, encounter zone, ownership, "public" flag).

Placement constraints the generator checks and reports:
- the moved interior must not intersect other *sectors*;
- where it intersects exterior geometry or terrain (always near the entrance, often for houses),
  that is resolved at runtime by layers (below), not by moving things.

Why offline: the engine then loads moved refs, navmesh and doors natively. Pathfinding,
combat, detection, projectiles, saves and quests need no runtime translation at all. Doing the
same at runtime (moving refs every session, injecting navmesh) would put every one of those back
at risk.

### 2. The runtime (SKSE plugin)

**a. Sector environment (light box).** Per frame: which sector the camera (and player) is in,
blended across each entrance's transition zone. Drives:
- lighting: directional ambient, directional light, fog (interior lighting data / template);
- image space (existing blend, `UpdateTone`, generalised to sector weights);
- sky, sun, weather and precipitation hidden inside;
- music type and acoustic space; ambient sound mix at entrances;
- the player's current location and "in interior" state for quests, scripts and crime
  (hooked queries, since the player's cell is now an exterior cell).

**b. Layers.** Where a sector overlaps exterior content (a house's rooms extending past its
shell, a barrow's first hall inside the hillside, terrain above a cave), the two must not
interact except through the entrance:
- collision: a sector-aware collision filter (each body knows its sector: exterior = 0); bodies
  of different sectors don't collide, except near an entrance doorway, where both sides are
  solid for both (the doorway's frame is shared);
- rendering: shapes of other sectors are clipped at the entrance planes (the Community Shaders
  patch already does this for merged rooms: `SeamlessDoorClips`), and terrain/LOD/sky are hidden
  inside a sector;
- raycasts (line of sight, crosshair, projectiles): the same filter, so an NPC outside sees in only
  through the doorway.

**c. Entrances.** The transition zone of each doorway (radius, depth): outdoor light and sound
mixing with the sector's, both layers solid.

## Places with several entrances

One rigid placement puts only one entrance exactly onto its outdoor door. Measured on the
current load order (`Places fit *`): of 154 single-cell interiors with two or more doors outdoors,
4 fit within 64 units at every door, 23 within 256, 57 within 1024, 78 are further off (Bleak
Falls Barrow's exit lands 15,690 units from where the exit really is), and 19 lead into different
worldspaces. Whiterun's shops are 290-410 units off (their rooms are bigger than their shells).

So a place is placed in **pieces**, each rigid, each anchored to one entrance; pieces meet at
**seams**: a doorway whose two sides are not in the same spot of the world, crossed with the
portal technique (drawn through, the player and NPCs carried across without a load screen).
- The generator fits each place: one piece if every entrance lands within tolerance (doors
  nudged by the residual); otherwise pieces split at the place's own internal doorways: internal
  load doors of multi-cell places first (Bleak Falls: each cell anchored by its own entrance),
  then the engine's room portals inside a cell (the boundaries of its occlusion rooms), chosen so
  each seam is as far from the entrances as possible.
- Inside a piece everything is native. Across a seam, NPCs follow by walking through (carried, as
  `NpcTravel` does now) and the vanilla door-pursuit AI; line of sight through a seam is the one
  thing that stays limited, so seams go where it matters least (deep inside, between rooms).
- Places into two worldspaces (Dragonsreach: city and outside) always have a seam.

## What the portal code becomes

- `Doors::Compute` (door pair transform) → used by the generator and for entrance geometry.
- Community Shaders patch (door plane clip, per-shape lighting of another space) → sector clip and
  per-sector lighting.
- `UpdateTone` and the flash guard → sector image-space blending.
- `RemoteCell`, `Travel`, `PortalRender`'s second view, `NpcTravel` → retired only for places made
  sectors. They stay the main path for every other door.
- The line-of-sight hook (ID 37761, built for seams) applies to portal doorways as well: NPCs
  seeing and chasing the player through a house door.

## Phases and acceptance tests

Each phase ends with an in-game test the agent runs itself (bridge, agent-mode game).

**P0 — spikes (prove the risky parts before building on them)**
- S1 moved refs: an override plugin moving one interior's references into an exterior cell
  (Mutagen). Loads, renders, saves and reloads; a quest alias pointing into the interior still
  resolves. Pass: refs at the new positions after save/load; no errors in the papyrus log.
- S2 navmesh: the interior's navmesh converted and linked at the entrance. Pass: an NPC from the
  interior walks out of the entrance onto the street (walk package / `pathtoref`).
- S3 collision filter: a hook on Skyrim's collision filter that keeps two marked sets of bodies from
  colliding. Pass: the player walks through a marked wall and is stopped by an unmarked one.
- S4 location / interior state (the main compatibility item, see above): with the player in an
  exterior cell, the current location reported as the sector's (quest objective "go to X"
  completes, `GetInCurrentLocation` true). Decides between hooking the queries and rewriting
  conditions in the generator.

**P1 — one dungeon end to end.** A single-cell cave near Whiterun (Swindler's Den): generated,
lit as its interior inside, blended at the entrance, bandits who see the player at the entrance and
chase them out. Pass: the scenario plus save/load inside, a quest touching the place, no load
screen anywhere.

**P2 — layers for overlap.** A Whiterun house (Arcadia's Cauldron): rooms larger than the shell, two
entrances. Pass: walking in either door, nothing from the street inside, nothing of the shop
outside, NPCs outside can't see through walls.

**P3 — multi-cell places and the rest of a hold.** Barrows with several cells (Bleak Falls
Barrow), place definitions for a whole hold, generator robustness on a real load order.

**P4 — release.** Installer instructions (run the patcher after load-order changes), MCM page,
"back to vanilla" (patch disabled), compatibility notes.

## Findings so far (spikes on Swindler's Den and the Bannered Mare)

Generator (`tools/places`, `Places generate <cell> <out.esp> [--arrival] [--anchor <door id>]`):
- Persistent references are moved (same FormIDs; survive save/load), temporary ones copied with
  the originals disabled in the interior.
- Rotation of a placed reference is `Rx(-x) Ry(-y) Rz(-z)`; a turned place needs the full
  composition, not yaw added to z (tilted rocks floated otherwise).
- Interior occlusion data must not come along: room/portal markers, multibound references and room
  links on objects and NPCs crashed the engine in an exterior cell (ID 19165). Room markers are
  written to `sectors.json` as boxes instead.
- Some interiors' doors hang in the void in front of their rooms (Swindler's Den's, 300 units
  short): `--arrival` places by the spot the game puts the player on entering.
- Directions "into the place" come from the teleport targets, never from door rotations (a door
  model's axes are its own).
- Each entrance gets a `CollisionMarker` box across its threshold (the floor both sides share).
- Navmesh: cut per exterior cell, linked between pieces, grid rebuilt (per cell a u32 count and
  u16 triangle indices), joined to the outdoor navmesh at the entrance (its door triangle linked to
  the place's, the outdoor navmesh overridden). Each plugin's NAVI lists only the navmeshes it adds
  or changes and the game merges them; a navmesh without an entry crashed the game at load. Cell
  coordinates are stored Y first (navmesh parent and NAVI entry alike), and every navmesh carries
  the same CRC (A5E9A03C).
- Each navmesh piece is merged into its cell's outdoor navmesh (overridden) as more triangles of it:
  pathing follows links between navmeshes only at cell borders and load doors, and a link to the
  street's navmesh in the middle of its cell was never taken (people sent out walked into a corner).
  The entrance is then two neighbouring triangles of one mesh. Verified: Saadia walked out of the
  Bannered Mare's front door to the street.
- Seam doors stay linked load doors (their outside's target moved with the place), with their door
  triangles: people path through a seam as through any load door. Only the entrance stops being one.
- Heights: a door's origin is ~50 units above its floor on either side; floors are aligned by the
  teleport targets (where the game sets the player down), and the threshold's top lies on the floor.
- Outdoor occlusion boxes (PlaneMarker) inside the place are disabled: the street's buildings are
  filled with them and from outside they hid the whole place behind its doorway.
- Copied references hang from a place enable marker: runtime precombiners (Faster Objects) leave
  enable-parented references alone instead of merging them with the outdoors. Faster Objects' Hi-Z
  culling is still to be checked against places (the user runs without it for now).

Runtime (`src/World/Sectors.cpp`):
- Collision: a hook on `bhkCollisionFilter` (pair filter, vtable at offset 0x10; ray filter at
  0x28). Havok returns `hkBool` through a hidden pointer: CommonLib's `bool` signature crashed.
- Membership: place bodies, the player's controller (by doorway crossings; after a jump by room
  boxes and, for underground places, being under the land), place NPCs' controllers and skeletons.
  In the doorway zone an actor ignores the outdoor geometry lying entirely beyond a cut 85 units
  out of the door, per triangle (Havok's shape collection filter; its two overloads sit in reverse
  order in the vtable, slot 0 being the two-container one), so a shell's porch steps stay and its
  wall behind the door goes; the threshold collides with all.
- People: everyone's movement body (NPCs' controllers are rigid bodies, the player's a proxy) and
  skeleton, by a tracked sector like the player's: the place's own start inside when on its floor,
  strangers outdoors unless in its rooms; it changes when they cross an entrance's door plane or are
  carried beside a seam door's side. Where a place overlaps the street (the inn's kitchen floor lies
  under the street behind it) position alone can't tell.
- Inside a place: a point on its floor (its navmesh triangles) or in its rooms; places without room
  markers (the Bannered Mare) have only the floor.
- Query shapes (the crosshair's is custom pick 2: a ray outdoors, a shape inside; camera, line of
  sight, item and path picks) are in the space they are cast from, the player's when near them.
- Rays are only refused by fixed geometry of another space (statics, land, trees): moving things
  can't be told apart reliably, and refusing them made the place's items unusable.
- Teleports onto a point below the land are lifted onto it (ID 37507 + 0xCEA): not inside a place.
- Light box: the interior's ambient, directional light, fog and image space; no sky. Community
  Shaders (fork) asks `SeamlessSector_InteriorView` and treats the frame as an interior.
- Deep inside: the loaded cells' other contents, land, grass, weather and distant land are hidden;
  near a door the doorway mask (the fork's merged-door clip) separates the two spaces.
- Seams (a place's other doors): crossing either side carries the player across (same spot
  relative to the door, turned with it). Verified in play on the Bannered Mare's back door.
- Place doors (load doors without an animation) are swung open by `Portal::Doors` as the player
  comes near.

Next: the view through a seam. Today a seam's doorway shows the place where it really stands (the
Bannered Mare's kitchen 409 units off its back door) instead of what lies behind the door. The
portal pass (PortalRender) draws a separate light scene that holds the other space's cell; a seam's
two sides are both in the main world scene, so the pass must draw the main scene a second time, from
the camera moved by the seam's transform (outer door onto inner, or back), clipped at the far door,
composited into the near doorway as before, with the near doorway masked like an entrance's (the
doorway mask, with the seam's door instead of the nearest entrance). Risk to settle first: drawing
the main light scene with a second accumulator crashed once ("as soon as the player moved",
PortalRender), so the spike is a second accumulator over the main scene, standing still and moving.
Seeing across a seam: the AI's line of sight is ID 37761 (observer, target, mode; called by
detection and combat, IDs 39639/39640/41246 among twelve callers; ID 53829 HasLineOfSight is only
a dispatcher to it). Hooked at its entry: when it says no and the two are on opposite sides of a
seam, the target is carried across the seam into the observer's side; if the sight line passes the
doorway and a ray each (to the doorway, from the other doorway on) is clear, they see each other.
People's sides are published in the snapshot for the AI's threads. Built, not yet tested.

Seam view: done as planned (the world drawn again from the camera carried across, culling only
ObjectLODRoot and LODRoot: culling the world's light scene itself crashed; masked at the far door
during the pass). Verified on the Bannered Mare's back door from both sides.

Not related to this work, still open: loading a save, travelling to Swindler's Den and loading again
crashes the game with all of this switched off.

## Open questions to answer in P0

- Moving temporary (non-persistent) refs across cells in an override: does the engine accept a
  temporary ref overridden into another cell, or must those become new refs (copies) with only
  persistent/quest-relevant ones moved?
- Exterior cells whose surface navmesh is finalized: does adding a second navmesh island in the
  same cell need regenerated NAVI data the patcher can produce?
- Collision filter hook point and cost (called for every broadphase pair).
- How the player's location is derived each frame (one hook, or several).
