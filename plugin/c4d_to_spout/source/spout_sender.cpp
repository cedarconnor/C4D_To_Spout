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
bool g_halfFloat = false;
}

bool SpoutOpen(const char* senderName, bool halfFloat)
{
	const std::string name = (senderName && *senderName) ? senderName : "C4D_LatLong";
	if (g_sender && g_name == name && g_halfFloat == halfFloat)
		return true;

	SpoutClose();

	auto sender = std::make_unique<spoutDX>();
	if (!sender->OpenDirectX11())
		return false;

	sender->SetSenderFormat(halfFloat ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM);
	sender->SetSenderName(name.c_str());

	g_sender = std::move(sender);
	g_name = name;
	g_halfFloat = halfFloat;
	return true;
}

bool SpoutSend(const void* pixels, std::uint32_t width, std::uint32_t height, std::uint32_t pitch)
{
	if (!g_sender || !pixels || width == 0 || height == 0)
		return false;
	return g_sender->SendImage(static_cast<const unsigned char*>(pixels), width, height, pitch);
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
