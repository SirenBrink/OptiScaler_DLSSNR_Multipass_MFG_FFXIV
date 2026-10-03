#pragma once

#include "SysUtils.h"

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12Utils.h>
#include <shaders/Shader_Dx12.h>

// One heap per in-flight frame so descriptors are never rewritten while the GPU may still read them.
#define UE_NUM_OF_HEAPS 4

// Extracts a UI colour + alpha image from (final frame, HUD-less frame). See UE_Common.h.
class UE_Dx12 : public Shader_Dx12
{
  private:
    FrameDescriptorHeap _frameHeaps[UE_NUM_OF_HEAPS];

    uint32_t InNumThreadsX = 16;
    uint32_t InNumThreadsY = 16;

  public:
    // finalImage/hudless must be in NON_PIXEL_SHADER_RESOURCE (or a state that allows SRV reads),
    // output must be in UNORDERED_ACCESS. All three must have the same size.
    bool Dispatch(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* finalImage, DXGI_FORMAT finalFormat,
                  ID3D12Resource* hudless, DXGI_FORMAT hudlessFormat, ID3D12Resource* output, float threshold,
                  uint32_t radius);

    UE_Dx12(std::string InName, ID3D12Device* InDevice);
    ~UE_Dx12();
};
