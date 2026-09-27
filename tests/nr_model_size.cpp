#include "../OptiScaler/dlssnr/NrModelSize.h"
#include <cassert>
#include <initializer_list>
#include <cstdio>
int main() {
    using DlssNr::AlignWorkSize;
    for (unsigned native : {720u, 1080u, 1440u, 2160u, 1081u, 3u}) {
        assert(AlignWorkSize(native, native) == native);
        for (unsigned scaled = 1; scaled <= native * 2; ++scaled) {
            const auto actual = AlignWorkSize(scaled, native);
            assert(actual > 0);
            assert(actual == native || actual % 16 == 0);
            if (scaled < native) assert(actual <= native);
        }
    }
    assert(AlignWorkSize(723, 1080) == 720);
    assert(AlignWorkSize(1447, 2160) == 1440);
    std::puts("PASS: native sizes unchanged; resampled sizes aligned without overshooting native on reduction");
}
