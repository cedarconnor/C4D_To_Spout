#ifndef RENDER_CAPTURE_H__
#define RENDER_CAPTURE_H__

#include "c4d.h"

#include "settings.h"

namespace c2s
{

struct CaptureOptions
{
	cinema::Int32 width = 0;  // 0 = render settings' XRES.
	Engine engine = Engine::VIEWPORT;
	OutputFormat format = OutputFormat::RGBA8_DISPLAY;
};

struct CaptureResult
{
	maxon::BaseArray<maxon::UChar> pixels; // Top row first, tightly packed.
	cinema::Int32 width = 0;
	cinema::Int32 height = 0;
	cinema::Int32 bytesPerPixel = 4;       // 4 (RGBA8) or 8 (RGBA16F).
	cinema::Float renderMs = 0.0;
	maxon::String warning;                 // Non-fatal issues (aspect, camera type).
};

// Renders the document's scene camera with its active render settings at #options.width
// pixels wide. The height follows the render settings' frame, XRES:YRES (never the viewport
// aspect). RGBA8 output has the OCIO view transform baked (display-referred); RGBA16F output
// is raw render space (linear). Must be called from the main thread.
maxon::Result<CaptureResult> CaptureLatLong(cinema::BaseDocument* doc, const CaptureOptions& options);

} // namespace c2s

#endif // RENDER_CAPTURE_H__
