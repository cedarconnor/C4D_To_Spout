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
- [ ] Phase 2+: C++ plugin (see PLAN.md §4 and the repo layout in §5).

## User preferences
Brief and concise, with no flattery. Disagree and propose alternatives when warranted. The
user knows C4D well, so trust their observations about C4D behaviour over assumptions.

## Git
Development branch: `claude/relaxed-franklin-wj3tkw`. Don't open a PR unless asked.
