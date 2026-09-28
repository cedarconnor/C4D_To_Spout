# C4D_To_Spout: project memory

Read `PLAN.md` for the full research and plan. This file holds the decisions and context that
must not be re-litigated.

## Goal
A Cinema 4D plugin that sends the **active camera** as a **fixed-size, 2:1 lat-long image**
over **Spout**. The user maps it onto sphere geometry in **UE5** for fast 360 review.

## Environment
- Cinema 4D **2026.x** (latest, 2026.3), Redshift, Windows x64. Spout is Windows-only.
- C4D C++ SDK: CMake-only (since 2026.0), C++20, VS 2022. The SDK ships with C4D. The
  GitHub `Maxon-Computer` repos contain examples only.
- Spout: `leadedge/Spout2` v2.007.017 (BSD-2). Use **SpoutDX** (`OpenDirectX11` +
  `SendImage`), owned by one dedicated thread.
- developers.maxon.net was blocked from the cloud sandbox. API details came from search
  results and the SDK examples on GitHub (`examples_ocio.cpp`, `simplematerial.cpp`).

## Decisions (confirmed by the user)
1. **Camera** = Redshift Camera, Projection Type **Spherical** (scene: `Camping.c4d`, object
   "RS Spherical Camera").
2. **The C4D viewport DOES draw the RS Spherical projection** and covers the full 360°×180°
   edge to edge inside the camera frame. (I originally claimed it couldn't; the user
   corrected this with a screenshot.)
3. **Output aspect = camera/render-settings frame (safe frame), NEVER the viewport aspect.**
   The viewport is letterboxed. Derive the height from `RDATA_FILMASPECT`, and warn if it's
   not 2:1.
4. **Updates only on scene change** are fine. A continuous frame rate isn't needed.
5. **Method A chosen:** `RenderDocument()` with the Viewport Renderer
   (`RDATA_RENDERENGINE_PREVIEWHARDWARE`) at a fixed resolution, on a document clone.
   - Fallback B: `BaseDraw::GetViewportImage` cropped to `GetSafeFrame` and resampled.
   - Optional C: the same path with Redshift (`1036219`) as a quality mode.
6. **Colour:** OCIO docs. Bake the view transform (`RDATA_BAKE_OCIO_VIEW_TRANSFORM_RENDER`
   or `BakeOcioViewToBitmap`) and send RGBA8 by default. 16F linear is optional.

## Status
- [x] Research + plan (`PLAN.md`)
- [x] Phase 1 spike script written: `prototype/spike_method_a.py`.
- [x] **Spike PASSED (2026-09-26, C4D 2026.3.4, run via MCP bridge):** `RenderDocument`
  with the Viewport Renderer through the RS Spherical camera produces a correct 2:1
  equirect. +Z is at the centre, +X at 3/4 width, −X at 1/4, −Z at the seam, the poles
  are correct, and there is no HUD. Each round trip includes PNG save: 1K < 1 s,
  2K ≈ 0.9 s, 4K ≈ 1.4 s. Redshift (method C) renders the same geometry. Scene:
  `prototype/spout_test_scene.c4d`; outputs are in `prototype/out/`.
  - `RenderDocument` returns code 1 (and the viewport is blank) if a document's
    colour management carries a broken OCIO config. Here it came from the machine-wide
    `OCIO` env var, now removed. The plugin should detect code 1 and report it clearly.
  - The Redshift render data needs the Redshift video post (1036219) attached, or the
    output is black.
  - `preview_render` in the MCP bridge adds viewport HUD overlays; `RenderDocument` with
    the doc's RenderData does not.
- [x] **Phase 2 MVP C++ plugin (2026-09-26):** built with `build.ps1` (VS 2022 v143, Windows SDK
  10.0.22621). It extracts the SDK from the C4D install's `sdk.zip` into the gitignored
  `external/c4d_sdk_2026`, builds a static SpoutDX lib (`/MD`) from the pinned
  `external/Spout2` submodule (2.007.017), and wires in our module via `plugin/custom_paths.txt`
  (alias `C2S` = repo root). It's installed with a junction from the 2026 prefs `plugins`
  folder to the build output. Two commands (dev IDs 1000001/1000002; **register real IDs
  before release**):
  - "Send Test Pattern": a colour-coded 2:1 grid.
  - "Render Now": Viewport Renderer + `AUTO_SETUP|OCIO_BAKE_RENDERING` → RGBA8 → Spout
    sender `C4D_LatLong` (`R8G8B8A8_UNORM`).
  Both were verified with `build/spoutdx/bin/Release/spout_grab.exe`; outputs are in
  `prototype/out/spout_*.png`. Everything runs on the main thread (SpoutDX is owned by it).
  - Aspect comes from XRES:YRES, not `RDATA_FILMASPECT`: the film aspect can be stale
    and stretch the output. The plugin writes a consistent film aspect and pixel aspect 1
    into its settings copy.
  - SpoutDX's CPU receive paths (`ReceiveImage`/`ReadTexurePixels`) are double-buffered
    and return the previous frame. With send-on-change, a CPU receiver sees an empty frame
    until a second send. GPU receivers (UE) are unaffected.
