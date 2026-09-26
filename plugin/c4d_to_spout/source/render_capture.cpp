#include "render_capture.h"

#include "c4d_basebitmap.h"
#include "c4d_basedocument.h"
#include "drendersettings.h"

using namespace cinema;

namespace c2s
{

maxon::Result<CaptureResult> CaptureLatLong(BaseDocument* doc, Int32 width)
{
	iferr_scope;

	if (MAXON_UNLIKELY(!doc))
		return maxon::NullptrError(MAXON_SOURCE_LOCATION, "No document."_s);
	if (MAXON_UNLIKELY(!GeIsMainThread()))
		return maxon::UnexpectedError(MAXON_SOURCE_LOCATION, "CaptureLatLong must run on the main thread."_s);

	RenderData* const rd = doc->GetActiveRenderData();
	if (MAXON_UNLIKELY(!rd))
		return maxon::UnexpectedError(MAXON_SOURCE_LOCATION, "Document has no active render settings."_s);

	// Work on a copy so the document's render settings are never modified.
	BaseContainer settings = rd->GetDataInstanceRef();

	// The frame aspect comes from the render settings' resolution (XRES:YRES), never the viewport.
	// RDATA_FILMASPECT alone is not trusted: it can be stale relative to XRES/YRES (e.g. when set
	// via scripts), and the renderer then outputs a stretched frame. We write a consistent film
	// aspect and square pixels into the copy.
	const Float xres = settings.GetFloat(RDATA_XRES, 2048.0);
	const Float yres = settings.GetFloat(RDATA_YRES, 1024.0);
	const Float frameAspect = (xres > 0.0 && yres > 0.0) ? xres / yres : 2.0;
	if (width <= 0)
		width = Int32(xres);
	const Int32 height = maxon::Max(Int32(1), Int32(Float(width) / frameAspect + 0.5));

	settings.SetFloat(RDATA_XRES, Float(width));
	settings.SetFloat(RDATA_YRES, Float(height));
	settings.SetFloat(RDATA_FILMASPECT, Float(width) / Float(height));
	settings.SetFloat(RDATA_PIXELASPECT, 1.0);
	settings.SetInt32(RDATA_RENDERENGINE, RDATA_RENDERENGINE_PREVIEWHARDWARE);
	settings.SetBool(RDATA_SAVEIMAGE, false);
	settings.SetBool(RDATA_MULTIPASS_SAVEIMAGE, false);

	MultipassBitmap* bmp = AllocateRenderBitmap(&settings);
	finally
	{
		if (bmp)
			MultipassBitmap::Free(bmp);
	};
	if (MAXON_UNLIKELY(!bmp))
		return maxon::OutOfMemoryError(MAXON_SOURCE_LOCATION, "Failed to allocate the render bitmap."_s);

	// AUTO_SETUP handles OCIO raw vs baked setup; OCIO_BAKE_RENDERING bakes the view transform so
	// the result is display-referred.
	const RENDERFLAGS flags = RENDERFLAGS::AUTO_SETUP | RENDERFLAGS::OCIO_BAKE_RENDERING;
	const maxon::TimeValue start = maxon::TimeValue::GetTime();
	const RENDERRESULT res = RenderDocument(doc, settings, nullptr, nullptr, bmp, flags, nullptr);
	const Float renderMs = (maxon::TimeValue::GetTime() - start).GetMilliseconds();

	if (res != RENDERRESULT::OK)
	{
		// Code 1 (OUTOFMEMORY) has been observed when a document carries a broken OCIO config.
		return maxon::UnexpectedError(MAXON_SOURCE_LOCATION,
			FormatString("RenderDocument failed with code @. If this is 1, check the document's OCIO color management settings.", Int32(res)));
	}

	CaptureResult out;
	out.width = width;
	out.height = height;
	out.renderMs = renderMs;
	out.notTwoToOne = maxon::Abs(frameAspect - 2.0) > 0.001;

	// Convert row by row to 8-bit RGB, then expand to RGBA with opaque alpha.
	out.rgba.Resize(Int(width) * Int(height) * 4) iferr_return;
	maxon::BaseArray<UChar> row;
	row.Resize(Int(width) * 3) iferr_return;
	for (Int32 y = 0; y < height; ++y)
	{
		bmp->GetPixelCnt(0, y, width, row.GetFirst(), 3, COLORMODE::RGB, PIXELCNT::NONE);
		UChar* dst = out.rgba.GetFirst() + Int(y) * Int(width) * 4;
		const UChar* src = row.GetFirst();
		for (Int32 x = 0; x < width; ++x)
		{
			dst[0] = src[0];
			dst[1] = src[1];
			dst[2] = src[2];
			dst[3] = 255;
			dst += 4;
			src += 3;
		}
	}

	return out;
}

} // namespace c2s
