# C4D → Spout: Research & Build Plan

**Goal:** when the scene changes, render the active camera, a Redshift **spherical
lat-long** camera, at a **fixed 2:1 resolution** (e.g. 2048×1024), and publish the frame as a
Spout sender. UE5 receives it and maps it onto sphere geometry for fast review.

- Update model: re-render on scene change. Continuous frame rate is not required.
- Target: Cinema 4D 2026.x (2026.3 is current) with Redshift, on Windows 10/11 x64. Spout is
  Windows-only.

---

## 1. Why this can't be a viewport grab

- The C4D viewport draws with a perspective projection. It **cannot display a spherical
  lat-long projection**. Only Redshift renders that camera type.
- `BaseDraw::GetViewportImage` returns whatever size the viewport happens to be, with the
  HUD, grid and overlays included. It doesn't give a fixed 2:1 output.
- Redshift's interactive render (the RenderView IPR, or the IPR shown in the viewport) has
  **no public SDK access to its frame buffer**.

**So the plugin drives Redshift renders itself:** `RenderDocument()` with Redshift at the
fixed resolution. It's triggered on scene change, debounced, and a newer change cancels an
older render.

## 2. Research findings

### Cinema 4D SDK (2026.x)
- **Build:** CMake-only since 2026.0, targeting C++20, built with VS 2022. The full SDK ships
  with C4D and on developers.maxon.net. The `Maxon-Computer` GitHub repos contain only
  examples.
- **`RenderDocument(doc, settings, progressHook, …, bmp, flags, BaseThread* th)`.** From the
  SDK's own `examples_ocio.cpp`: "RenderDocument itself is not inherently main thread bound".
  It blocks the calling thread while the render runs on a dedicated render thread. The `th`
  argument lets a caller's thread cancel the render.
- **Colour (important in 2026):** new documents are OCIO by default. The SDK example:
  - renders into a `MultipassBitmap` with `COLORMODE::RGBf`
  - sets `RDATA_BAKE_OCIO_VIEW_TRANSFORM_RENDER = false`, which keeps linear render-space data
  - calls `BakeOcioViewToBitmap(bmp, settings, savebit)` to get display-referred pixels

  We do the same, and get output that matches what the user sees in Redshift.
- **Scene-change events:** `MessageData::CoreMessage` receives `EVMSG_CHANGE`.
  `MessageData::GetTimer` provides a debounce tick. `bd->GetSceneCamera(doc)` gives the active
  camera.
- **Threading rules:** clone the document on the main thread
  (`doc->GetClone(COPYFLAGS::DOCUMENT)`), then render the clone on a worker thread. Never touch
  the live document off the main thread.

### Redshift
- Spherical output comes from the Redshift camera settings: Type **Spherical**, projection
  **Latitude-Longitude**, 360°×180° FOV. Rendered through RenderDocument, it produces a proper
  equirectangular image.
- **Render settings strategy:** don't hard-code Redshift parameter IDs. The user creates a
  render setting in the scene, e.g. **"Spout Preview"**, with the Redshift renderer, low
  samples and the denoiser. The plugin renders with that `RenderData` and overrides only the
  resolution. Quality and speed stay under the user's control, with no Redshift API coupling.
- **Latency is the main unknown.** Each RenderDocument call re-translates the whole scene.
  There's no incremental update like the IPR has. The first render also pays the Redshift GPU
  init cost. It needs measuring (see Phase 1).
- Licensing: RenderDocument with Redshift GPU needs a Redshift GPU licence, just as a normal
  render would.

### Spout 2 (leadedge/Spout2, v2.007.017, BSD-2-Clause)
- **SpoutDX** creates its own D3D11 device (`OpenDirectX11()`).
  `SendImage(pixels, w, h, pitch)` uploads a CPU buffer to the shared texture.
- It supports `SetSenderFormat()`: `DXGI_FORMAT_R8G8B8A8_UNORM` for display-referred 8-bit, or
  `R16G16B16A16_FLOAT` for linear HDR if UE should handle the colour.
- Not thread-safe, so one thread owns it. Senders don't need a steady frame rate: receivers
  keep the last frame, which suits "update on change".
