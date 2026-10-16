#include "../OptiScaler/menu/OverlaySubmissionRing.h"
#include <cassert>
#include <cstdio>
int main()
{
    OverlaySubmissionRing<32> ring;
    // Four complete 8X bursts can be queued while the GPU is behind, without
    // reusing any of ImGui's outstanding uploads or command allocators.
    for(unsigned i=0;i<32;++i){
        assert(ring.Index()==i && ring.Ready(0));
        ring.DrawRecorded();ring.Submitted(i,i+1);
    }
    assert(ring.Index()==0 && !ring.Ready(0));
    // A busy skip leaves the upload ring aligned. Only GPU completion makes
    // the next slot reusable; no CPU waiting or guessing from swapchain index.
    assert(ring.Index()==0 && !ring.Ready(0));
    assert(ring.Ready(1));ring.DrawRecorded();ring.Submitted(0,33);
    assert(!ring.Ready(1) && ring.Ready(2));
    assert(!ring.Ready(UINT64_MAX));
    ring.Submitted(ring.Index(),UINT64_MAX);assert(!ring.Ready(100));
    ring.Reset();assert(ring.Index()==0 && ring.Ready(0));
    // A recorded draw whose list failed to close still advanced ImGui's ring,
    // but owns no GPU work. The next frame must follow that backend index.
    ring.DrawRecorded();assert(ring.Index()==1 && ring.Ready(0));
    puts("PASS: 8X burst uploads, nonblocking busy skips, fence retirement, failed submission and reset");
}
