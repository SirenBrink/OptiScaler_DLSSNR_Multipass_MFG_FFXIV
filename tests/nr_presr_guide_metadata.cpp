// Production PreSR metadata -> production region resolver. The previous code's
// missing low-resolution flag exposed padded motion to NR at non-native sizes.
#include <shaders/dlssnr/DlssNr_Common.h>
#include <shaders/dlssnr/DlssNr_Guides.h>
#include <cstdio>
#include <stdexcept>
constexpr unsigned NVSDK_NGX_DLSS_Feature_Flags_DepthInverted=1,
                   NVSDK_NGX_DLSS_Feature_Flags_MVLowRes=2;
#include "nr_presr_guides_production.inl"
void expect(bool value,const char* reason) { if(!value) throw std::runtime_error(reason); }
DlssNr::GuideRegions Resolve(const DlssNrFrameInfo& f,DlssNr::GuideExtent depth,DlssNr::GuideExtent motion) {
    return DlssNr::ResolveGuideRegions(depth,motion,
        DlssNr::GuideRenderExtent({f.RenderSubrectWidth,f.RenderSubrectHeight},{f.GuideSourceWidth,f.GuideSourceHeight}),
        {f.OutputWidth,f.OutputHeight},f.MotionVectorsLowResolution,0,0,0,0);
}
int main() try {
    constexpr auto low=NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    auto f=BuildProduction(2257,1270,3840,2160,low);
    auto r=Resolve(f,{2257,1270},{3840,2160});
    expect(r.motion.width==2257 && r.motion.height==1270,"PreSR sampled padded 4K motion outside Balanced's active region");
    expect(r.depth.width==2257 && r.depth.height==1270,"PreSR depth extent changed");
    for (const auto dims : {DlssNr::GuideExtent{3840,2160},{2560,1440},{1920,1080}}) {
        f=BuildProduction(dims.width,dims.height,3840,2160,low);
        r=Resolve(f,{3840,2160},{3840,2160});
        expect(r.motion.width==dims.width && r.motion.height==dims.height,"Quality/native MV extent mismatch");
        auto compact=Resolve(f,dims,dims); // normalized alternate-frame fields
        expect(compact.motion.width==dims.width && compact.motion.height==dims.height,"Compact alternate-frame extent changed");
    }
    // High-resolution motion stays output-sized, even in a padded allocation.
    f=BuildProduction(2257,1270,3840,2160,0);
    r=Resolve(f,{2257,1270},{4096,2304});
    expect(r.motion.width==3840 && r.motion.height==2160,"Output-resolution MV extent mismatch");
    // Synthetic compact color retains explicit full-frame guide coverage.
    f=BuildProduction(1920,910,3840,1820,low,3840,1820);
    r=Resolve(f,{3840,1820},{3840,1820});
    expect(r.motion.width==3840 && r.depth.height==1820,"Synthetic bridge guide override lost");
    std::puts("PreSR guide metadata: active padded, compact alternating, DLAA/Quality/Performance, display-resolution and synthetic guides passed");
    return 0;
} catch(const std::exception& e) { std::puts(e.what()); return 1; }
