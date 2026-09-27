#pragma once
#include <xefg_swapchain_d3d12.h>
#include <shaders/hdr/Hdr10.h>

namespace XeFGHdr
{
// OptiHDR's presentation image is PQ/BT.2020. XeFG requires an identical
// encoding and pixel format for its HUD-free input. Never modify the SDR
// resource retained by the upscaler, NR, or ReShade.
inline bool PrepareHudless(ID3D12Device* device,ID3D12GraphicsCommandList* commands,
                           xefg_swapchain_d3d12_resource_data_t& resource)
{
    if(resource.type!=XEFG_SWAPCHAIN_RES_HUDLESS_COLOR)return false;
    auto* converted=Hdr10::Convert(device,commands,resource.pResource,resource.incomingState);
    if(!converted)return false;
    resource.pResource=converted;
    resource.incomingState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    // XeFG records its own copy on this list, before the conversion pool may
    // recycle the texture. UNTIL_NEXT_PRESENT would outlive that protection.
    resource.validity=XEFG_SWAPCHAIN_RV_ONLY_NOW;
    return true;
}
}
