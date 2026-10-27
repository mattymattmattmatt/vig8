# VR (Quest via Virtual Desktop)

Optional. A normal `Setup.cmd` / `Play.cmd` build does not open a headset and does not change
the flat frame. `--vr` renders the game to an OpenXR runtime, using the runtime's own D3D12
adapter (`XR_KHR_D3D12_enable`). Virtual Desktop's VDXR runtime and SteamVR both speak that
extension. The game itself is still the Windows build: the Quest is the display, through
Virtual Desktop or a link cable's OpenXR runtime.

Nothing here was run on a headset or against the game. The camera math has a host-side test
(`project/src/vr/vr_math_test.cpp`). The session, the shaders and the hooks need a Windows GPU
and your own copy of the game.

## Build

From the repository root, in the same window you would use for a normal setup:

```
Setup.cmd -Vr
```

That does the usual seven steps, with two differences:

- The ReXGlue SDK comes from `https://github.com/mattymattmattmatt/rexglue-sdk` at the VR
  commit pinned in `tools/setup.ps1` (`cursor/openxr-vr-bridge-095c`). A flat setup still clones
  `sp00nznet/rexglue-sdk` at `d50d599`. That VR commit only adds an optional bridge in the D3D12
  device and present path. Nothing calls it unless `--vr` registers it. If the commit is not on
  the fork yet, setup checks out `d50d599` and applies `tools/rexglue-vr-bridge.patch`, which is
  the same change, and says so on the console and in `setup.log`.
- The Khronos OpenXR SDK tag `release-1.1.65` (`24a1f2b`) is cloned to `tools\openxr-sdk` and
  the loader is built with `-DDYNAMIC_LOADER=ON`. `openxr_loader.dll` is copied next to
  `vig8.exe`. The headers are not vendored into this repository.

`Setup.cmd -Vr` also writes `PlayVR.cmd`. Re-running setup resumes; it rebuilds the SDK only
when the pinned commit changed.

If configure cannot see `tools\openxr-sdk\include\openxr\openxr.h`, the game still builds and
`--vr` logs that this build has no loader. Point `OPENXR_SDK` at a checkout of the same tag if
you built the loader yourself, then reconfigure.

## Run

