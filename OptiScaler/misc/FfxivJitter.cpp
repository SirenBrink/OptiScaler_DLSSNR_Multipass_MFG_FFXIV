#include "pch.h"
#include "FfxivJitter.h"
#include "TemporalSampling.h"
#include <detours/detours.h>
#include <intrin.h>
#include <array>
#include <mutex>
#include <TlHelp32.h>
#include <vector>

namespace FfxivJitter {
namespace {
using Getter=void* (*)(uintptr_t);
Getter original=nullptr;
using Generator=uintptr_t (*)(uintptr_t,float*,float*,unsigned,unsigned);
Generator originalGenerator=nullptr;
uintptr_t executable=0;
std::mutex mutex;
std::array<Sample,32> samples{};
unsigned cursor=0;
thread_local Sample current{};
thread_local bool matched=false;
bool Read(uintptr_t address,void* result,size_t size) {
    SIZE_T copied=0;return address && ReadProcessMemory(GetCurrentProcess(),(void*)address,result,size,&copied) && copied==size;
}
uintptr_t Generate(uintptr_t opaque,float* x,float* y,unsigned frame,unsigned phases) {
    if(uintptr_t(_ReturnAddress())!=executable+0x25f79e || !x || !y)
        return originalGenerator(opaque,x,y,frame,phases);
    uintptr_t settings=0,renderer=0;
    std::array<unsigned char,0x30> config{};
    unsigned short extent[3]{};
    if(!Read(executable+0x290e470,&settings,sizeof(settings)) ||
       !Read(executable+0x290e490,&renderer,sizeof(renderer)) ||
       !Read(settings+0x4c,config.data(),config.size()) || config[8]!=2 ||
       !Read(renderer+0x700,extent,sizeof(extent)) || !extent[2])
        return originalGenerator(opaque,x,y,frame,phases);
    float scale=0,amount=0;memcpy(&scale,config.data(),4);memcpy(&amount,config.data()+0x2c,4);
    // Exact native camera expression verified in Ghidra and the executable.
    // 0x211dd64 is 0.001f (not 1.0f); keep the game's own lower bound.
    const float ratio=(float(extent[0])/extent[2])*scale;
    const float strength=(amount-.001f)*ratio*ratio+.001f;
    if(!std::isfinite(strength) || strength<.125f || strength>4)
        return originalGenerator(opaque,x,y,frame,phases);
    // The native DLSS path supplies UINT_MAX (an effectively unbounded lattice).
    // Change the generator's phase count before rasterization, never just NGX's
    // offset. Preserve the native frame counter and all jitter-enable gates.
    uintptr_t device=0;unsigned render[2]{},output[2]{};
    if(phases==UINT_MAX && Read(renderer+0x428,render,sizeof(render)) &&
       Read(executable+0x2907d00,&device,sizeof(device)) &&
       Read(device+0x8c,output,sizeof(output)))
        phases=PatternPhases(phases,render[0],render[1],output[0],output[1]);
    auto result=originalGenerator(opaque,x,y,frame,phases);
    NormalizeHalton(*x,*y,strength);
    return result;
}
void* Observe(uintptr_t camera) {
    auto* result=original(camera);
    // Ghidra-verified world-scene NGX packet producer. Other camera users are not sampled.
    if(uintptr_t(_ReturnAddress())!=executable+0x374122 || !result) return result;
    Sample s;uintptr_t renderer=0;float values[4]{};unsigned size[2]{};
    if(!Read(uintptr_t(result),values,sizeof(values)) ||
       !Read(executable+0x290e490,&renderer,sizeof(renderer)) ||
       !Read(renderer+0x428,size,sizeof(size))) return result;
    s.rawX=values[0];s.rawY=values[1];s.ndcX=values[2];s.ndcY=values[3];
    s.width=size[0];s.height=size[1];s.time=GetTickCount64();float x=0,y=0;
    if(!Pixels(s,x,y)) return result;
    {std::lock_guard lock(mutex);samples[cursor++%samples.size()]=s;}
    // This exact caller immediately copies two floats into its queued NGX packet.
    // Returning the actual raster offset here preserves its frame identity, even
    // across render-thread delays or backend recreation. Other callers retain
    // the native pointer and full native jitter-state layout.
    thread_local float packet[2];packet[0]=x;packet[1]=y;
    return packet;
}
}
void Install() {
    static std::once_flag once;
    std::call_once(once,[]{
        if(State::Instance().gameExe!="ffxiv_dx11.exe")return;
        auto base=uintptr_t(GetModuleHandleW(nullptr));
        // Installed 2026-10-20 executable MD5 6363cbeb6ca97e87a37da0a8e737a5a0.
        // Validate getter, unique producer call, camera raster routine and renderer global load.
        constexpr unsigned char getter[]{0x48,0x8b,0x81,0x78,0x2,0,0,0xc3};
        constexpr unsigned char call[]{0xe8,0xee,0xb4,0xee,0xff,0x0f,0x57,0xc0,0x41,0xbd,0xc4,0x03,0x02};
        constexpr unsigned char camera[]{0x40,0x55,0x57,0x48,0x8d,0xac,0x24,0x68,0xfe,0xff,0xff};
        constexpr unsigned char rendererLoad[]{0x48,0x8b,0x05,0x68,0xed,0x6a,0x02};
        constexpr unsigned char generator[]{0x48,0x8b,0xc4,0x48,0x89,0x58,0x8,0x48,0x89,0x70,0x10,0x57,0x48,0x83,0xec,0x70};
        constexpr unsigned char generatorCall[]{0xe8,0xf2,0xc5,0x08,0x00,0x48,0x8b,0x0d,0xeb,0xec,0x6a,0x02};
        unsigned char a[sizeof(getter)]{},b[sizeof(call)]{},c[sizeof(camera)]{},d[sizeof(rendererLoad)]{};
        unsigned char e[sizeof(generator)]{},f[sizeof(generatorCall)]{};
        if(!Read(base+0x25f610,a,sizeof(a)) || memcmp(a,getter,sizeof(a)) ||
           !Read(base+0x37411d,b,sizeof(b)) || memcmp(b,call,sizeof(b)) ||
           !Read(base+0x25f690,c,sizeof(c)) || memcmp(c,camera,sizeof(c)) ||
           !Read(base+0x25f721,d,sizeof(d)) || memcmp(d,rendererLoad,sizeof(d)) ||
           !Read(base+0x2ebd90,e,sizeof(e)) || memcmp(e,generator,sizeof(e)) ||
           !Read(base+0x25f799,f,sizeof(f)) || memcmp(f,generatorCall,sizeof(f))) {
            LOG_WARN("FFXIV jitter: native projection verification unavailable for this executable; offsets unchanged");return;
        }
        original=reinterpret_cast<Getter>(base+0x25f610);
        originalGenerator=reinterpret_cast<Generator>(base+0x2ebd90);executable=base;
        std::vector<HANDLE> threads;
        HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
        if(snapshot==INVALID_HANDLE_VALUE){original=nullptr;return;}
        THREADENTRY32 entry{};entry.dwSize=sizeof(entry);
        bool next=Thread32First(snapshot,&entry)!=FALSE,ready=next;
        while(next){
            if(entry.th32OwnerProcessID==GetCurrentProcessId() && entry.th32ThreadID!=GetCurrentThreadId()){
                HANDLE thread=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_SET_CONTEXT,FALSE,entry.th32ThreadID);
                if(thread)threads.push_back(thread);else if(GetLastError()!=ERROR_INVALID_PARAMETER)ready=false;
            }
            next=Thread32Next(snapshot,&entry)!=FALSE;
        }
        CloseHandle(snapshot);
        if(!ready || DetourTransactionBegin()!=NO_ERROR){for(auto t:threads)CloseHandle(t);original=nullptr;return;}
        bool ok=DetourUpdateThread(GetCurrentThread())==NO_ERROR;
        for(auto t:threads)ok &= DetourUpdateThread(t)==NO_ERROR;
        if(ok)ok=DetourAttach(reinterpret_cast<PVOID*>(&original),Observe)==NO_ERROR;
        if(ok)ok=DetourAttach(reinterpret_cast<PVOID*>(&originalGenerator),Generate)==NO_ERROR;
        LONG status=ERROR_INVALID_FUNCTION;
        if(ok)status=DetourTransactionCommit();else DetourTransactionAbort();
        for(auto t:threads)CloseHandle(t);
        if(status!=NO_ERROR){original=nullptr;LOG_WARN("FFXIV jitter: native observer unavailable ({})",status);}
        else LOG_INFO("FFXIV jitter: full-pixel native Halton coverage and same-frame NGX packet offsets enabled");
    });
}
void Detach() {
    if(!original)return;
    if(DetourTransactionBegin()!=NO_ERROR)return;
    bool ok=DetourUpdateThread(GetCurrentThread())==NO_ERROR &&
        DetourDetach(reinterpret_cast<PVOID*>(&original),Observe)==NO_ERROR;
    if(ok)ok=DetourDetach(reinterpret_cast<PVOID*>(&originalGenerator),Generate)==NO_ERROR;
    if(ok){if(DetourTransactionCommit()==NO_ERROR){original=nullptr;originalGenerator=nullptr;}}else DetourTransactionAbort();
}
Binding::Binding(NVSDK_NGX_Parameter* p):params(p) {
    matched=false;current={};
    if(!original || !p || p->Get(NVSDK_NGX_Parameter_Jitter_Offset_X,&x)!=NVSDK_NGX_Result_Success ||
       p->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y,&y)!=NVSDK_NGX_Result_Success) return;
    // Raw Halton samples identify the queued scene even when the game and render threads differ.
    unsigned width=0,height=0;
    p->Get(NVSDK_NGX_Parameter_Width,&width);p->Get(NVSDK_NGX_Parameter_Height,&height);
    if(!width || !height){return;}
    const auto now=GetTickCount64();
    {std::lock_guard lock(mutex);
        for(unsigned i=0;i<samples.size();++i){const auto& s=samples[(cursor+samples.size()-1-i)%samples.size()];
            float px=0,py=0;
            if(!Pixels(s,px,py))continue;
            const bool rawMatch=std::abs(s.rawX-x)<1e-7f && std::abs(s.rawY-y)<1e-7f;
            const bool packetMatch=std::abs(px-x)<1e-7f && std::abs(py-y)<1e-7f;
            if(s.time && now-s.time<1000 && std::abs(int(s.width)-int(width))<=1 && std::abs(int(s.height)-int(height))<=1 &&
               (rawMatch || packetMatch)){current=s;matched=true;break;}
        }
    }
    if(!matched){return;}
    float px=0,py=0;if(!Pixels(current,px,py)){matched=false;return;}
    if(std::abs(px-x)>1e-5f || std::abs(py-y)>1e-5f){
        p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X,px);p->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y,py);changed=true;
    }
}
Binding::~Binding(){
    if(changed){params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X,x);params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y,y);}
    matched=false;
}
}
