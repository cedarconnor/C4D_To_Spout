// Only Windows/Spout headers here; see spout_sender.h.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "spout_sender.h"

#include <memory>
#include <string>

#include "SpoutDX.h"

namespace c2s
{

namespace
{
std::unique_ptr<spoutDX> g_sender;
std::string g_name;
}

bool SpoutOpen(const char* senderName)
{
	const std::string name = senderName ? senderName : "C4D_LatLong";
	if (g_sender && g_name == name)
		return true;

	SpoutClose();

	auto sender = std::make_unique<spoutDX>();
	if (!sender->OpenDirectX11())
		return false;

	// Display-referred 8-bit RGBA, matching the OCIO-baked render output.
	sender->SetSenderFormat(DXGI_FORMAT_R8G8B8A8_UNORM);
	sender->SetSenderName(name.c_str());

	g_sender = std::move(sender);
	g_name = name;
	return true;
}

bool SpoutSendRGBA8(const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height)
{
	if (!g_sender || !rgba || width == 0 || height == 0)
		return false;
	return g_sender->SendImage(rgba, width, height, width * 4);
}

void SpoutClose()
{
	if (!g_sender)
		return;
	g_sender->ReleaseSender();
	g_sender->CloseDirectX11();
	g_sender.reset();
	g_name.clear();
}

} // namespace c2s
