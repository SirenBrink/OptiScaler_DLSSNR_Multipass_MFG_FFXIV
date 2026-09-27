#pragma once
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
namespace FfxivCompanion
{
void DrawSourceMarkers(ID3D11DeviceContext* context, ID3D11Texture2D* target);
void DrawSettings();
bool DiagnosticsEnabled();
}
