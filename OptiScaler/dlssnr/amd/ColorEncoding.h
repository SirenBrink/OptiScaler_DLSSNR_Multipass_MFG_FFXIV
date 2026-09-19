#pragma once

// Adapted from MatheusGViana/dlss-5-amd-project (7b9dcb9).
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <stdexcept>

namespace AmdPreSr
{
inline constexpr char EncodingShader[] = R"(
Texture2D<float4> src:register(t0);RWTexture2D<float4> dst:register(u0);
cbuffer Params:register(b0){uint w,h,mode,inverse;}
float transform(float v){float a=abs(v);float y=a;
 if(mode==2)y=inverse?(a<=0.0031308?12.92*a:1.055*pow(a,1.0/2.4)-0.055):(a<=0.04045?a/12.92:pow((a+0.055)/1.055,2.4));
 if(mode==3)y=pow(a,inverse?1.0/2.2:2.2);
 return sign(v)*y;}
[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){if(p.x>=w||p.y>=h)return;float4 c=src.Load(int3(p.xy,0));dst[p.xy]=float4(transform(c.r),transform(c.g),transform(c.b),c.a);}
)";

class ColorEncoding
{
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Resource> output;

    static void Check(HRESULT result)
    {
        if (FAILED(result))
            throw std::runtime_error("AMD encoding D3D12 error " + std::to_string(static_cast<UINT>(result)));
    }

    static void Barrier(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
                        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        if (before == after)
            return;
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
        commands->ResourceBarrier(1, &barrier);
    }

  public:
    explicit ColorEncoding(ID3D12Device* sourceDevice) : device(sourceDevice)
    {
        D3D12_DESCRIPTOR_RANGE ranges[] {
            { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 },
            { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1 },
        };
        D3D12_ROOT_PARAMETER params[2] {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = { 2, ranges };
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = { 0, 0, 4 };

        D3D12_ROOT_SIGNATURE_DESC rootDesc {};
        rootDesc.NumParameters = 2;
        rootDesc.pParameters = params;
        ComPtr<ID3DBlob> blob, error;
        Check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error));
        Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)));
        Check(D3DCompile(EncodingShader, sizeof(EncodingShader), "AMD encoding", nullptr, nullptr, "main", "cs_5_0",
                         0, 0, &blob, &error));

        D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc {};
        pipelineDesc.pRootSignature = root.Get();
        pipelineDesc.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
        Check(device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(&pipeline)));

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 2;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)));
    }

    ID3D12Resource* Run(ID3D12GraphicsCommandList* commands, ID3D12Resource* source,
                        D3D12_RESOURCE_STATES sourceState, UINT width, UINT height, UINT mode, bool inverse)
    {
        if (!output || output->GetDesc().Width != width || output->GetDesc().Height != height)
        {
            D3D12_HEAP_PROPERTIES heapProperties {};
            heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC resourceDesc {};
            resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            resourceDesc.Width = width;
            resourceDesc.Height = height;
            resourceDesc.DepthOrArraySize = 1;
            resourceDesc.MipLevels = 1;
            resourceDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            resourceDesc.SampleDesc.Count = 1;
            resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            output.Reset();
            Check(device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc,
                                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                  IID_PPV_ARGS(&output)));
        }

        auto format = source->GetDesc().Format;
        // Request raw channel values, avoiding automatic sRGB decoding twice.
        if (format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
            format = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_TYPELESS)
            format = DXGI_FORMAT_B8G8R8A8_UNORM;

        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateShaderResourceView(source, &srv, cpu);
        cpu.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(output.Get(), nullptr, &uav, cpu);

        Barrier(commands, source, sourceState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(commands, output.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto descriptorHeap = heap.Get();
        commands->SetDescriptorHeaps(1, &descriptorHeap);
        commands->SetComputeRootSignature(root.Get());
        commands->SetPipelineState(pipeline.Get());
        commands->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        const UINT constants[] { width, height, mode, inverse ? 1u : 0u };
        commands->SetComputeRoot32BitConstants(1, 4, constants, 0);
        commands->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        Barrier(commands, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(commands, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, sourceState);
        return output.Get();
    }
};
} // namespace AmdPreSr