- Sources needed: `SpoutDX.cpp/.h` plus these from `SPOUTSDK/SpoutGL/`: `SpoutDirectX`,
  `SpoutSenderNames`, `SpoutFrameCount`, `SpoutCopy`, `SpoutUtils`, `SpoutSharedMemory`,
  `SpoutCommon.h`. Link `d3d11.lib`, `dxgi.lib`.

### UE5 side (outside this repo, noted for completeness)
- Use a UE5 Spout receiver plugin, e.g. the Off World Live toolkit or an open-source Spout2
  UE plugin. It writes to a Render Target.
- Use an unlit material with the Render Target on the sphere. If you view from inside, check
  the U flip / inward normals and the seam orientation. Equirect U=0 in Redshift vs the
  sphere's UV seam may need a 90° or 180° offset.
- UE doesn't need a matching 2:1 texture size; any power-of-two-ish 2:1 works.

---

## 3. Architecture

```
Main thread (MessageData)                         Render worker (C4DThread)          Spout thread
─────────────────────────                         ─────────────────────────          ────────────
EVMSG_CHANGE → mark dirty, restart debounce
Timer tick (debounce elapsed, e.g. 250 ms):
  if render running → request cancel (th)
  clone doc; set camera = bd->GetSceneCamera()
  pick "Spout Preview" RenderData, set XRES/YRES
  hand clone to worker ────────────────────────▶  RenderDocument(clone, rd, …, bmp, th)
                                                  if cancelled → drop
                                                  BakeOcioViewToBitmap → RGBA8/16F
                                                  flip rows if needed
                                                  push latest frame ──────────────▶ SendImage()
                                                  free clone                        (sender kept
                                                                                     alive between
                                                                                     frames)
```

- **Latest wins:** at most one render runs at a time. Changes that arrive during a render
  mark it stale. It gets cancelled via `th`, or finished and immediately followed by a
  re-render, depending on which proves faster in Phase 1.
- **Change filtering:** `EVMSG_CHANGE` also fires on selection and UI changes. Start by
  accepting all changes, plus the debounce. Refine later if it re-renders too often, e.g. by
  comparing `doc->GetDirty(DIRTYFLAGS::DATA | DIRTYFLAGS::MATRIX)` and the camera matrix.
- **No live-document mutation:** all overrides apply to the clone and a copy of the render
  settings.

### Plugin components
| Class | Type | Role |
|---|---|---|
| `SpoutPreviewMessage` | `MessageData` | Listens for changes, debounces on a timer, clones the doc and dispatches renders |
| `RenderWorker` | `C4DThread` | Runs RenderDocument, the OCIO bake and pixel conversion |
| `SpoutSenderThread` | `C4DThread` / `std::thread` | Owns SpoutDX; sends the latest frame |
| `SpoutPreviewCommand` + `SpoutPreviewDialog` | `CommandData` + `GeDialog` | UI (below) |
| `main.cpp` | — | Registration. On `C4DPL_ENDACTIVITY`: cancel the render, join threads, release the sender |

**Dialog:** Enable toggle · Sender name (default `C4D_LatLong`) · Resolution preset
(1024×512 / 2048×1024 / 4096×2048 / custom, locked 2:1 by default) · Render setting picker
(default: one named "Spout Preview", else active) · Output: 8-bit display (view transform
baked) or 16-bit float linear · Debounce ms · "Render now" button · Status: last render time
and state (idle / rendering / cancelled / error).

Settings persist in the world plugin container. Optionally they can also live in the
document, per scene.

---

## 4. Phased plan

### Phase 0 – Environment (½ day)
C4D 2026.3 + Redshift, VS 2022, CMake, and the C4D C++ SDK. Build `example.hello_world` and
confirm it loads. Register 2 plugin IDs on developers.maxon.net. Add Spout2 as a git
submodule, pinned to a tag. Install the SpoutReceiver demo and a UE5 Spout receiver.

### Phase 1 – Latency spike (1 day) — **decides whether the approach is fast enough**
Use a quick **Python** script in the Script Manager. It needs no build, because
`c4d.documents.RenderDocument` is available there. Render the active spherical camera with
the "Spout Preview" settings at 1024×512, 2048×1024 and 4096×2048, and measure:
- first render vs subsequent renders (Redshift init cost)
- scene translation time vs render time, on light and heavy test scenes
- the effect of samples and denoiser on time
- that the output is correct equirect, and that colours match after the OCIO bake

