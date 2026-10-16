#pragma once
#include <d3d11.h>
#include <functional>
#include <atomic>
namespace FfxivPreviewDLSS {
inline std::atomic<unsigned long long> evaluated{0},fallbacks{0};
inline std::atomic<unsigned long long> jittered{0};
inline std::atomic<unsigned> instances{0};
void Tag(ID3D11PixelShader*,unsigned crc);
void Draw(ID3D11DeviceContext*,const std::function<void()>& nativeDraw);
void Copy(ID3D11DeviceContext*,ID3D11Resource* destination,ID3D11Resource* source,bool full);
void ClearDepth(ID3D11DeviceContext*,ID3D11DepthStencilView*,UINT flags);
void Tick();
void Shutdown();
}
