#include "pch.h"
#include "UE_Dx12.h"

#include "UE_Common.h"

#include <shaders/Shader_Common.h>
#include <State.h>

#include <algorithm>

bool UE_Dx12::Dispatch(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* finalImage, DXGI_FORMAT finalFormat,
                       ID3D12Resource* hudless, DXGI_FORMAT hudlessFormat, ID3D12Resource* output, float threshold,
                       uint32_t radius)
{
    if (!_init || _device == nullptr || cmdList == nullptr || finalImage == nullptr || hudless == nullptr ||
        output == nullptr)
    {
        return false;
    }

    const auto outDesc = output->GetDesc();

    _counter++;
    _counter = _counter % UE_NUM_OF_HEAPS;
    FrameDescriptorHeap& currentHeap = _frameHeaps[_counter];

    CreateShaderResourceView(_device, finalImage, currentHeap.GetSrvCPU(0), finalFormat);
    CreateShaderResourceView(_device, hudless, currentHeap.GetSrvCPU(1), hudlessFormat);
    CreateUnorderedAccessView(_device, output, currentHeap.GetUavCPU(0), 0);

    UEConstants constants {};
    constants.Threshold = std::clamp(threshold, 0.0f, 1.0f);
    constants.Radius = std::min<uint32_t>(radius, 4);
    constants.Width = static_cast<uint32_t>(outDesc.Width);
    constants.Height = outDesc.Height;

    if (!CreateConstantsBuffer(_device, _constantBuffer, constants, currentHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);

    cmdList->SetComputeRootSignature(_rootSignature);
    cmdList->SetPipelineState(_pipelineState);
    cmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    const UINT groupsX = (constants.Width + InNumThreadsX - 1) / InNumThreadsX;
    const UINT groupsY = (constants.Height + InNumThreadsY - 1) / InNumThreadsY;
    cmdList->Dispatch(groupsX, groupsY, 1);

    return true;
}

UE_Dx12::UE_Dx12(std::string InName, ID3D12Device* InDevice) : Shader_Dx12(InName, InDevice)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    if (!SetupRootSignature(InDevice, 2, 1, 1))
    {
        LOG_ERROR("[{0}] Failed to setup root signature", _name);
        return;
    }

    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(UEConstants));
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    auto result =
        InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                          nullptr, IID_PPV_ARGS(&_constantBuffer));

    if (result != S_OK)
    {
        LOG_ERROR("[{0}] CreateCommittedResource error {1:x}", _name, (unsigned int) result);
        return;
    }

    // No precompiled bytecode ships for this shader: always compile it at runtime (d3dcompiler_47).
    ID3DBlob* shaderBlob = CompileShader(ueShaderCode.c_str(), "CSMain", "cs_5_0");
    if (shaderBlob == nullptr)
    {
        LOG_ERROR("[{0}] Failed to compile the UI extraction shader", _name);
        return;
    }

    const bool pipelineOk =
        CreateComputeShader(InDevice, _rootSignature, &_pipelineState, shaderBlob, D3D12_SHADER_BYTECODE {});
    shaderBlob->Release();

    if (!pipelineOk)
    {
        LOG_ERROR("[{0}] Failed to create compute pipeline", _name);
        return;
    }

    _init = InitHeaps(InDevice, _frameHeaps, UE_NUM_OF_HEAPS);
    LOG_INFO("[{0}] UI extraction shader ready: {1}", _name, _init);
}

UE_Dx12::~UE_Dx12()
{
    if (!_init || State::Instance().isShuttingDown)
        return;

    for (int i = 0; i < UE_NUM_OF_HEAPS; i++)
        _frameHeaps[i].ReleaseHeaps();
}
