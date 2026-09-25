# C4D → Spout: Research & Build Plan

**Goal:** publish the active camera, a Redshift **Spherical** camera, as a lat-long image at
a **fixed 2:1 resolution** (e.g. 2048×1024) through Spout. UE5 receives it and maps it onto
sphere geometry for fast review.

- Update model: on scene change. Continuous frame rate is not required.
- Target: Cinema 4D 2026.x (2026.3 is current) with Redshift, on Windows 10/11 x64. Spout is
  Windows-only.

---

## 1. Key fact: the viewport already draws the spherical projection

With a Redshift Camera set to **Projection → Type: Spherical** as the scene camera, the
standard C4D hardware viewport draws the 360° lat-long projection itself. No Redshift render
or IPR is involved. That means we can use the fast viewport pipeline instead of a full
Redshift render.

**Aspect rule: output = camera frame, never the viewport.** In C4D the camera frame's aspect
comes from the render settings: `RDATA_XRES`/`RDATA_YRES`, `RDATA_FILMASPECT` and
`RDATA_PIXELASPECT`. The viewport shows that frame letterboxed, which is the darkened safe-frame
bands. The plugin therefore:
- reads the aspect from the active or "Spout Preview" render setting, and never imposes its
  own. The user picks only the output width, and height = width / film aspect. The default is
  the render setting's exact XRES×YRES.
- warns when a Spherical camera is active but the aspect isn't 2:1, since that would stretch
  the lat-long
- in method B, crops the grabbed viewport to `bd->GetSafeFrame(&l, &t, &r, &b)` before
  resampling. This discards the letterbox bands and must account for the DPI scale if the
  grabbed image is in device pixels.

The remaining problem is getting **fixed-size** output. The on-screen viewport has whatever
size the window gives it, plus HUD, grid and overlays. There are three ways to get the
pixels, in order of preference:

| # | Method | Fixed size? | Speed | Notes |
|---|---|---|---|---|
| **A** | `RenderDocument()` with the **Viewport Renderer** (`RDATA_RENDERENGINE_PREVIEWHARDWARE`) at 2048×1024 | ✅ exact | fast (hardware viewport, offscreen) | Clean output with no HUD or grid, and viewport display settings come from the render setting. **Unverified: whether the Viewport Renderer honours the RS Spherical projection the way the interactive viewport does.** Phase 1 test #1. |
| B | `BaseDraw::GetViewportImage()` of the live viewport, cropped to the safe-frame rect (`bd->GetSafeFrame`) and resampled to 2048×1024 | ≈ (resampled) | fastest (already drawn) | Quality depends on the viewport's on-screen size. HUD and grid must be turned off in that view's filter. Fallback if A doesn't do spherical. |
| C | `RenderDocument()` with **Redshift** at 2048×1024 | ✅ exact | slowest (full scene translation per render) | Final-quality lighting. Offered as a "quality" mode, not the default. |

Methods A and C share the same code path; only the render engine differs. So the plugin gets
a **Render engine: Viewport / Redshift** option almost for free. B is a separate capture path
that gets built only if A fails.

**Viewport caveat to check:** a real-time viewport can only bend geometry at the vertices, if
that's how it implements the spherical projection. Large polygons, such as a big ground plane
or long walls, may then show straight edges where the true projection curves. Compare against
a Redshift render in Phase 1.

---

## 2. Research findings

### Cinema 4D SDK (2026.x)
- **Build:** CMake-only since 2026.0, targeting C++20, built with VS 2022. The full SDK ships
  with C4D and on developers.maxon.net. The `Maxon-Computer` GitHub repos contain only
  examples.
- **`RenderDocument(doc, settings, progressHook, …, bmp, flags, BaseThread* th)`.** From the
  SDK's `examples_ocio.cpp`: "RenderDocument itself is not inherently main thread bound".
  It blocks the caller, and `th` allows cancellation. The SDK's material-preview example uses
  `RDATA_RENDERENGINE_PREVIEWHARDWARE` through the same call, which confirms Viewport Renderer
  rendering from code.
- **Colour (2026 OCIO default):** render into a `MultipassBitmap` with `COLORMODE::RGBf` and
  `RDATA_BAKE_OCIO_VIEW_TRANSFORM_RENDER = false`. Then
  `BakeOcioViewToBitmap(bmp, settings, savebit)` produces display-referred pixels. This is
  per the SDK example.
