#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <future>
#include <cstdio>
#include <stdexcept>
#include "../OptiScaler/misc/InteropAuxiliaryPresent.h"
struct Params {int marker=7;};
struct Swapchain {
    unsigned calls=0,calls1=0,sync=0,flags=0;
    const Params* params=nullptr;
    HRESULT result=S_OK;
    HRESULT Present(UINT s,UINT f){++calls;sync=s;flags=f;return result;}
    HRESULT Present1(UINT s,UINT f,const Params* p){++calls1;sync=s;flags=f;params=p;return result;}
};
using IDXGISwapChain1=Swapchain;
static unsigned visibleUpdates=0;
static HRESULT Run(Swapchain* pSwapChain,UINT SyncInterval,UINT Flags,const Params* pPresentParameters){
#include "auxiliary_present_production.inl"
    ++visibleUpdates;
    return S_FALSE;
}
static void check(bool v,const char* msg){if(!v)throw std::runtime_error(msg);}
int main()try {
    Swapchain s;Params params;
    check(Run(&s,1,2,nullptr)==S_FALSE && visibleUpdates==1 && s.calls==0,"ordinary visible presentation was bypassed");
    {
        InteropAuxiliaryPresent::Scope outer;
        check(Run(&s,0,8,nullptr)==S_OK && s.calls==1 && visibleUpdates==1,"auxiliary present entered visible bookkeeping or failed to forward");
        check(s.sync==0 && s.flags==8,"auxiliary Present arguments changed");
        {
            InteropAuxiliaryPresent::Scope inner;
            check(Run(&s,2,16,&params)==S_OK && s.calls1==1 && visibleUpdates==1,"auxiliary Present1 did not bypass bookkeeping");
            check(s.sync==2 && s.flags==16 && s.params==&params,"Present1 metadata lost");
        }
        check(InteropAuxiliaryPresent::Active(),"nested scope cleared its parent");
        check(!std::async(std::launch::async,[]{return InteropAuxiliaryPresent::Active();}).get(),"scope affected another presentation thread");
        s.result=E_FAIL;
        check(Run(&s,0,8,nullptr)==E_FAIL && visibleUpdates==1,"real failure was swallowed");
    }
    check(!InteropAuxiliaryPresent::Active(),"scope leaked past the hidden presentation");
    check(Run(&s,1,0,nullptr)==S_FALSE && visibleUpdates==2,"visible output bookkeeping did not resume");
    try {InteropAuxiliaryPresent::Scope throwing;throw 1;}catch(int){}
    check(!InteropAuxiliaryPresent::Active(),"exception leaked auxiliary state");
    puts("PASS: production auxiliary bypass preserves Present/Present1 calls, flags, errors, nesting, thread isolation and visible-frame bookkeeping");
    return 0;
}catch(const std::exception& e){puts(e.what());return 1;}
