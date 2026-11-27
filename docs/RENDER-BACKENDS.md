# Render backends: Community Shaders fork, or the game's own shaders (and ENB)

Seamless doors need changes to how shapes are drawn: the doorway mask of render-merge
(two spaces drawn in one pass, each only where the other isn't), the far door's clip in the portal
pass, and the light, ambient and fog of the space behind the door. `src/Render/Backend.h` hides
who does that.

| Backend | When | What does the work |
|---|---|---|
| Community Shaders (fork) | `CommunityShaders.dll` with `SeamlessPortal_SetShaderData` (an older release's patch lacks it: None) | The fork's shaders (COMMUNITY-SHADERS-PATCH.md). |
| Native | No Community Shaders (vanilla shaders, and ENB, which runs on them) | `src/Render/NativeMask.cpp`, `src/Render/NativeLighting.cpp` |
| None | A stock Community Shaders (no fork exports) | Nothing: no render-merge; the portal pass is drawn unclipped, with only the sun and ambient the portal code sets. Crossing without a loading screen works. |

How many rooms can be merged at once (`Render::Backend::MergedRooms`): 4 with
the fork, else 1. `SetMergedDoor` and
`SetMergedLighting` take the room's slot; the native backend accepts slot 0 only.

Chosen once when the portal hooks are installed (`Portal::Manager::Install`, kDataLoaded).
`[Render] sBackend` in `SeamlessSkyrim.ini` overrides: `auto` (default), `cs`, `native`, `none`.
`native` with Community Shaders loaded is allowed for testing: the mask works on CS's shaders too,
but CS lights the world itself, so the room's light doesn't match.

`dump_world` / `portal_status` show it under `render` (backend, why, ENB, and the native backend's
counters). The in-game menu shows the backend's name.

## Native: the doorway mask

No shader is changed. For a draw that must be cut, a geometry shader is bound behind the engine's
vertex shader. It passes every vertex on unchanged and adds five clip distances
(`SV_ClipDistance0/1`); the rasterizer cuts at them per pixel. A plane's distance is linear in
world space and clip distances interpolate perspective-correctly, so the cut is exact.

- One room only: with two doorways, a shape near both would be cut into the pieces outside each
  doorway's X, multiplied; not solved. `[Portals] iShownRooms` above 1 keeps the other rooms loaded
  out of sight here.
- The rule is the fork's `SeamlessDoorClips`: X = beyond the door plane (5-unit margin) and seen
  through the doorway's rectangle. X is convex (the plane, plus four planes through the camera and
  the doorway's edges). The other space's shapes are kept in X (one draw); the camera's space's
  shapes outside X, drawn as up to five disjoint convex pieces. Through a city gate X ends where
  the portal pass's cells end (`SetGatePassage`): a sixth plane and piece. Shapes whose bounding sphere is
  50 units clear of a plane are drawn unchanged, or not at all, without a geometry shader.
- Planes are turned into clip space with the renderer's current view-projection (row vectors,
  camera-relative, as `DrawDoorQuad` uses it), so the portal pass's camera works the same way.
- Which draw: hooks on `SetupGeometry` / `RestoreGeometry` (vfuncs 6 and 7) of the Lighting,
  Utility and Effect shaders plan the cut, bind the geometry shader after the engine's
  `SetupGeometry` and unbind it at `RestoreGeometry`; the engine never binds a geometry shader
  itself. Main pass: depth target `kMAIN` and the world light scene, so shadow maps aren't cut.
  Portal pass: accumulator drawing with the portal light scene (State slot 3).
- One draw per pass: the pieces are drawn by the geometry shader, which puts each triangle out
  once per piece with that piece's clip distances (constant buffer: all pieces' planes and their
  count); no pieces leaves the shape out. A geometry shader may put out 1024 scalars per
  triangle, so a vertex shader with many outputs gets fewer pieces (one in Whiterun: 56 scalars,
  5 pieces); a pass needing more is drawn uncut (`drawsUncut`).
- Not at the draw calls: the immediate context's function table sits inside the context object
  and the runtime rewrites it in place several times a frame, between two sets of its own
  functions. Hooks in it, put back before each pass, still missed 1-3 passes a frame on SE 1.5.97:
  random shapes of a merged room gone (sky in their place) or a street wall in its doorway for a
  frame, the main view's shadow mask on parts of the city behind Whiterun's gate.
- Geometry shaders are generated per vertex shader output signature: the engine keeps each vertex
  shader's bytecode (`BSGraphics::VertexShader::rawBytecode`), it is reflected, HLSL written,
  compiled with d3dcompiler (`gs_5_0`), and its signatures checked against the vertex shader's
  before use. About a dozen in Whiterun, ~50 ms in all, once per session.
- Effect shaders are cut too (the fork cuts only Lighting and Utility), so glow cards and
  particles in a merged room or before the far door are handled.

## Native: light, ambient and fog

