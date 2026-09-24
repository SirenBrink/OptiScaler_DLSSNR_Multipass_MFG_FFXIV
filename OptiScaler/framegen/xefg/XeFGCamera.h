#pragma once
#include <cmath>

namespace XeFGCamera
{
// Row-major world-to-view. Unknown pose uses a nonsingular stationary view;
// motion vectors still carry the game's screen-space camera motion.
inline bool BuildView(float* out, const float* position, const float* right,
                      const float* up, const float* forward)
{
    for (int i=0;i<16;++i) out[i] = i % 5 == 0 ? 1.0f : 0.0f;
    for (int i=0;i<3;++i)
        if (!std::isfinite(position[i]) || !std::isfinite(right[i]) ||
            !std::isfinite(up[i]) || !std::isfinite(forward[i])) return false;
    const float determinant = right[0]*(up[1]*forward[2]-up[2]*forward[1]) -
        right[1]*(up[0]*forward[2]-up[2]*forward[0]) +
        right[2]*(up[0]*forward[1]-up[1]*forward[0]);
    if (!std::isfinite(determinant) || std::abs(determinant)<1e-6f) return false;
    for (int i=0;i<3;++i) { out[i*4]=right[i]; out[i*4+1]=up[i]; out[i*4+2]=forward[i]; }
    out[12]=-(position[0]*right[0]+position[1]*right[1]+position[2]*right[2]);
    out[13]=-(position[0]*up[0]+position[1]*up[1]+position[2]*up[2]);
    out[14]=-(position[0]*forward[0]+position[1]*forward[1]+position[2]*forward[2]);
    return true;
}
}
