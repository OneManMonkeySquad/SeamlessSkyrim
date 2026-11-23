# Changelog

## 0.5.0

### Doors

- Doors open when you use them, without a loading screen, and shut when used again or a few
  seconds after nobody stands in them. They no longer open as you walk up.
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

- Rain and snow fall in the street seen through a doorway or a city gate.
- A doorway's view is as sharp and smooth as the scene around it with upscaling or TAA, and
  follows the camera when turning.
- The outdoors seen through a door has the sun's shadows: trees behind a city gate no longer
  glint in the low sun where cliffs and mountains shade them.

### Fixes

- The Ratway's doorway no longer shows Riften's canal.
- A door placed at a different size on each side looks like one doorway.
- Riften's arched doors leave no hole in the wall.
