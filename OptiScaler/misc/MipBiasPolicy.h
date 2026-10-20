#pragma once
#include <algorithm>
#include <cmath>
#include <optional>
namespace MipBiasPolicy {
inline float Recommended(unsigned render,unsigned output,float nativeBias=0,float epsilon=1) {
    if (!render || !output || !std::isfinite(nativeBias) || !std::isfinite(epsilon)) return 0;
    // DLSS guide 3.5. epsilon=0 preserves detail; 1 is the documented less-aggressive bound.
    return std::clamp(nativeBias+std::log2(float(render)/float(output))-1.0f+epsilon,-16.0f,15.99f);
}
inline float Apply(float original,float minLod,float maxLod,unsigned filter,std::optional<float> value,
                   bool fixed,bool scale,bool all) {
    if (!value || !std::isfinite(*value) || !std::isfinite(original)) return original;
    // Default scope: existing negative, mipmapped ordinary colour samplers.
    // Comparison/min/max filters and mipless UI must not acquire a world-texture bias.
    if (!all && (!(minLod<maxLod) || original>=0 || (filter&0x180u)!=0)) return original;
    const float changed = fixed ? *value : scale ? original * *value : original + *value;
    return std::isfinite(changed)?std::clamp(changed,-16.0f,15.99f):original;
}
}
