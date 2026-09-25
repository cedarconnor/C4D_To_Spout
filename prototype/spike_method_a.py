"""Phase 1 spike for method A: render the active camera with the Viewport Renderer.

Run in Cinema 4D 2026 via Extensions > Script Manager, with the RS Spherical camera set as the
scene camera of the active view. Nothing in the document is modified: every render works on a
copy of the active render settings, and RenderDocument clones the document itself.

It answers:
  1. Does the Viewport Renderer output the RS Spherical projection (full 360x180 lat-long,
     no letterbox) like the interactive viewport does?  -> open the saved PNGs.
  2. How long does a render take at several widths, cold vs warm?
  3. (optional) Does the Viewport Renderer work from a background C4DThread?
  4. (optional) How does it compare to a Redshift render of the same frame?

Output: PNGs plus report.txt in <Desktop>/c4d_spout_spike/.
"""
import os
import time

import c4d
from c4d import documents, bitmaps, storage

# --- Options ------------------------------------------------------------------------------------
WIDTHS = (1024, 2048, 4096)  # Output widths; height follows the render setting's film aspect.
RUNS_PER_WIDTH = 2           # First run is "cold", later runs are "warm".
RUN_THREAD_TEST = False      # Save your scene first: rendering the viewport off-thread may crash.
RUN_REDSHIFT = False         # Adds one Redshift render at WIDTHS[1] for comparison (slow).

RENDERENGINE_REDSHIFT = 1036219
OUT_DIR = os.path.join(storage.GeGetC4DPath(c4d.C4D_PATH_DESKTOP), "c4d_spout_spike")

report_lines = []


def log(msg):
    print(msg)
    report_lines.append(msg)


def make_settings(doc, width, engine):
    """Copy of the active render settings with the given width and engine.

    The height is derived from the film aspect so the output matches the camera frame
    (the safe frame in the viewport), never the viewport's own aspect.
    """
    rd = doc.GetActiveRenderData()
    bc = rd.GetDataInstance().GetClone(c4d.COPYFLAGS_NONE)
    aspect = bc[c4d.RDATA_FILMASPECT] or (bc[c4d.RDATA_XRES] / float(bc[c4d.RDATA_YRES]))
    height = max(1, int(round(width / aspect)))
    bc[c4d.RDATA_XRES] = float(width)
    bc[c4d.RDATA_YRES] = float(height)
    bc[c4d.RDATA_FILMASPECT] = width / float(height)
    bc[c4d.RDATA_PIXELASPECT] = 1.0
    bc[c4d.RDATA_RENDERENGINE] = engine
    bc[c4d.RDATA_SAVEIMAGE] = False
    bc[c4d.RDATA_MULTIPASS_SAVEIMAGE] = False

    # OCIO documents (default since 2026): bake the view transform into the in-memory image so
    # the 8-bit result is display-referred, i.e. looks like the viewport.
    ocio = getattr(c4d, "DOCUMENT_COLOR_MANAGEMENT_OCIO", None)
    bake = getattr(c4d, "RDATA_BAKE_OCIO_VIEW_TRANSFORM_RENDER", None)
    if ocio is not None and bake is not None and doc[c4d.DOCUMENT_COLOR_MANAGEMENT] == ocio:
        bc[bake] = True
    return bc, width, height


def render(doc, bc, width, height, thread=None):
    """Render into a fresh 8-bit bitmap. Returns (bitmap or None, seconds, result code)."""
    bmp = bitmaps.BaseBitmap()
    if bmp.Init(width, height, 24) != c4d.IMAGERESULT_OK:
        return None, 0.0, "bitmap init failed"
    t0 = time.perf_counter()
    res = documents.RenderDocument(doc, bc, bmp, c4d.RENDERFLAGS_EXTERNAL, thread)
    dt = time.perf_counter() - t0
    return (bmp if res == c4d.RENDERRESULT_OK else None), dt, res


def save(bmp, name):
    path = os.path.join(OUT_DIR, name)
    bmp.Save(path, c4d.FILTER_PNG)
    return path


class RenderThread(c4d.threading.C4DThread):
    def __init__(self, doc, bc, width, height):
        super().__init__()
        self.args = (doc, bc, width, height)
        self.result = (None, 0.0, "not run")

    def Main(self):
        self.result = render(*self.args, thread=self.Get())


def main():
    doc = documents.GetActiveDocument()
    bd = doc.GetActiveBaseDraw()
    cam = bd.GetSceneCamera(doc) if bd else None
    if cam is None:
        c4d.gui.MessageDialog("No scene camera in the active view.")
        return
    os.makedirs(OUT_DIR, exist_ok=True)

    rd = doc.GetActiveRenderData()
    log("C4D %s | doc: %s" % (c4d.GetC4DVersion(), doc.GetDocumentName()))
    log("Camera: %s (type %d) | render setting: %s" % (cam.GetName(), cam.GetType(), rd.GetName()))
    log("Render setting res: %dx%d, film aspect %.4f" % (
        rd[c4d.RDATA_XRES], rd[c4d.RDATA_YRES], rd[c4d.RDATA_FILMASPECT]))
    if abs(rd[c4d.RDATA_FILMASPECT] - 2.0) > 1e-3:
        log("WARNING: film aspect is not 2:1 - a lat-long will be stretched.")

    # 1 + 2: Viewport Renderer, main thread, several widths.
    log("\n[Viewport Renderer, main thread]")
    for width in WIDTHS:
        bc, w, h = make_settings(doc, width, c4d.RDATA_RENDERENGINE_PREVIEWHARDWARE)
        for run in range(RUNS_PER_WIDTH):
            bmp, dt, res = render(doc, bc, w, h)
            tag = "cold" if run == 0 else "warm"
            if bmp is None:
                log("  %dx%d %s: FAILED (%s)" % (w, h, tag, res))
                break
            log("  %dx%d %s: %.0f ms" % (w, h, tag, dt * 1000.0))
            if run == 0:
                save(bmp, "viewport_%dx%d.png" % (w, h))

    # 3: Viewport Renderer from a background thread.
    if RUN_THREAD_TEST:
        log("\n[Viewport Renderer, C4DThread]")
        bc, w, h = make_settings(doc, WIDTHS[1], c4d.RDATA_RENDERENGINE_PREVIEWHARDWARE)
        th = RenderThread(doc, bc, w, h)
        th.Start()
        th.Wait(False)
        bmp, dt, res = th.result
        if bmp is None:
            log("  %dx%d: FAILED (%s)" % (w, h, res))
        else:
            log("  %dx%d: %.0f ms" % (w, h, dt * 1000.0))
            save(bmp, "viewport_thread_%dx%d.png" % (w, h))

    # 4: Redshift reference render.
    if RUN_REDSHIFT:
        log("\n[Redshift, main thread]")
        bc, w, h = make_settings(doc, WIDTHS[1], RENDERENGINE_REDSHIFT)
        bmp, dt, res = render(doc, bc, w, h)
        if bmp is None:
            log("  %dx%d: FAILED (%s)" % (w, h, res))
        else:
            log("  %dx%d: %.0f ms" % (w, h, dt * 1000.0))
            save(bmp, "redshift_%dx%d.png" % (w, h))

    log("\nCheck the PNGs: spherical projection? full 360x180 edge to edge? no letterbox?")
    log("Colours match the viewport? Straight edges on large polygons vs the Redshift render?")
    with open(os.path.join(OUT_DIR, "report.txt"), "w") as f:
        f.write("\n".join(report_lines) + "\n")
    storage.ShowInFinder(OUT_DIR, True)


if __name__ == "__main__":
    main()
