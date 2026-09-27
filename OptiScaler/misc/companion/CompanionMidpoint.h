#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace FfxivCompanion::Midpoint
{
// Never waits for a future sample. One midpoint may replace the first presentation
// of an already-arrived sample; its endpoint must be shown before another midpoint.
struct Gate
{
    uint64_t seen = 0, endpoint = 0;
    bool Offer(uint64_t current, uint64_t previous, double sourceInterval, double layerInterval,
               double age, bool enabled)
    {
        const bool first = current != seen;
        seen = current;
        return enabled && first && previous && current > previous && current == previous + 1 && endpoint == previous &&
            std::isfinite(sourceInterval) && std::isfinite(layerInterval) && std::isfinite(age) &&
            sourceInterval >= 0.004 && sourceInterval <= 0.050 &&
            layerInterval >= 0.001 && layerInterval <= 0.008 &&
            sourceInterval >= 2.0 * layerInterval && age >= 0 && age <= sourceInterval * 0.5;
    }
    void Presented(uint64_t sequence, bool midpoint) { if (!midpoint) endpoint = sequence; }
    void Reset() { seen = endpoint = 0; }
};

// POSITION.xy only, verified float layouts. Every other byte must match, including
// depth, colour/alpha, UVs and effects. Each quad must translate rigidly. This avoids
// stretching glyphs, cross-fading text, or smoothing a visibility/material change.
inline bool Positions(std::span<const unsigned char> previous, std::span<const unsigned char> current,
                      uint32_t stride, uint32_t offset, std::vector<unsigned char>& output)
{
    if (!stride || stride > 128 || offset > stride || stride - offset < 8 || current.empty() ||
        current.size() != previous.size() || current.size() % stride ||
        (current.size() / stride) % 4 || current.size() > 8 * 1024 * 1024) return false;
    double quadX = 0, quadY = 0;
    for (size_t vertex = 0, n = 0; vertex < current.size(); vertex += stride, ++n)
    {
        auto a = previous.data() + vertex; auto b = current.data() + vertex;
        if (memcmp(a,b,offset) || memcmp(a+offset+8,b+offset+8,stride-offset-8)) return false;
        float p[2],q[2]; memcpy(p,a+offset,8); memcpy(q,b+offset,8);
        for (int axis=0;axis<2;++axis)
            if (!std::isfinite(p[axis]) || !std::isfinite(q[axis]) ||
                std::abs(p[axis]) > 65536 || std::abs(q[axis]) > 65536) return false;
        const double dx=double(q[0])-p[0], dy=double(q[1])-p[1];
        if (dx*dx+dy*dy > 24.0*24.0) return false;
        if (n%4==0) { quadX=dx; quadY=dy; }
        else if (std::abs(dx-quadX)>0.01 || std::abs(dy-quadY)>0.01) return false;
    }
    output.resize(current.size()); memcpy(output.data(),current.data(),current.size());
    for (size_t vertex=0;vertex<current.size();vertex+=stride)
    {
        float p[2],q[2]; memcpy(p,previous.data()+vertex+offset,8); memcpy(q,current.data()+vertex+offset,8);
        q[0]=float((double(p[0])+q[0])*0.5); q[1]=float((double(p[1])+q[1])*0.5);
        memcpy(output.data()+vertex+offset,q,8);
    }
    return true;
}
}
