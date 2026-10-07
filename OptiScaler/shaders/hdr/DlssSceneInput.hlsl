Texture2D<float4> Native : register(t0);
Texture2D<float4> Scene : register(t1);
Texture2D<float4> Reference : register(t2);
Texture2D<float4> Edited : register(t3);
RWTexture2D<float4> Target : register(u0);
cbuffer Settings : register(b0) { uint Width; uint Height; uint Mode; uint Padding; float4 Rect; };
float3 Linear(float3 x) { return float3(x.r<=.04045?x.r/12.92:pow((x.r+.055)/1.055,2.4),x.g<=.04045?x.g/12.92:pow((x.g+.055)/1.055,2.4),x.b<=.04045?x.b/12.92:pow((x.b+.055)/1.055,2.4)); }
float3 Encode(float3 x) { x=max(x,0);return float3(x.r<=.0031308?12.92*x.r:1.055*pow(x.r,1/2.4)-.055,x.g<=.0031308?12.92*x.g:1.055*pow(x.g,1/2.4)-.055,x.b<=.0031308?12.92*x.b:1.055*pow(x.b,1/2.4)-.055); }
float3 Guide(Texture2D<float4> tex,float2 uv) {
 uint w,h;tex.GetDimensions(w,h);float2 p=(Rect.xy+uv*Rect.zw)*float2(w,h)-.5;int2 q=int2(floor(p));float2 f=frac(p);
 int2 lo=int2(round(Rect.xy*float2(w,h))),hi=int2(round((Rect.xy+Rect.zw)*float2(w,h)))-1;
 return lerp(lerp(tex.Load(int3(clamp(q,lo,hi),0)).rgb,tex.Load(int3(clamp(q+int2(1,0),lo,hi),0)).rgb,f.x),lerp(tex.Load(int3(clamp(q+int2(0,1),lo,hi),0)).rgb,tex.Load(int3(clamp(q+1,lo,hi),0)).rgb,f.x),f.y);
}
[numthreads(8,8,1)] void CSMain(uint3 id:SV_DispatchThreadID) {
 if(id.x>=Width || id.y>=Height)return;
 // Reuse the reconstructed output in place after its SDR reference is ready.
 if(Mode==2){float4 value=Target[id.xy];Target[id.xy]=float4(pow(clamp(value.rgb,0,60000),1/2.2),value.a);return;}
 float4 s=Native.Load(int3(id.xy,0));
 if(Mode==0){
  float3 base=Linear(saturate(s.rgb));
  if(Padding){float2 uv=(float2(id.xy)+.5)/float2(Width,Height);
   float3 ref=Guide(Reference,uv),hdr=pow(max(Guide(Scene,uv),0),2.2);
   float3 delta=abs(s.rgb-saturate(ref));float coverage=1-smoothstep(.08,.25,max(delta.r,max(delta.g,delta.b)));
   base*=lerp(1,max(hdr/max(pow(saturate(ref),2.2),1e-4),1),coverage);
  }
  Target[id.xy]=float4(min(base,60000),s.a);
 }else if(Mode==1){
  // Restore the native SDR tone curve for SDR effects. The independent HDR
  // output keeps DLSS's reconstructed highlights, not this spatial inverse.
  float3 restored=max(s.rgb,0);
  if(Padding){float2 uv=(float2(id.xy)+.5)/float2(Width,Height);
   float3 ref=Guide(Reference,uv),hdr=pow(max(Guide(Scene,uv),0),2.2);
   restored/=max(hdr/max(pow(saturate(ref),2.2),1e-4),1);
  }
  Target[id.xy]=float4(saturate(Encode(restored)),s.a);
 }

}
