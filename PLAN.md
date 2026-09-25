# C4D → Spout: Research & Build Plan

Goal: a Cinema 4D plugin that publishes the live viewport as a Spout sender, so
TouchDesigner, Resolume, OBS, Unreal, etc. can receive it on the same Windows machine.

Target: Cinema 4D 2026.x (2026.3 is current), Windows 10/11 x64. Spout is Windows-only.

---

## 1. Research findings

### Cinema 4D SDK (2026.x)
- **Build system:** CMake. CMake arrived in 2025.2 and became the only supported system in
  2026.0. The SDK targets **C++20**. Builds use Visual Studio 2022. 2026.3 added Windows
  ARM64 and Xcode 26 support.
- **SDK source:** the full SDK (frameworks + CMake tooling) ships with C4D and is on
  developers.maxon.net/downloads. The GitHub repos under `Maxon-Computer` hold **examples
  only**, not the frameworks (`Cinema-4D-Cpp-API-Examples`, `Cinema-4D-Python-API-Examples`).
- Each plugin module still has a `project/projectdefinition.txt` file (APIs used, `C4D=true`,
  `ModuleId`). The CMake layer reads it. Output is a `.xdl64` in a folder under
  `plugins/`.
- **Viewport capture:** `BaseDraw::GetViewportImage(maxon::ImageRef&)` returns the current
  colour framebuffer of a viewport. It is read-only and **C++ only** (Python doesn't wrap
  it). Maxon staff have said it isn't built for high throughput. It's a GPU→CPU readback.
- **No GPU handle is exposed.** The SDK doesn't give access to the viewport's
  D3D/Vulkan/Metal device or textures. That rules out a zero-copy GPU-to-Spout path through
  the public API. **Every workable path involves a CPU readback followed by a GPU upload.**
- Useful hook points:
  - `SceneHookData::Draw` runs inside the viewport draw cycle (per `BaseDraw`, per draw pass).
  - `MessageData::CoreMessage` receives `EVMSG_CHANGE`, `EVMSG_DOCUMENTRECALCULATED`, and
    others after redraws.
  - `MessageData::GetTimer` provides a periodic tick on the main thread.
  - `CommandData` + `GeDialog` provide the UI.

### Spout 2 SDK (leadedge/Spout2, v2.007.017, BSD-2-Clause)
- Components are **SpoutDX** (DirectX 11), **SpoutGL** (OpenGL) and **SpoutLibrary** (a
  C-compatible DLL).
- **SpoutDX** fits best. It creates its own D3D11 device with `OpenDirectX11()`, and
  `SendImage(pData, w, h, pitch)` uploads a CPU pixel buffer into the shared texture through
  `UpdateSubresource`. That matches our situation exactly: we have CPU pixels and no host GPU
  context.
- Pixel format: the shared texture defaults to `DXGI_FORMAT_B8G8R8A8_UNORM`, and
  `SetSenderFormat()` can change it. SendImage assumes 4 bytes per pixel.
- Not thread-safe. Keep all SpoutDX calls on **one** thread.
- SpoutDX.cpp needs these files from `SPOUTSDK/SpoutGL/`: `SpoutDirectX`,
  `SpoutSenderNames`, `SpoutFrameCount`, `SpoutCopy`, `SpoutUtils`, `SpoutSharedMemory`,
  `SpoutCommon.h`. Link `d3d11.lib`, `dxgi.lib`.
- Extras: `HoldFps()`, `SetFrameSync()`, and `SetFrameCount` (frame counting for receivers).

### Prior art
None found. No public C4D Spout sender exists (other hosts such as Unreal have one).

---

## 2. Approach options

| Option | How | Verdict |
|---|---|---|
| **A. C++ + `GetViewportImage` + SpoutDX::SendImage** | Read the viewport framebuffer, convert it to BGRA8, and send it on a worker thread | **Chosen.** This is the only way to get the real interactive viewport at interactive rates. |
| B. Offscreen "Viewport Renderer" via `RenderDocument` | Render the document with the hardware preview renderer at a fixed resolution | Keep as a secondary mode (Phase 4). It allows arbitrary resolution and alpha with no HUD or grid, but it's slower and not the "live viewport". |
| C. Python + SpoutGL pip package | Python has no `GetViewportImage`, so it would have to use RenderDocument | Rejected: too slow, and adds a GL context inside C4D. |
| D. Zero-copy GPU sharing | Needs the viewport's D3D/Vulkan texture | Not possible with the public SDK. |
| E. Screen capture (DXGI Desktop Duplication) of the viewport's screen rect | Capture pixels from the OS | Rejected as primary: it grabs overlapping windows, breaks on multiple monitors or DPI scaling, and is fragile. Possible last resort if A fails the spike. |

