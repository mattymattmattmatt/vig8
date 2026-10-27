# Architecture

Four parts; everything below the first line is built on your machine:

```
  your XBLA package ──unpack (360tools)──> extracted/        default.xex + data/*.ib, *.ibz
                                               │
  project/vig8_manifest.toml ─────────┐        │
                                      ▼        ▼
                             rexglue codegen (Setup step 5, and a build step)
                                      │
                                      ▼
                             generated/default/*.cpp      PowerPC → C++, ~60 files, never committed
                                      │
  project/src/main.cpp ───────────────┤
  project/src/stubs.cpp ──────────────┤
  project/src/net.cpp + tools/xlive ──┤
  project/src/vr/  (optional OpenXR) ─┤
                                      ▼
                             project/build/vig8.exe ──links──> rexruntime.dll + rexgpu-xenos.dll
                                                               (ReXGlue: kernel, XAM, D3D12, audio, input)
```

## What each part owns

**ReXGlue SDK** (`tools/rexglue-sdk`, cloned by Setup from
[our fork](https://github.com/sp00nznet/rexglue-sdk), pinned in `tools/setup.ps1`). The recompiler
(`rexglue codegen`) and the runtime: the Xbox 360 kernel and XAM, the Xenos GPU on D3D12 (a plugin,
`rexgpu-xenos.dll`), XMA audio, SDL input and windowing, and the overlays (F3 stats, F4 settings,
`` ` `` console). Its `ReXApp` class runs startup and loads and saves settings; vig8 overrides a
few hooks. The fork is upstream `development` plus a handful of fixes, listed in its README.

**The manifest** (`project/vig8_manifest.toml`). Where the XEX is and where generated code goes,
plus 22 functions codegen's scanner can't find or bound by itself: vtable-only functions, C++
adjustor thunks (`addi r3,r3,N; b target`), and one function the scanner splits in two. Each group
says how it was found. A missing one shows up at run time as
`Call to invalid or unregistered function at guest address 0x...`.

The 0.1 SDK needed 31 function entries and 81 hand-listed switch tables; 0.10's scanner finds the
switch tables and the rest itself.

**`main.cpp`.** `Vig8App` (a `ReXApp`): default paths, defaults for SDK settings (full game
license, keyboard as controller 1), the F11 fullscreen key, and starting/stopping networking.

**`stubs.cpp`.** Overrides of kernel imports (`REX_EXTERN(__imp__Name)`) and game functions
(`REX_EXTERN(sub_XXXXXXXX)`, which can call the original as `__imp__sub_XXXXXXXX`). The linker's
`/force:multiple` lets them replace the SDK's and the generated weak ones. It signs in up to four
local profiles for split-screen (the SDK signs in one), keeps a failed Live session from quitting
the game, and unlocks vehicles on request.

**`net.cpp`.** System Link and relay play; see [networking.md](networking.md).

**`src/vr/`.** Optional headset rendering. It is compiled into every build and does nothing
unless `--vr` is passed and an OpenXR session is running. `vr_hooks.cpp` overrides the game's
render, view, projection, HUD and resolve functions the same way `stubs.cpp` does. The OpenXR
session is built only when Setup was run with `-Vr` (or `OPENXR_SDK` points at the Khronos
headers); otherwise `--vr` stays on the flat window and says so in the log. See [vr.md](vr.md).

## Settings

vig8's options are SDK cvars, so they appear in the F4 overlay under **Vig8** and are saved with
the SDK's own to `project\build\vig8.toml`. Any cvar can also be given on the command line
(`vig8.exe --signed_in_players=2`).

| cvar | Default | |
| --- | --- | --- |
| `signed_in_players` | 1 | Profiles signed in. 2-4 for split-screen; each player also needs a controller. |
| `unlock_all_cars` | false | Every vehicle selectable. Restart after changing. |
| `lan_port`, `relay_host`, `relay_port` | | See networking.md. |
| `license_mask` (SDK) | set to full by vig8 | `0` plays the XBLA trial. |
| `mnk_mode` (SDK) | on, set by vig8 | Keyboard as controller 1. |
| `vr` and the other `vr_*` cvars | off | Headset rendering. Listed in [vr.md](vr.md). |

## The game

| | |
| --- | --- |
| Title ID | `584108A8`, XBLA, 2008 |
| Image | base `0x82000000`, code `0x82090000`-`0x8238D8F8` |
| Engine | Isopod Labs' own; data in `.ib` (raw) and `.ibz` (zlib) bundles under `data/` |
| Package | a LIVE container of 59 files: `default.xex`, `data/`, achievement and rating images |

NetDll imports take a handle as their first argument (`r3`), so their real arguments start at
`r4`; the 9th and later arguments are on the guest stack from `r1+84` (PowerPC ABI).
