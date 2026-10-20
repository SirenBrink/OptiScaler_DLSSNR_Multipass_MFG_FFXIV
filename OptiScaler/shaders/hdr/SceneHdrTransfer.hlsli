#ifndef OPTISCALER_SCENE_HDR_TRANSFER
#define OPTISCALER_SCENE_HDR_TRANSFER
// Native and its SDR reference must be decoded with the SAME transfer curve.
// Form the relative SDR edit first: matching samples then have an exact unit
// ratio, rather than a brightness-dependent gain from mixing sRGB and gamma 2.2.
// A reference-black channel cannot describe a recoverable gain. Keep the native
// channel there instead of magnifying small SDR differences by an epsilon floor.
float3 TransferSceneHdr(float3 nativeLinear, float3 referenceLinear, float3 sceneLinear, float coverage) {
    float3 supported=step(1e-6,referenceLinear);
    float3 relative=nativeLinear/max(referenceLinear,1e-6);
    float3 restored=relative*max(sceneLinear,referenceLinear);
    return lerp(nativeLinear,restored,coverage*supported);
}
#endif
