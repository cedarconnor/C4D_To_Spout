#include "auto_update.h"

#include "c4d_basedraw.h"
#include "c4d_messagedata.h"

#include "pipeline.h"
#include "settings.h"

using namespace cinema;

namespace c2s
{

namespace
{

constexpr Int32 TIMER_MS = 50;

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
	Bool forced = false;

	Int32 GetTimer() override { return GetSettings().enabled ? TIMER_MS : 0; }

	Bool CoreMessage(Int32 id, const BaseContainer& bc) override
	{
		// Wake-up after the toggle: GetTimer() is re-queried after this message.
		if (id == ID_C2S_AUTO_UPDATE_MESSAGE)
			return true;
		const Settings& settings = GetSettings();
		if (id != MSG_TIMER || !settings.enabled)
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

		const Bool stable = now - _lastChangeMs >= Int64(settings.debounceMs);
		const Bool overdue = now - _firstChangeMs >= Int64(settings.throttleMs);
		if (!forced && !stable && !overdue)
			return true;

		const maxon::Result<void> result = RenderAndSend(doc); // Updates GetStatus().
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
	}

private:
	Signature _pending; // Latest observed state.
	Signature _sent;    // State of the last render sent.
	Signature _failed;  // State of the last failed render (to avoid log spam).
	Int64 _firstChangeMs = 0;
	Int64 _lastChangeMs = 0;
};

AutoUpdateMessage* g_autoUpdate = nullptr; // Owned by Cinema 4D after registration.

} // namespace

bool RegisterAutoUpdate()
{
	g_autoUpdate = NewObjClear(AutoUpdateMessage);
	if (!g_autoUpdate)
		return false;
	// Start with whatever was persisted.
	g_autoUpdate->forced = GetSettings().enabled;
	return RegisterMessagePlugin(ID_C2S_AUTO_UPDATE_MESSAGE, "C4D to Spout Auto Update"_s, 0, g_autoUpdate);
}

bool IsAutoUpdateEnabled()
{
	return GetSettings().enabled;
}

void SetAutoUpdateEnabled(bool enabled)
{
	GetSettings().enabled = enabled;
	SaveSettings();
	BumpStateCounter();
	if (!g_autoUpdate)
		return;
	g_autoUpdate->Reset();
	g_autoUpdate->forced = enabled; // Send the current state right away.
	// Wake the message plugin so GetTimer() is re-queried.
	SpecialEventAdd(ID_C2S_AUTO_UPDATE_MESSAGE);
}

void RequestAutoUpdate()
{
	if (g_autoUpdate && GetSettings().enabled)
	{
		g_autoUpdate->forced = true;
		SpecialEventAdd(ID_C2S_AUTO_UPDATE_MESSAGE);
	}
}

} // namespace c2s