---

## 3. Architecture (Option A)

```
C4D main thread                              Sender thread (owns SpoutDX)
────────────────                             ─────────────────────────────
Trigger (draw hook / timer / EVMSG)
  └─ BaseDraw* bd = doc->GetActiveBaseDraw() or chosen view
  └─ bd->GetViewportImage(img)
  └─ convert img → BGRA8 (+ flip if needed)
  └─ push into triple buffer ───────────────▶ wait for new frame
                                              └─ spout.SendImage(buf, w, h)
                                              └─ recreate sender on size change
```

- **Main thread work stays minimal:** readback plus conversion into a pre-allocated buffer.
  Skip the frame if the conversion would exceed a budget.
- **Triple buffer / latest-frame-wins:** the sender never blocks C4D, and stale frames are
  dropped.
- **SpoutDX lives entirely on the sender thread:** `OpenDirectX11`, `SetSenderName`,
  `SendImage`, `ReleaseSender`, `CloseDirectX11`.
- **Resize handling:** SendImage recreates the shared texture when w/h changes. Receivers
  pick that up automatically.
- **Pixel conversion:** use the Image API (`ImageRef::GetPixelFormat`, `GetPixelHandler` /
  `GetPixelStorage`) to read rows as `PixelFormats::RGBA::U8()`. Then either swizzle to BGRA,
  or call `SetSenderFormat(DXGI_FORMAT_R8G8B8A8_UNORM)` and skip the swizzle. Check whether
  the buffer is float or linear (OCIO in 2026) and apply sRGB/view transform if it looks wrong
  in receivers.

### Plugin components
| Class | Type | Role |
|---|---|---|
| `SpoutSenderThread` | `maxon::Thread` or `std::thread` | Owns SpoutDX and runs the send loop |
| `ViewportGrabber` | helper | GetViewportImage → BGRA8 buffer |
| `SpoutCaptureHook` | `SceneHookData` **or** `MessageData` (chosen in the spike) | Decides when to grab |
| `SpoutCommand` + `SpoutDialog` | `CommandData` + `GeDialog` | Start/Stop, sender name, viewport choice, FPS cap, flip, status (res/fps) |
| `main.cpp` | — | `PluginStart` / `PluginMessage` / `PluginEnd`. Stop the thread and release Spout on `C4DPL_ENDACTIVITY` |

Settings persist in the world plugin container (`SetWorldPluginData`).

---

## 4. Phased build plan

### Phase 0 – Environment (½ day)
1. Install C4D 2026.3, VS 2022 (Desktop C++), CMake ≥ the SDK minimum, and a Windows SDK.
2. Download the matching C4D C++ SDK. Build `example.hello_world` through the SDK's CMake
   presets and confirm it loads in C4D.
3. Register plugin IDs on developers.maxon.net: one for the command, one for the
   hook/message plugin.
4. Add Spout2 as a git submodule under `external/Spout2`, pinned to a release tag.
5. Install receivers for testing: SpoutReceiver demo (from the Spout release) and
   TouchDesigner or OBS + Spout plugin.

### Phase 1 – Capture spike (1–2 days) — **the key risk**
Measure the following before building anything else:
- Where `GetViewportImage` returns a complete frame:
  (a) `SceneHookData::Draw` at the last draw pass,
  (b) `MessageData::CoreMessage(EVMSG_CHANGE)` after the redraw,
  (c) a timer tick.
- Whether it includes the HUD, grid, safe frames, or selection highlights, and whether those
  can be toggled.
- Pixel format, bit depth, colour space, row order (flip?), and alpha contents.
- Cost at 1080p and 4K (ms per call). Target is under 8 ms at 1080p.
- Behaviour during timeline playback, with multiple viewports, and with Redshift IPR in the
  viewport.

Exit criteria: a reliable trigger point and known pixel format. If GetViewportImage is
unusable, fall back to Option E or B.

