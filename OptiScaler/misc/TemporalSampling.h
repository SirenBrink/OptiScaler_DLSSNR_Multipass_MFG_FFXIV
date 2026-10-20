#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace TemporalSampling {
// NVIDIA DLSS Programming Guide 3.7.1.1: 8 * output/input pixel area.
inline unsigned PhaseCount(unsigned w,unsigned h,unsigned outW,unsigned outH) {
    if (!w || !h || !outW || !outH) return 8;
    const double area=(double(outW)/w)*(double(outH)/h);
    return unsigned(std::clamp(std::ceil(8.0*area),8.0,4096.0));
}
inline float Halton(uint64_t index,unsigned base) {
    float factor=1,result=0;
    for(;index;index/=base) {factor/=base;result+=factor*float(index%base);}
    return result-.5f;
}
inline float ResampledJitter(float value,unsigned source,unsigned render) {
    if (!source || !render || !std::isfinite(value)) return value;
    // Match the physical pixel shift after a complete-frame spatial resample.
    // Never clamp/synthesize a new sequence at the NGX boundary.
    return value*(float(render)/float(source));
}
}
