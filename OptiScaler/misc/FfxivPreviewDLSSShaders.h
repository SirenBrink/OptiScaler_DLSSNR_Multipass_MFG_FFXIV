#pragma once
namespace FfxivPreviewDLSSShaders {
inline constexpr char code[]=R"(
Texture2D<float4> Source:register(t0);Texture2D<float4> Motion:register(t1);Texture2D<float> Depth:register(t2);Texture2D<float4> Original:register(t3);
Texture2D<float4> NrCorrection:register(t4);
Texture2D<float4> NrReference:register(t5);
SamplerState Linear:register(s0);
cbuffer Settings:register(b0){float2 Jitter;float2 InvSize;};
cbuffer NativeCamera:register(b1){float4 Camera[59];};
struct V{float4 pos:SV_Position;float2 uv:TEXCOORD;};
V VS(uint id:SV_VertexID){V o;float2 uv=float2((id<<1)&2,id&2);o.uv=uv;o.pos=float4(uv*float2(2,-2)+float2(-1,1),0,1);return o;}
float4 Copy(V v):SV_Target{return Source.SampleLevel(Linear,v.uv,0);}
struct N{float4 colour:SV_Target0;float depth:SV_Target1;float2 motion:SV_Target2;};
N NRInputs(V v){
 float2 uv=v.uv+Jitter*InvSize;
 uint w,h;Depth.GetDimensions(w,h);
 int2 p=clamp(int2(uv*float2(w,h)),int2(0,0),int2(w,h)-1);
 N o;o.colour=Source.SampleLevel(Linear,uv,0);o.depth=Depth.Load(int3(p,0));
 o.motion=Motion.SampleLevel(Linear,uv,0).xy;return o;
}
struct G{float depth:SV_Target0;float2 motion:SV_Target1;};
G Guides(V v){int2 p=int2(v.pos.xy);G o;o.depth=Depth.Load(int3(p,0));o.motion=Motion.Load(int3(p,0)).xy-Camera[58].zw;
 if(!all(isfinite(o.motion)))o.motion=0;
 o.motion=clamp(o.motion,-2,2);return o;}
float4 Alpha(V v):SV_Target{float3 c=Source.SampleLevel(Linear,v.uv,0).rgb;float a=Original.SampleLevel(Linear,v.uv+Jitter*InvSize,0).a;return float4(c,a);}
float4 AlphaNR(V v):SV_Target{
 float4 result=Alpha(v);
 // Both NR images use the same unjittered native-resolution grid. Subtract
 // the exact model input, not a separately resampled native texture: a
 // no-op NR result must contribute zero even at subpixel edges.
 float3 delta=NrCorrection.SampleLevel(Linear,v.uv,0).rgb-NrReference.SampleLevel(Linear,v.uv,0).rgb;
 result.rgb+=delta;return result;
}
)";
}
