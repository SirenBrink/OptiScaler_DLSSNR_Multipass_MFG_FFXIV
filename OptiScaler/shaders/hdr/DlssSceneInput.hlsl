Texture2D<float4> Native : register(t0);
#include "HdrCompatibility.hlsli"
#include "SceneHdrTransfer.hlsli"
Texture2D<float4> Scene : register(t1);
Texture2D<float4> Reference : register(t2);
Texture2D<float4> Edited : register(t3);
RWTexture2D<float4> Target : register(u0);
cbuffer Settings : register(b0) {
 uint Width; uint Height; uint Mode; uint Padding; float4 Rect;
 uint2 RenderSize; float2 MotionScale; float2 RasterJitter; uint HasDepth; uint Reserved;
};
float3 Linear(float3 x) { return float3(x.r<=.04045?x.r/12.92:pow((x.r+.055)/1.055,2.4),x.g<=.04045?x.g/12.92:pow((x.g+.055)/1.055,2.4),x.b<=.04045?x.b/12.92:pow((x.b+.055)/1.055,2.4)); }
float3 Encode(float3 x) { x=max(x,0);return float3(x.r<=.0031308?12.92*x.r:1.055*pow(x.r,1/2.4)-.055,x.g<=.0031308?12.92*x.g:1.055*pow(x.g,1/2.4)-.055,x.b<=.0031308?12.92*x.b:1.055*pow(x.b,1/2.4)-.055); }
float3 HistoryHalf(float3 value) {
 // Typed FP16 stores may truncate toward zero. Recursive history then loses
 // energy on every blend/encode, even for a perfectly static bright feature.
 // Explicitly round to nearest-even in FP32 before the exact FP16 store.
 uint3 bits=asuint(value),magnitude=bits&0x7fffffffu;
 uint3 rounded=(magnitude+0xfffu+((magnitude>>13)&1u))&0xffffe000u;
 float3 normal=asfloat(rounded|(bits&0x80000000u));
 return float3(magnitude.x<0x38800000u?round(value.x*16777216)/16777216:normal.x,
               magnitude.y<0x38800000u?round(value.y*16777216)/16777216:normal.y,
               magnitude.z<0x38800000u?round(value.z*16777216)/16777216:normal.z);
}
float3 Guide(Texture2D<float4> tex,float2 uv) {
 uint w,h;tex.GetDimensions(w,h);float2 p=(Rect.xy+uv*Rect.zw)*float2(w,h)-.5;int2 q=int2(floor(p));float2 f=frac(p);
 int2 lo=int2(round(Rect.xy*float2(w,h))),hi=int2(round((Rect.xy+Rect.zw)*float2(w,h)))-1;
 return lerp(lerp(tex.Load(int3(clamp(q,lo,hi),0)).rgb,tex.Load(int3(clamp(q+int2(1,0),lo,hi),0)).rgb,f.x),lerp(tex.Load(int3(clamp(q+int2(0,1),lo,hi),0)).rgb,tex.Load(int3(clamp(q+1,lo,hi),0)).rgb,f.x),f.y);
}
int2 GuidePixel(uint2 pixel) {
 return clamp(int2((float2(pixel)+.5)*float2(RenderSize)/float2(Width,Height)+RasterJitter),0,int2(RenderSize)-1);
}
float4 History(float2 uv) {
 float2 pos=uv*float2(Width,Height)-.5;int2 q=int2(floor(pos));float2 f=frac(pos);
 float4 a=Scene.Load(int3(clamp(q,0,int2(Width,Height)-1),0));
 float4 b=Scene.Load(int3(clamp(q+int2(1,0),0,int2(Width,Height)-1),0));
 float4 c=Scene.Load(int3(clamp(q+int2(0,1),0,int2(Width,Height)-1),0));
 float4 d=Scene.Load(int3(clamp(q+1,0,int2(Width,Height)-1),0));
 a.rgb=pow(max(a.rgb,0),2.2);b.rgb=pow(max(b.rgb,0),2.2);
 c.rgb=pow(max(c.rgb,0),2.2);d.rgb=pow(max(d.rgb,0),2.2);
 return lerp(lerp(a,b,f.x),lerp(c,d,f.x),f.y);
}
[numthreads(8,8,1)] void CSMain(uint3 id:SV_DispatchThreadID) {
 if(id.x>=Width || id.y>=Height)return;
 if(Mode==4){
  float4 current=Native.Load(int3(id.xy,0));int2 guide=GuidePixel(id.xy);
  float2 motion=Edited.Load(int3(guide,0)).xy*MotionScale;
  float2 uv=(float2(id.xy)+.5+motion)/float2(Width,Height);
  float weight=.8*(1-smoothstep(.25,2,length(motion)));
  if(weight<=0 || any(uv<=0) || any(uv>=1) || !all(isfinite(motion))){Target[id.xy]=current;return;}
  float depth=Reference.Load(int3(guide,0)).r;float4 previous=History(uv);
  if(!all(isfinite(previous)) || !isfinite(depth) || abs(previous.a-depth)>max(1e-5,.02*max(abs(previous.a),abs(depth)))){Target[id.xy]=current;return;}
  float3 lo=current.rgb,hi=current.rgb,sum=0,square=0;
  [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x){
   float3 value=Native.Load(int3(clamp(int2(id.xy)+int2(x,y),0,int2(Width,Height)-1),0)).rgb;
   float3 delta=value-current.rgb;
   lo=min(lo,value);hi=max(hi,value);sum+=delta;square+=delta*delta;
  }
  float3 averageDelta=sum/9,mean=current.rgb+averageDelta,sigma=sqrt(max(square/9-averageDelta*averageDelta,0));
  // The current pixel must remain inside the clipping box. Otherwise a stable
  // thin highlight is repeatedly clipped toward its darker neighbours.
  lo=min(current.rgb,max(lo,mean-1.5*sigma));hi=max(current.rgb,min(hi,mean+1.5*sigma));
  float relative=max(abs(previous.r-current.r),max(abs(previous.g-current.g),abs(previous.b-current.b)))/max(.1,max(current.r,max(current.g,current.b)));
  weight*=1-saturate((relative-.05)/.1);
  Target[id.xy]=float4(HistoryHalf(lerp(current.rgb,clamp(previous.rgb,lo,hi),weight)),current.a);return;
 }
 // Reuse the reconstructed output in place after its SDR reference is ready.
 if(Mode==2){float4 value=Target[id.xy];float depth=HasDepth?Reference.Load(int3(GuidePixel(id.xy),0)).r:value.a;float3 encoded=pow(clamp(value.rgb,0,60000),1/2.2);Target[id.xy]=float4(HasDepth?HistoryHalf(encoded):encoded,depth);return;}
 float4 s=Native.Load(int3(id.xy,0));
 if(Mode==3){
  // PreSR may edit an older clean frame. Carry that edit into its matching
  // HDR reconstruction before replacing the SDR reference used to exclude UI.
  float3 oldSDR=Reference.Load(int3(id.xy,0)).rgb;
  float4 encoded=Target[id.xy];
  if(any(s.rgb!=oldSDR)){
   float3 hdrLinear=pow(max(encoded.rgb,0),2.2);
   float oldPeak=max(hdrLinear.r,max(hdrLinear.g,hdrLinear.b));
   // Undo the same compatibility shoulder used by SR. Multiplying an SDR
   // delta by a luminance gain is not its inverse and changed PreSR brightness.
   float3 answer=Padding?HdrCompatExpand(s.rgb,max(oldPeak*4,4)):Linear(saturate(s.rgb));
   float peak=max(answer.r,max(answer.g,answer.b));
   if(peak>60000)answer*=60000/peak;
   encoded.rgb=pow(answer,1/2.2);
  }
  Target[id.xy]=encoded;return;
 }
 if(Mode==0){
  float3 base=Linear(saturate(s.rgb));
  if(Padding){float2 uv=(float2(id.xy)+.5)/float2(Width,Height);
   float3 ref=Guide(Reference,uv),hdr=pow(max(Guide(Scene,uv),0),2.2);
   float3 delta=abs(s.rgb-saturate(ref));float coverage=1-smoothstep(.08,.25,max(delta.r,max(delta.g,delta.b)));
   base=TransferSceneHdr(base,Linear(saturate(ref)),hdr,coverage);
  }
  Target[id.xy]=float4(min(base,60000),s.a);
 }else if(Mode==1){
  // Derive the compatibility image from the reconstructed pixel itself.
  // Never divide by an unfiltered scene guide: that reintroduced aliased
  // geometry and jitter even though DLSS had already reconstructed HDR.
  // This is the opaque main scene, not a UI surface. NGX does not provide a
  // compositing-alpha contract for its private SR output.
  Target[id.xy]=float4(Padding?HdrCompatCompress(s.rgb):saturate(Encode(s.rgb)),1);
 }

}
