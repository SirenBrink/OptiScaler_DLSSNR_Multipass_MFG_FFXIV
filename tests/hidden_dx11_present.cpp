#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#include <cstdio>
#include <stdexcept>
#include "../OptiScaler/misc/InteropAuxiliaryPresent.h"
static unsigned warnings=0;
#define LOG_WARN(...) (++warnings)
namespace FramePacingTrace {
enum class Phase {HiddenPresent};
struct Scope {explicit Scope(Phase){}};
}
struct Swapchain {
    UINT interval=99,flags=0,calls=0,blockingCalls=0;
    HRESULT result=S_OK;
    bool busy=false;
    HRESULT Present(UINT sync,UINT presentFlags) {
        if(!InteropAuxiliaryPresent::Active())throw std::runtime_error("hidden-present scope missing");
        ++calls;interval=sync;flags=presentFlags;
        if(busy){
            if(flags & DXGI_PRESENT_DO_NOT_WAIT)return DXGI_ERROR_WAS_STILL_DRAWING;
            ++blockingCalls; // The previous implementation could block here.
        }
        return result;
    }
};
static void HiddenPresent(Swapchain* _real,UINT Flags){
#include "hidden_present_production.inl"
}
static void check(bool value,const char* msg){if(!value)throw std::runtime_error(msg);}
int main()try {
    Swapchain chain;
    HiddenPresent(&chain,DXGI_PRESENT_RESTART);
    check(!InteropAuxiliaryPresent::Active(),"hidden-present scope leaked");
    check(chain.calls==1 && chain.interval==0,"hidden present changed synchronization/call count");
    check(chain.flags==(DXGI_PRESENT_RESTART|DXGI_PRESENT_DO_NOT_WAIT),"hidden present can block or lost caller flags");
    chain.busy=true;
    for(unsigned i=0;i<1000;++i)HiddenPresent(&chain,0);
    check(chain.calls==1001 && chain.blockingCalls==0,"busy hidden present waited or retried");
    check(warnings==0,"ordinary busy status was treated as an error");
    chain.busy=false;chain.result=DXGI_ERROR_DEVICE_REMOVED;
    HiddenPresent(&chain,0);
    check(warnings==1,"real device failure was hidden");
    puts("PASS: production hidden DX11 present never requests a wait; no busy retry/warning spam; real failures remain reported");
    return 0;
}catch(const std::exception& e){puts(e.what());return 1;}
