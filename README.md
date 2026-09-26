# C4D to Spout

A Cinema 4D 2026 plugin that sends the active camera as a fixed-size **2:1 lat-long
(equirectangular) image over [Spout](https://leadedge.github.io/)**. The intended use is fast
360° review: receive the stream in Unreal Engine 5 (or any Spout receiver) and map it onto
sphere geometry.

It renders the scene camera with Cinema 4D's **Viewport Renderer**, which draws a Redshift
Camera set to **Projection → Spherical** as a full 360°×180° lat-long. Updates are sent when
the scene changes, not on a fixed frame rate.

Windows only (Spout uses DirectX 11 texture sharing).

## Requirements

- Windows 10/11 x64
- Cinema 4D 2026.x with Redshift (developed on 2026.3.4)
- To build: Visual Studio 2022 Build Tools (v143 toolset), Windows SDK 10.0.22621, CMake ≥ 3.30, git
- A Spout receiver to view the output, e.g. the Spout demo receiver from the
  [Spout releases](https://github.com/leadedge/Spout2/releases), or a UE5 Spout plugin

## Build

```powershell
git clone --recurse-submodules https://github.com/cedarconnor/C4D_To_Spout.git
cd C4D_To_Spout
.\build.ps1                      # Release; use -Config Debug for a debug build
```

`build.ps1`:
1. extracts the C4D SDK from `C:\Program Files\Maxon Cinema 4D 2026\sdk.zip` into
   `external/c4d_sdk_2026` (once; pass `-C4DInstall <path>` for another install location),
2. builds a static SpoutDX library from the `external/Spout2` submodule (pinned to 2.007.017),
3. configures the SDK with `plugin/custom_paths.txt` and builds the module.

Output: `external/c4d_sdk_2026/_build_x64_v143/bin/Release/plugins/c4d_to_spout/`
(`c4d_to_spout.xdl64`).

## Install

Either copy the `c4d_to_spout` output folder into your Cinema 4D 2026 preferences plugins folder:

```
%APPDATA%\Maxon\Maxon Cinema 4D 2026_<id>\plugins\
```

or, for development, link it so rebuilds are picked up on the next C4D start:

```powershell
New-Item -ItemType Junction `
  -Path "$env:APPDATA\Maxon\Maxon Cinema 4D 2026_<id>\plugins\c4d_to_spout" `
  -Target "$PWD\external\c4d_sdk_2026\_build_x64_v143\bin\Release\plugins\c4d_to_spout"
```

Restart Cinema 4D. The console shows `[C4D to Spout] Loaded.` Note that C4D locks the
`.xdl64` while running, so close it before rebuilding.

## Scene setup

1. Add a **Redshift Camera** and set **Object → Projection → Type: Spherical**. Make it the
   scene camera (select it, then *Cameras → Use Camera*, or the camera's crosshair icon).
2. In the **active render settings**, set the resolution to a **2:1** frame, e.g. 2048×1024.
   The output always uses the render settings' frame (XRES:YRES), never the viewport's
   aspect; a non-2:1 frame is sent anyway but the plugin warns that it will be stretched.
3. For the default Viewport engine, the render settings' Viewport Renderer options control
   the look (display mode, textures, etc.).

## Use

Open the dialog with the **C4D to Spout…** command: search "C4D to Spout" in the command
palette (Shift+C), or find it in the Extensions menu. The dialog is dockable, so you can
add it to a layout.

| Setting | Meaning |
|---|---|
| Auto update | Re-render and send whenever the scene changes (objects, materials, tags, render settings, the scene camera, the current frame, or switching documents). Selecting objects does not trigger a send. |
| Sender name | The Spout sender name receivers connect to. Default `C4D_LatLong`. |
| Output width | Output width in pixels; the height follows the render settings' aspect. `0` uses the render settings' resolution. |
| Engine | **Viewport Renderer (fast)**, the default. **Render settings engine** uses the active render settings' own renderer, e.g. Redshift for final-quality lighting (much slower; the render settings need the Redshift video post). |
| Output | **8-bit display** (default): the OCIO view transform is baked in, so it looks like the viewport/Picture Viewer; `R8G8B8A8_UNORM`. **16-bit float linear**: raw scene-linear render data, `R16G16B16A16_FLOAT`, for receivers that apply their own view transform. Slower and twice the bandwidth, so only use it when you need linear data. |
| Debounce | Auto update waits until the scene has been unchanged this long (default 150 ms)… |
| Max interval while changing | …but sends at least this often during continuous changes such as drags or playback (default 500 ms). Each render briefly blocks the UI, so raise this or lower the resolution if dragging feels sluggish. |
| Render Now | Render and send once, regardless of Auto update. |
| Send Test Pattern | Send a colour-coded 2048×1024 grid for checking orientation on the receiver. |

The status line shows the last result (size, format, render time), and any warning, such as
the scene camera not being an RS Spherical camera or the frame not being 2:1.

Settings are saved in Cinema 4D's preferences and restored at startup, including Auto update.

The same actions are also available as separate commands for shortcuts and layouts:
*C4D to Spout: Render Now*, *C4D to Spout: Auto Update* (toggle with checkmark) and
*C4D to Spout: Send Test Pattern*.

## Receiving

**Spout demo receiver:** start it and pick `C4D_LatLong` from its sender list. The receiver
keeps showing the last frame between updates.

**Unreal Engine 5:** use a Spout receiver plugin that writes into a Render Target, then an
unlit material on a sphere viewed from the inside. Use *Send Test Pattern* to check
orientation. From the sphere's centre you should see:

| Direction | Test pattern | Test scene object |
|---|---|---|
| Straight ahead (+Z, image centre) | red | red cube |
| Right (+X, ¾ width) | green | green cylinder |
| Left (−X, ¼ width) | yellow | yellow cone |
| Behind (−Z, left/right image edge = seam) | blue | blue sphere |
| Up / down | magenta / cyan bands | torus / platonic |

If the image is mirrored or rotated, adjust the sphere's UVs or the material's U offset and
flip; the seam sits at the image's left/right edge.

`prototype/spout_test_scene.c4d` is a ready-made scene with these markers.

## Troubleshooting

- **Viewport is blank / renders are black or fail with code 1.** Check for a system-wide `OCIO`
  environment variable. A legacy config (e.g. ACES 1.2) set there breaks the C4D 2026 viewport
  and makes `RenderDocument` fail for documents created while it was set. Remove the variable
  (or launch C4D with `prototype/launch_c4d2026_no_ocio.bat`), fully restart any launcher
  (the Maxon App passes its environment to C4D), and recreate affected documents.
- **Redshift engine output is black.** The active render settings need the Redshift video post
  and the scene needs lights (Redshift has no default light).
- **Stretched output.** The render settings' frame is not 2:1.
- **A CPU-based receiver shows an old or empty frame.** SpoutDX's CPU receive path is
  double-buffered and lags one frame; since this plugin only sends on change, such receivers
  may show the previous state until the next change. Receivers that read the shared texture on
  the GPU (typical for UE5 plugins) are not affected.

## Verification tool

`tools/spout_grab` is a console receiver that saves one frame from a sender to a BMP
(RGBA8 as-is; RGBA16F sRGB-encoded for preview). It is built with the SpoutDX library:

```powershell
.\build\spoutdx\bin\Release\spout_grab.exe C4D_LatLong out.bmp 5000
```

## Repository layout

```
build.ps1                        build script
plugin/custom_paths.txt          adds the module to the C4D SDK build (alias C2S = repo root)
plugin/spoutdx_lib/              static SpoutDX lib (/MD) + spout_grab target
plugin/c4d_to_spout/
  project/projectdefinition.txt
  source/main.cpp                registration, commands, test pattern
  source/dialog.cpp              settings dialog
  source/settings.cpp            persisted settings and status
  source/auto_update.cpp         change detection (MessageData timer)
  source/pipeline.cpp            render → Spout
  source/render_capture.cpp      RenderDocument, OCIO bake, pixel conversion
  source/spout_sender.cpp        SpoutDX wrapper (isolated from Maxon headers)
tools/spout_grab/                test receiver
prototype/                       Phase 1 spike, test scene, reference outputs
external/Spout2                  submodule (BSD-2-Clause)
PLAN.md                          research and design notes
```

## Notes

- Plugin IDs 1000001–1000005 are Maxon's development IDs. Register real IDs at
  <https://developers.maxon.net/forum/pid> before distributing builds.
- Rendering and sending run on Cinema 4D's main thread.

## Licenses

Spout 2 is © Lynn Jarvis and contributors, BSD-2-Clause (see `external/Spout2/LICENSE`).
Cinema 4D and Redshift are trademarks of Maxon Computer GmbH; this project is not affiliated
with Maxon.