### Phase 2 – MVP sender (2–3 days)
1. CMake: add the Spout sources (SpoutDX + the SpoutGL helpers listed above) to the module
   and link `d3d11`, `dxgi`. Compile Spout without C4D's stylecheck, or build it as a static
   lib target.
2. Implement `SpoutSenderThread` with a latest-frame buffer and a condition variable.
3. Implement `ViewportGrabber` and the chosen trigger.
4. Add a menu command that toggles send on/off using sender name "Cinema 4D".
5. Verify the image in the SpoutReceiver demo: orientation, colours, resize, start/stop,
   and closing C4D with the sender active.

### Phase 3 – UI & robustness (2–3 days)
- Dialog: sender name, source viewport (active / specific view / render view), FPS cap
  (`HoldFps` or trigger throttling), flip, alpha on/off, and a live status line.
- Handle document switching, viewport layout changes, sender name collisions, and D3D device
  loss.
- Force continuous redraw option (e.g. `DrawViews` on a timer) for receivers that want a
  steady frame rate while the scene is idle.
- Colour management: match the viewport's view transform (OCIO) in the output.

### Phase 4 – Optional extras
- **Render mode (Option B):** fixed output resolution with clean alpha, rendered by the
  Viewport/Hardware renderer into its own sender.
- Multiple senders (one per viewport).
- Frame sync (`SetFrameSync`) for receivers that need it.
- Python binding (`c4d.plugins` command IDs) so scripts can start and stop the sender.
- macOS Syphon counterpart (same grabber, different transport).

### Phase 5 – Packaging
- Release build `.xdl64` + `res/` folder zipped as `C4D_To_Spout/`. Add a README with install
  steps and the Spout BSD-2 licence notice.
- GitHub Actions on `windows-latest` is only possible if the C4D SDK can be fetched in CI.
  The SDK isn't redistributable, so plan on local builds unless a private artifact is set up.

---

## 5. Proposed repo layout

```
C4D_To_Spout/
  PLAN.md
  README.md
  external/Spout2/              (submodule)
  plugin/c4d_to_spout/
    project/projectdefinition.txt
    CMakeLists.txt              (if the SDK's CMake layer needs per-module additions)
    res/                        (c4d_symbols.h, dialogs, strings_us)
    source/
      main.cpp
      spout_command.cpp/.h      (CommandData + GeDialog)
      capture_hook.cpp/.h       (SceneHookData or MessageData)
      viewport_grabber.cpp/.h
      spout_sender_thread.cpp/.h
```

Build: copy or symlink `plugin/c4d_to_spout` into the SDK's `plugins/` folder, then run
the SDK CMake preset. Alternatively, point the SDK's CMake config at this repo if it supports
external plugin paths.

---

## 6. Risks & open questions
1. **GetViewportImage behaviour and cost.** The whole approach depends on it, and Phase 1
   settles it.
2. **HUD/grid in the image.** If they can't be excluded, users have to hide them in view
   settings, or use Render mode.
3. **Colour space.** The 2026 OCIO pipeline may give linear or float data, which needs a
   transform.
4. **Frame rate.** A CPU round-trip at 4K will not hit 60 fps. 1080p should be fine.
5. **Idle viewport.** C4D only redraws on change, so the sender sends nothing while the scene
   is idle. That's normal for Spout (receivers keep the last frame), but the force-redraw
   option covers users who need continuous frames.
6. **SDK details not verified here.** developers.maxon.net was unreachable from this
   environment. Confirm the exact `GetViewportImage` signature and the per-module CMake hooks
   against the 2026.3 SDK docs.

## 7. References
- Spout: https://spout.zeal.co/ · SDK: https://github.com/leadedge/Spout2 (SpoutDX: `SPOUTSDK/SpoutDirectX/SpoutDX`)
- C4D C++ SDK docs: https://developers.maxon.net/docs/cpp/ · 2026.3 SDK release: https://developers.maxon.net/forum/topic/16419/maxon-cinema-4d-2026.3-sdk-release
- CMake migration: https://developers.maxon.net/docs/cpp/2026_1_0/manual_migrating_to_2026.html
- Viewport capture thread (GetViewportImage): https://developers.maxon.net/topic/14684/stream-the-viewport-to-python-api-that-sends-an-image-back-and-use-that-as-a-texture
- Examples: https://github.com/Maxon-Computer/Cinema-4D-Cpp-API-Examples