`BSLightingShader::SetupGeometry` (ID 107300) reads, per draw: the pass's lights (sceneLights[0]
the sun, then point lights), how many to upload from bits 3-5 / 6-8 of the shader's raw technique
(+0x94; the vertex and pixel shaders don't depend on these bits, ID 108699 / 108700), and the
directional ambient at ID 390984. For the length of the engine's call these are swapped:

- Portal pass: the nearest lights of the portal light scene that reach the shape (up to 7,
  shadow-casting ones dimmed by `LightOcclusion`), as the fork's strict-light path does. The
  engine had paired almost none with the portal's shapes.
- Merged room: the room's lights (they stay in the portal light scene), and the sun colour and
  direction and ambient from `SetMergedLighting`, scaled by `RoomLightScale` (the room's
  brightness against the view's, measured; 1 within a few steps of the doorway).
- Street merged into an interior: the street's fog. Fog is uploaded with the technique (ID 107320
  from SetupTechnique), so the scene's fog property is given the street's values, the technique is
  set up again, and afterwards the property is put back and the batch renderer's cached technique
  (ID 415272) cleared. Set up again from ID 415272 (the technique being drawn), not the pass's
  `passEnum`, which lacks the shadow bits added below: the street then lay in flat light. The SSAO composite (ID 107245 + 0xD26) fogs the picture by depth too and
  gets a copy of the fog property with the street's fog; without it the mountains seen through
  the doorway were black.
- Room merged into the street: its cell's own fog on its shapes. The SSAO composite fogs every
  pixel alike and keeps the street's (it takes the merged fog only with the player indoors).
- Street merged into an interior: the sun's shadows. The frame setup (ID 36561 + 0x374) marks an
  interior's frame sunless; while the street is merged, the interior check is skipped and the
  worldspace it reads is one with a sky (as Community Shaders' Interior Sun does, which owns the
  same sites when it is loaded), so the engine draws the sun's shadow maps and mask indoors. The
  street's shapes get `kShadowDir | kDefShadow` added to their technique (they come with an
  interior's, which doesn't read the mask); the interior's shapes don't, so the outdoor sun's
  shadows don't fall on them. The shadow light points at the outdoor sun (`AimSunShadow`).
- Portal pass (a city through its gate): the sun's shadows. The game's lighting shader reads a
  screen-space shadow mask (PS t14, `.x` the sun) that the engine builds only for the main view.
  The portal pass gets one of its own (`NativePortalShadows.cpp`): a depth prepass first (the
  portal scene culled again into an accumulator in the engine's depth mode, copied from the main
  prepass's, ID 415008, and submitted with kDepthPrepass), the shadow maps drawn for the portal
  camera (`RenderPortalShadows`), then a pass of ours that turns depth and maps into the mask, and
  the lighting passes get `kShadowDir | kDefShadow` with the mask put into the engine's record of
  PS t14 (`RendererShadowState::PSTexture[14]` and its modified bit) after `SetupGeometry`, which
  the draw applies, and the engine's put back at `RestoreGeometry`. Not for a doorway picture's
  capture. Costs a second cull and a
  depth-only draw of what the gateway shows.

Not done natively:
- Skylighting settle and Sky Sync's sun direction (the computed one is used): Community Shaders
  features.
- For a room seen from across the street, the street's light through its doorway and the street's
  fog in front of it (`SetMergedDaylight`, `SetFrontFog`): both are per pixel in the fork's
  shaders.
- Between two interiors, each room's light through the doorway (`SetMergedDaylight`,
  `SetHostDaylight`).
- Water masked at a doorway, and more than one merged room (above).

## ENB

`src/Render/Enb.cpp` finds ENB's SDK exports (`ENBGetSDKVersion` in d3d11.dll / dxgi.dll) and
reports version and whether effects are on. ENB uses the native backend: no game shader is
replaced, so ENB's recognition of the game's shaders is untouched, and the mask leaves no depth
where it cuts, which ENB's screen-space effects read. With ENB the engine's context is ENB's proxy;
its function table is hooked the same way (tested on SE 1.5.97 with ENB 0.505).

With its effects on, ENB binds a screen-sized texture of its own to the lighting pixel shader at
t37, made for the main view, and reads it where the sun's mask (t14) says shadow. Our mask bound
there instead turned shadow black, so it is a term on what the shadow leaves. In the portal pass it
painted the main view's shapes behind a city gate (Tamriel's low-detail Whiterun) faintly over
the picture's shadows. Turning off ENB's shadow, skylighting, AO, IBL and parallax-shadow effects
in `enbseries.ini` didn't remove it. The portal pass's lighting draws get a white texture in every
screen-sized slot from t16 up (`Neutralized` in `NativeMask.cpp`), the one thing still done in
hooks on the context's draw calls (see above: a draw now and then goes past them).

What stays: ENB grades interiors and exteriors with separate settings and switches when the
current cell changes, i.e. at the moment of crossing. The view through a doorway is graded as the
space the player is in.

## Tested

AE 1.7.104, Whiterun (Belethor's, Bannered Mare), except ENB:


- Vanilla shaders (CommunityShaders.dll moved aside): auto picks native; street through
  Belethor's doorway and the Bannered Mare's room from the street both masked cleanly; the portal
  pass (bMergeOutdoors=0) clipped and lit with the inn's lights; mountains through the doorway
  fogged instead of black. This load order is tuned for Community Shaders (inverse-square
  lights), so without it everything was overexposed, with or without portals.
- Community Shaders fork: auto picks it; looks as before.
- CS fork with sBackend=native: mask correct both ways, the room dark (CS ignores pass lights).
- ENB: 0.505 on SE 1.5.97 only (above; what is open is in KNOWN-ISSUES.md).
- Not tested: a stock Community Shaders; performance on a slow GPU (the cut costs a geometry
  shader on the shapes near the doorway and on every shape of the other space that crosses the
  doorway's edges).
