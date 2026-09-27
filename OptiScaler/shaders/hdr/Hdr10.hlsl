Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Target : register(u0);
cbuffer Settings : register(b0) { uint Width; uint Height; float PaperWhite; float Peak; float Expansion; float Contrast; float Saturation; float Vibrance; };
float3 Linear(float3 x) { return float3(x.x <= .04045 ? x.x/12.92 : pow((x.x+.055)/1.055,2.4), x.y <= .04045 ? x.y/12.92 : pow((x.y+.055)/1.055,2.4), x.z <= .04045 ? x.z/12.92 : pow((x.z+.055)/1.055,2.4)); }
float3 PQ(float3 nits) {
    float3 p = pow(max(nits,0)/10000.0, 2610.0/16384.0);
    return pow((3424.0/4096.0+(2413.0/128.0)*p)/(1+(2392.0/128.0)*p),2523.0/32.0);
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
