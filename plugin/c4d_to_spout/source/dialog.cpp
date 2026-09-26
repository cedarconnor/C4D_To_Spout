#include "dialog.h"

#include "c4d.h"
#include "c4d_gui.h"

#include "auto_update.h"
#include "pipeline.h"
#include "settings.h"

using namespace cinema;

namespace c2s
{

namespace
{

enum : Int32
{
	IDC_ENABLED = 1000,
	IDC_SENDER_NAME,
	IDC_WIDTH,
	IDC_ENGINE,
	IDC_FORMAT,
	IDC_DEBOUNCE,
	IDC_THROTTLE,
	IDC_RENDER_NOW,
	IDC_TEST_PATTERN,
	IDC_STATUS,
	IDC_WARNING,
};

constexpr Int32 REFRESH_MS = 250;

class SpoutDialog : public GeDialog
{
public:
	Bool CreateLayout() override
	{
		SetTitle("C4D to Spout"_s);

		GroupBegin(0, BFH_SCALEFIT | BFV_TOP, 2, 0, String(), 0);
		GroupBorderSpace(8, 8, 8, 8);
		GroupSpace(8, 4);

		AddStaticText(0, BFH_LEFT, 0, 0, "Auto update"_s, 0);
		AddCheckbox(IDC_ENABLED, BFH_LEFT, 0, 0, "Send on every scene change"_s);

		AddStaticText(0, BFH_LEFT, 0, 0, "Sender name"_s, 0);
		AddEditText(IDC_SENDER_NAME, BFH_SCALEFIT, 200);

		AddStaticText(0, BFH_LEFT, 0, 0, "Output width (0 = render settings)"_s, 0);
		AddEditNumberArrows(IDC_WIDTH, BFH_LEFT, 90);

		AddStaticText(0, BFH_LEFT, 0, 0, "Engine"_s, 0);
		AddComboBox(IDC_ENGINE, BFH_SCALEFIT);
		AddChild(IDC_ENGINE, Int32(Engine::VIEWPORT), "Viewport Renderer (fast)"_s);
		AddChild(IDC_ENGINE, Int32(Engine::RENDER_SETTINGS), "Render settings engine (e.g. Redshift, slow)"_s);

		AddStaticText(0, BFH_LEFT, 0, 0, "Output"_s, 0);
		AddComboBox(IDC_FORMAT, BFH_SCALEFIT);
		AddChild(IDC_FORMAT, Int32(OutputFormat::RGBA8_DISPLAY), "8-bit display (view transform baked)"_s);
		AddChild(IDC_FORMAT, Int32(OutputFormat::RGBA16F_LINEAR), "16-bit float linear (slower)"_s);

		AddStaticText(0, BFH_LEFT, 0, 0, "Debounce (ms)"_s, 0);
		AddEditNumberArrows(IDC_DEBOUNCE, BFH_LEFT, 90);

		AddStaticText(0, BFH_LEFT, 0, 0, "Max interval while changing (ms)"_s, 0);
		AddEditNumberArrows(IDC_THROTTLE, BFH_LEFT, 90);

		GroupEnd();

		GroupBegin(0, BFH_SCALEFIT, 2, 0, String(), 0);
		GroupBorderSpace(8, 0, 8, 4);
		AddButton(IDC_RENDER_NOW, BFH_SCALEFIT, 0, 0, "Render Now"_s);
		AddButton(IDC_TEST_PATTERN, BFH_SCALEFIT, 0, 0, "Send Test Pattern"_s);
		GroupEnd();

		GroupBegin(0, BFH_SCALEFIT, 1, 0, String(), 0);
		GroupBorderSpace(8, 4, 8, 8);
		AddStaticText(IDC_STATUS, BFH_SCALEFIT, 0, 0, String(), BORDER_THIN_IN);
		AddStaticText(IDC_WARNING, BFH_SCALEFIT, 0, 0, String(), 0);
		GroupEnd();

		return true;
	}

