#ifndef PIPELINE_H__
#define PIPELINE_H__

#include "c4d.h"

namespace c2s
{

constexpr const char* SENDER_NAME = "C4D_LatLong";
constexpr cinema::Int32 DEFAULT_WIDTH = 2048;

struct SendInfo
{
	cinema::Int32 width = 0;
	cinema::Int32 height = 0;
	cinema::Float renderMs = 0.0;
	cinema::Bool notTwoToOne = false;
};

// Sends a tightly packed RGBA8 image over the plugin's Spout sender (opened on demand).
maxon::Result<void> SendRGBA8(const maxon::UChar* rgba, cinema::Int32 w, cinema::Int32 h);

// Renders the document's scene camera with the Viewport Renderer at the render settings'
// resolution and sends it over Spout. Main thread only.
maxon::Result<SendInfo> RenderAndSend(cinema::BaseDocument* doc);

} // namespace c2s

#endif // PIPELINE_H__
