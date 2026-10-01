#pragma once
#include "../include/imgui/imgui.h"
#include <algorithm>

namespace MenuHdrColors {
// OptiHDR owns the final SDR-theme -> HDR conversion. Feeding it colours
// already compressed by the legacy UI mapper dims white and reduces contrast.
inline bool NeedsLegacyToneMap(bool optiHdr, bool hdrOutput, bool nativeHdrInput)
{
    return !optiHdr && (hdrOutput || nativeHdrInput);
}

inline ImVec4 Transform(const ImVec4& colour, bool legacyToneMap)
{
    if (!legacyToneMap)
        return colour;
    const float peak = std::max(colour.x, std::max(colour.y, colour.z));
    if (peak <= 0.0f)
        return colour;
    const float scale = 1.0f / (1.0f + peak);
    return ImVec4(colour.x * scale, colour.y * scale, colour.z * scale, colour.w);
}
}
