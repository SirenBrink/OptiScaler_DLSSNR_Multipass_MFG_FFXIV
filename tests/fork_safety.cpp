#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <vector>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <limits>
#include <cstdio>
#include <stdexcept>
#include "../OptiScaler/misc/FrameLimitTiming.h"
#define LOG_ERROR(...) ((void)0)
#define LOG_INFO(...) ((void)0)
struct Fence {
    UINT64 value=0, afterWait=0;
    HRESULT eventResult=S_OK;
    UINT64 GetCompletedValue() {return value;}
    HRESULT SetEventOnCompletion(UINT64,HANDLE) {return eventResult;}
};
static Fence* waiting=nullptr;
static DWORD injectedWaitResult=WAIT_OBJECT_0;
static DWORD TestWait(HANDLE,DWORD) {if(waiting)waiting->value=waiting->afterWait;return injectedWaitResult;}
#define WaitForSingleObject TestWait
struct Commands {unsigned closed=0;HRESULT result=S_OK;HRESULT Close(){++closed;return result;}};
using ID3D12CommandList = Commands;
struct Queue { unsigned executed=0;UINT64 signaled=0;HRESULT result=S_OK;
    void ExecuteCommandLists(UINT,Commands**){++executed;}
    HRESULT Signal(Fence*,UINT64 value){signaled=value;return result;}
};
class IFGFeature_Dx12 {
public:
    static constexpr unsigned BUFFER_COUNT=4;
    UINT64 _uiAllocatorFenceValues[4]{}, _scAllocatorFenceValues[4]{}, _scFenceValue=0;
    bool _scSubmissionFailed=false;
    Fence* _scFence=nullptr;HANDLE _scFenceEvent=nullptr;
    Queue* _gameCommandQueue=nullptr;
    bool WaitForSCAllocator(UINT);bool SubmitSCCommandList(UINT);
    Fence* _uiFence=nullptr;
    HANDLE _uiFenceEvent=nullptr;
    bool _uiCommandListResetted[4]{},_scCommandListResetted[4]{};
    Commands* _uiCommandList[4]{},*_scCommandList[4]{};
    std::shared_mutex _resourceMutex[4];std::map<int,int> _frameResources[4],_resourceReady[4];
    UINT64 _lastDispatchedFrame=0,_frameCount=9;bool _waitingNewFrameData=false;
    unsigned submitted=0,deactivated=0;
    void Deactivate(){++deactivated;for(bool open:_uiCommandListResetted)if(open)++submitted;}
    void CancelPendingUpscalerWork();
    bool WaitForUIAllocator(UINT);
};
class Dx11wDx12SC {
public:
    bool _copySubmissionFailed=false;
    Fence* _copyFence=nullptr;HANDLE _copyFenceEvent=nullptr;
    std::vector<UINT64> _copyAllocatorFenceValues{0,0};
    UINT64 _lastInteropCopyFenceValue=0;
    bool _WaitForCopyAllocator(UINT);
    bool _WaitForCopyQueueIdle();
};
#include "fork_safety_production.inl"
void check(bool pass,const char* what) {if(!pass)throw std::runtime_error(what);}
int main() try {
    Fence fence;waiting=&fence;
    IFGFeature_Dx12 fg;
    fg._uiFence=&fence;fg._uiFenceEvent=HANDLE(1);fg._uiAllocatorFenceValues[0]=4;
    fg._scFence=&fence;fg._scFenceEvent=HANDLE(1);fg._scAllocatorFenceValues[0]=4;
    check(!fg.WaitForUIAllocator(4),"invalid UI index accepted");
    check(fg.WaitForUIAllocator(1),"unused allocator rejected");
    Dx11wDx12SC bridge;bridge._copyFence=&fence;bridge._copyFenceEvent=HANDLE(1);bridge._copyAllocatorFenceValues[0]=4;
    auto all=[&](bool expected){
        const auto value=fence.value;
        check(fg.WaitForSCAllocator(0)==expected,"SC fence incorrectly classified");fence.value=value;
        check(fg.WaitForUIAllocator(0)==expected,"UI fence incorrectly classified");fence.value=value;
        check(bridge._WaitForCopyAllocator(0)==expected,"bridge fence incorrectly classified");fence.value=value;
        check(bridge._WaitForCopyQueueIdle()==expected,"bridge idle incorrectly classified");
        bridge._copyAllocatorFenceValues[0]=4;
    };
    fence.value=UINT64_MAX;all(false); // DXGI device removal sentinel.
    fence.value=4;all(true);
    fence.value=3;fence.afterWait=3;all(false); // Stale event cannot prove completion.
    fence.value=3;fence.afterWait=UINT64_MAX;all(false); // Removal during the wait.
    fence.value=3;fence.afterWait=4;all(true);
    fence.value=3;injectedWaitResult=WAIT_TIMEOUT;all(false);injectedWaitResult=WAIT_OBJECT_0;
    fence.value=3;fence.eventResult=E_FAIL;all(false);fence.eventResult=S_OK;
    fg._scFence=nullptr;fg._uiFence=nullptr;bridge._copyFence=nullptr;all(false);
    fg._uiFence=&fence;bridge._copyFence=&fence;fence.value=4;
    bridge._copySubmissionFailed=true;
    check(!bridge._WaitForCopyAllocator(0) && !bridge._WaitForCopyQueueIdle(),"failed signal reused old completion");
    check(!bridge._WaitForCopyAllocator(99),"bad copy slot accepted");
    Commands ui,sc;fg._uiCommandList[0]=&ui;fg._scCommandList[0]=&sc;
    fg._uiCommandListResetted[0]=fg._scCommandListResetted[0]=true;
    fg._frameResources[0][1]=2;fg._resourceReady[0][1]=2;
    fg.CancelPendingUpscalerWork();
    check(fg.submitted==0,"shutdown submitted work that should have been cancelled");
    check(fg.deactivated==1 && ui.closed==1 && sc.closed==1 && fg._uiAllocatorFenceValues[0]==0 && fg._frameResources[0].empty() && fg._resourceReady[0].empty() && fg._waitingNewFrameData,"cancellation did not retire pending state");
    Queue queue;fg._gameCommandQueue=&queue;fg._scFence=&fence;
    fg._scCommandListResetted[0]=true;
    check(fg.SubmitSCCommandList(0) && queue.executed==1 && queue.signaled==1 && !fg._scCommandListResetted[0],"SC list not submitted with completion signal");
    fg._scCommandListResetted[0]=true;queue.result=E_FAIL;
    check(!fg.SubmitSCCommandList(0) && fg._scSubmissionFailed && !fg.WaitForSCAllocator(0),"SC signal failure did not block reuse");
    check(!fg.SubmitSCCommandList(0) && queue.executed==2,"failed SC submission retried");
    fg._scSubmissionFailed=false;fg._scCommandListResetted[0]=true;sc.result=E_FAIL;
    check(!fg.SubmitSCCommandList(0) && queue.executed==2,"SC close failure still submitted work");
    using FrameLimitTiming::Interval;
    check(Interval(0,false)==0 && Interval(-60,false)==0 && Interval(std::numeric_limits<float>::quiet_NaN(),false)==0 && Interval(std::numeric_limits<float>::infinity(),false)==0,"invalid FPS setting was not disabled");
    check(Interval(60,false)==16666666 && Interval(60,true)==33333332 && Interval(144,false)==6944444,"valid FPS interval changed incorrectly");
    check(Interval(std::numeric_limits<float>::denorm_min(),false)==100000000000ULL,"tiny cap overflowed");
    puts("PASS: production FG cancellation order, device removal/stale event/timeout/failed signal checks, invalid and normal limiter settings");
    return 0;
} catch(const std::exception& e) {puts(e.what());return 1;}
