# Changelog

All notable changes to this project. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.1.0] - 2026-10-20

First release: the game rebuilt on ReXGlue 0.10. Single player runs through to battles; multiplayer
is untested on this SDK (see ROADMAP.md).

### Added

- `Setup.cmd` / `tools/setup.ps1`: one-shot setup that checks prerequisites, unpacks your XBLA
  package (or copies an extracted folder), builds the pinned SDK and the game, and leaves
  `Play.cmd`. It keeps the recomp-launcher contract (`-Yes`, `-Game`, `[n/7]` steps).
- `launcher/launcher.json` and the installer workflow: every push to `main` builds the
  recomp-launcher installer, and a `v*` tag attaches it to the release.
- `VIG8_AUTOPLAY=1`: a scripted controller for unattended test runs (menus into a Quest battle),
  and `VIG8_AUTOPLAY_DRIVE=<s>` to drive and fire after that long.
- The F11 fullscreen key, on top of the SDK's overlays (F3 stats, F4 settings, `` ` `` console).
- `LICENSE` (MIT), `CHANGELOG.md`, `ROADMAP.md`, `CONTRIBUTING.md`, `CONTRIBUTORS.md`,
  `docs/architecture.md`, `docs/networking.md`.

### Changed

- Rebuilt on ReXGlue 0.10 (`development@bd833a2`, from 0.1.2), through our fork
  [sp00nznet/rexglue-sdk](https://github.com/sp00nznet/rexglue-sdk) at `d50d599`. Two SDK fixes
  came out of the port: sp00nznet/rexglue-sdk#1 (libmspack symlinks break a Windows clone) and
  sp00nznet/rexglue-sdk#2 (build the `XUsbcam` exports).
- The codegen config is now a 0.10 manifest, `project/vig8_manifest.toml`: 22 function entries
  instead of 31 entries and 81 switch tables.
- vig8's options are SDK cvars in the F4 overlay, saved to `project\build\vig8.toml`:
  `signed_in_players` (was the per-slot "connected" switches), `unlock_all_cars`, `lan_port`,
  `relay_host`, `relay_port`. The full-game unlock is the SDK's `license_mask`, on by default.
- Networking ported to 0.10 and trimmed of its debug output; the relay setting is `relay_host`
  (empty = off) instead of a separate on/off switch.
- README rewritten around Getting Started; the dated notes stay in `docs/progress-log.md`.

### Fixed

- The relay registered sessions under title ID `584109A8`; the game's is `584108A8`.

### Removed

- The generated C++ headers and config that were committed (`generated/`, `ppc/`), the
  XenonRecomp-era runtime (`src/`, root `CMakeLists.txt`, `config/`), and tools that now live in
  [360tools](https://github.com/sp00nznet/360tools).
- The menu bar, settings file and dialogs, FPS overlay and keyboard driver: the SDK provides all
  of them now.
- Save/Load State (it could save but never load) and the unimplemented invulnerability option.
- `CLAUDE.md` from the repository.

## History before the changelog

Before 2026-10-20 progress was kept as a dated log, preserved in
[docs/progress-log.md](docs/progress-log.md). Milestones from it: XenonRecomp output booting on a
hand-written runtime (2026-02-19), the move to ReXGlue 0.1 and full 3D gameplay (2026-02-23),
menus and settings (2026-02-23), local split-screen (2026-02-24), and LAN/relay networking
(2026-02-25 to 2026-03-03).

[Unreleased]: https://github.com/sp00nznet/vig8/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/sp00nznet/vig8/releases/tag/v0.1.0