- **`BaseDraw::GetViewportImage(maxon::ImageRef&)`** reads the viewport colour buffer. It's
  C++ only and read-only, and it's only needed for method B.
- **Change detection:** `MessageData::CoreMessage` receives `EVMSG_CHANGE`.
  `MessageData::GetTimer` provides the debounce. `bd->GetSceneCamera(doc)` gives the active
  camera.
- **Threading:** clone the document on the main thread (`GetClone(COPYFLAGS::DOCUMENT)`) and
  render the clone on a worker. Open question: whether the Viewport Renderer has to run on the
  main thread, since it uses the GPU context. Phase 1 test #3.

### Spout 2 (leadedge/Spout2, v2.007.017, BSD-2-Clause)
- **SpoutDX** creates its own D3D11 device, and `SendImage(pixels, w, h, pitch)` uploads a CPU
  buffer. `SetSenderFormat()` picks `R8G8B8A8_UNORM` (display-referred) or
  `R16G16B16A16_FLOAT` (linear).
- Not thread-safe, so one thread owns it. Receivers keep the last frame, which fits updating
  on change.
- Sources: `SpoutDX.cpp/.h` plus these from `SPOUTSDK/SpoutGL/`: `SpoutDirectX`,
  `SpoutSenderNames`, `SpoutFrameCount`, `SpoutCopy`, `SpoutUtils`, `SpoutSharedMemory`,
  `SpoutCommon.h`. Link `d3d11.lib`, `dxgi.lib`.

### UE5 side (outside this repo)
- Use a Spout receiver plugin (e.g. the Off World Live toolkit or an open-source Spout2 UE
  plugin) that writes to a Render Target. Apply it with an unlit material on the sphere.
- Expect to align the seam and U direction: Redshift's equirect U=0 vs the sphere's UV seam,
  and a flip if viewed from inside.

---

## 3. Architecture

```
Main thread (MessageData)                        Render worker                        Spout thread
─────────────────────────                        ─────────────                        ────────────
EVMSG_CHANGE → mark dirty, restart debounce
Timer tick (debounce elapsed, ~150 ms):
  if render running → mark stale / cancel
  clone doc; camera = bd->GetSceneCamera()
  settings = "Spout Preview" RenderData copy
    XRES = user width, YRES = width / film aspect
    engine = Viewport|RS
  dispatch ─────────────────────────────────────▶ RenderDocument(clone, …, bmp, th)
                                                  BakeOcioViewToBitmap → RGBA8
                                                  push latest ──────────────────────▶ SendImage()
```

If Phase 1 shows the Viewport Renderer must run on the main thread, the render happens
directly in the timer tick. At 2K this should take milliseconds to tens of milliseconds, so
it's acceptable. Redshift mode stays on the worker.

- **Latest wins:** at most one render in flight. A change during a render triggers one
  follow-up render.
- **Render settings:** the user creates a **"Spout Preview"** render setting in the scene.
  It controls viewport display options (lines off, textures on, etc.) or Redshift sample
  settings. The plugin overrides only the resolution and, optionally, the engine. No Redshift
  parameter IDs are hard-coded.
- **No live-document mutation:** all changes apply to the clone or a copy of the settings.

### Plugin components
| Class | Type | Role |
|---|---|---|
| `SpoutPreviewMessage` | `MessageData` | Change detection, debounce, clone and dispatch |
| `RenderWorker` | `C4DThread` | RenderDocument, OCIO bake, RGBA8 conversion |
| `SpoutSenderThread` | `C4DThread` | Owns SpoutDX; sends the latest frame |
| `SpoutPreviewCommand` + dialog | `CommandData` + `GeDialog` | UI |
| `main.cpp` | — | Registration. On `C4DPL_ENDACTIVITY`: cancel, join threads, release the sender |

**Dialog:** Enable · Sender name (`C4D_LatLong`) · Output width (default: render setting
XRES; height follows the camera/film aspect, shown read-only with a warning if it's not 2:1
for a spherical camera) · Engine (Viewport / Redshift) · Render setting picker
(default "Spout Preview") · Output 8-bit display / 16F linear · Debounce ms · Render now ·
Status (last render ms, state).

---

## 4. Phased plan

