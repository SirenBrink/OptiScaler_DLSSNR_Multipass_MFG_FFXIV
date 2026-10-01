#pragma once

// Used by OptiScaler's DX12 ImGui backend only. HDR is a mode, not a brightness
// multiplier. Map the SDR theme to a fixed 203-nit reference white, preserving
// black and relative luminance without the game's HDR expansion/contrast curve.
namespace Hdr10 {
inline constexpr char MenuPixelShader[] = R"(
cbuffer OptiHdrMenu : register(b1) { float HdrMenu; };
struct PS_INPUT { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
SamplerState sampler0 : register(s0);
Texture2D texture0 : register(t0);
float Linear(float x) { return x <= .04045 ? x / 12.92 : pow((x+.055)/1.055,2.4); }
float4 main(PS_INPUT input) : SV_Target {
    float4 c = input.col * texture0.Sample(sampler0, input.uv);
    if (HdrMenu > 0) {
        float3 rgb = float3(Linear(saturate(c.r)),Linear(saturate(c.g)),Linear(saturate(c.b)));
        float3 nits = 203.0 * float3(dot(rgb,float3(.627404,.329282,.043314)),
            dot(rgb,float3(.069097,.919540,.011362)),dot(rgb,float3(.016391,.088013,.895595)));
        float3 p = pow(max(nits,0)/10000.0,2610.0/16384.0);
        c.rgb = pow((3424.0/4096.0+(2413.0/128.0)*p)/(1+(2392.0/128.0)*p),2523.0/32.0);
    }
    return c;
})";
}
