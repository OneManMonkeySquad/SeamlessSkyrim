# SeamlessSkyrim

An SKSE plugin for Skyrim AE 1.7.104 that shows the space behind a load door through its open
doorway and lets the player walk through without a loading screen. Part of it is a patch to
Community Shaders, kept in a separate repo: `E:\projects\CommunityShaders-Seamless`, branch
`seamless-portal`.

## Requirements

- Give the illusion of connected spaces.
- Nothing that needs a hand-made patch per location mod. A patcher the user runs on their own
  load order is fine.
- Keep the changes to Community Shaders small.
- Prefer a robust solution to a workaround. If a fix is a workaround, say so when reporting it.

## What to read

Read what the task touches, not everything.

| File | Read it for |
|---|---|
| `docs/BUILD.md` | Building, deploying, paths on this machine, driving the game |
| `docs/HANDOFF.md` (local only, not in the repo), section 7 onwards | How doorways work now. Sections 1 to 6 are history, apart from section 2, the engine facts that were expensive to find |
| `docs/APPROACHES.md` | What was tried and dropped, before proposing a new approach |
| `docs/RENDER-BACKENDS.md`, `docs/COMMUNITY-SHADERS-PATCH.md` | Rendering work |
| `docs/KNOWN-PROBLEMS.md` | Open bugs |

`docs/ARCHITECTURE.md` and `docs/WORLD-SECTORS.md` describe designs that were replaced. Don't
work from them.

When a task teaches something that cost time to find out (an engine function's real behaviour, a
render flag, a trap in the tooling), add it to `docs/HANDOFF.md` (if present) or `docs/BUILD.md` in the same
change.

## Building

```powershell
tools\build.ps1             # build and copy into the game; the game must not be running
tools\build.ps1 -NoDeploy   # compile only; safe while the game runs
```

Don't call `cmake` directly: it isn't on `PATH`.

## Coding style

- No defensive programming. Don't null-check, clamp or early-return around states that shouldn't
  happen; assert or let it crash. A crash at the violated assumption is easier to find than a
  wrong frame later.
- When the number of items is guaranteed to be small, use a flat `std::vector` (linear search)
  rather than a map or set.

## The game

- Before launching, quitting or deploying, check `Get-Process SkyrimSE`. A game with no agent
  bridge (the plugin log says `agent mode: off`) is the user playing: leave it alone and ask.
- There is one game and one bridge. If the bridge answers but you didn't launch the game, another
  session is testing: don't quit or relaunch it.

## Testing a change

- Use `place_go` and `run_route` rather than walking step by step. If the place or the walk you
  need isn't stored, store it (`place_save`, `route_save`) so the next test of that door is one
  call.
- Reproduce the bug before changing code, at the place the user reported it.
- A fix for something visual counts as verified only with a capture from that place after the
  change, compared with one from before. Otherwise report it as built but not verified, and say
  what the user should look at.
- One screenshot can't show a glitch that lasts a frame or half a second (a flash on crossing, a
  door seen closed for a frame, low detail showing briefly). Take every frame instead:
  `run_route(..., capture=true)` or `capture_frames` (see `docs/BUILD.md`), before and after the
  change.
- After a shader include changes or the shader cache is cleared, shaders compile in the background
  for minutes. Don't judge a test before they are done.

## Finishing

- Commit when the user says so, on the branch that is checked out, and name the branch in the
  report. Subject line: what the player now sees, as in `git log`.
- Remove diagnostics added for the task, or make them a bridge command worth keeping. Don't leave
  them uncommitted in the working tree.
- A problem you found and didn't fix goes into `docs/KNOWN-PROBLEMS.md`; one you fixed comes out.
