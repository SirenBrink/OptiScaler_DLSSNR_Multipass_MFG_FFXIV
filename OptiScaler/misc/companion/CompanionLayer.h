#pragma once
#include "CompanionPackets.h"
#include <d3d11.h>
namespace FfxivCompanion::Layer
{
void DrawSettings();
void DrawDiagnostics();
void Capture(const Packets::Queue& before);
void BeginSnapshot();
bool ReplaceBatch(void(__fastcall* original)(uintptr_t,char), uintptr_t renderer, char mode);
bool BeginVisibilityNative(uintptr_t base,uintptr_t renderer,UINT indexBytes,const std::vector<Packets::Packet>& packets,ULONGLONG capturedAt);
void EndVisibilityNative();
void EmitVisibilityCommand(uintptr_t command);
void BindVisibilityCommand(uintptr_t nativeContext,uintptr_t state);
bool InterceptVisibilityIndexed(ID3D11DeviceContext*,UINT,UINT,INT,void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,INT));
void Observe(ID3D11DeviceContext* context);
void Tick(ID3D11DeviceContext* context);
void Stop();
void InvalidateFrame();
void TagLayout(ID3D11InputLayout*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T);
void TagPixel(ID3D11PixelShader*, const void*, SIZE_T);
}
