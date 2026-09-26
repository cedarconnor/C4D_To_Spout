#include "settings.h"

using namespace cinema;

namespace c2s
{

namespace
{

enum : Int32
{
	KEY_ENABLED = 1,
	KEY_SENDER_NAME,
	KEY_WIDTH,
	KEY_ENGINE,
	KEY_RENDER_SETTING_NAME_UNUSED, // Reserved; keeps later key values stable.
	KEY_FORMAT,
	KEY_DEBOUNCE,
	KEY_THROTTLE,
};

UInt32 g_counter = 0;

} // namespace

// Function-local statics: these hold maxon::Strings, which must not be constructed during DLL
// static initialization (before the Maxon runtime is up); doing so stops the module loading.
Settings& GetSettings()
{
	static Settings settings;
	return settings;
}

Status& GetStatus()
{
	static Status status;
	return status;
}

void LoadSettings()
{
	const BaseContainer* bc = GetWorldPluginData(ID_C2S_DIALOG);
	if (!bc)
		return;
	Settings& s = GetSettings();
	const Settings defaults;
	s.enabled = bc->GetBool(KEY_ENABLED, defaults.enabled);
	s.senderName = bc->GetString(KEY_SENDER_NAME, defaults.senderName);
	if (s.senderName.IsEmpty())
		s.senderName = defaults.senderName;
	s.width = maxon::Max(Int32(0), bc->GetInt32(KEY_WIDTH, defaults.width));
	s.engine = Engine(bc->GetInt32(KEY_ENGINE, Int32(defaults.engine)));
	s.format = OutputFormat(bc->GetInt32(KEY_FORMAT, Int32(defaults.format)));
	s.debounceMs = maxon::Max(Int32(0), bc->GetInt32(KEY_DEBOUNCE, defaults.debounceMs));
	s.throttleMs = maxon::Max(Int32(50), bc->GetInt32(KEY_THROTTLE, defaults.throttleMs));
}

void SaveSettings()
{
	const Settings& s = GetSettings();
	BaseContainer bc;
	bc.SetBool(KEY_ENABLED, s.enabled);
	bc.SetString(KEY_SENDER_NAME, s.senderName);
	bc.SetInt32(KEY_WIDTH, s.width);
	bc.SetInt32(KEY_ENGINE, Int32(s.engine));
	bc.SetInt32(KEY_FORMAT, Int32(s.format));
	bc.SetInt32(KEY_DEBOUNCE, s.debounceMs);
	bc.SetInt32(KEY_THROTTLE, s.throttleMs);
	SetWorldPluginData(ID_C2S_DIALOG, bc, false);
}

UInt32 GetStateCounter() { return g_counter; }
void BumpStateCounter() { ++g_counter; }

} // namespace c2s
