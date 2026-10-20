#pragma once
#include <cmath>
#include <string>
#include "TemporalSampling.h"
#include <nvsdk_ngx.h>
namespace FfxivJitter {
inline unsigned PatternPhases(unsigned native,unsigned w,unsigned h,unsigned outW,unsigned outH) {
    if(native!=0xffffffffu || !w || !h || !outW || !outH ||
       w>16384 || h>16384 || outW>16384 || outH>16384 || w>outW || h>outH) return native;
    return TemporalSampling::PhaseCount(w,h,outW,outH);
}
struct Sample {
    float rawX=0,rawY=0,ndcX=0,ndcY=0;
    unsigned width=0,height=0;
    unsigned long long time=0;
};
// The projection carries NDC offsets; NGX requires offsets in render pixels.
// Never clamp or synthesize a different sequence from the rasterized frame.
inline bool Pixels(const Sample& s,float& x,float& y) {
    if(!s.width || !s.height || s.width>16384 || s.height>16384 ||
       !std::isfinite(s.rawX) || !std::isfinite(s.rawY) || !std::isfinite(s.ndcX) || !std::isfinite(s.ndcY) ||
       std::abs(s.rawX)>4 || std::abs(s.rawY)>4) return false;
    x=s.ndcX*s.width*.5f;y=-s.ndcY*s.height*.5f;
    if(s.rawX==0 && s.rawY==0) x=y=0; // native jitter-disabled path leaves old NDC storage intact
    // Both axes must describe the same native jitter strength. Reject torn,
    // stale or unrelated camera storage rather than inventing an offset.
    if(std::abs(x*s.rawY-y*s.rawX)>1e-5f || x*s.rawX+y*s.rawY < -1e-6f) return false;
    return std::isfinite(x) && std::isfinite(y) && std::abs(x)<=2 && std::abs(y)<=2 &&
        (std::abs(s.rawX)>1e-7f || std::abs(x)<1e-5f) &&
        (std::abs(s.rawY)>1e-7f || std::abs(y)<1e-5f);
}
// Undo only the native projection's amplitude multiplier. The original engine
// Halton generator still owns the frame counter, sequence and jitter gates.
inline bool NormalizeHalton(float& x,float& y,float strength) {
    if(!std::isfinite(x) || !std::isfinite(y) || std::abs(x)>.501f || std::abs(y)>.501f ||
       !std::isfinite(strength) || strength<.125f || strength>4) return false;
    if(std::abs(strength-1)<1e-6f) return false;
    x/=strength;y/=strength;return true;
}
void Install();
void Detach();
class Binding {
    NVSDK_NGX_Parameter* params=nullptr;
    float x=0,y=0;
    bool changed=false;
public:
    explicit Binding(NVSDK_NGX_Parameter* p);
    ~Binding();
    Binding(const Binding&)=delete;
    Binding& operator=(const Binding&)=delete;
};
}
