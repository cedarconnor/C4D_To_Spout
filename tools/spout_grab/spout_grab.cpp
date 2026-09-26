// spout_grab: receives one frame from a Spout sender and writes it to a 32-bit BMP.
// Usage: spout_grab [sender_name] [out.bmp] [timeout_ms]
// Used to verify the C4D plugin's output without a GUI receiver.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "SpoutDX.h"

static bool WriteBmp(const char* path, const unsigned char* rgba, unsigned w, unsigned h)
{
	FILE* f = std::fopen(path, "wb");
	if (!f)
		return false;
	const unsigned imageSize = w * h * 4;
	BITMAPFILEHEADER fh{};
	BITMAPINFOHEADER ih{};
	fh.bfType = 0x4D42;
	fh.bfOffBits = sizeof(fh) + sizeof(ih);
	fh.bfSize = fh.bfOffBits + imageSize;
	ih.biSize = sizeof(ih);
	ih.biWidth = LONG(w);
	ih.biHeight = -LONG(h); // top-down
	ih.biPlanes = 1;
	ih.biBitCount = 32;
	ih.biCompression = BI_RGB;
	ih.biSizeImage = imageSize;
	std::fwrite(&fh, sizeof(fh), 1, f);
	std::fwrite(&ih, sizeof(ih), 1, f);
	std::vector<unsigned char> bgra(imageSize);
	for (unsigned i = 0; i < w * h; ++i)
	{
		bgra[i * 4 + 0] = rgba[i * 4 + 2];
		bgra[i * 4 + 1] = rgba[i * 4 + 1];
		bgra[i * 4 + 2] = rgba[i * 4 + 0];
		bgra[i * 4 + 3] = rgba[i * 4 + 3];
	}
	std::fwrite(bgra.data(), 1, bgra.size(), f);
	std::fclose(f);
	return true;
}

int main(int argc, char** argv)
{
	const std::string name = argc > 1 ? argv[1] : "C4D_LatLong";
	const std::string out = argc > 2 ? argv[2] : "spout_grab.bmp";
	const int timeoutMs = argc > 3 ? std::atoi(argv[3]) : 5000;

	spoutDX receiver;
	if (!receiver.OpenDirectX11())
	{
		std::fprintf(stderr, "OpenDirectX11 failed\n");
		return 2;
	}
	receiver.SetReceiverName(name.c_str());

	// Read the sender's shared texture directly (GPU path, like UE receivers). The CPU
	// ReceiveImage path is double-buffered and returns the previous frame, which is empty
	// for a sender that only sends on change.
	for (int waited = 0; waited <= timeoutMs; waited += 50)
	{
		if (receiver.ReceiveTexture())
		{
			receiver.IsUpdated(); // Clear the update flag after connecting.
			ID3D11Texture2D* tex = receiver.GetSenderTexture();
			const unsigned w = receiver.GetSenderWidth(), h = receiver.GetSenderHeight();
			if (tex && w && h)
			{
				std::vector<unsigned char> pixels(size_t(w) * h * 4);
				// ReadTexurePixels is double-buffered through two staging textures: the first call
				// returns the (empty) previous staging copy, the second returns this frame.
				receiver.ReadTexurePixels(tex, pixels.data());
				if (receiver.ReadTexurePixels(tex, pixels.data()))
				{
					if (!WriteBmp(out.c_str(), pixels.data(), w, h))
						return 3;
					std::printf("Received '%s' %ux%u format=%d -> %s\n", name.c_str(), w, h,
						int(receiver.GetSenderFormat()), out.c_str());
					receiver.ReleaseReceiver();
					return 0;
				}
			}
		}
		Sleep(50);
	}
	std::fprintf(stderr, "No frame from sender '%s' within %d ms\n", name.c_str(), timeoutMs);
	return 1;
}