- [ ] Phase 2 remaining: verify on the UE5 sphere (seam, U direction, colour).
- [x] **Phase 3 auto-update (2026-09-26):** "C4D to Spout: Auto Update" toggle command
  (1000004, checkmark state) plus a `MessageData` timer (1000003, 50 ms tick, only while
  enabled), in `source/auto_update.cpp`.
  - It polls a change signature: active doc, scene camera plus its MATRIX/DATA dirty,
    current time, and `doc->GetHDirty(OBJECT|MATRIX|HIERARCHY|TAG|MATERIAL|SHADER|
    RENDERSETTINGS|VP)`. NBITS is excluded, so selection doesn't re-render.
  - It renders when the scene has been stable for 150 ms, or at most every 500 ms during
    continuous change. Latest wins. It skips while an external (Picture Viewer) render runs.
  - The post-render signature is recorded as "sent", so the plugin can't re-render its own
    side effects.
  - Still main thread only (deviation from PLAN §3's worker/Spout threads). The Viewport
    Renderer is fast enough, and it removes all cancel/join shutdown issues.
  - Verified via MCP and `spout_grab`: toggling on sends immediately; moving an object and
    rotating the camera propagate within about 0.8 s; switching documents follows the
    active doc (and resizes the sender); idle CPU stays ~4% of one core (no loop); a clean
    shutdown with it on leaves no crash report.
  - Not yet exercised: interactive drags, playback throttle feel, a concurrent Picture
    Viewer render.
- [x] **Phase 4 (2026-09-26):** a dockable "C4D to Spout..." dialog (1000005, `dialog.cpp`)
  with auto update, sender name, width (0 = render settings), engine (Viewport | render
  settings' engine), output (8-bit display, the default | 16F linear, opt-in because it's
  slower), debounce/throttle, Render Now, Test Pattern, and a status/warning line.
  - Settings persist in world plugin data (key 1000005, `settings.cpp`), including the
    enabled state.
  - Warnings: no scene camera, camera not RS Spherical (RS camera 1057516, param 1001 == 14),
    frame not 2:1.
  - README.md documents build, install, use, receiving, troubleshooting.
  - Deviation: no render-settings picker. It always uses the *active* render settings;
    rendering a non-active one would need a doc clone or mutating the live doc. For
    Redshift, make a Redshift render setting active and choose "Render settings engine".
  - **Gotcha:** globals holding `maxon::String` (static init at DLL load) stop the module
    from loading at all, silently. Use function-local statics.
  - SpoutDX `ReadTexurePixels` assumes 4 bytes/pixel, so `spout_grab` does its own staging
    readback (it handles RGBA16F).
  - Verified: dialog Render Now; 16F sender (format 10) with correct linear content;
    auto update and format persisting across a restart.
  - Not verified: the Redshift engine through the plugin, timing 8-bit vs 16F.
  - The user's saved settings were left at **16F output + auto update on** from testing.
    Switch the output back to 8-bit in the dialog.
- [ ] Phase 5: packaging (zip of .xdl64 + README), real plugin IDs, UE5 verification.

## Blender port (`blender/latlong_spout`, 2026-09-28)
- The user wants **viewport drawing only, not Cycles** (a path tracer is too slow). Method: 6×
  `GPUOffScreen.draw_view3d` 90° cube faces from the scene camera, drawn inside a SpaceView3D
  POST_PIXEL handler (it needs a draw context), plus a GPU unwrap shader with an n×n
  supersample grid → numpy → SpoutGL `sendImage` on its own thread with its own GL context
  (`createOpenGL`). The shading follows the largest 3D view.
- Verified in Blender 5.2.1 (OpenGL, A6000) with `spout_grab`: correct orientation (same as
  C4D) and colour, no seams (including EEVEE shadows and SSR), and auto update sends on
  move/frame/shading change but not on selection or when idle.
- Per changed frame at 2K: Solid ~25 ms; EEVEE ~210 ms (100 without shadows); Material Preview
  ~280 ms. Numbers measured with an unchanged scene (~15 ms) are cached and misleading.
- Projection-jitter AA in EEVEE costs ~45 ms per draw (per-view reset), so use the
  supersampled unwrap instead.
- Solid needs World Space Lighting (the studio light follows the view → steps at face
  edges).
- SpoutGL sends B8G8R8A8 (format 87). `spout_grab` now handles BGRA.
- Dev install: a junction into `%APPDATA%\...\5.2\extensions\user_default\latlong_spout`.
  `blender/build.ps1` downloads the wheels and builds the zip.
- The Claude app's environment still has a stale `OCIO` (ACES 1.2) variable, so Blender
  launched from here uses ACES. The machine and user env are clean.
- Not yet done: UE5 check, Vulkan backend, heavy real scenes.

## User preferences
Brief and concise, with no flattery. Disagree and propose alternatives when warranted. The
user knows C4D well, so trust their observations about C4D behaviour over assumptions.

## Git
Development branch: `claude/relaxed-franklin-wj3tkw`. Don't open a PR unless asked.
