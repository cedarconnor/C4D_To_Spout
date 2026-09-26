#include "pipeline.h"

#include "render_capture.h"
#include "spout_sender.h"

using namespace cinema;

namespace c2s
{

maxon::Result<void> SendRGBA8(const UChar* rgba, Int32 w, Int32 h)
{
	if (!SpoutOpen(SENDER_NAME))
		return maxon::UnexpectedError(MAXON_SOURCE_LOCATION, "Could not initialize Spout (D3D11)."_s);
	if (!SpoutSendRGBA8(rgba, UInt32(w), UInt32(h)))
		return maxon::UnexpectedError(MAXON_SOURCE_LOCATION, "Spout SendImage failed."_s);
	return maxon::OK;
}

maxon::Result<SendInfo> RenderAndSend(BaseDocument* doc)
{
	iferr_scope;
	const Int32 width = doc && doc->GetActiveRenderData()
		? Int32(doc->GetActiveRenderData()->GetDataInstanceRef().GetFloat(RDATA_XRES, DEFAULT_WIDTH))
		: DEFAULT_WIDTH;

	CaptureResult cap = CaptureLatLong(doc, width) iferr_return;
	SendRGBA8(cap.rgba.GetFirst(), cap.width, cap.height) iferr_return;

	SendInfo info;
	info.width = cap.width;
	info.height = cap.height;
	info.renderMs = cap.renderMs;
	info.notTwoToOne = cap.notTwoToOne;
	return info;
}

} // namespace c2s
