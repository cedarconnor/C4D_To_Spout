#ifndef SETTINGS_H__
#define SETTINGS_H__

#include "c4d.h"

namespace c2s
{

// Development IDs (Maxon reserves 1000001-1000010 for testing). Replace with registered IDs
// from https://developers.maxon.net/forum/pid before distributing.
constexpr cinema::Int32 ID_C2S_TEST_PATTERN = 1000001;
constexpr cinema::Int32 ID_C2S_RENDER_NOW = 1000002;
constexpr cinema::Int32 ID_C2S_AUTO_UPDATE_MESSAGE = 1000003;
constexpr cinema::Int32 ID_C2S_AUTO_UPDATE_TOGGLE = 1000004;
constexpr cinema::Int32 ID_C2S_DIALOG = 1000005; // Also the world plugin data key.

enum class Engine : cinema::Int32
{
	VIEWPORT = 0,         // Force the Viewport Renderer (fast, default).
	RENDER_SETTINGS = 1,  // Use the render settings' own engine (e.g. Redshift for quality).
};

enum class OutputFormat : cinema::Int32
{
	RGBA8_DISPLAY = 0,  // OCIO view transform baked, 8-bit (R8G8B8A8_UNORM).
	RGBA16F_LINEAR = 1, // Raw render space, half float (R16G16B16A16_FLOAT).
};

struct Settings
{
	cinema::Bool enabled = false;          // Auto update.
	maxon::String senderName = "C4D_LatLong"_s;
	cinema::Int32 width = 0;               // 0 = render settings' XRES. Height follows XRES:YRES.
	Engine engine = Engine::VIEWPORT;      // Always uses the document's active render settings.
	OutputFormat format = OutputFormat::RGBA8_DISPLAY;
	cinema::Int32 debounceMs = 150;
	cinema::Int32 throttleMs = 500;
};

// Last result, shown in the dialog.
struct Status
{
	maxon::String text = "Idle"_s;
	maxon::String warning;
	cinema::Bool error = false;
};

Settings& GetSettings();
Status& GetStatus();

void LoadSettings();  // From the world plugin container (call once at startup).
void SaveSettings();  // To the world plugin container.

// Bumped whenever settings or status change, so the dialog knows to refresh.
cinema::UInt32 GetStateCounter();
void BumpStateCounter();

} // namespace c2s

#endif // SETTINGS_H__
