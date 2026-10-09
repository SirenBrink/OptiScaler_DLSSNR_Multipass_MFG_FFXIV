// Point-resample NR depth/motion active rectangles. Source vectors retain their units.
// Field offsets match the prefix of DlssNrConstants. Dedicated typed UAVs avoid
// binding scalar depth and RG motion to the colour composition shader's float4 UAVs.
cbuffer Params : register(b0)
{
    uint gMode; float gWhite; uint gWidth, gHeight;
    float gMotionWidth, gMotionHeight; uint gDepthX; float gRatio;
    uint gPass; float gScaleX, gScaleY; uint gDepthWidth, gDepthHeight;
    uint gDepthY; float gSplit, gZoom; uint gMotionX, gMotionY;
};
Texture2D<float4> gDepth : register(t0);
Texture2D<float4> gMotion : register(t1);
RWTexture2D<float> gOutDepth : register(u0);
RWTexture2D<float2> gOutMotion : register(u1);
[numthreads(8,8,1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);
    uint2 ds = max(uint2(gDepthWidth, gDepthHeight), 1);
    uint2 ms = max(uint2(gMotionWidth, gMotionHeight), 1);
    uint2 d = uint2(gDepthX, gDepthY) + min(uint2(uv * ds), ds - 1);
    uint2 m = uint2(gMotionX, gMotionY) + min(uint2(uv * ms), ms - 1);
    gOutDepth[id.xy] = gDepth.Load(int3(d,0)).r;
    gOutMotion[id.xy] = gMotion.Load(int3(m,0)).xy;
}
