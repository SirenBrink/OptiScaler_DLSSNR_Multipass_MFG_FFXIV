#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <stdexcept>
static HWND testForeground=reinterpret_cast<HWND>(1);
static DWORD testOwner=17;
static unsigned queries=0;
static HWND TestForeground(){++queries;return testForeground;}
static DWORD TestOwner(HWND,DWORD* out){*out=testOwner;return 1;}
static DWORD TestProcess(){return 17;}
#define GetForegroundWindow TestForeground
#define GetWindowThreadProcessId TestOwner
#define GetCurrentProcessId TestProcess
#include "../OptiScaler/dlssnr/NrFocusPolicy.h"
#undef GetForegroundWindow
#undef GetWindowThreadProcessId
#undef GetCurrentProcessId
static void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
int main()try {
    using DlssNr::FocusPolicy::Paused;
    check(!Paused(false) && queries==0,"disabled setting queried focus");
    check(!Paused(true),"game focus paused NR");
    testForeground=reinterpret_cast<HWND>(2);
    check(!Paused(true),"in-process overlay paused NR");
    testOwner=99;check(Paused(true),"other process did not pause NR");
    testOwner=0;check(Paused(true),"unknown testOwner did not pause NR");
    testForeground=nullptr;check(Paused(true),"no testForeground did not pause NR");
    testForeground=reinterpret_cast<HWND>(1);testOwner=17;
    check(!Paused(true),"focus return did not resume NR");
    testOwner=99;check(!Paused(false),"disabled preference paused NR");
    puts("PASS: production NR focus policy: opt-in, game/overlay focus, alt-tab, unavailable window, focus recovery");
    return 0;
}catch(const std::exception& e){puts(e.what());return 1;}