### Phase 0 – Environment (½ day)
C4D 2026.3 + Redshift, VS 2022, CMake, and the C4D C++ SDK. Build `example.hello_world` and
confirm it loads. Register 2 plugin IDs. Add Spout2 as a pinned submodule. Install the
SpoutReceiver demo and a UE5 Spout receiver.

### Phase 1 – Python spike (1 day, no build needed)
Run `prototype/spike.py` in the Script Manager against a scene like `Camping.c4d`:
1. **The deciding test:** `RenderDocument` with the Viewport Renderer at 2048×1024 using the
   RS Spherical camera. Does the output show the spherical projection, or a plain perspective
   view?
2. Timing at 1K/2K/4K: Viewport Renderer vs Redshift, and first render vs subsequent renders.
3. Whether the Viewport Renderer works from a `C4DThread` or needs the main thread.
4. Aspect: confirm the output matches the safe frame (letterbox excluded) and that the
   lat-long spans the full 360°×180° edge to edge. For B, confirm that `GetSafeFrame`
   coordinates line up with the `GetViewportImage` pixels, including under DPI scaling.
5. Colours after the OCIO bake vs the viewport. Large-polygon straight-edge artifacts vs the
   Redshift render.

Outcome: choose between A and B as the default path.

### Phase 2 – MVP C++ plugin (2–3 days)
1. CMake module with the Spout sources (as a separate static lib target, outside the C4D
   stylecheck) and `d3d11`/`dxgi` linked.
2. `SpoutSenderThread` + a test-pattern command. Verify in SpoutReceiver and UE5.
3. Render path from Phase 1 + OCIO bake + send, via "Render now". Verify orientation, seam and
   colour on the UE sphere.

### Phase 3 – Auto-update on change (1–2 days)
`MessageData` with EVMSG_CHANGE, debounce and latest-wins. Test by dragging objects, orbiting
the camera, switching documents, closing C4D mid-render, and running user renders at the same
time.

### Phase 4 – UI & polish (1–2 days)
Dialog, persistence, the Redshift engine option, 16F output, status and errors (no camera,
camera not spherical, render failure).

### Phase 5 – Packaging
`.xdl64` + `res/` zip. README covering install, how to set up the "Spout Preview" render
setting, the UE5 material setup, and the Spout BSD-2 notice.

---

## 5. Repo layout

```
C4D_To_Spout/
  PLAN.md
  README.md
  external/Spout2/                 (submodule)
  prototype/spike.py               (Phase 1)
  plugin/c4d_to_spout/
    project/projectdefinition.txt
    res/
    source/
      main.cpp
      preview_message.cpp/.h
      render_worker.cpp/.h
      spout_sender_thread.cpp/.h
      preview_command.cpp/.h
```

## 6. Risks & open questions
1. **Whether the Viewport Renderer supports RS Spherical.** If not, fall back to B (grab the
   live viewport and resample), which is proven to show spherical but depends on window size.
2. **Viewport projection accuracy.** Large polygons may render with straight edges. Compare
   with Redshift; mitigate by subdividing big surfaces or switching to Redshift mode.
3. **Viewport Renderer threading.** It may need the main thread; fine at preview resolutions.
4. **Redshift mode latency.** Each render re-translates the whole scene, which is acceptable
   for occasional "quality" updates.
5. **Clone cost on heavy scenes.** It causes a main-thread hitch per update; the debounce
   limits it.
6. **Not verified from here.** developers.maxon.net was unreachable from this environment.
   Confirm the API details against the 2026.3 docs. The RenderDocument/OCIO details come from
   the SDK's example code on GitHub.

## 7. References
- Spout: https://spout.zeal.co/ · SDK: https://github.com/leadedge/Spout2
- C4D C++ SDK docs: https://developers.maxon.net/docs/cpp/ · 2026.3 SDK release: https://developers.maxon.net/forum/topic/16419/maxon-cinema-4d-2026.3-sdk-release
- RenderDocument + OCIO example: https://github.com/Maxon-Computer/Cinema-4D-Cpp-API-Examples (`plugins/example.image/source/examples_ocio.cpp`; Viewport Renderer usage in `plugins/example.main/source/shader/simplematerial.cpp`)
- GetViewportImage: https://developers.maxon.net/topic/14684/stream-the-viewport-to-python-api-that-sends-an-image-back-and-use-that-as-a-texture
