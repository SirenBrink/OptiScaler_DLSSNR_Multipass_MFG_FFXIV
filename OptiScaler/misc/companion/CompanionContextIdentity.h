#pragma once
#include <d3d11.h>
#include <atomic>

namespace FfxivCompanion::ContextIdentity
{
// ReShade may forward an engine context to a different COM pointer in our draw
// hook. A device-child private-data marker follows that forwarding without
// guessing wrapper layouts or accepting a different immediate context.
inline constexpr GUID tag{0x4e6480c2,0x40cb,0x496a,{0xa2,0x5d,0x78,0xce,0xd4,0x34,0xdd,0x83}};
inline std::atomic<unsigned long long> sequence{0};
inline unsigned long long Stamp(ID3D11DeviceContext* context)
{
    if(!context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return 0;
    auto token=++sequence;
    return SUCCEEDED(context->SetPrivateData(tag,sizeof(token),&token))?token:0;
}
inline bool Matches(ID3D11DeviceContext* context,unsigned long long expected)
{
    unsigned long long actual=0;UINT size=sizeof(actual);
    return context && expected && SUCCEEDED(context->GetPrivateData(tag,&size,&actual)) &&
           size==sizeof(actual) && actual==expected;
}
}
