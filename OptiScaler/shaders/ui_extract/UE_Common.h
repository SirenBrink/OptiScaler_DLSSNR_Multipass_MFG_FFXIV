#pragma once

#include "SysUtils.h"

// Builds a "UI colour + alpha" image for frame generation from two copies of the same frame:
// the final image (with UI) and the HUD-less image (same frame, captured right before the UI).
// Pixels that differ, plus a small dilation to catch anti-aliased edges, are UI: they get the
// final colour with alpha 1. Everything else is transparent black. Alpha is strictly 0 or 1,
// so the result is the same for premultiplied and straight-alpha consumers.

struct alignas(256) UEConstants
{
    float Threshold;
    uint32_t Radius;
    uint32_t Width;
    uint32_t Height;
};

inline static std::string ueShaderCode = R"(
cbuffer Params : register(b0)
{
    float Threshold;
    uint Radius;
    uint Width;
    uint Height;
};

Texture2D<float4> FinalTexture : register(t0);
Texture2D<float4> HudlessTexture : register(t1);
RWTexture2D<float4> UiTexture : register(u0);

float PixelDifference(int2 p)
{
    p = clamp(p, int2(0, 0), int2((int) Width - 1, (int) Height - 1));
    float3 d = abs(FinalTexture.Load(int3(p, 0)).rgb - HudlessTexture.Load(int3(p, 0)).rgb);
    return max(max(d.r, d.g), d.b);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;

    int2 p = int2(id.xy);
    int r = (int) Radius;
    bool isUi = false;

    [loop]
    for (int y = -r; y <= r; ++y)
    {
        [loop]
        for (int x = -r; x <= r; ++x)
        {
            if (PixelDifference(p + int2(x, y)) > Threshold)
                isUi = true;
        }
    }

    float3 finalColor = FinalTexture.Load(int3(p, 0)).rgb;
    UiTexture[id.xy] = isUi ? float4(finalColor, 1.0) : float4(0.0, 0.0, 0.0, 0.0);
}
)";
