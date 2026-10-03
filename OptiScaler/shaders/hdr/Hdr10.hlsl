#include "renodx/reinhard.hlsl"
Texture2D<float4> Source : register(t0);
Texture2D<float4> Scene : register(t1);
Texture2D<float4> Reference : register(t2);
RWTexture2D<float4> Target : register(u0);
cbuffer Settings : register(b0) { uint Width; uint Height; float PaperWhite; float Peak; float Expansion; float Contrast; float Saturation; float Vibrance; uint SceneMode; uint ReShadeHighlights; uint2 Padding; float4 SceneRect; };
float3 Linear(float3 x) { return float3(x.x <= .04045 ? x.x/12.92 : pow((x.x+.055)/1.055,2.4), x.y <= .04045 ? x.y/12.92 : pow((x.y+.055)/1.055,2.4), x.z <= .04045 ? x.z/12.92 : pow((x.z+.055)/1.055,2.4)); }
float3 PQ(float3 nits) {
    float3 p = pow(max(nits,0)/10000.0, 2610.0/16384.0);
    return pow((3424.0/4096.0+(2413.0/128.0)*p)/(1+(2392.0/128.0)*p),2523.0/32.0);
}
// Bilinear guide sampling without another sampler binding, including SR scaling.
float3 Guide(Texture2D<float4> tex,float2 uv) {
 uint w,h;tex.GetDimensions(w,h);float2 p=(SceneRect.xy+uv*SceneRect.zw)*float2(w,h)-.5;int2 q=int2(floor(p));float2 f=frac(p);
 int2 lower=int2(round(SceneRect.xy*float2(w,h)));
 int2 limit=int2(round((SceneRect.xy+SceneRect.zw)*float2(w,h)))-1;
 return lerp(lerp(tex.Load(int3(clamp(q,lower,limit),0)).rgb,tex.Load(int3(clamp(q+int2(1,0),lower,limit),0)).rgb,f.x),
             lerp(tex.Load(int3(clamp(q+int2(0,1),lower,limit),0)).rgb,tex.Load(int3(clamp(q+1,lower,limit),0)).rgb,f.x),f.y);
}
[numthreads(8,8,1)] void CSMain(uint3 id:SV_DispatchThreadID) {
    if(id.x>=Width || id.y>=Height) return;
    float4 s=Source.Load(int3(id.xy,0));
    float3 rgb=Linear(saturate(s.rgb));
    float y=dot(rgb,float3(.2126,.7152,.0722));
    // A bounded sigmoid in luminance, pivoted at 18% grey. Unlike a straight
    // power curve this preserves both endpoints without clipping highlights.
    if (y > 0 && y < 1 && Contrast != 1) {
        float odds = (y / (1-y)) / (.18/.82);
        float adjusted = 1 / (1 + (.82/.18) * pow(odds, -Contrast));
        rgb *= adjusted/y;
        y = adjusted;
    }
    // Keep shadows and middle grey stable, but begin expanding above middle
    // grey rather than only above 74% sRGB. The latter left most game lighting
    // entirely inside the SDR range. Smooth onset avoids a visible knee.
    float h=saturate((y-.18)/.82); h=h*h*(3-2*h);
    float gain=PaperWhite+(Peak-PaperWhite)*Expansion*h;
    rgb*=gain;
    if(SceneMode) {
        float2 uv=(float2(id.xy)+.5)/float2(Width,Height);
        float3 sceneEncoded=Guide(Scene,uv), referenceEncoded=Guide(Reference,uv);
        // RenoDX FFXIV keeps extended gamma-encoded values until its final pass.
        // Values >1 come from the exposed scene, never an inverse SDR curve.
        float3 sceneLinear=pow(max(sceneEncoded,0),2.2);
        float3 neutral=pow(saturate(referenceEncoded),2.2);
        float3 scale=sceneLinear/max(neutral,1e-4);
        // Suppress HDR residual on changed overlays; the real SDR/UI draw is kept.
        // Arbitrary spatial ReShade effects require a dedicated HDR-aware path.
        float difference=max(abs(s.rgb-saturate(referenceEncoded)).r,max(abs(s.rgb-saturate(referenceEncoded)).g,abs(s.rgb-saturate(referenceEncoded)).b));
        float coverage=1-smoothstep(.08,.25,difference);

        if(ReShadeHighlights) {
            // Grading may change colour without moving the effect. Restore only
            // measured scene excess where both images still have a highlight.
            float finalPeak=max(s.rgb.r,max(s.rgb.g,s.rgb.b));
            float referencePeak=max(referenceEncoded.r,max(referenceEncoded.g,referenceEncoded.b));
            float highlight=smoothstep(.65,.95,min(finalPeak,saturate(referencePeak)));
            coverage=max(coverage,highlight);
            // One gain preserves the final post-effect colour ratios.
            float scenePeak=max(sceneLinear.r,max(sceneLinear.g,sceneLinear.b));
            float neutralPeak=max(neutral.r,max(neutral.g,neutral.b));
            scale=max(scenePeak/max(neutralPeak,1e-4),1).xxx;
        }
        scale=lerp(1,max(scale,1),coverage);
        float3 extended=rgb*scale;
        float high=max(extended.r,max(extended.g,extended.b));
        // Neutral range is unchanged. Forward-compress only the preserved scene
        // highlight excess; the display peak is an asymptote, not a hard clip.
        if(high>PaperWhite) {
            float mapped=PaperWhite+renodx::tonemap::Reinhard(high-PaperWhite,Peak-PaperWhite);
            extended*=mapped/high;
        }
        rgb=extended;
    }
    float3 bt2020=float3(dot(rgb,float3(.627404,.329282,.043314)), dot(rgb,float3(.069097,.919540,.011362)), dot(rgb,float3(.016391,.088013,.895595)));
    if ((Saturation != 1 || Vibrance != 0) && (rgb.r != rgb.g || rgb.g != rgb.b)) {
        float luma=dot(bt2020,float3(.2627,.6780,.0593));
        float high=max(bt2020.r,max(bt2020.g,bt2020.b));
        float low=min(bt2020.r,min(bt2020.g,bt2020.b));
        float muted=1-saturate((high-low)/max(high,1e-6));
        float amount=Saturation*(1+Vibrance*muted);
        float3 chroma=bt2020-luma;
        // Scale all chroma channels together at the gamut boundary. This avoids
        // independent RGB clipping changing the hue or neutral brightness.
        [unroll] for(int i=0;i<3;i++) {
            if(chroma[i]>1e-6) amount=min(amount,max(Peak-luma,0)/chroma[i]);
            if(chroma[i]<-1e-6) amount=min(amount,-max(luma,0)/chroma[i]);
        }
        bt2020=luma+chroma*max(amount,0);
    }
    Target[id.xy]=float4(PQ(min(bt2020,Peak)),s.a);
}
