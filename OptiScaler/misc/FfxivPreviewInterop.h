#pragma once
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
namespace FfxivPreviewInterop {
using Microsoft::WRL::ComPtr;
struct Surface{ComPtr<ID3D11Texture2D> dx11;ComPtr<ID3D12Resource> dx12;};
inline bool CreateSurface(ID3D11Device* dx11,ID3D12Device* dx12,Surface& surface,UINT w,UINT h,DXGI_FORMAT format){
    D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=format;
    desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    if(FAILED(dx11->CreateTexture2D(&desc,nullptr,&surface.dx11)))return false;
    ComPtr<IDXGIResource1> shared;if(FAILED(surface.dx11.As(&shared)))return false;
    HANDLE handle=nullptr;if(FAILED(shared->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle)))return false;
    HRESULT result=dx12->OpenSharedHandle(handle,IID_PPV_ARGS(&surface.dx12));CloseHandle(handle);return SUCCEEDED(result);
}
inline bool CreateFence(ID3D11Device5* dx11,ID3D12Device* dx12,ComPtr<ID3D11Fence>& fence11,ComPtr<ID3D12Fence>& fence12){
    if(FAILED(dx12->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence12))))return false;
    HANDLE handle=nullptr;if(FAILED(dx12->CreateSharedHandle(fence12.Get(),nullptr,GENERIC_ALL,nullptr,&handle)))return false;
    HRESULT result=dx11->OpenSharedFence(handle,IID_PPV_ARGS(&fence11));CloseHandle(handle);return SUCCEEDED(result);
}
}
