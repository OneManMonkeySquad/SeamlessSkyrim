# Building, deploying and testing

How to build both halves, get them into the game, and drive a test, on this machine. Engine
facts are in ENGINE.md.

## Paths

| What | Where |
|---|---|
| Game | `E:\SteamLibrary\steamapps\common\Skyrim Special Edition` |
| Plugin log | `Documents\My Games\Skyrim Special Edition\SKSE\SeamlessSkyrim.log` |
| Community Shaders log | same folder, `CommunityShaders.log` (shader compile errors land here) |
| Crash logs | same folder, `Crashlogs\*.log` (trainwreck); `re_lookup` turns a `SkyrimSE.exe+offset` into an ID |
| Plugin settings | `<game>\Data\SKSE\Plugins\SeamlessSkyrim.ini` |
| Load order | `%LOCALAPPDATA%\Skyrim Special Edition\plugins.txt` |
| Community Shaders fork | `E:\projects\CommunityShaders-Seamless`, branch `seamless-portal` (COMMUNITY-SHADERS-PATCH.md) |

## SeamlessSkyrim (this repo)

```powershell
tools\build.ps1                     # RelWithDebInfo, copied into the game
tools\build.ps1 -NoDeploy           # compile only; works while the game runs
tools\build.ps1 -CommunityShaders   # the fork's DLL too (see below)
```

The script finds `cmake`, which isn't on `PATH` (it comes with Visual Studio), and stops before
building when the game is running and the copy would fail.

- Output: `build/RelWithDebInfo/SeamlessSkyrim.dll` and `.pdb`.
- The post-build step copies both into `<game>\Data\SKSE\Plugins` (`SKYRIM_PLUGINS_DIR` in
  CMakeLists.txt). The copy fails while the game runs, so quit it first. It writes into the
  existing files (`tools/copy-in-place.ps1`), so when Vortex deploys the DLL as a hard link the
  build lands in the Vortex mod too.

## As a Vortex mod

`tools/deploy-vortex-mod.ps1` puts the plugin (Release build, no PDB), its two ini files from
`package/`, and the Community Shaders fork's DLL (its ~100 MB PDB only with `-CsPdb`) and changed
files into `E:\Vortex Mods\skyrimse\SeamlessSkyrim`:

```powershell
tools\deploy-vortex-mod.ps1 -Build   # -Build: both DLLs first
```

- New mod folder: restart Vortex, enable `SeamlessSkyrim`, make it load after Community Shaders
  and Skylighting, deploy. Later runs write into the deployed files and need no redeploy unless
  the script says files were added or removed.
- Doors tuned in game (`<game>\Data\SKSE\Plugins\SeamlessSkyrim_Doors.ini`) are copied back into
  `package/` when newer; commit them there.
- `-RestoreStockCommunityShaders` puts stock 1.9.1 back into Vortex's own Community Shaders mod.
  A `cp` into the game's Data folder writes through Vortex's hard links into whichever mod owns
  the file, which is how the fork's files once ended up in the stock mod.
