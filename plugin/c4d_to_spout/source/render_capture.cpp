#include "render_capture.h"

#include "c4d_basebitmap.h"
#include "c4d_basedocument.h"
#include "c4d_basedraw.h"
#include "drendersettings.h"

using namespace cinema;

namespace c2s
{

namespace
{

// Redshift camera object and its projection parameter (Spherical = 14). Only used for a warning.
constexpr Int32 ID_RS_CAMERA = 1057516;
constexpr Int32 RS_CAMERA_PROJECTION_TYPE = 1001;
constexpr Int32 RS_CAMERA_PROJECTION_SPHERICAL = 14;

UInt16 FloatToHalf(Float32 value)
{
	UInt32 f;
	memcpy(&f, &value, sizeof(f));
	const UInt32 sign = (f >> 16) & 0x8000;
	Int32 exponent = Int32((f >> 23) & 0xff) - 127 + 15;
	UInt32 mantissa = f & 0x7fffff;
	if (((f >> 23) & 0xff) == 0xff) // Inf/NaN
		return UInt16(sign | 0x7c00 | (mantissa ? 0x200 : 0));
	if (exponent >= 31) // Overflow -> Inf
		return UInt16(sign | 0x7c00);
	if (exponent <= 0) // Subnormal or zero
	{
		if (exponent < -10)
			return UInt16(sign);
		mantissa |= 0x800000;
		const UInt32 shift = UInt32(14 - exponent);
		UInt32 half = mantissa >> shift;
		if ((mantissa >> (shift - 1)) & 1) // Round to nearest.
			++half;
		return UInt16(sign | half);
	}
	UInt32 half = sign | (UInt32(exponent) << 10) | (mantissa >> 13);
	if (mantissa & 0x1000) // Round to nearest (may carry into the exponent, which is correct).
		++half;
	return UInt16(half);
}

maxon::String CameraWarning(BaseDocument* doc)
{
	BaseDraw* bd = doc->GetRenderBaseDraw();
	BaseObject* cam = bd ? bd->GetSceneCamera(doc) : nullptr;
	if (!cam)
		return "No scene camera: rendering the editor camera."_s;
	if (cam->GetType() != ID_RS_CAMERA)
		return FormatString("Scene camera '@' is not an RS Spherical camera.", cam->GetName());
	GeData data;
	if (cam->GetParameter(ConstDescID(DescLevel(RS_CAMERA_PROJECTION_TYPE)), data, DESCFLAGS_GET::NONE) &&
			data.GetInt32() != RS_CAMERA_PROJECTION_SPHERICAL)
		return FormatString("RS camera '@' projection is not Spherical.", cam->GetName());
	return maxon::String();
}

} // namespace

maxon::Result<CaptureResult> CaptureLatLong(BaseDocument* doc, const CaptureOptions& options)
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
	const Int32 width = options.width > 0 ? options.width : Int32(xres);
	const Int32 height = maxon::Max(Int32(1), Int32(Float(width) / frameAspect + 0.5));

	settings.SetFloat(RDATA_XRES, Float(width));
	settings.SetFloat(RDATA_YRES, Float(height));
	settings.SetFloat(RDATA_FILMASPECT, Float(width) / Float(height));
	settings.SetFloat(RDATA_PIXELASPECT, 1.0);
	if (options.engine == Engine::VIEWPORT)
		settings.SetInt32(RDATA_RENDERENGINE, RDATA_RENDERENGINE_PREVIEWHARDWARE);
	settings.SetBool(RDATA_SAVEIMAGE, false);
	settings.SetBool(RDATA_MULTIPASS_SAVEIMAGE, false);

	const Bool halfFloat = options.format == OutputFormat::RGBA16F_LINEAR;
	if (halfFloat)
		settings.SetInt32(RDATA_FORMATDEPTH, RDATA_FORMATDEPTH_32);

	MultipassBitmap* bmp = AllocateRenderBitmap(&settings);
	finally
	{
		if (bmp)
			MultipassBitmap::Free(bmp);
	};
	if (MAXON_UNLIKELY(!bmp))
		return maxon::OutOfMemoryError(MAXON_SOURCE_LOCATION, "Failed to allocate the render bitmap."_s);

	// AUTO_SETUP returns raw render-space data for OCIO documents (used for linear 16F output).
	// OCIO_BAKE_RENDERING additionally bakes the view transform (display-referred, for 8-bit).
	const RENDERFLAGS flags = halfFloat ? RENDERFLAGS::AUTO_SETUP
																			: RENDERFLAGS::AUTO_SETUP | RENDERFLAGS::OCIO_BAKE_RENDERING;
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
	out.bytesPerPixel = halfFloat ? 8 : 4;

	maxon::String warning = CameraWarning(doc);
	if (maxon::Abs(frameAspect - 2.0) > 0.001)
	{
		if (!warning.IsEmpty())
			warning += " "_s;
		warning += FormatString("Render settings frame is @x@, not 2:1: the lat-long will be stretched.", Int32(xres), Int32(yres));
	}
	out.warning = warning;

	out.pixels.Resize(Int(width) * Int(height) * out.bytesPerPixel) iferr_return;
	if (halfFloat)
	{
		maxon::BaseArray<Float32> row;
		row.Resize(Int(width) * 3) iferr_return;
		for (Int32 y = 0; y < height; ++y)
		{
			bmp->GetPixelCnt(0, y, width, reinterpret_cast<UChar*>(row.GetFirst()), 12, COLORMODE::RGBf, PIXELCNT::NONE);
			UInt16* dst = reinterpret_cast<UInt16*>(out.pixels.GetFirst() + Int(y) * Int(width) * 8);
			const Float32* src = row.GetFirst();
			for (Int32 x = 0; x < width; ++x)
			{
				dst[0] = FloatToHalf(src[0]);
				dst[1] = FloatToHalf(src[1]);
				dst[2] = FloatToHalf(src[2]);
				dst[3] = 0x3c00; // 1.0
				dst += 4;
				src += 3;
			}
		}
	}
	else
	{
		maxon::BaseArray<UChar> row;
		row.Resize(Int(width) * 3) iferr_return;
		for (Int32 y = 0; y < height; ++y)
		{
			bmp->GetPixelCnt(0, y, width, row.GetFirst(), 3, COLORMODE::RGB, PIXELCNT::NONE);
			UChar* dst = out.pixels.GetFirst() + Int(y) * Int(width) * 4;
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
	}

	return out;
}

} // namespace c2s
