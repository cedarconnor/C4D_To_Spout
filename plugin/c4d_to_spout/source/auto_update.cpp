#include "auto_update.h"

#include "c4d_basedraw.h"
#include "c4d_messagedata.h"

#include "pipeline.h"

using namespace cinema;

namespace c2s
{

namespace
{

// Development ID (Maxon reserves 1000001-1000010 for testing).
constexpr Int32 ID_C2S_AUTO_UPDATE_MESSAGE = 1000003;

constexpr Int32 TIMER_MS = 50;
constexpr Int64 DEBOUNCE_MS = 150;  // Render once the scene is stable this long...
constexpr Int64 THROTTLE_MS = 500;  // ...or at least this often during continuous changes.

// Everything that should trigger a re-render. Selection/visibility bits (NBITS) are excluded
// so that merely selecting objects does not re-render.
struct Signature
{
	const BaseDocument* doc = nullptr;
	const BaseObject* camera = nullptr;
	Int64 timeTicks = 0;
	UInt32 hdirty = 0;
	UInt32 cameraDirty = 0;

	Bool operator==(const Signature& o) const
	{
		return doc == o.doc && camera == o.camera && timeTicks == o.timeTicks && hdirty == o.hdirty &&
					 cameraDirty == o.cameraDirty;
	}
	Bool operator!=(const Signature& o) const { return !(*this == o); }
};

Signature ComputeSignature(BaseDocument* doc)
{
	Signature s;
	if (!doc)
		return s;
	s.doc = doc;
	BaseDraw* bd = doc->GetRenderBaseDraw();
	s.camera = bd ? bd->GetSceneCamera(doc) : nullptr;
	s.timeTicks = Int64(doc->GetTime().Get() * 1000000.0); // Microseconds.
	const HDIRTYFLAGS mask = HDIRTYFLAGS::OBJECT | HDIRTYFLAGS::OBJECT_MATRIX | HDIRTYFLAGS::OBJECT_HIERARCHY |
													 HDIRTYFLAGS::TAG | HDIRTYFLAGS::MATERIAL | HDIRTYFLAGS::SHADER |
													 HDIRTYFLAGS::RENDERSETTINGS | HDIRTYFLAGS::VP;
	s.hdirty = doc->GetHDirty(mask);
	// Camera matrix/data changes (e.g. orbiting through the RS camera in the viewport).
	if (s.camera)
		s.cameraDirty = s.camera->GetDirty(DIRTYFLAGS::MATRIX | DIRTYFLAGS::DATA);
	return s;
}

Int64 NowMs()
{
	return Int64(maxon::TimeValue::GetTime().GetMilliseconds());
}

class AutoUpdateMessage : public MessageData
{
public:
	Bool enabled = false;
	Bool forced = false;

	Int32 GetTimer() override { return enabled ? TIMER_MS : 0; }

	Bool CoreMessage(Int32 id, const BaseContainer& bc) override
	{
		// Re-query GetTimer promptly after the toggle command.
		if (id == ID_C2S_AUTO_UPDATE_MESSAGE)
			return true;
		if (id != MSG_TIMER || !enabled)
			return true;

		// Do not compete with a user's Picture Viewer render.
		if (CheckIsRunning(CHECKISRUNNING::EXTERNALRENDERING))
			return true;

		BaseDocument* doc = GetActiveDocument();
		const Signature sig = ComputeSignature(doc);
		const Int64 now = NowMs();

		if (sig != _pending)
		{
			// Scene changed since the last tick: restart the debounce window.
			if (_pending == _sent && !forced)
				_firstChangeMs = now; // First change after an idle period.
			_pending = sig;
			_lastChangeMs = now;
		}

		const Bool dirty = forced || _pending != _sent;
		if (!dirty || !doc)
			return true;

		const Bool stable = now - _lastChangeMs >= DEBOUNCE_MS;
		const Bool overdue = now - _firstChangeMs >= THROTTLE_MS;
		if (!forced && !stable && !overdue)
			return true;

		const maxon::Result<SendInfo> result = RenderAndSend(doc);
		if (result == maxon::FAILED)
		{
			// Log once per scene state, not every tick.
			if (_pending != _failed)
				ApplicationOutput("[C4D to Spout] Auto-update render failed: @", result.GetError());
			_failed = _pending;
			_sent = _pending;
			forced = false;
			return true;
		}
		const SendInfo& info = result.GetValue();

		if (info.notTwoToOne && !_warnedAspect)
		{
			ApplicationOutput("[C4D to Spout] Warning: render settings frame is not 2:1; the lat-long will be stretched.");
			_warnedAspect = true;
		}
		StatusSetText(FormatString("Spout @: @x@ (@ ms)", String(SENDER_NAME), info.width, info.height, Int32(info.renderMs)));

		// Rendering may itself bump dirty counters; record the post-render state as sent so we
		// don't re-render our own side effects.
		_sent = ComputeSignature(doc);
		_pending = _sent;
		_failed = Signature();
		_firstChangeMs = _lastChangeMs = NowMs();
		forced = false;
		return true;
	}

	void Reset()
	{
		_pending = _sent = _failed = Signature();
		_firstChangeMs = _lastChangeMs = 0;
		_warnedAspect = false;
	}

private:
	Signature _pending; // Latest observed state.
	Signature _sent;    // State of the last render sent.
	Signature _failed;  // State of the last failed render (to avoid log spam).
	Int64 _firstChangeMs = 0;
	Int64 _lastChangeMs = 0;
	Bool _warnedAspect = false;
};

AutoUpdateMessage* g_autoUpdate = nullptr; // Owned by Cinema 4D after registration.

} // namespace

bool RegisterAutoUpdate()
{
	g_autoUpdate = NewObjClear(AutoUpdateMessage);
	if (!g_autoUpdate)
		return false;
	return RegisterMessagePlugin(ID_C2S_AUTO_UPDATE_MESSAGE, "C4D to Spout Auto Update"_s, 0, g_autoUpdate);
}

bool IsAutoUpdateEnabled()
{
	return g_autoUpdate && g_autoUpdate->enabled;
}

void SetAutoUpdateEnabled(bool enabled)
{
	if (!g_autoUpdate)
		return;
	g_autoUpdate->enabled = enabled;
	g_autoUpdate->Reset();
	g_autoUpdate->forced = enabled; // Send the current state right away.
	// Wake the message plugin so GetTimer() is re-queried.
	SpecialEventAdd(ID_C2S_AUTO_UPDATE_MESSAGE);
}

void RequestAutoUpdate()
{
	if (g_autoUpdate && g_autoUpdate->enabled)
	{
		g_autoUpdate->forced = true;
		SpecialEventAdd(ID_C2S_AUTO_UPDATE_MESSAGE);
	}
}

} // namespace c2s
