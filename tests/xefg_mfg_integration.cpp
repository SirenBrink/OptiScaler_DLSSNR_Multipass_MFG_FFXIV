#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <limits>
#include <cassert>
#include <cstdio>
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
template<class T>struct Setting {T v;T value_or_default() const{return v;}};
struct Config {static constexpr int XeFGMaxInterpolations=7;Setting<bool> FGXeFGUnlockEnabled{true},FGXeFGExtraPacing{false};Setting<int> FGXeFGMaxInterpolatedFrames{7};static Config* Instance(){static Config c;return &c;}};
int protectCalls=0,failCall=0;
BOOL TestProtect(void* p,SIZE_T n,DWORD v,DWORD* old){if(++protectCalls==failCall)return FALSE;return VirtualProtect(p,n,v,old);}
#define VirtualProtect TestProtect
#include "pacing-production.h"
#include "unlock-production.h"
#include "../OptiScaler/framegen/xefg/XeFGCamera.h"
#include "../OptiScaler/framegen/xefg/XeFGDynamic.h"
#undef VirtualProtect
void* TimestampStub(void*,int64_t* out,void*,void*,uint32_t,uint32_t){if(out)*out=100000;return out;}
bool SchedulerStub(void*,void*,uint8_t,void*,uint32_t){Sleep(2);return true;}
int main(){
 XeFGDynamic dynamic;int factor=2;
 for(uint64_t t=1;t<15000;t+=40)factor=dynamic.Select(40,138,t,factor,8);
 assert(factor==4); // Sustained low real FPS reaches, but never exceeds, 4X.
 for(uint64_t t=15001;t<35000;t+=8)factor=dynamic.Select(8,138,t,factor,8);
 assert(factor==2); // Sustained headroom drops one step at a time.
 assert(dynamic.Select(16,138,36000,8,8)==4);
 dynamic.Reset();factor=2;
 for(uint64_t t=1;t<2900;t+=40)factor=dynamic.Select(40,138,t,factor,4);
 assert(factor==2); // Cooldown prevents reacting immediately.
 factor=dynamic.Select(3000,138,2901,factor,4);assert(factor==2);
 for(uint64_t t=2902;t<5500;t+=40)factor=dynamic.Select(40,138,t,factor,4);
 assert(factor==2); // Loading pause restarts the observation window.
 assert(dynamic.Select(NAN,138,5501,2,4)==2);
 assert(dynamic.Select(40,138,5502,4,2)==2); // Runtime capability wins.

 float m[16],zero[3]={},right[3]={1,0,0},up[3]={0,1,0},forward[3]={0,0,1},position[3]={2,3,4};
 assert(!XeFGCamera::BuildView(m,zero,zero,zero,zero));
 for(int i=0;i<16;++i)assert(m[i]==(i%5==0?1.0f:0.0f));
 assert(XeFGCamera::BuildView(m,position,right,up,forward));assert(m[12]==-2&&m[13]==-3&&m[14]==-4);
 float rotatedRight[3]={0,0,-1},rotatedForward[3]={1,0,0};
 assert(XeFGCamera::BuildView(m,zero,rotatedRight,up,rotatedForward));assert(m[2]==1&&m[8]==-1);
 position[0]=NAN;assert(!XeFGCamera::BuildView(m,position,right,up,forward));assert(m[0]==1&&m[15]==1);

 auto* image=(uint8_t*)VirtualAlloc(nullptr,0x015ED000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);assert(image);
 auto* dos=(IMAGE_DOS_HEADER*)image;dos->e_magic=IMAGE_DOS_SIGNATURE;dos->e_lfanew=0x100;
 auto* nt=(IMAGE_NT_HEADERS*)(image+0x100);nt->Signature=IMAGE_NT_SIGNATURE;nt->FileHeader.NumberOfSections=1;
 nt->FileHeader.SizeOfOptionalHeader=sizeof(IMAGE_OPTIONAL_HEADER);nt->FileHeader.TimeDateStamp=0x69CB0F4D;nt->OptionalHeader.SizeOfImage=0x015ED000;
 auto* section=IMAGE_FIRST_SECTION(nt);memcpy(section->Name,".text",5);section->VirtualAddress=0x1000;section->Misc.VirtualSize=0x300000;
 const unsigned rvas[]={0x20DA4F,0x1A5DE4,0x1A517D,0x1A45C2,0x20973B};
 const unsigned sizes[]={6,2,5,10,5};
 const uint8_t bytes[5][10]={{0x0f,0x85,0xcc,0,0,0},{0x74,9},{0xbb,3,0,0,0},{0xc7,0x87,0x6c,1,0,0,1,0,0,0},{0xb8,1,0,0,0}};
 for(int i=0;i<5;++i)memcpy(image+rvas[i],bytes[i],sizes[i]);
 auto pristine=[&](){for(int i=0;i<5;++i)assert(memcmp(image+rvas[i],bytes[i],sizes[i])==0);};
 nt->FileHeader.TimeDateStamp=1;assert(!XeFGUnlock::Apply((HMODULE)image));pristine();assert(protectCalls==0);
 nt->FileHeader.TimeDateStamp=0x69CB0F4D;image[rvas[4]]=0xff;assert(!XeFGUnlock::Apply((HMODULE)image));assert(protectCalls==0);image[rvas[4]]=bytes[4][0];pristine();
 failCall=3;assert(!XeFGUnlock::Apply((HMODULE)image));pristine();failCall=0;
 assert(XeFGUnlock::Apply((HMODULE)image));assert(image[rvas[0]]==0xe9&&image[rvas[1]]==0xeb);
 assert(*(uint32_t*)(image+rvas[2]+1)==7&&*(uint32_t*)(image+rvas[3]+6)==7&&*(uint32_t*)(image+rvas[4]+1)==7);
 int writes=protectCalls;assert(XeFGUnlock::Apply((HMODULE)image));assert(protectCalls==writes);
 using namespace XeFGPacing;
 g_tsNative=TimestampStub;g_ring=nullptr;alignas(16) int64_t timing[4]={0,8000000,0,0};int64_t out=0;
 for(unsigned count=1;count<=7;++count){g_lastTsIndex=0;g_lastTsCountPlus1=0;for(unsigned i=1;i<=count;++i){TsDetour(nullptr,&out,nullptr,timing,i,count+1);assert(out==100000+(int64_t(i)-1)*(8000000/(count+1)));}}
 TsDetour(nullptr,&out,nullptr,timing,8,8);assert(out==100000);
 g_freq.QuadPart=1000000;g_periodNs=0;g_sampleCount=g_samplePos=0;PushPeriod(10000000);PushPeriod(12000000);PushPeriod(1000000000);assert(g_periodNs==12000000);
 g_renderTimeNs=12000000;g_renderSampleMs=GetTickCount64();assert(RenderTimeMs()==12.0);g_renderSampleMs=0;assert(RenderTimeMs()==0.0);
 // A 20ms interval with 5ms blocked must yield 15ms even if the median is 12ms.
 g_lastBurstQpc=100000;g_burstBlockQpc=5000;NoteFrame(1,3,120000);
 assert(g_renderTimeNs.load()==15000000);
 // A rebuild pause is never exported, and recovery starts a fresh window.
 ObserveWorkInterval(3138650000,10000000);assert(g_renderTimeNs.load()==0);assert(g_workCount==0);
 ObserveWorkInterval(20000000,10000000);assert(g_renderTimeNs.load()==10000000);
 ObserveWorkInterval(22000000,10000000);ObserveWorkInterval(120000000,10000000);
 assert(g_renderTimeNs.load()==12000000);
 ObserveWorkInterval(10000000,11000000);assert(g_renderTimeNs.load()==0);
 // Native final-frame waits must be counted once; they do not start a new
 // burst at 3X+, while the single generated frame at 2X does.
 QueryPerformanceFrequency(&g_freq);g_schedNative=SchedulerStub;g_enabled=true;
 alignas(16) uint8_t context[4096]={},burst[128]={};*(uint64_t*)(burst+8)=3;
 g_burstBlockQpc=0;g_lastBurstQpc=0;
 assert(SchedForwarder(context,burst,1,timing,3));assert(g_burstBlockQpc>0);assert(g_lastBurstQpc==0);
 auto blocked=g_burstBlockQpc;
 assert(SchedForwarder(context,burst,1,timing,2));assert(g_burstBlockQpc==blocked);
 *(uint64_t*)(burst+8)=1;
 assert(SchedForwarder(context,burst,1,timing,1));assert(g_lastBurstQpc>0);assert(g_burstBlockQpc>0);
 // Recovery is consumed at a burst boundary, not by an intermediate frame.
 g_renderTimeNs=10000000;g_renderSampleMs=GetTickCount64();RequestRecovery();assert(RenderTimeMs()==0.0);
 NoteFrame(2,4,100);assert(g_recoveryRequested.load());
 NoteFrame(1,4,100);assert(!g_recoveryRequested.load()&&g_recoveryBursts==32&&g_sampleCount==0);
 // Native 20ms estimate versus stale 100ms median: at 5X, recover with
 // 4ms spacing instead of extending each step to 20ms. Mid-burst stays fixed.
 alignas(16) uint8_t ring[4096]={};g_ring=ring;
 *(float*)(ring+RingMeasuredOffset)=20.0f;timing[1]=100000000;
 for(int b=0;b<32;++b){
  TsDetour(nullptr,&out,nullptr,timing,1,5);assert(out==100000&&g_burstStepNs==4000000);
  TsDetour(nullptr,&out,nullptr,timing,2,5);assert(out==4100000);
  TsDetour(nullptr,&out,nullptr,timing,3,5);assert(out==8100000);
  TsDetour(nullptr,&out,nullptr,timing,4,5);assert(out==12100000);
 }
 assert(g_recoveryBursts==0);
 TsDetour(nullptr,&out,nullptr,timing,1,5);assert(out==100000&&g_burstStepNs==4000000);
 g_ring=nullptr;
 g_enabled=false;
 // Native present is measured after scheduling, so add its duration once.
 g_burstTiming={};g_burstTiming.begin=100000;g_burstBlockQpc=2000;
 NoteGeneratedPresentDuration(3000);assert(g_burstBlockQpc==5000);
 assert(g_burstTiming.present==3000&&g_burstTiming.presentCalls==1);
 NoteGeneratedPresentDuration(-1);assert(g_burstBlockQpc==5000);
 g_freq.QuadPart=1000000;g_lastBurstQpc=100000;g_workCount=g_workPos=0;
 NoteFrame(1,4,120000);assert(g_renderTimeNs.load()==15000000);
 // Diagnostic accounting uses completed bursts and isolates factor changes.
 g_freq.QuadPart=1000000;g_burstTiming={};g_timingWindow={};g_timingBursts=0;
 BeginDiagnosticBurst(4,100000);g_burstTiming.scheduler[1]=2000;g_burstTiming.scheduler[3]=3000;
 g_burstTiming.present=1000;g_burstTiming.presentCalls=3;
 BeginDiagnosticBurst(4,120000);
 assert(g_timingBursts==1&&g_timingElapsed==20000&&SchedulerTotal(g_timingWindow)==5000);
 assert(g_timingWindow.present==1000&&g_timingWindow.presentCalls==3);
 BeginDiagnosticBurst(5,140000);assert(g_timingBursts==0);
 BeginDiagnosticBurst(5,1140000);assert(g_timingBursts==0);
 QueryPerformanceFrequency(&g_freq);
 Config::Instance()->FGXeFGExtraPacing.v=true;
 memcpy(image+PresentThunkRva,PresentThunkExpected,16);memcpy(image+SchedThunkRva,SchedThunkExpected,16);memcpy(image+TimestampThunkRva,TimestampThunkExpected,16);
 assert(Install(image));assert(g_enabled&&g_native&&g_schedNative&&g_tsNative);assert(image[PresentThunkRva]==0xff);
 // No synthetic provider machine code is executed; only the C++ pacing arithmetic above.
 puts("XeMFG: build/byte rejection, write-failure rollback, 8X ceiling, idempotence, 2X-8X deadlines, telemetry freshness and thunk install passed");
 VirtualFree(image,0,MEM_RELEASE);
}