- `-ClearShaderCache` after a change to a `Common/*.hlsli` (see below).
- Vortex shows the version as `?`. A folder it finds in its staging folder gets only a name and
  an install time (`refreshMods` in Vortex's code); version, author and so on come from Nexus
  metadata of the downloaded archive, and no file in the mod folder supplies them. Don't name
  the folder like a Nexus download (`Name-12345-1-0-...`): Vortex takes a `-<number>-` in the
  name for a Nexus mod ID and may pull some other mod's metadata.

## Release archive

`tools/make-release.ps1 -Build` writes `build/release/SeamlessSkyrim-<version>.zip` (version from
`CMakeLists.txt`), a FOMOD installer (`package/fomod/`):

- `Core/`, always installed: the plugin (Release, no PDB), `package/`'s ini, and the game's
  `SeamlessSkyrim_Doors.ini`, which the script first copies into `package/` (commit it).
- `CommunityShaders/`, optional: the fork's DLL and changed files. The installer preselects it
  when `SKSE\Plugins\CommunityShaders.dll` is already installed, and leaves it unselected but
  selectable otherwise. On its own the fork is a partial Community Shaders: its DLL runs without
  the shaders and data it expects, rendering breaks, and SeamlessSkyrim picks the fork backend
  instead of the native one. It still can't tell Community Shaders versions apart: the option's text says 1.9.1.
- `LICENSE` and `SOURCE.txt` at the archive's root, outside both options: the GPL text and the
  GitHub links to this repo and the fork (`-SourceUrl`, `-CsSourceUrl`).
- Quit the game first: `-Build` runs the post-build copy into the game, which fails while it runs.
  Without `-Build` a DLL older than its sources stops the script.

## Community Shaders fork

`tools\build.ps1 -CommunityShaders` does the two steps below. By hand: the build tree is
`build/ALL` (Visual Studio generator), and only the `CommunityShaders` target is needed:

```sh
cd /e/projects/CommunityShaders-Seamless
cmake --build build/ALL --config Release --target CommunityShaders   # about 1.5 minutes
cp build/ALL/Release/CommunityShaders.dll build/ALL/Release/CommunityShaders.pdb \
   "/e/SteamLibrary/steamapps/common/Skyrim Special Edition/Data/SKSE/Plugins/"
```

CMake's build of the fork doesn't copy anything into the game, so do it by hand as above, or run
`tools/build.ps1 -CommunityShaders` or `tools/deploy-vortex-mod.ps1`. `cp` writes into the
existing file, which keeps the game's DLL hard link intact, and therefore also changes the Vortex
mod that file belongs to: once the SeamlessSkyrim mod is deployed that's the right one, before
it's stock Community Shaders.

Shaders aren't built; the game compiles them from `<game>\Data\Shaders`. After editing one in
`package/Shaders`, copy it over the game's file:

```sh
cp package/Shaders/Utility.hlsl "/e/SteamLibrary/steamapps/common/Skyrim Special Edition/Data/Shaders/"
```

- Community Shaders only notices a change to a shader's own file. After changing an include
  (`Common/*.hlsli`), delete that shader's folder under `<game>\Data\ShaderCache\` (for example
  `Lighting`, `Utility`).
- The lighting shaders then recompile in the background for minutes. Until they're done, the
  patch doesn't show.
- A shader that fails to compile is reported in `CommunityShaders.log` ("Failed to compile ...").

## Driving the game from an agent (tools/skyrim-mcp)

Launching and the bridge:
- `game_launch(save=...)` starts the game with the agent bridge and loads the save straight from
  the main menu (`SEAMLESS_AGENT_LOAD`).
- Never launch or quit while the user is playing: check `Get-Process SkyrimSE` first. A game the
  user started has no bridge, and the plugin log says `agent mode: off`.
- Background mode moves the game window to a second virtual desktop **once** per launch and then
  leaves it alone. `game_window_mode("show")` brings it to the front.
- Launched games are silent (`game_audio` toggles it). The plugin mutes XAudio2 inside the game,
  never the Windows volume mixer: Windows keeps a mixer mute across runs, so a crash while muted
  left the user's own games silent.
- Changes to `server.py` only apply after the MCP server is reconnected (`/mcp` in Claude Code).
- `tools\build.ps1` doesn't run from the Bash tool (execution policy); use the PowerShell tool.

Tests:
- A game loaded at launch and one loaded later in the same session are not the same test (the
  persistent references, ENGINE.md). Loading saves back to back across worldspaces with portals
  on crashes now and then (KNOWN-ISSUES.md): relaunch rather than run a long reload loop.
- Walking through a door: `walk_to` a position a little past the door along its inward direction.
  Aimed at a door reference in the other space, the player crosses at an angle beside the
  doorway, and the engine moved them out of the frame's collision, under Belethor's porch.
