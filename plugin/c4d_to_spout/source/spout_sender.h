#ifndef SPOUT_SENDER_H__
#define SPOUT_SENDER_H__

#include <cstdint>

// Thin wrapper around SpoutDX. The implementation is isolated in spout_sender.cpp so that
// Windows/D3D headers never meet the Maxon headers in the same translation unit.
//
// SpoutDX is not thread-safe: all calls must come from the same thread (the main thread).
namespace c2s
{

// Opens the D3D11 device and names the sender. Re-opens only if the name or format changed.
// #halfFloat selects R16G16B16A16_FLOAT, otherwise R8G8B8A8_UNORM.
// Returns false if D3D11 could not be initialized.
bool SpoutOpen(const char* senderName, bool halfFloat);

// Sends an image in the format given to SpoutOpen. #pitch is the row size in bytes.
// The sender is (re)created to match width/height.
bool SpoutSend(const void* pixels, std::uint32_t width, std::uint32_t height, std::uint32_t pitch);

// Releases the sender and the D3D11 device.
void SpoutClose();

} // namespace c2s

#endif // SPOUT_SENDER_H__
