// Birdie — HUD overlay for MSFS 2024. GPL-3.0-only.
// PNG read/write through WIC (RGBA, 8 bit per channel, values stored as given: no premultiply conversion).
#include "png.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace birdie::test {
namespace {

IWICImagingFactory* factory() {
  static ComPtr<IWICImagingFactory> f = [] {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> p;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&p));
    return p;
  }();
  return f.Get();
}

}  // namespace

bool load_png_rgba(const std::filesystem::path& file, Image& img) {
  IWICImagingFactory* f = factory();
  if (!f) return false;
  ComPtr<IWICBitmapDecoder> dec;
  if (FAILED(f->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)))
    return false;
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(dec->GetFrame(0, &frame))) return false;
  // 32bppRGBA is the PNG's own format for 8-bit RGBA: the converter passes values through unchanged
  ComPtr<IWICFormatConverter> conv;
  if (FAILED(f->CreateFormatConverter(&conv))) return false;
  if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeCustom)))
    return false;
  UINT w = 0, h = 0;
  conv->GetSize(&w, &h);
  img.w = static_cast<int>(w);
  img.h = static_cast<int>(h);
  img.px.resize(static_cast<size_t>(w) * h * 4);
  return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(img.px.size()), img.px.data()));
}

bool save_png_rgba(const std::filesystem::path& file, const Image& img) {
  IWICImagingFactory* f = factory();
  if (!f) return false;
  ComPtr<IWICStream> stream;
  if (FAILED(f->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)))
    return false;
  ComPtr<IWICBitmapEncoder> enc;
  if (FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
      FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
    return false;
  ComPtr<IWICBitmapFrameEncode> frame;
  if (FAILED(enc->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) return false;
  frame->SetSize(img.w, img.h);
  // the PNG encoder takes BGRA, not RGBA (it rewrites fmt; asking for RGBA used to leave an empty file)
  WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
  frame->SetPixelFormat(&fmt);
  if (fmt != GUID_WICPixelFormat32bppBGRA) return false;
  std::vector<BYTE> bgra(img.px.begin(), img.px.end());
  for (size_t i = 0; i + 3 < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
  if (FAILED(frame->WritePixels(img.h, img.w * 4, static_cast<UINT>(bgra.size()), bgra.data()))) return false;
  return SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}

}  // namespace birdie::test
