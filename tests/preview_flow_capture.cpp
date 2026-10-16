#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstdio>
#include <stdexcept>
#include "../OptiScaler/misc/FfxivPreviewFlow.h"
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"d3dcompiler.lib")
using Microsoft::WRL::ComPtr;
void Check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("D3D failure");}
int wmain(int argc,wchar_t** argv)
{
    try {
    if(argc!=3)throw std::runtime_error("Expected capture directory and output file");
    const std::filesystem::path folder(argv[1]);
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
    ComPtr<ID3DBlob> shader,error;
    Check(D3DCompile(FfxivPreviewFlow::FlowShaderCode,sizeof(FfxivPreviewFlow::FlowShaderCode)-1,
        nullptr,nullptr,nullptr,"CSMain","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&shader,&error));
    ComPtr<ID3D11ComputeShader> cs;Check(device->CreateComputeShader(shader->GetBufferPointer(),shader->GetBufferSize(),nullptr,&cs));
    ComPtr<ID3D11ShaderResourceView> inputs[3];UINT width=0,height=0;
    const wchar_t* names[]{L"current.bin",L"previous.bin",L"guide.bin"};
    for(unsigned i=0;i<3;++i){
        std::ifstream file(folder/names[i],std::ios::binary);UINT header[3]{};
        file.read(reinterpret_cast<char*>(header),sizeof(header));
        if(!file || !header[0] || !header[1] || header[0]>8192 || header[1]>8192 || (header[2]!=24 && header[2]!=28))
            throw std::runtime_error("Invalid source capture");
        if(i && (header[0]!=width || header[1]!=height))throw std::runtime_error("Mismatched captures");
        width=header[0];height=header[1];std::vector<char> pixels(size_t(width)*height*4);
        file.read(pixels.data(),std::streamsize(pixels.size()));if(!file)throw std::runtime_error("Truncated capture");
        D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT(header[2]);desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.Usage=D3D11_USAGE_IMMUTABLE;
        D3D11_SUBRESOURCE_DATA data{pixels.data(),width*4,0};ComPtr<ID3D11Texture2D> texture;
        Check(device->CreateTexture2D(&desc,&data,&texture));Check(device->CreateShaderResourceView(texture.Get(),nullptr,&inputs[i]));
    }
    const UINT fw=(width+3)/4,fh=(height+3)/4;
    D3D11_TEXTURE2D_DESC desc{};desc.Width=fw;desc.Height=fh;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> output,readback;ComPtr<ID3D11UnorderedAccessView> uav;
    Check(device->CreateTexture2D(&desc,nullptr,&output));Check(device->CreateUnorderedAccessView(output.Get(),nullptr,&uav));
    desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Check(device->CreateTexture2D(&desc,nullptr,&readback));
    D3D11_SAMPLER_DESC samplerDesc{};samplerDesc.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU=samplerDesc.AddressV=samplerDesc.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;samplerDesc.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;Check(device->CreateSamplerState(&samplerDesc,&sampler));
    D3D11_BUFFER_DESC bufferDesc{};bufferDesc.ByteWidth=16;bufferDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    const UINT values[]{width,height,fw,fh};D3D11_SUBRESOURCE_DATA data{values,0,0};ComPtr<ID3D11Buffer> constants;
    Check(device->CreateBuffer(&bufferDesc,&data,&constants));
    context->CSSetShader(cs.Get(),nullptr,0);context->CSSetConstantBuffers(0,1,constants.GetAddressOf());
    context->CSSetSamplers(0,1,sampler.GetAddressOf());context->CSSetUnorderedAccessViews(0,1,uav.GetAddressOf(),nullptr);
    ID3D11ShaderResourceView* views[]{inputs[0].Get(),inputs[1].Get(),nullptr,nullptr,inputs[2].Get()};
    context->CSSetShaderResources(0,5,views);
    ComPtr<ID3D11Query> start,end,disjoint;D3D11_QUERY_DESC query{D3D11_QUERY_TIMESTAMP,0};
    Check(device->CreateQuery(&query,&start));Check(device->CreateQuery(&query,&end));
    query.Query=D3D11_QUERY_TIMESTAMP_DISJOINT;Check(device->CreateQuery(&query,&disjoint));
    context->Begin(disjoint.Get());context->End(start.Get());
    for(unsigned i=0;i<3;++i)context->Dispatch((fw+7)/8,(fh+7)/8,1);
    context->End(end.Get());context->End(disjoint.Get());context->CopyResource(readback.Get(),output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};Check(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));
    std::ofstream saved(argv[2],std::ios::binary);const UINT header[]{fw,fh,UINT(desc.Format)};
    saved.write(reinterpret_cast<const char*>(header),sizeof(header));
    for(UINT y=0;y<fh;++y)saved.write(static_cast<const char*>(mapped.pData)+y*mapped.RowPitch,fw*8);
    context->Unmap(readback.Get(),0);if(!saved)throw std::runtime_error("Readback output failed");
    UINT64 a=0,b=0;D3D11_QUERY_DATA_TIMESTAMP_DISJOINT timing{};
    Check(context->GetData(start.Get(),&a,sizeof(a),0));Check(context->GetData(end.Get(),&b,sizeof(b),0));
    Check(context->GetData(disjoint.Get(),&timing,sizeof(timing),0));
    if(!timing.Disjoint)printf("GPU image-motion: %.3f ms per dispatch, %ux%u field\n",double(b-a)*1000.0/double(timing.Frequency)/3,fw,fh);
    puts("PASS: production motion shader dispatched and captured on the local GPU");
    return 0;
    } catch(const std::exception& e){printf("FAIL: %s\n",e.what());return 1;}
}
