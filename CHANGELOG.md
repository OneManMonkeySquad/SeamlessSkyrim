# Changelog

## 0.7.2

- Inside a building the street's daylight falls in through the open door onto the floor and
  walls of the entrance.
- No crash from water added after a loading screen.

## 0.7.1

- Trapdoors and hatches stay ordinary doors with their loading screen instead of being made
  seamless, and the door menu names them as a trapdoor.
- People across the street seen from inside a building stay drawn instead of vanishing and
  reappearing every half second.
- Without Community Shaders, the street seen from inside a building has its sun shadows instead
  of lying in flat light.

## 0.7.0

### Support

- **Bottled Shaders 2.0.3 and Community Shaders 2.0.0-rc.2: the installer offers a patch for
  each next to the one for Community Shaders 1.9.1, and in MO2 picks the one that fits the
  installed release.** It no longer offers the Community Shaders patch to Bottled Shaders users.
- **Runs on Skyrim AE 1.6.353 (SKSE 2.1.5), which refused to load the plugin.** A doorway's
  picture there shows in its doorway instead of floating beside it.
- The in-game menu is in French, German, Italian, Spanish, Polish, Russian, Japanese and Chinese
  when the game is. Other languages: see "Translating the in-game menu" in the README. A
  translation with a mistake in a text leaves that text in English instead of crashing the menu.

### Doors

- **A room seen through a door from another interior looks as it does from inside, its tone
  blends across the doorway, and the two rooms light each other through it.**
- A seamless door's prompt says "Open Door" and "Close Door" instead of naming where it leads,
  and changes as soon as the door is used.
- The street seen from inside a building is lit by the sky as outside, instead of darker and less
  blue until walked out into.
- Fires and other particles in a room seen from the street burn and move instead of standing
  still or missing.
- A door set to always open stands open even before its doorway has a picture, showing an
  almost black doorway until the room behind has loaded.

### City gates

- **Walking out through a city gate takes the player out, instead of stopping them short of the
  gate or sending them straight back in.**
- Running back into a city right after walking out of its gate takes the player into the city,
  instead of onto the land mounds of its low-detail copy outside or through a loading screen.
- Walking behind or above a gate's opening, in line with it, no longer takes the player through
  the gate with a loading screen.

### Fixes

- People in a room or street walked into stay solid instead of fading in after the crossing.
- With frame generation on, walking through a doorway no longer shows one smeared frame seen
  from another place.
- Walking out of a shop no longer flashes the shop door's prompt for a frame.
- With ENB, the city seen through its gate has clean shadows instead of faint lines of the
  outside's low-detail city printed over them.
- Without Community Shaders:
  - looking at Whiterun's gate from outside no longer crashes the game;
  - the street seen from inside a building and the city seen through its gate have their sun
    shadows instead of lying in flat light, and those shadows no longer flicker;
  - bushes, grass and leaves seen through a gate or in a doorway's picture have their cut-out
    shapes instead of showing as solid cards;
  - a room seen through its doorway no longer loses random shapes for a frame.

## 0.6.1

### Fixes

- **Cave mouths and other automatic load doors take the player through again (with the game's
  fade, as without the mod). The HUD and menus no longer disappear until a game is loaded.**
- An automatic door in a street shown through a doorway no longer takes the player through
  from inside the building (AE 1.7.104 only).

## 0.6.0

### Doors

- **NPCs cross an open doorway with their walk animation at their own speed, and continue to
  their destination instead of stopping at the door.**
- Doors swing with their own `Open`/`Close` animations; the scripted bounce is gone.
- Exterior doors seen from inside stay shut until the exterior is loaded, then swing open on
  the live view. The captured picture of the exterior is removed.

### City gates

- **Until it loads, a gate shows the current worldspace's LOD city instead of a captured picture.
  Fixes the brick wall in Solitude's gate.**
- Shadows seen through a gate match the city's own at any distance.
- The worldspace behind a gate loads from 4000 units (was 750), and a gate is prioritised over
  the doors around it. A house door still wins when walked up to.
- A gate pair's view is scaled by the ratio of the two doorways, and only when both sides are
  the same mesh more than 1% apart. Whiterun's gate is placed 10% larger on the city side; its
  far arch no longer sits offset.
- A gate's leaves swing in the same direction as the far gate's seen through it, and finish
  swinging before the gate is released.

### Performance

- Persistent references' 3D loads on the engine's model loader thread instead of the main
  thread: attaching at Whiterun's gate takes 21 ms instead of 79.
- After a crossing, light scenes take in 32 lights per frame for 30 frames: the frame after
  leaving Understone Keep takes 30 ms instead of 96.
- The exterior's cells and land are read while the player is still inside: no 200 ms hitch on
  the first walk out after loading a save.
- The portal pass is scissored to the doorway's screen rectangle: 2.35 ms of GPU time instead
  of 3.96 at Solitude's gate.
- A merged room's actors are listed once instead of every frame; the list grew without bound.

### Fixes

- **Items dropped in a room no longer vanish after walking out and back in.**
- A door's 3D is cloned from a loaded door of the same model, and inherited what the portals
  had changed on it. Clones are now made from the unmodified state:
  - doors keep leaves, handles and collision after loading a save inside a building with open
    doors;
  - interior doors loaded while a building with the same doors was merged into the street no
    longer have their collision misplaced.
- Whiterun's gate: the capture built a plain door's shape instead of the gate's, so it re-ran
  every other frame and the view lagged a frame.
- Whiterun's gate: guards fell through the road. Far-side collision is removed from the
  passage only where its bound's centre is on the player's side of the doorway.
- The exterior's image space no longer applies a few steps past a shop door's plane (blue
  tint); it blends in only at the doorway or inside.
- The doorway just crossed shows its space as soon as it loads, without a fade.
- An always-open exterior door is open on the first frame after entering the shop.

## 0.5.0

### Doors

- **Doors open when you use them, without a loading screen, and shut when used again or a few
  seconds after nobody stands in them. They no longer open as you walk up.**
- Doors swing with a bounce off their stop and frame, and play their own sounds.
- NPCs open shut doors on their way through and walk straight through open ones.
- New setting **Doors** (in-game menu, `iForceDoors` in `SeamlessSkyrim.ini`): as set per door,
  every door open (locked ones too), or every door shut until used.
- Walking into an open doorway before the space behind it has loaded takes you through with a
  loading screen instead of holding you at the threshold.
- Door tunings made in the in-game menu are kept in `SeamlessSkyrim_Doors_User.ini` and survive
  updating the mod.

### City gates

- City gates swing open and stand open.
- A gate shows its doorway picture from any distance until the city behind it has loaded,
  instead of the low-detail copy of the city.
- Pictures of gates and double doors are sharper: each is taken in the doorway's own shape.
- Walking back through a gate you just came out of no longer leaves you in the low-detail copy
  of the city.

### Looks

- **Rain and snow fall in the street seen through a doorway or a city gate.**
- A doorway's view is as sharp and smooth as the scene around it with upscaling or TAA, and
  follows the camera when turning.
- The outdoors seen through a door has the sun's shadows: trees behind a city gate no longer
  glint in the low sun where cliffs and mountains shade them.

### Fixes

- The Ratway's doorway no longer shows Riften's canal.
- A door placed at a different size on each side looks like one doorway.
- Riften's arched doors leave no hole in the wall.
