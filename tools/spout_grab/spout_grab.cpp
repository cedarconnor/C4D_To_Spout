// spout_grab: receives one frame from a Spout sender and writes it to a 32-bit BMP.
// Usage: spout_grab [sender_name] [out.bmp] [timeout_ms]
// Used to verify the C4D plugin's output without a GUI receiver.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "SpoutDX.h"

static float HalfToFloat(unsigned short h)
{
	const unsigned sign = (h >> 15) & 1, exp = (h >> 10) & 0x1f, mant = h & 0x3ff;
	float v;
	if (exp == 0)
		v = std::ldexp(float(mant), -24);
	else if (exp == 31)
		v = mant ? NAN : INFINITY;
	else
		v = std::ldexp(float(mant | 0x400), int(exp) - 25);
	return sign ? -v : v;
}

// Preview conversion for RGBA16F linear senders: clamp, then sRGB-encode to 8-bit.
static std::vector<unsigned char> HalfLinearToSrgb8(const std::vector<unsigned char>& raw, unsigned w, unsigned h)
{
	std::vector<unsigned char> out(size_t(w) * h * 4);
	const unsigned short* src = reinterpret_cast<const unsigned short*>(raw.data());
	for (size_t i = 0; i < size_t(w) * h; ++i)
	{
		for (int c = 0; c < 4; ++c)
		{
			float v = HalfToFloat(src[i * 4 + c]);
			v = v != v ? 0.0f : std::fmin(std::fmax(v, 0.0f), 1.0f);
			if (c < 3)
				v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
			out[i * 4 + c] = static_cast<unsigned char>(v * 255.0f + 0.5f);
		}
	}
	return out;
}

// Copies #tex to a CPU staging texture and reads it row by row using the real row pitch.
static bool ReadTexture(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex,
	unsigned char* out, unsigned bytesPerPixel)
{
	if (!device || !ctx || !tex)
		return false;
	D3D11_TEXTURE2D_DESC desc{};
	tex->GetDesc(&desc);
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.MiscFlags = 0;
	ID3D11Texture2D* staging = nullptr;
	if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
		return false;
	ctx->CopyResource(staging, tex);
	D3D11_MAPPED_SUBRESOURCE mapped{};
	bool ok = SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
	if (ok)
	{
		const size_t rowBytes = size_t(desc.Width) * bytesPerPixel;
		for (unsigned y = 0; y < desc.Height; ++y)
			std::memcpy(out + y * rowBytes, static_cast<const unsigned char*>(mapped.pData) + size_t(y) * mapped.RowPitch, rowBytes);
		ctx->Unmap(staging, 0);
	}
	staging->Release();
	return ok;
}

// #bgra: the pixels are already in BMP byte order (a B8G8R8A8 sender, e.g. SpoutGL).
static bool WriteBmp(const char* path, const unsigned char* rgba, unsigned w, unsigned h, bool bgra)
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
	std::vector<unsigned char> out(rgba, rgba + imageSize);
	if (!bgra)
	{
		for (unsigned i = 0; i < w * h; ++i)
			std::swap(out[i * 4 + 0], out[i * 4 + 2]);
	}
	std::fwrite(out.data(), 1, out.size(), f);
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
				const bool halfFloat = receiver.GetSenderFormat() == DXGI_FORMAT_R16G16B16A16_FLOAT;
				std::vector<unsigned char> raw(size_t(w) * h * (halfFloat ? 8 : 4));
				// Own staging readback: SpoutDX's ReadTexurePixels assumes 4 bytes/pixel (breaks for
				// RGBA16F) and is double-buffered (returns the previous frame).
				if (ReadTexture(receiver.GetDX11Device(), receiver.GetDX11Context(), tex, raw.data(), halfFloat ? 8 : 4))
				{
					std::vector<unsigned char> pixels = halfFloat ? HalfLinearToSrgb8(raw, w, h) : raw;
					const bool bgra = receiver.GetSenderFormat() == DXGI_FORMAT_B8G8R8A8_UNORM;
						if (!WriteBmp(out.c_str(), pixels.data(), w, h, bgra))
						return 3;
					std::printf("Received '%s' %ux%u format=%d frame=%ld -> %s\n", name.c_str(), w, h,
						int(receiver.GetSenderFormat()), receiver.GetSenderFrame(), out.c_str());
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
