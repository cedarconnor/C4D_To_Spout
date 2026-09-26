#ifndef AUTO_UPDATE_H__
#define AUTO_UPDATE_H__

#include "c4d.h"

namespace c2s
{

// Auto-update: re-render and send whenever the active document's scene changes.
//
// A MessageData timer polls a cheap change signature (active document, scene camera, current
// time and the document's hierarchical dirty counters). A render happens once the signature
// has been stable for the debounce time, or at most every throttle interval during continuous
// edits (drags, playback). Only the latest state is rendered. Everything runs on the main
// thread, so there is at most one render in flight and nothing to cancel on shutdown.

bool RegisterAutoUpdate();

bool IsAutoUpdateEnabled();
void SetAutoUpdateEnabled(bool enabled);

// Forces the next timer tick to render, even if nothing changed (e.g. after enabling).
void RequestAutoUpdate();

} // namespace c2s

#endif // AUTO_UPDATE_H__
