// Pointwise SDR compatibility curve shared by main SR and the PreSR carrier.
// Native/ReShade textures remain SDR; the private reconstruction stays light HDR.
float3 HdrCompatLinear(float3 x) {
    x=saturate(x);
    return float3(x.r<=.04045?x.r/12.92:pow((x.r+.055)/1.055,2.4),
                  x.g<=.04045?x.g/12.92:pow((x.g+.055)/1.055,2.4),
                  x.b<=.04045?x.b/12.92:pow((x.b+.055)/1.055,2.4));
}
float3 HdrCompatEncode(float3 x) {
    x=max(x,0);
    return float3(x.r<=.0031308?12.92*x.r:1.055*pow(x.r,1/2.4)-.055,
                  x.g<=.0031308?12.92*x.g:1.055*pow(x.g,1/2.4)-.055,
                  x.b<=.0031308?12.92*x.b:1.055*pow(x.b,1/2.4)-.055);
}
float3 HdrCompatCompress(float3 value) {
    value=max(value,0);float peak=max(value.r,max(value.g,value.b));
    if(peak>.75){float excess=peak-.75;value*=(.75+.25*excess/(.25+excess))/peak;}
    return saturate(HdrCompatEncode(value));
}
float3 HdrCompatExpand(float3 value,float maximumPeak) {
    float3 light=HdrCompatLinear(value);float peak=max(light.r,max(light.g,light.b));
    if(peak>.75) {
        float excess=peak-.75;
        float expanded=.75+.25*excess/max(1-peak,1e-6);
        light*=min(expanded,maximumPeak)/peak;
    }
    return light;
}
