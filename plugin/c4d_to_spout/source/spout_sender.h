#ifndef SPOUT_SENDER_H__
#define SPOUT_SENDER_H__

#include <cstdint>

// Thin wrapper around SpoutDX. The implementation is isolated in spout_sender.cpp so that
// Windows/D3D headers never meet the Maxon headers in the same translation unit.
//
// SpoutDX is not thread-safe: all calls must come from the same thread (the main thread for now).
namespace c2s
{

// Opens the D3D11 device and names the sender. Safe to call repeatedly; re-opens only if the
// name changed. Returns false if D3D11 could not be initialized.
bool SpoutOpen(const char* senderName);

// Sends a tightly packed RGBA8 image. The sender is (re)created to match width/height.
bool SpoutSendRGBA8(const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height);

// Releases the sender and the D3D11 device.
void SpoutClose();

} // namespace c2s

#endif // SPOUT_SENDER_H__
