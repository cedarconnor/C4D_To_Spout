#ifndef PIPELINE_H__
#define PIPELINE_H__

#include "c4d.h"

namespace c2s
{

// Sends a tightly packed RGBA8 image over the plugin's Spout sender (opened on demand).
maxon::Result<void> SendRGBA8(const maxon::UChar* rgba, cinema::Int32 w, cinema::Int32 h);

// Sends the colour-coded 2:1 test pattern (implemented in main.cpp).
maxon::Result<void> SendTestPattern();

// Renders the document's scene camera according to GetSettings() and sends it over Spout.
// Updates GetStatus() with the result (success, warning or error). Main thread only.
maxon::Result<void> RenderAndSend(cinema::BaseDocument* doc);

} // namespace c2s

#endif // PIPELINE_H__
