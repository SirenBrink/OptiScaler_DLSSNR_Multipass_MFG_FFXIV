#pragma once
#include "SysUtils.h"
struct ID3D11DeviceContext;

class D3D11Hooks
{
  public:
    static void Hook(HMODULE dx11Module);
    static void HookToDevice(ID3D11Device* InDevice);
    static void Unhook();
    static void UpdateAutomaticMipBias(ID3D11DeviceContext* context, unsigned render, unsigned output);
    static void ResetAutomaticMipBias();
};