1. Install [Virtual Desktop](https://www.vrdesktop.net/) on the Quest and the PC streamer, and
   in the streamer set the OpenXR runtime to VDXR. SteamVR as the active OpenXR runtime is the
   other known path. The registry value the loader reads is
   `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime`.
2. Connect the headset so the runtime is already running.
3. `PlayVR.cmd`, or `Play.cmd --vr`.

The desktop window stays. Until the OpenXR session is running, that window is the normal flat
image (boot and menus included). When the session starts, the log says `VR: session running`
and the window becomes the side-by-side image the headset is being fed.

`F9` recenters yaw. It does not change pitch or roll. The first tracked pose recenters on its
own. World scale and the other knobs are cvars, so they show up in the F4 overlay under **VR**
and in `project\build\vig8.toml`.

| cvar | default | |
| --- | --- | --- |
| `vr` | off | Init only. `--vr` / `--no-vr`. |
| `vr_mode` | `stereo` | `theater` puts the whole frame on a virtual screen. |
| `vr_render` | `auto` | `auto` uses the game's 2-player side-by-side layout until a resolve target is known, then draws each eye itself. `sbs`, `stereo`, `alternate`, `constants`. |
| `vr_camera` | `chase` | `seated` keeps orientation and the eye separation, and drops head translation. |
| `vr_world_scale` | 1 | Larger makes the world feel bigger. Eye separation scales with it. |
| `vr_ipd_scale` | 1 | Extra scale on the runtime's eye separation only. |
| `vr_near` | 0.1 | The game's near plane. |
| `vr_proj_y_sign` | 1 | Set `-1` if the view is shifted up or down. |
| `vr_hud` | `quad` | `baked` draws the HUD into each eye. `skip` omits it. See below. |
| `vr_hud_basis` | 1080 | HUD argument is the full-frame aspect times this (1080 or 1440). |
| `vr_menu_theater` | on | An open menu is shown on the virtual screen. |
| `vr_menu_mode` | `pointer` | `address` if `this+0x670` is the menu object rather than a pointer to it. |
| `vr_widescreen` | `auto` | `on` or `off` forces the game's 16:9 device factor. |
| `vr_viewport_floats` | off | Pass viewport x/y/w/h as float bits. Default is integers. |
| `vr_swap_eyes` | off | |
| `vr_pace` | `game` | `headset` waits up to 8 ms for a new frame before reprojecting. Neither mode blocks the game thread on `xrWaitFrame`. |
| `vr_screen_distance` | 2 | Meters, for the virtual screen. |
| `vr_screen_width` | 2.5 | Meters. |
| `vr_show_mirror_quad` | off | Also shows the whole presented image on the virtual screen. |
| `vr_quad_rect` | empty | Crop of that screen, four normalized numbers: `x y w h`. |
| `vr_color_rt` | empty | Hex guest address of the color target, if the learned one is wrong. |
| `vr_force_camera_mode` | -1 | Writes `cam+0x12C` when set. The field might be a byte. Leave it at -1. |
| `vr_constant_fallback` | off | With `vr_render=auto`, patch view and projection in shader constants instead of the CPU hooks. Ignored when a CPU eye path is selected. |
| `bind_vr_recenter` | F9 | |

`vr_render=alternate` draws one eye per frame into the whole window. Both headset views currently
receive that same image, each with its own pose. `vr_render=constants` does not touch the game's
matrices on the CPU; it rewrites the view and projection it can recognize in the vertex-shader
constants, one eye per frame, and can also rewrite a shadow matrix if that matrix looks like the
main projection.

## What the hooks do

The profile is the Vigilante 8 Arcade frame loop, bound the same way as `stubs.cpp`
(`REX_EXTERN`, original available as `__imp__sub_...`).

- `sub_820BFA00` is the render function. For one player and no menu it draws player 1's camera
  twice. The head pose is applied to the view **after** `sub_820B8478`, as
  `V_eye = V × inverse(head in the game's view space)`. The game's view space is x right, y down,
  z forward, so an OpenXR pose (x right, y up, z back) has y and z negated. `cam+0x40` is not
  written: physics, audio and AI read it.
- `sub_8212B760` is replaced with the runtime's asymmetric frustum, in the pre-aspect form the
  rest of `sub_820BE790` expects (it multiplies column 1 by the aspect and converts to reversed
  Z). Horizontal field of view stays the runtime's, not a fixed 90 degrees.
- The runtime's per-eye view pose already includes the IPD. `vr_ipd_scale` scales only the
  offset from the midpoint of the two eyes.
- HUD slots passed into the two eye draws are NULL (`vr_hud=quad` or `skip`), and
  `sub_820B5348` plus the menu `sub_820AAA60` run once afterwards. There is no separate HUD
  color target in the notes, so that pass is not submitted as a headset quad (it would be an
  empty layer). `--vr_hud=baked` is the way to see the HUD: each eye draws player 1's slot.
- Until `sub_82132078` is seen (device, color target, x, y), `auto` does not guess a render
  target. It forces the 2-player layout, points both viewports at player 1's camera, and lets
  the game resolve the two halves itself. The log line `VR: using color target` is the switch
  to the manual path. `--vr_render=sbs` stays on the game's layout. `--vr_color_rt=<hex>`
  overrides the learned pointer.
- A menu, `vr_mode=theater`, or more than one player skips the eye hooks and puts the flat
  frame on the virtual screen.
- Each eye pass re-renders the 1536² shadow map. The radial-blur constant at `0x823902E0` is
  left alone; its consumer was not identified, so it is not nopped.
- Split-screen (`--signed_in_players=2` and up, or a player count other than 1) is not combined
  with stereo.

World units look roughly metric (near 0.1, shadows 10–150) but that was not measured in a
headset. If the car feels the wrong size, change `vr_world_scale` before chasing IPD.

## If it looks wrong

| What you see | What to try |
| --- | --- |
| Flat window, nothing in the headset | The runtime was not active, or this build has no loader. The log says which. |
| Headset image is one flat picture on a screen | The session was not running yet, a menu is open, or the player count was not 1. |
| Both eyes show the same half, or the image is black after `using color target` | `--vr_render=sbs`. If a later `VR: resolve` line names a different target, set `vr_color_rt` to that address. |
| Image is shifted up or down | `--vr_proj_y_sign=-1` |
| HUD missing | `--vr_hud=baked` |
| Eyes swapped | `--vr_swap_eyes` |
| Viewport is a smear or a tiny corner | `--vr_viewport_floats` (the game may be passing x/y/w/h as float bits) |
| The world is huge or tiny | `--vr_world_scale` |
| Want a cockpit | `--vr_camera=seated`. `--vr_force_camera_mode=1` tries the game's alternate camera and may be wrong. |

## Logs to send

`project\build\logs\vig8_*.log`, the newest one. Every VR line is prefixed `VR:`. Also
`setup.log` if the build failed.

Useful lines, if they are there:

- `VR: runtime system ...` and the adapter LUID
- `VR: session running` and `VR: render path ...`
- every `VR: resolve dev ... target ... at x,y` (there are at most 16 distinct ones)
- `VR: using color target ...`
- any `VR:` error, including an OpenXR result number

Also say whether the desktop window was flat, side-by-side, or a single stretched eye, and
whether the headset showed the virtual screen, a stereo image, or nothing. A GPU crash dialog
from Virtual Desktop or SteamVR is worth including too.

## What is still unknown

These are the gaps from the static analysis. They are cvars rather than guesses:

- Whether `this+0x670` is a pointer (`vr_menu_mode=pointer`) or the menu object itself.
- Whether viewport x/y/w/h are integers (`vr_viewport_floats`).
- The sign of the vertical projection offset (`vr_proj_y_sign`).
- The real scale of a game unit (`vr_world_scale`, `vr_ipd_scale`).
- Camera mode 1 (`vr_force_camera_mode`), and whether `cam+0x12C` is a byte.
- The color-target pointer, which is a local in `sub_820BFA00`. The resolve hook learns it.
- Whether any draw culls with its own frustum. If objects pop at the edge of the wider eye,
  the mesh draws are `sub_821266F0` and `sub_820D4628`.
- Whether drawing the viewport twice advances particles or overflows the deferred queue at
  `0x8247CB00`. Split-screen already calls it up to four times, which is why the second eye is
  expected to be safe, but that was not run.

## Sources

The camera addresses and the matrix convention come from static analysis of the user's own
`default.xex` (not published). The projection written for `sub_8212B760` is the pre-aspect
form from that analysis. The final reversed-Z form used for the untracked HUD globals matches
the 1280×720 matrix measured there: `[1, 0, 0, 0] [0, -16/9, 0, 0] [0, 0, 0, 1] [0, 0, 0.1, 0]`.

The OpenXR session follows the Khronos OpenXR 1.0 specification and the
[OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) `release-1.1.65` headers (Apache-2.0).
The asymmetric frustum is the same construction as the loader's `hello_xr` sample
(`openxr_linear`); it is reimplemented here, and no sample code is copied. The shape of the
feature (draw an eye, take the FOV from the runtime, put the HUD on a quad, don't block the
game on `xrWaitFrame`) is the usual one used by flat-to-VR ports of single-threaded games,
including the SM64 VR mod, OpenMW-VR, Dolphin VR and UEVR. Several of those are GPL. None of
their code is in this tree.

The D3D12 bridge lives in the SDK fork, not in upstream ReXGlue. Upstream has asked not to
receive AI-assisted changes, so it stays on the fork.
