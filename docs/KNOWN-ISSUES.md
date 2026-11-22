- SE 1.5.97: `coc MarkarthUnderstoneKeep` from Markarth's street, with the keep merged into the
  street (walked in and out just before), hung the game during the load (once; log ends at the
  `coc`). Not tried on AE.
- SE 1.5.97 with the Community Shaders fork: not tried (the fork builds for SE; Community Shaders
  isn't installed in the 1.5.97 test game).
- The skylight (the indoor stand-in for the sun) in The Drunken Huntsman turns off when seen from outside (jarring).
- Physics objects and arrows are not carried through a doorway.
- Doorways are drawn in front of transparent things.
- Physics objects can break.
- The street's particles (Riften's falling leaves) drawn inside an interior it is merged into, in front of the doorway too (Mistveil Keep's hall, looking out). Cause not looked into.
- Understone Keep seen from Markarth's street: frame ~13.7 ms, ~9.6 ms with it hidden. Left: the game drawing it, the fork's light setup per shape with `LightOcclusion::Visibility` (~1.5 ms), our cull (~0.5 ms).
- Walking into Understone Keep: the first frame takes ~88 ms (the engine's teleport, releasing the keep's merge ~18 ms, attaching the street ~18 ms). Walking out: ~80 ms (teleport ~30 ms, attaching and merging the keep ~35 ms), and the next frame ~50 ms (~20 ms of it the engine taking the keep's lights into our light scene, each with a cull for the shapes it lights, which our scenes don't use; putting them into the active list directly skips that but leaves lights in a state the engine never makes).
- Walking out of Riften's jail into the street: the first frame outside takes ~280 ms (releasing the street ~90 ms, the teleport ~140 ms).
- Whiterun's hanging shop signs: torn off and fallen on walking out of Belethor's, twice in ~30 walk-outs (`belethors-walk-out`; once right after loading the save coming from Riften); cause not found.
- Whiterun's distant objects and trees gone right after walking out of Belethor's (the user had been in and out of it and the Bannered Mare a few times): every LOD node's object and tree layer state 2 (none), the land's normal, the mountains bare low LOD land until a game is loaded. Not reproduced in ~30 walk-outs of either of its doors (front and back), quick or after waiting inside.
- Loading saves one after another across worldspaces (Whiterun's Belethor's, Riften's jail) with portals on crashes now and then (ID 19165 putting an actor's 3D into a cell without 3D), also without the persistent references' 3D loading. During the sectors work (APPROACHES.md, 5), loading a save, travelling to Swindler's Den and loading again crashed with all of that switched off.
- Looking out of an interior built of rooms (Mistveil Keep at Riften), the street's distant tree
  LOD is hidden: there it came into the sun's shadow map twice now and then, the batch list looped
  and the game hung (ENGINE.md, Culling). What adds it twice isn't known.
- Walking out of the Smelter Overseer's House (Markarth's mines), first time after loading the
  save: a flat grey wall, like distant LOD, drawn over the pipe and the city behind the house's
  corner. In each of 22 full suite runs (22:05–23:03), then in none of 7 runs after; cause not
  looked into.
- Walking into Understone Keep in the suite (`understone-keep-walk-in-out`, right after the smelter
  house's route): the game was gone once, with no crash log and the plugin log lost to the
  relaunch. Not again in 4 runs of the same two routes. `run_suite` now keeps the log.
- Loading the Smelter Overseer's House's save again 0.4 s after the street had been attached
  (a launch with the save, then the route's own load): 41 of the merged street's shapes drawn at
  the street's own place, not the room's. Once; not in 5 loads after. Cause not looked into.
- `place_go` out of that house with the street merged into it: `coc Wilderness` left the player
  in the house's cell, then moved to the street's coordinates and falling. Once.
- The street merged into an interior stays merged until it is let go, also far from the door
  (2400 units in an interior with few doors): the cost `closeBy` in `Manager::Update` is there
  to avoid (2.6 ms a frame in Honningbrew Meadery's cellar) is only avoided coming from afar.
  Nothing takes a merge back short of letting the space go.
- Outdoors with few doors around, a fourth space coming within reach halves the reach
  (`sparse`), and every kept room beyond the shorter one is let go in the same frame.
- Seen from indoors without Sky Sync, the street's sun direction and colours are those of the
  moment the player came in, for half a game hour; then they jump to the computed ones
  (`PortalRender.cpp`, `RecentOutdoors`).
- A door's 3D rebuilt while held open is noticed at the next `Doors::Open` on it; the door the
  player came out of is opened once and waits for another opener.
- Walking out of Solitude's gate and turning round at once: for up to 4 s the gateway shows
  Tamriel's own side of the gatehouse (a brick wall), until the view back is attached after the
  purge (`Travel.cpp`, `purgeDeadline`).
- Through Solitude's gate from the city: the snowy mountains behind the fort, seen once outside,
  aren't there; the city's worldspace's own distance shows where the portal pass drew sky.
- Sven and Hilde's house door in Riverwood (01CBB0): its hand turn (-1.6°) was made against the
  door's teleport marker and not converted when the doorway took the door's panel instead (the
  door was locked in every save at hand). The others were.
- Standing at Solitude's gate in the city (the Battle for Solitude save), both gates lost their 3D
  once, 25 s after walking up, and the portal stopped drawing ("a door has no 3D"); the city behind
  the gate showed. Once; not after a reload. Cause not looked into.
- `coc` out of Solitude's Hall of the Dead with the street merged in: the portal was let go, then
  attached to the Hall's door again before the load, and stayed active and merged in Belethor's
  until a second `coc`. Once.
- Walking out of Belethor's (`belethors-walk-out`): one frame just after the crossing drawn almost
  white, as if overexposed. Once in two runs. Cause not looked into.
