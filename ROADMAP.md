# Roadmap

## Next

- **System Link on 0.10.** Run two instances (two machines, or one plus a relay) through host,
  search, join and a race. The last 0.1 test found the host's session and then crashed; the
  timing workarounds written after it have never run. See [docs/networking.md](docs/networking.md).
- **Split-screen on 0.10.** `--signed_in_players=2` with two controllers, through vehicle select
  into a race.
- **Startup takes about 90 seconds** before the window appears, all of it inside SDL's video
  initialisation before any vig8 or SDK code logs. This machine has several virtual display
  adapters installed; check whether a clean machine starts at once, and if not, fix it in the SDK.
- **Retire the unregistered-call workaround.** The SDK fork logs an indirect call to an unknown
  function instead of aborting. Once the manifest has every vtable-only function vig8 reaches,
  build against a fork without that change.

## House-style gaps (repo rules, section 10)

- **Headless mode**, `--headless --record out.mp4`, so test runs need no display. SDL's
  `offscreen` video driver (`--video_driver=offscreen`) runs the game but nothing presents, so
  this belongs in ReXGlue's presenter.
- **Conformance harness.** Ground truth would be a scripted controller run with checkpoint frames
  (title, menu, a race) compared against reference captures, reporting how many it reached. The
  reference frames can't be redistributed, so it must skip cleanly without them.
- **netlab builds.** recomp-netlab's builders cross-compile with clang-cl and an xwin splat;
  ReXGlue builds with the GNU-style clang driver and pulls in SDL3, FFmpeg and DXC. Until a builder
  can build ReXGlue, this project builds on the workstation.
- **CI.** The installer workflow runs on every push to `main`. A build check would need the SDK
  built on a hosted runner, and without game data it could only check that the glue compiles.

## Deferred

- Invulnerability (a debug option on the 0.1 build that was never wired to the game).
- Save states. The 0.1 build could save the kernel state but never load it.
- Xbox Live features beyond finding sessions: leaderboards, achievements sync.

## Out of scope

- Distributing the game, a build of it, its generated C++, or any of its data.
- Other Vigilante 8 games (the PlayStation and N64 releases).
