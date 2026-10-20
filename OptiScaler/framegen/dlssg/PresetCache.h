#pragma once
#include <span>
#include <cstdint>
#include <cstring>
namespace DlssgPresetCache {
inline constexpr unsigned Setting=0x10e41df1;
// Verified cached-settings reader in the supported 310.9 DLL. No fixed RVA.
inline uintptr_t FindReader(std::span<const unsigned char> image){
    constexpr unsigned char call[]={0x48,0x8d,0x54,0x24,0x68,0xc7,0x44,0x24,0x68,0,0,0,0,0xb9,0xf1,0x1d,0xe4,0x10,0xe8};
    constexpr unsigned char prologue[]={0x48,0x89,0x5c,0x24,0x10,0x89,0x4c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xfa,0x8b,0xd9,0xe8};
    uintptr_t found=0;
    for(size_t i=0;i+sizeof(call)+6<=image.size();++i){
        if(std::memcmp(image.data()+i,call,sizeof(call)) || image[i+23]!=0x84 || image[i+24]!=0xc0)continue;
        if(found)return 0;
        int32_t displacement=0;std::memcpy(&displacement,image.data()+i+19,4);
        const int64_t target=int64_t(i)+23+displacement;
        if(target<0 || uint64_t(target)+sizeof(prologue)>image.size() || std::memcmp(image.data()+target,prologue,sizeof(prologue)))return 0;
        found=reinterpret_cast<uintptr_t>(image.data()+target);
    }
    return found;
}
// Caller supplies the original, unmodified driver selection for Automatic.
inline bool Override(unsigned id,unsigned* output,int requested,uint64_t baseline){
    if(id!=Setting || !output || requested<0 || requested>3)return false;
    if(!requested && baseline==UINT64_MAX)return false;
    *output=requested?unsigned(requested):unsigned(baseline);return true;
}
void ObserveDriverQuery(void* caller);
bool Ensure();
const char* UnavailableReason();
}