	Bool InitValues() override
	{
		const Settings& s = GetSettings();
		SetBool(IDC_ENABLED, s.enabled);
		SetString(IDC_SENDER_NAME, s.senderName);
		SetInt32(IDC_WIDTH, s.width, 0, 16384, 256);
		SetInt32(IDC_ENGINE, Int32(s.engine));
		SetInt32(IDC_FORMAT, Int32(s.format));
		SetInt32(IDC_DEBOUNCE, s.debounceMs, 0, 5000, 10);
		SetInt32(IDC_THROTTLE, s.throttleMs, 50, 10000, 50);
		RefreshStatus();
		SetTimer(REFRESH_MS);
		return true;
	}

	Bool Command(Int32 id, const BaseContainer& msg) override
	{
		Settings& s = GetSettings();
		switch (id)
		{
			case IDC_ENABLED:
			{
				Bool enabled = false;
				GetBool(IDC_ENABLED, enabled);
				SetAutoUpdateEnabled(enabled); // Saves settings.
				return true;
			}
			case IDC_SENDER_NAME:
			{
				String name;
				GetString(IDC_SENDER_NAME, name);
				if (name.IsEmpty())
					name = "C4D_LatLong"_s;
				s.senderName = name;
				break;
			}
			case IDC_WIDTH:
				GetInt32(IDC_WIDTH, s.width);
				s.width = maxon::Max(Int32(0), s.width);
				break;
			case IDC_ENGINE:
			{
				Int32 v = 0;
				GetInt32(IDC_ENGINE, v);
				s.engine = Engine(v);
				break;
			}
			case IDC_FORMAT:
			{
				Int32 v = 0;
				GetInt32(IDC_FORMAT, v);
				s.format = OutputFormat(v);
				break;
			}
			case IDC_DEBOUNCE:
				GetInt32(IDC_DEBOUNCE, s.debounceMs);
				break;
			case IDC_THROTTLE:
				GetInt32(IDC_THROTTLE, s.throttleMs);
				break;
			case IDC_RENDER_NOW:
			{
				// Errors are reported through the status line.
				iferr (RenderAndSend(GetActiveDocument()))
					ApplicationOutput("[C4D to Spout] Render failed: @", err);
				RefreshStatus();
				return true;
			}
			case IDC_TEST_PATTERN:
			{
				iferr (SendTestPattern())
					ApplicationOutput("[C4D to Spout] Test pattern failed: @", err);
				RefreshStatus();
				return true;
			}
			default:
				return GeDialog::Command(id, msg);
		}

		// A setting changed: persist it and re-send with the new settings.
		SaveSettings();
		BumpStateCounter();
		RequestAutoUpdate();
		return true;
	}

	void Timer(const BaseContainer& msg) override
	{
		if (_seenCounter != GetStateCounter())
			RefreshStatus();
	}

private:
	void RefreshStatus()
	{
		_seenCounter = GetStateCounter();
		const Status& st = GetStatus();
		SetString(IDC_STATUS, st.text);
		SetString(IDC_WARNING, st.warning.IsEmpty() ? String() : "Warning: "_s + st.warning);
		// Keep the checkbox in sync with the Auto Update command.
		SetBool(IDC_ENABLED, GetSettings().enabled);
	}

	UInt32 _seenCounter = 0;
};

class SpoutDialogCommand : public CommandData
{
public:
	Bool Execute(BaseDocument* doc, GeDialog* parentManager) override
	{
		return _dialog.Open(DLG_TYPE::ASYNC, ID_C2S_DIALOG, -1, -1, 380, 0);
	}

	Bool RestoreLayout(void* secret) override
	{
		return _dialog.RestoreLayout(ID_C2S_DIALOG, 0, secret);
	}

private:
	SpoutDialog _dialog;
};

} // namespace

bool RegisterDialogCommand()
{
	return RegisterCommandPlugin(ID_C2S_DIALOG, "C4D to Spout..."_s, 0, nullptr,
		"Open the C4D to Spout settings: auto update, sender name, resolution, engine, output format"_s,
		NewObjClear(SpoutDialogCommand));
}

} // namespace c2s
