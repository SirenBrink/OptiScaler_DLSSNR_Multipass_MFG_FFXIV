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
#undef VirtualProtect
void* TimestampStub(void*,int64_t* out,void*,void*,uint32_t,uint32_t){if(out)*out=100000;return out;}
int main(){
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
 Config::Instance()->FGXeFGExtraPacing.v=true;
 memcpy(image+PresentThunkRva,PresentThunkExpected,16);memcpy(image+SchedThunkRva,SchedThunkExpected,16);memcpy(image+TimestampThunkRva,TimestampThunkExpected,16);
 assert(Install(image));assert(g_enabled&&g_native&&g_schedNative&&g_tsNative);assert(image[PresentThunkRva]==0xff);
 // No synthetic provider machine code is executed; only the C++ pacing arithmetic above.
 puts("XeMFG: build/byte rejection, write-failure rollback, 8X ceiling, idempotence, 2X-8X deadlines, telemetry freshness and thunk install passed");
 VirtualFree(image,0,MEM_RELEASE);
}
