// C4D to Spout: module entry point and commands.
//
// Everything (render + Spout send) runs on the main thread. SpoutDX is owned by the main
// thread, which satisfies its single-thread requirement.

#include "c4d.h"
#include "c4d_plugin.h"
#include "c4d_resource.h"

#include "auto_update.h"
#include "pipeline.h"
#include "spout_sender.h"

using namespace cinema;
using c2s::DEFAULT_WIDTH;
using c2s::SENDER_NAME;

namespace
{

// Development IDs (Maxon reserves 1000001-1000010 for testing). Replace with registered IDs
// from https://developers.maxon.net/forum/pid before distributing. 1000003 is the auto-update
// message plugin (auto_update.cpp).
constexpr Int32 ID_C2S_TEST_PATTERN = 1000001;
constexpr Int32 ID_C2S_RENDER_NOW = 1000002;
constexpr Int32 ID_C2S_AUTO_UPDATE_TOGGLE = 1000004;

// Colour-coded lat-long matching prototype/spout_test_scene.c4d: centre (+Z) red, 3/4 width
// (+X) green, 1/4 width (-X) yellow, seam (-Z) blue, top band magenta, bottom band cyan, with
// grid lines every 30 degrees. Lets you check orientation and the seam on the UE sphere.
void MakeTestPattern(maxon::BaseArray<UChar>& rgba, Int32 w, Int32 h)
{
	for (Int32 y = 0; y < h; ++y)
	{
		const Float v = (Float(y) + 0.5) / Float(h); // 0 = top (+90 deg), 1 = bottom (-90 deg)
		for (Int32 x = 0; x < w; ++x)
		{
			const Float u = (Float(x) + 0.5) / Float(w); // 0..1 across 360 deg
			UChar r = 40, g = 40, b = 40;

			const Float lonDeg = u * 360.0;
			const Float latDeg = v * 180.0;
			const Bool gridLine = maxon::Abs(lonDeg - maxon::Round(lonDeg / 30.0) * 30.0) < 0.35 ||
														maxon::Abs(latDeg - maxon::Round(latDeg / 30.0) * 30.0) < 0.35;
			if (gridLine)
				r = g = b = 160;

			auto nearU = [u](Float c) { return maxon::Abs(u - c) < 0.03 || maxon::Abs(u - c + 1.0) < 0.03 || maxon::Abs(u - c - 1.0) < 0.03; };
			const Bool equatorBand = maxon::Abs(v - 0.5) < 0.06;
			if (equatorBand && nearU(0.5)) { r = 255; g = 0; b = 0; }       // +Z front
			if (equatorBand && nearU(0.75)) { r = 0; g = 255; b = 0; }      // +X right
			if (equatorBand && nearU(0.25)) { r = 255; g = 255; b = 0; }    // -X left
			if (equatorBand && nearU(0.0)) { r = 0; g = 80; b = 255; }      // -Z back (seam)
			if (v < 0.05) { r = 255; g = 0; b = 255; }                      // up
			if (v > 0.95) { r = 0; g = 255; b = 255; }                      // down

			UChar* p = rgba.GetFirst() + (Int(y) * Int(w) + x) * 4;
			p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
		}
	}
}

maxon::Result<void> SendTestPattern()
{
	iferr_scope;
	const Int32 w = DEFAULT_WIDTH;
	const Int32 h = DEFAULT_WIDTH / 2;
	maxon::BaseArray<UChar> rgba;
	rgba.Resize(Int(w) * Int(h) * 4) iferr_return;
	MakeTestPattern(rgba, w, h);
	c2s::SendRGBA8(rgba.GetFirst(), w, h) iferr_return;
	ApplicationOutput("[C4D to Spout] Sent @x@ test pattern as '@'.", w, h, String(SENDER_NAME));
	return maxon::OK;
}

class TestPatternCommand : public CommandData
{
public:
	Bool Execute(BaseDocument* doc, GeDialog* parentManager) override
	{
		iferr (SendTestPattern())
			ApplicationOutput("[C4D to Spout] Test pattern failed: @", err);
		return true;
	}
};

class RenderNowCommand : public CommandData
{
public:
	Bool Execute(BaseDocument* doc, GeDialog* parentManager) override
	{
		const maxon::Result<c2s::SendInfo> result = c2s::RenderAndSend(doc);
		if (result == maxon::FAILED)
		{
			ApplicationOutput("[C4D to Spout] Render failed: @", result.GetError());
			return true;
		}
		const c2s::SendInfo& info = result.GetValue();
		if (info.notTwoToOne)
			ApplicationOutput("[C4D to Spout] Warning: render settings frame is not 2:1; the lat-long will be stretched.");
		ApplicationOutput("[C4D to Spout] Sent @x@ (render @ ms) as '@'.",
			info.width, info.height, Int32(info.renderMs), String(SENDER_NAME));
		return true;
	}
};

class AutoUpdateToggleCommand : public CommandData
{
public:
	Bool Execute(BaseDocument* doc, GeDialog* parentManager) override
	{
		const bool enable = !c2s::IsAutoUpdateEnabled();
		c2s::SetAutoUpdateEnabled(enable);
		ApplicationOutput("[C4D to Spout] Auto update @.", enable ? "on"_s : "off"_s);
		return true;
	}

	Int32 GetState(BaseDocument* doc, GeDialog* parentManager) override
	{
		return CMD_ENABLED | (c2s::IsAutoUpdateEnabled() ? CMD_VALUE : 0);
	}
};

} // namespace

Bool cinema::PluginStart()
{
	if (!RegisterCommandPlugin(ID_C2S_TEST_PATTERN, "C4D to Spout: Send Test Pattern"_s, 0, nullptr,
				"Send a colour-coded 2:1 lat-long test pattern over Spout"_s, NewObjClear(TestPatternCommand)))
		return false;
	if (!RegisterCommandPlugin(ID_C2S_RENDER_NOW, "C4D to Spout: Render Now"_s, 0, nullptr,
				"Render the scene camera with the Viewport Renderer and send it over Spout"_s, NewObjClear(RenderNowCommand)))
		return false;
	if (!RegisterCommandPlugin(ID_C2S_AUTO_UPDATE_TOGGLE, "C4D to Spout: Auto Update"_s, 0, nullptr,
				"Re-render and send over Spout whenever the scene changes"_s, NewObjClear(AutoUpdateToggleCommand)))
		return false;
	if (!c2s::RegisterAutoUpdate())
		return false;
	ApplicationOutput("[C4D to Spout] Loaded.");
	return true;
}

void cinema::PluginEnd()
{
	c2s::SpoutClose();
}

Bool cinema::PluginMessage(Int32 id, void* data)
{
	switch (id)
	{
		case C4DPL_INIT_SYS:
			return g_resource.Init();

		case C4DPL_ENDACTIVITY:
			c2s::SpoutClose();
			return true;
	}
	return false;
}