- `place_save` and `route_save` write their JSON with CRLF: `sed -i 's/\r$//'` before committing.

Scripts beside the MCP server (for loops, and commands with no MCP tool such as `shown_rooms`,
`merged_cull`):
- The bridge serves several clients at once (127.0.0.1:47800, one JSON request per line).
- Run with `PYTHONPATH=. uv run python script.py` in `tools/skyrim-mcp`, then `import server`:
  `server.bridge.call(cmd, **args)` for a bridge command,
  `getattr(server.run_route, "fn", server.run_route)(steps=[...])` for an MCP tool.
- `re_read` takes an offset from the module base. For a heap address pass `address - base`; the
  base is `(Get-Process SkyrimSE).MainModule.BaseAddress` (0x7FF73F6A0000 on every launch so far).

Every frame of a moment (a flash on crossing, a door drawn closed once, low detail over the real
town falls between screenshots):
- `run_route(name, capture=true)` saves the frames of each crossing and of the merge that follows
  it (40 frames, about a second) and lists them under `bursts`, each with its `oddFrames`: frames
  that differ from the one before and the one after, which resemble each other. A route step
  `{"frames": n}` starts a burst at that point of the route.
- `capture_frames(frames, crop)` takes a burst now and returns it as one contact sheet. `crop`
  cuts to the doorway and makes the odd-frame scores more sensitive. The single frames stay in
  the burst's folder at full size.
- `frames_sheet` currently fails ("Unable to serialize ... Image"): read the burst's frames
  directly, or paste them into a sheet with PIL.
- The odd-frame score only finds what lasts a single frame. Something wrong for several frames in
  a row (a weapon drawn black for four) has to be seen.
- Not while measuring stutter: writing a burst out stalls the game for a moment.

Diagnostics:
- `dump_world` writes the outdoors probe to the plugin log: the sky and its colours, the computed
  outdoor lighting, the doorway, and the street references that can't be drawn and why.
- `portal_status` has the active portal, the remote cell, the merged rooms and the distant LOD
  (`lod`: shapes of its land, objects and trees).
- A piece of a merged space missing:
  - bridge `ref_3d {ref}`: the chain of parents with node types and each node's slot in its
    parent, and each node's physics body (flags, motion type, where the physics has it, its
    constraints);
  - `find_nodes {name}`: nodes by name in the world's and the portal's scene;
  - `orphans {cell?}`: the remote cells' references whose 3D hangs nowhere, and each portal
    graph's always-drawn and pending lists.

Frame rate:
- `merged_cull` returns frame times (`frames`) and the main thread's time in the player view
  (`viewMs`, culling and draw submission). `{"mode": 3}` hides the merged room, which shows what it
  costs; `{"mode": 2}` puts it back.
- `travel_profile {file}` samples the main thread for 1.5 s from the next crossing. For a steady
  view, `move_to` back in front of the door right after crossing. The first second or two after a
  merge is the light-occlusion rays catching up (`lightOcclusion.raysTotal` in `portal_status`).
- `sampler_report.py file [from_ms to_ms]` names the functions in SeamlessSkyrim's and the fork's
  frames from their build folders' PDBs, so the DLLs in the game must be those builds.
- The camera moving changes the numbers a lot. Measure with nobody at the controls.

## Graphics debugging

- RenderDoc: Community Shaders writes captures to `<game>\Data\SKSE\Plugins\CommunityShaders\Captures`
  and they are deleted when RenderDoc closes them, so copy them out first.
- From an agent: turn on "Enable RenderDoc Capture" in Community Shaders' `SettingsUser.json`,
  launch, then bridge `renderdoc_capture`. FSR frame generation breaks afterwards; relaunch.
- Analysis scripts are in `tools/renderdoc/` and run in RenderDoc's own Python:
  `qrenderdoc --python script.py`, with `RDC_CAPTURE` and `RDC_OUT` set.
