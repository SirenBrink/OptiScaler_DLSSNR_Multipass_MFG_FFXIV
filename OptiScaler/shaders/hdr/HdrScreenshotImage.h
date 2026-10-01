#pragma once
#include <windows.h>
#include <dxgiformat.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <span>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace Hdr10::ScreenshotImage {
inline void Check(HRESULT hr) { if (FAILED(hr)) throw hr; }
inline float DecodePQ(float v)
{
    const double p = std::pow(std::clamp(double(v), 0.0, 1.0), 32.0 / 2523.0);
    return float(10000.0 * std::pow(std::max(p - 3424.0 / 4096.0, 0.0) /
        (2413.0 / 128.0 - (2392.0 / 128.0) * p), 16384.0 / 2610.0));
}
inline uint32_t ReadBE(const BYTE* p) { return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3]; }
inline void WriteBE(std::ostream& out, uint32_t v) {
    const BYTE bytes[]={BYTE(v>>24),BYTE(v>>16),BYTE(v>>8),BYTE(v)};
    out.write(reinterpret_cast<const char*>(bytes),4);
}
inline uint32_t Crc(const BYTE* bytes, size_t size) {
    uint32_t crc=0xffffffff;
    for(size_t i=0;i<size;++i) { crc^=bytes[i]; for(int k=0;k<8;++k) crc=(crc>>1)^(0xedb88320u & (0u-(crc&1))); }
    return ~crc;
}
inline void WriteColourChunk(std::ostream& out, bool hdr)
{
    // PNG Third Edition: cICP 9/16/0/1 = BT.2020, PQ, RGB, full range.
    // SDR uses the standard sRGB chunk with perceptual rendering intent.
    const BYTE pq[]={'c','I','C','P',9,16,0,1}, srgb[]={'s','R','G','B',0};
    const BYTE* bytes=hdr?pq:srgb;
    const size_t length=hdr?sizeof(pq):sizeof(srgb);
    WriteBE(out,uint32_t(length-4));out.write(reinterpret_cast<const char*>(bytes),length);WriteBE(out,Crc(bytes,length));
}
inline void SavePng(const std::filesystem::path& path, const void* pixels, UINT width, UINT height,
                    UINT rowPitch, bool hdr, DXGI_FORMAT sourceFormat=DXGI_FORMAT_R10G10B10A2_UNORM)
{
    using Microsoft::WRL::ComPtr;
    if (!pixels || !width || !height || width > 32768 || height > 32768 || rowPitch < width * 4)
        throw E_INVALIDARG;
    const bool bgra = sourceFormat==DXGI_FORMAT_B8G8R8A8_UNORM || sourceFormat==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || sourceFormat==DXGI_FORMAT_B8G8R8A8_TYPELESS;
    const bool rgba = sourceFormat==DXGI_FORMAT_R8G8B8A8_UNORM || sourceFormat==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || sourceFormat==DXGI_FORMAT_R8G8B8A8_TYPELESS;
    if(hdr ? sourceFormat!=DXGI_FORMAT_R10G10B10A2_UNORM : !(rgba||bgra)) throw E_INVALIDARG;
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    ComPtr<IStream> stream;
    Check(CreateStreamOnHGlobal(nullptr, TRUE, &stream));
    ComPtr<IWICBitmapEncoder> encoder;
    Check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
    Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    Check(encoder->CreateNewFrame(&frame, &properties));
    Check(frame->Initialize(properties.Get()));
    Check(frame->SetSize(width, height));
    const auto requested = hdr ? GUID_WICPixelFormat48bppRGB : GUID_WICPixelFormat24bppBGR;
    WICPixelFormatGUID format = requested;
    Check(frame->SetPixelFormat(&format));
    if (format != requested) throw WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    std::vector<std::array<uint16_t,3>> hdrRow(hdr?width:0);
    std::vector<std::array<BYTE,3>> sdrRow(hdr?0:width);
    for (UINT y = 0; y < height; ++y)
    {
        const auto* src = reinterpret_cast<const uint32_t*>(static_cast<const BYTE*>(pixels) + size_t(y)*rowPitch);
        for (UINT x = 0; x < width; ++x) {
            if(hdr) for(unsigned k=0;k<3;++k) hdrRow[x][k]=uint16_t((((src[x]>>(10*k))&1023)*65535+511)/1023);
            else {
                // Preserve the original SDR bytes exactly. ReShade has already
                // applied the user's grading; no HDR roundtrip or second curve.
                auto* c=reinterpret_cast<const BYTE*>(&src[x]);
                sdrRow[x]=bgra?std::array<BYTE,3>{c[0],c[1],c[2]}:std::array<BYTE,3>{c[2],c[1],c[0]};
            }
        }
        UINT stride=width*(hdr?6:3);
        auto* row=hdr?reinterpret_cast<BYTE*>(hdrRow.data()):reinterpret_cast<BYTE*>(sdrRow.data());
        Check(frame->WritePixels(1, stride, stride, row));
    }
    Check(frame->Commit());
    Check(encoder->Commit());
    // WIC versions differ in cICP support. Insert the explicit colour chunk
    // after IHDR and remove conflicting legacy colour metadata, without
    // re-encoding or changing the IDAT pixel bytes.
    STATSTG stat{}; Check(stream->Stat(&stat,STATFLAG_NONAME));
    if(stat.cbSize.QuadPart>512ull*1024*1024) throw E_OUTOFMEMORY;
    // Read the stream's existing storage instead of duplicating the complete
    // encoded image (particularly costly for large 16-bit HDR captures).
    HGLOBAL storage=nullptr; Check(GetHGlobalFromStream(stream.Get(),&storage));
    if(stat.cbSize.QuadPart>GlobalSize(storage)) throw E_FAIL;
    const auto* data=static_cast<const BYTE*>(GlobalLock(storage));
    if(!data) throw E_OUTOFMEMORY;
    struct Unlock { HGLOBAL memory; ~Unlock(){GlobalUnlock(memory);} } unlock{storage};
    const std::span<const BYTE> png(data,size_t(stat.cbSize.QuadPart));
    const BYTE signature[]={137,80,78,71,13,10,26,10};
    if(png.size()<33 || memcmp(png.data(),signature,8)) throw E_FAIL;
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    if(!out) throw E_ACCESSDENIED;
    out.exceptions(std::ios::failbit|std::ios::badbit);
    out.write(reinterpret_cast<const char*>(png.data()),8);
    bool header=false, end=false;
    for(size_t offset=8;offset<png.size();) {
        if(png.size()-offset<12) throw E_FAIL;
        const size_t length=ReadBE(png.data()+offset);
        if(length>png.size()-offset-12) throw E_FAIL;
        auto* type=png.data()+offset+4;
        if(!memcmp(type,"IHDR",4)) {
            if(header || offset!=8 || length!=13) throw E_FAIL;
            out.write(reinterpret_cast<const char*>(png.data()+offset),length+12);
            WriteColourChunk(out,hdr);header=true;
        } else {
            if(!header) throw E_FAIL;
            if(memcmp(type,"cICP",4) && memcmp(type,"sRGB",4) && memcmp(type,"gAMA",4) && memcmp(type,"cHRM",4) && memcmp(type,"iCCP",4))
                out.write(reinterpret_cast<const char*>(png.data()+offset),length+12);
        }
        offset+=length+12;
        if(!memcmp(type,"IEND",4)) { if(length!=0 || offset!=png.size()) throw E_FAIL; end=true; }
    }
    if(!end) throw E_FAIL;
    out.close();
}
}
