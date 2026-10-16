#pragma once
namespace FfxivPreviewFlow
{
inline constexpr char FlowShaderCode[] = R"(
cbuffer Params : register(b0) { uint Width; uint Height; uint FlowWidth; uint FlowHeight; };
Texture2D<float4> Current : register(t0);
Texture2D<float4> Previous : register(t1);
Texture2D<float4> Guide : register(t4);
SamplerState Linear : register(s0);
RWTexture2D<float4> Flow : register(u0);

float3 CurrentAt(float2 p) { return Current.SampleLevel(Linear, (p + .5) / float2(Width, Height), 0).rgb; }
float Error(float2 p, float2 motion)
{
    static const float2 offsets[5] = {float2(0,0),float2(-4,0),float2(4,0),float2(0,-4),float2(0,4)};
    float error = 0;
    [unroll] for (uint i = 0; i < 5; ++i)
    {
        float2 q = p + offsets[i];
        float4 old = Previous.SampleLevel(Linear, (q + motion + .5) / float2(Width, Height), 0);
        // Never match against pixels outside the captured HUD layer.
        if (old.a < .75) return 1000;
        error += dot(abs(old.rgb - CurrentAt(q)), float3(1.0/3.0,1.0/3.0,1.0/3.0));
    }
    return error / 5.0;
}

[numthreads(8,8,1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= FlowWidth || id.y >= FlowHeight) return;
    int2 p = min(int2(id.xy * 4 + 2), int2(Width, Height) - 1);
    float4 guide = Guide.Load(int3(p,0));
    if (guide.a <= .75) { Flow[id.xy] = 0; return; }
    float2 native = (guide.rg * 255.0 - 128.0) * (128.0 / 255.0);
    float initial = Error(p, native);
    float2 best = native;
    float bestError = initial;
    // Flat backgrounds and unchanged patches keep native motion. This also
    // avoids searching the large empty portion of character/portrait panels.
    float3 centre = CurrentAt(p);
    float gradient = max(max(length(CurrentAt(p+float2(4,0))-centre),
                             length(CurrentAt(p-float2(4,0))-centre)),
                         max(length(CurrentAt(p+float2(0,4))-centre),
                             length(CurrentAt(p-float2(0,4))-centre)));
    if (initial > .003 && initial < 1000 && gradient > .003)
    {
        [loop] for (int y = -12; y <= 12; y += 4)
        [loop] for (int x = -12; x <= 12; x += 4)
        {
            float2 candidate = native + float2(x,y);
            float error = Error(p,candidate);
            if (error < bestError) { bestError = error; best = candidate; }
        }
        float2 coarse = best;
        [loop] for (int fy = -2; fy <= 2; ++fy)
        [loop] for (int fx = -2; fx <= 2; ++fx)
        {
            float2 candidate = coarse + float2(fx,fy);
            float error = Error(p,candidate);
            if (error < bestError) { bestError = error; best = candidate; }
        }
    }
    bool improved = bestError < initial * .8 && initial > .003;
    Flow[id.xy] = float4(improved ? best : native, improved ? 1.0 : 0.0, 1.0);
}
)";
}
