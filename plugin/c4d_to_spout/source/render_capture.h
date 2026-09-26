#ifndef RENDER_CAPTURE_H__
#define RENDER_CAPTURE_H__

#include "c4d.h"

namespace c2s
{

struct CaptureResult
{
	maxon::BaseArray<maxon::UChar> rgba; // Tightly packed RGBA8, top row first.
	cinema::Int32 width = 0;
	cinema::Int32 height = 0;
	cinema::Float renderMs = 0.0;
	cinema::Bool notTwoToOne = false; // Render settings frame (XRES:YRES) is not 2:1.
};

// Renders the document's scene camera with the Viewport Renderer at #width pixels wide. The
// height follows the active render settings' frame, XRES:YRES (never the viewport aspect).
// The OCIO view transform is baked, so the pixels are display-referred.
// Must be called from the main thread.
maxon::Result<CaptureResult> CaptureLatLong(cinema::BaseDocument* doc, cinema::Int32 width);

} // namespace c2s

#endif // RENDER_CAPTURE_H__
