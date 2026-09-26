// C4D to Spout: module entry point and commands.
//
// Everything (render + Spout send) runs on the main thread. SpoutDX is owned by the main
// thread, which satisfies its single-thread requirement.

#include "c4d.h"
#include "c4d_plugin.h"
#include "c4d_resource.h"

#include "auto_update.h"
#include "dialog.h"
#include "pipeline.h"
#include "settings.h"
#include "spout_sender.h"

using namespace cinema;

namespace c2s
{

namespace
{

constexpr Int32 TEST_PATTERN_WIDTH = 2048;

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
		iferr (RenderAndSend(doc))
		{
			ApplicationOutput("[C4D to Spout] Render failed: @", err);
			return true;
		}
		const Status& st = GetStatus();
		ApplicationOutput("[C4D to Spout] @", st.text);
		if (!st.warning.IsEmpty())
			ApplicationOutput("[C4D to Spout] Warning: @", st.warning);
		return true;
	}
};

class AutoUpdateToggleCommand : public CommandData
{
public:
	Bool Execute(BaseDocument* doc, GeDialog* parentManager) override
	{
		const bool enable = !IsAutoUpdateEnabled();
		SetAutoUpdateEnabled(enable);
		ApplicationOutput("[C4D to Spout] Auto update @.", enable ? "on"_s : "off"_s);
		return true;
	}

	Int32 GetState(BaseDocument* doc, GeDialog* parentManager) override
	{
		return CMD_ENABLED | (IsAutoUpdateEnabled() ? CMD_VALUE : 0);
	}
};

} // namespace

maxon::Result<void> SendTestPattern()
{
	iferr_scope;
	const Int32 w = TEST_PATTERN_WIDTH;
	const Int32 h = TEST_PATTERN_WIDTH / 2;
	maxon::BaseArray<UChar> rgba;
	rgba.Resize(Int(w) * Int(h) * 4) iferr_return;
	MakeTestPattern(rgba, w, h);
	SendRGBA8(rgba.GetFirst(), w, h) iferr_return;
	ApplicationOutput("[C4D to Spout] Sent @x@ test pattern as '@'.", w, h, GetSettings().senderName);
	return maxon::OK;
}

} // namespace c2s

Bool cinema::PluginStart()
{
	c2s::LoadSettings();

	if (!RegisterCommandPlugin(c2s::ID_C2S_TEST_PATTERN, "C4D to Spout: Send Test Pattern"_s, 0, nullptr,
				"Send a colour-coded 2:1 lat-long test pattern over Spout"_s, NewObjClear(c2s::TestPatternCommand)))
		return false;
	if (!RegisterCommandPlugin(c2s::ID_C2S_RENDER_NOW, "C4D to Spout: Render Now"_s, 0, nullptr,
				"Render the scene camera and send it over Spout"_s, NewObjClear(c2s::RenderNowCommand)))
		return false;
	if (!RegisterCommandPlugin(c2s::ID_C2S_AUTO_UPDATE_TOGGLE, "C4D to Spout: Auto Update"_s, 0, nullptr,
				"Re-render and send over Spout whenever the scene changes"_s, NewObjClear(c2s::AutoUpdateToggleCommand)))
		return false;
	if (!c2s::RegisterDialogCommand())
		return false;
	if (!c2s::RegisterAutoUpdate())
		return false;
	ApplicationOutput("[C4D to Spout] Loaded. Auto update is @.", c2s::GetSettings().enabled ? "on"_s : "off"_s);
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
