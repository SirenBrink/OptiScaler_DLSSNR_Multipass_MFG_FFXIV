// Headless regression for independent depth/MV metadata; no NVIDIA runtime required.
#include "../OptiScaler/shaders/dlssnr/DlssNr_Guides.h"
#include <cassert>
#include <climits>
#include <cstdio>

int main()
{
    using namespace DlssNr;
    // Quality input and output-resolution vectors, also when NR executes pre-SR.
    auto g = ResolveGuideRegions({2560, 1440}, {3840, 2160}, {2560, 1440}, {3840, 2160},
                                 false, 0, 0, 0, 0);
    assert(g.depth.width == 2560 && g.depth.height == 1440);
    assert(g.motion.width == 3840 && g.motion.height == 2160);
    // MVLowRes selects the active input size, not the padded guide allocations.
    g = ResolveGuideRegions({3840, 2160}, {4096, 2304}, {1920, 1080}, {3840, 2160},
                            true, 16, 8, 64, 32);
    assert(g.depth.x == 16 && g.depth.y == 8 && g.depth.width == 1920 && g.depth.height == 1080);
    assert(g.motion.x == 64 && g.motion.y == 32 && g.motion.width == 1920 && g.motion.height == 1080);
    // Clipping depth must not shrink motion vectors, and each axis clips independently.
    g = ResolveGuideRegions({1920, 1080}, {2048, 1200}, {1920, 1080}, {3840, 2160},
                            true, 16, 8, 64, 32);
    assert(g.depth.width == 1904 && g.depth.height == 1072);
    assert(g.motion.width == 1920 && g.motion.height == 1080);
    // FFXIV's bridge synthesizes a compact 1920x910 Color from a complete 3840x1820 frame. The
    // rewritten render subrect belongs to Color only; both untouched guides must retain the complete
    // source-frame extent, even when the game's feature labels motion vectors low resolution.
    const auto synthetic = GuideRenderExtent({1920, 910}, {3840, 1820});
    g = ResolveGuideRegions({3840, 1820}, {3840, 1820}, synthetic, {3840, 1820},
                            true, 0, 0, 0, 0);
    assert(g.depth.width == 3840 && g.depth.height == 1820);
    assert(g.motion.width == 3840 && g.motion.height == 1820);
    const auto ordinary = GuideRenderExtent({1920, 910}, {0, 0});
    assert(ordinary.width == 1920 && ordinary.height == 910);
    // Missing dimensions use each resource's available region; no unsigned underflow.
    g = ResolveGuideRegions({1920, 1080}, {3840, 2160}, {0, 0}, {0, 0}, false, 0, 0, 8, 4);
    assert(g.motion.width == 3832 && g.motion.height == 2156);
    g = ResolveGuideRegions({1920, 1080}, {3840, 2160}, {1920, 1080}, {3840, 2160},
                            false, UINT_MAX, 0, 0, UINT_MAX);
    assert(!g.depth.valid() && !g.motion.valid());
    std::puts("PASS: NR guide regions (pre/post SR, MVLowRes, offsets, clipping, missing/invalid metadata)");
}
