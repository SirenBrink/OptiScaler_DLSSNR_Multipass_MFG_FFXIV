#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstring>
#include <wrl/client.h>

inline bool ExternalHudlessNeedsNormalization(DXGI_FORMAT source, DXGI_FORMAT target) {
    return source == DXGI_FORMAT_B8G8R8A8_UNORM && target == DXGI_FORMAT_R8G8B8A8_UNORM;
}

// Normalize byte-channel order through typed texture reads, without changing gamma or brightness.
inline constexpr const char* ExternalHudlessNormalizeShader = R"(
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Target : register(u0);
[numthreads(16,16,1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    uint w,h; Target.GetDimensions(w,h);
    if(id.x<w && id.y<h) Target[id.xy]=Source.Load(int3(id.xy,0));
}
)";

class ExternalHudlessNormalize {
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader;
public:
    bool Copy(ID3D11Device* device, ID3D11DeviceContext* context,
              ID3D11Texture2D* source, ID3D11Texture2D* target) {
        using Microsoft::WRL::ComPtr;
        if(!shader) {
            ComPtr<ID3DBlob> code,error;
            if(FAILED(D3DCompile(ExternalHudlessNormalizeShader,strlen(ExternalHudlessNormalizeShader),
                 nullptr,nullptr,nullptr,"CSMain","cs_5_0",0,0,&code,&error)) ||
               FAILED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader)))return false;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
        ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11UnorderedAccessView> uav;
        if(FAILED(device->CreateShaderResourceView(source,&sd,&srv)) ||
           FAILED(device->CreateUnorderedAccessView(target,nullptr,&uav)))return false;
        ComPtr<ID3D11ComputeShader> oldShader;
        ID3D11ClassInstance* instances[256]{};UINT count=256;
        context->CSGetShader(&oldShader,instances,&count);
        ComPtr<ID3D11ShaderResourceView> oldSrv;ComPtr<ID3D11UnorderedAccessView> oldUav;
        context->CSGetShaderResources(0,1,&oldSrv);context->CSGetUnorderedAccessViews(0,1,&oldUav);
        auto* s=srv.Get();auto* u=uav.Get();UINT preserve=UINT(-1);
        context->CSSetShader(shader.Get(),nullptr,0);context->CSSetShaderResources(0,1,&s);
        context->CSSetUnorderedAccessViews(0,1,&u,&preserve);
        D3D11_TEXTURE2D_DESC td{};target->GetDesc(&td);
        context->Dispatch((td.Width+15)/16,(td.Height+15)/16,1);
        ID3D11ShaderResourceView* nullSrv=nullptr;ID3D11UnorderedAccessView* nullUav=nullptr;
        context->CSSetShaderResources(0,1,&nullSrv);context->CSSetUnorderedAccessViews(0,1,&nullUav,&preserve);
        s=oldSrv.Get();u=oldUav.Get();context->CSSetShaderResources(0,1,&s);
        context->CSSetUnorderedAccessViews(0,1,&u,&preserve);context->CSSetShader(oldShader.Get(),instances,count);
        for(UINT i=0;i<count;++i)if(instances[i])instances[i]->Release();
        return true;
    }
};
