# Plan: the fork on Community Shaders 1.9.1, 2.0 and Bottled Shaders 2.0.3

Written 2026-11-26. Steps 1 and 2 are ported (COMMUNITY-SHADERS-PATCH.md, "On Community Shaders
2.0" and "On Bottled Shaders 2.0.3"), and the installer and `tools/make-release.ps1` offer all
three. Goal: one release whose installer patches any of Community Shaders 1.9.1, Community
Shaders 2.0 and Bottled Shaders 2.0.3.

## The bases

| Base | State on 2026-11-26 | Fork branch |
|---|---|---|
| Community Shaders 1.9.1 | released | `seamless-portal` (today) |
| Community Shaders 2.0 | `v2.0.0-rc.2` tagged, not released | new, from the 2.0 tag |
| Bottled Shaders 2.0.x | 2.0.3 on Nexus (mod 192816); source `InTheBottle/Bottled-Shaders`, branch `Bottle-Compendium` | new, from the commit 2.0.3 was built from |

Bottled Shaders 1.0.x (1.0.28) is not a target.

## Order

1. **Community Shaders 2.0:** branch from the upstream tag and redo our 12 commits by hand. The
   fork's 1.9.1 is one squashed commit, so nothing is shared with upstream history and a rebase
   isn't possible. Most of the work is here: from 1.9.1 to 2.0 `dev`, `Lighting.hlsl` changed
   +877/-471, `LightLimitFix.cpp` +1241/-151, `State.cpp` +838/-246, `SharedData.hlsli`
   +341/-51. Start on rc.2, move to 2.0.0 when it is released.
2. **Bottled Shaders 2.0.3:** it merges Community Shaders `dev` every few days, so it is 2.0 plus its
   own changes. Cherry-pick the 2.0 port onto its 2.0.3 commit. Its own changes in our files are
   mostly shaders: `Lighting.hlsl` +739/-157, `DeferredCompositeCS.hlsl` +328/-157,
   `SharedData.hlsli` +293/-92, `Effect.hlsl` +179/-52.
3. **Community Shaders 1.9.1:** stays on `seamless-portal`; a change made on one branch is made
   on the others.

Each branch keeps `SeamlessPortal_*` the same, so SeamlessSkyrim's backend
(`src/Render/Backend.cpp`) needs no change. `COMMUNITY-SHADERS-PATCH.md` says what each change is
for; where a base differs, it says so there.

## Bottled Shaders facts

- Its DLL is `CommunityShaders.dll`, the file layout is Community Shaders'. Its Nexus archives are
  full installs (no fomod, files at the root), 833 files in 2.0.3.
- It releases often (212 commits in the 30 days before 2026-11-26). We ship the DLL and the files
  we change, as for Community Shaders, so each option fits one exact release; a Bottled Shaders
  update brings its own DLL back and SeamlessSkyrim falls to the "None" backend until our next
  release.
- 2.0.3 is `Bottle-Compendium` `e022b1c7` (no tag; `CMakeLists.txt` says 2.0.0): found by
  comparing the archive's shaders with the branch's history, all equal there. Its DLL is newer than
  the commit before and older than that one. The next release is found the same way.
- GPL-3.0: publish the branch, as for the Community Shaders fork (`SOURCE.txt`).

## Installer

One group, exactly one of: Community Shaders 1.9.1, Community Shaders 2.0, Bottled Shaders 2.0.3,
none. Each option is its own folder in the archive.

- MO2 preselects with `fileDependency`: Bottled Shaders by `Shaders\Features\SnowCover.ini`
  (in use since `baa3733`), 2.0 against 1.0 by `Rainbows.ini`; Community Shaders 2.0 against 1.9
  by `ViewAtmosphere.ini` or `SceneManager.ini`. Vortex can't (it answers from its plugin list), so
  the option texts name the exact version.
- `tools/make-release.ps1` builds and gathers each branch (`$forks`). `tools/build.ps1
  -CommunityShaders -CsFork <worktree>` builds the one the test game has; `deploy-vortex-mod.ps1`
  stages one fork into the SeamlessSkyrim mod (`-CsFork`, `-CsBase`, 1.9.1 by default).

## Testing

Each base is a separate game setup. The suite has to pass on each, and the bridge can't tell them
apart today: `portal_status` should report which base the fork was built on (an export, e.g.
`SeamlessPortal_Base()`).
