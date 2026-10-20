#include "../OptiScaler/framegen/dlssg/PresetCache.h"
#include "../OptiScaler/framegen/dlssg/PresetSelection.h"
#include <cassert>
#include <vector>
#include <cstdio>
int main(){
 unsigned value=123;
 for(int request:{1,2,3,0}){assert(DlssgPresetCache::Override(DlssgPresetCache::Setting,&value,request,2));assert(value==unsigned(request?request:2));}
 assert(!DlssgPresetCache::Override(7,&value,2,0));assert(!DlssgPresetCache::Override(DlssgPresetCache::Setting,nullptr,2,0));
 assert(!DlssgPresetCache::Override(DlssgPresetCache::Setting,&value,0,UINT64_MAX));assert(!DlssgPresetCache::Override(DlssgPresetCache::Setting,&value,4,0));
 DlssgPreset::CaptureOriginal(0);DlssgPreset::CaptureOriginal(3);assert(DlssgPreset::originalPreset.load()==0);
 std::vector<unsigned char> bytes(256,0xcc);
 const unsigned char call[]={0x48,0x8d,0x54,0x24,0x68,0xc7,0x44,0x24,0x68,0,0,0,0,0xb9,0xf1,0x1d,0xe4,0x10,0xe8};
 const unsigned char fn[]={0x48,0x89,0x5c,0x24,0x10,0x89,0x4c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xfa,0x8b,0xd9,0xe8};
 std::memcpy(bytes.data()+32,call,sizeof(call));int32_t delta=160-(32+23);std::memcpy(bytes.data()+51,&delta,4);bytes[55]=0x84;bytes[56]=0xc0;std::memcpy(bytes.data()+160,fn,sizeof(fn));
 assert(DlssgPresetCache::FindReader(bytes)==uintptr_t(bytes.data()+160));
 auto bad=bytes;bad[160]^=1;assert(!DlssgPresetCache::FindReader(bad));
 bad=bytes;bad[46]^=1;assert(!DlssgPresetCache::FindReader(bad));
 bad=bytes;delta=-1000;std::memcpy(bad.data()+51,&delta,4);assert(!DlssgPresetCache::FindReader(bad));
 bad=bytes;std::memcpy(bad.data()+72,bytes.data()+32,25);delta=160-(72+23);std::memcpy(bad.data()+91,&delta,4);assert(!DlssgPresetCache::FindReader(bad));
 puts("PASS cached FG preset guard: unique relative target, prologue, bounds, ambiguous/mutated rejection; A/B/C/Automatic, unknown settings, null outputs and original-profile preservation");
}