Targets: under 1 s at 2048×1024 for a typical review scene. If the times are far worse, the
alternatives are lower preview resolution, fewer samples, disabling GI in the preview
setting, or accepting the latency.

### Phase 2 – MVP C++ plugin (2–3 days)
1. CMake module with the Spout sources and `d3d11`/`dxgi` linked. Keep Spout out of C4D's
   stylecheck, e.g. as a separate static lib target.
2. `SpoutSenderThread` and a "send test pattern" command. Verify in the SpoutReceiver demo
   and in UE5.
3. `RenderWorker`: clone, RenderDocument, OCIO bake, RGBA8, send. Triggered by a "Render now"
   command.
4. Verify in UE: orientation, seam, colours, and 2:1 mapping on the sphere.

### Phase 3 – Auto-update on change (1–2 days)
`MessageData` with EVMSG_CHANGE, debounce, latest-wins, and cancellation. Test while dragging
objects and orbiting the camera, switching documents, closing C4D mid-render, and running a
normal Picture Viewer render at the same time (see risks).

### Phase 4 – UI & polish (1–2 days)
Dialog, persistence, the 16-bit float linear option, the status line, and error reporting:
no camera, non-Redshift render setting, render failure.

### Phase 5 – Packaging
Release `.xdl64` + `res/` as a zip. README covering install, how to set up the "Spout
Preview" render setting and the Redshift spherical camera, the UE5 material setup, and the
Spout BSD-2 notice. CI is unlikely, because the C4D SDK isn't redistributable.

---

## 5. Repo layout

```
C4D_To_Spout/
  PLAN.md
  README.md
  external/Spout2/                 (submodule)
  prototype/render_latency.py      (Phase 1 spike)
  plugin/c4d_to_spout/
    project/projectdefinition.txt
    res/                           (c4d_symbols.h, dialogs, strings_us)
    source/
      main.cpp
      preview_message.cpp/.h       (MessageData: change detection + dispatch)
      render_worker.cpp/.h         (RenderDocument + OCIO bake + conversion)
      spout_sender_thread.cpp/.h   (SpoutDX owner)
      preview_command.cpp/.h       (CommandData + GeDialog)
```

## 6. Risks & open questions
1. **Redshift RenderDocument latency.** A full scene translation happens on every change;
   Phase 1 measures it. This is the main risk to "fast review".
2. **Concurrent renders.** A background Redshift render may conflict with a user-started
   Picture Viewer render or the IPR on the same GPU. Test this, and optionally auto-pause
   while other renders run.
3. **Cancellation.** How quickly Redshift honours `BaseThread` cancellation. If it's slow, use
   "finish, then re-render latest" instead.
4. **Colour.** Choose between 8-bit display-referred (simple; UE shows it as-is on an unlit
   material) and 16F linear (correct if UE applies its own tonemapping). Default to 8-bit.
5. **Clone cost.** Cloning large documents on the main thread causes a UI hitch each time.
   The debounce limits how often; measure it on heavy scenes.
6. **Not verified from here.** developers.maxon.net was unreachable from this environment.
   Confirm the RenderDocument/OCIO details against the 2026.3 docs. They came from the SDK's
   own example code on GitHub.

## 7. References
- Spout: https://spout.zeal.co/ · SDK: https://github.com/leadedge/Spout2 (`SPOUTSDK/SpoutDirectX/SpoutDX`)
- C4D C++ SDK docs: https://developers.maxon.net/docs/cpp/ · 2026.3 SDK release: https://developers.maxon.net/forum/topic/16419/maxon-cinema-4d-2026.3-sdk-release
- CMake migration: https://developers.maxon.net/docs/cpp/2026_1_0/manual_migrating_to_2026.html
- RenderDocument + OCIO example: https://github.com/Maxon-Computer/Cinema-4D-Cpp-API-Examples (`plugins/example.image/source/examples_ocio.cpp`)
- Viewport capture limits: https://developers.maxon.net/topic/14684/stream-the-viewport-to-python-api-that-sends-an-image-back-and-use-that-as-a-texture
