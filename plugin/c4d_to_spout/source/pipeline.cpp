#include "pipeline.h"

#include "render_capture.h"
#include "settings.h"
#include "spout_sender.h"

using namespace cinema;

namespace c2s
{

namespace
{

maxon::Result<void> Send(const void* pixels, Int32 w, Int32 h, Int32 bytesPerPixel)
{
	iferr_scope;
	const Settings& s = GetSettings();
	const maxon::BaseArray<Char> name = s.senderName.GetCString() iferr_return;
	const bool halfFloat = bytesPerPixel == 8;
	if (!SpoutOpen(name.GetFirst(), halfFloat))
		return maxon::UnexpectedError(MAXON_SOURCE_LOCATION, "Could not initialize Spout (D3D11)."_s);
	if (!SpoutSend(pixels, UInt32(w), UInt32(h), UInt32(w * bytesPerPixel)))
		return maxon::UnexpectedError(MAXON_SOURCE_LOCATION, "Spout SendImage failed."_s);
	return maxon::OK;
}

void SetStatus(const maxon::String& text, const maxon::String& warning, Bool error)
{
	Status& st = GetStatus();
	st.text = text;
	st.warning = warning;
	st.error = error;
	BumpStateCounter();
}

} // namespace

maxon::Result<void> SendRGBA8(const UChar* rgba, Int32 w, Int32 h)
{
	iferr_scope;
	Send(rgba, w, h, 4) iferr_return;
	SetStatus(FormatString("Sent @x@ test pattern.", w, h), maxon::String(), false);
	return maxon::OK;
}

maxon::Result<void> RenderAndSend(BaseDocument* doc)
{
	iferr_scope_handler
	{
		SetStatus(FormatString("Error: @", err), maxon::String(), true);
		return err;
	};

	const Settings& s = GetSettings();
	CaptureOptions options;
	options.width = s.width;
	options.engine = s.engine;
	options.format = s.format;

	CaptureResult cap = CaptureLatLong(doc, options) iferr_return;
	Send(cap.pixels.GetFirst(), cap.width, cap.height, cap.bytesPerPixel) iferr_return;

	SetStatus(FormatString("Sent @x@ @ in @ ms.", cap.width, cap.height,
							cap.bytesPerPixel == 8 ? "RGBA16F"_s : "RGBA8"_s, Int32(cap.renderMs)),
		cap.warning, false);
	return maxon::OK;
}

} // namespace c2s
