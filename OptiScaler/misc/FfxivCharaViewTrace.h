#pragma once
#include <windows.h>
#include <array>
#include <vector>
#include <cstring>

// Read-only discovery. Layout reference: aers/FFXIVClientStructs,
// Graphics/Render/RenderTargetManager.cs and Graphics/Kernel/Texture.cs.
// GetCharaViewTexture's accessor and +0x2C8 were also verified in the installed
// executable with Ghidra (2026-10-20 MD5 6363cbeb6ca97e87a37da0a8e737a5a0).
// Never call methods through engine-owned pointers or hold their COM objects.
namespace FfxivCharaViewTrace {
inline bool Read(UINT_PTR address,void* out,size_t size) {
    SIZE_T got=0;
    return address && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),out,size,&got) && got==size;
}
inline UINT_PTR FindAccessor(const unsigned char* text,size_t size,UINT_PTR address) {
    constexpr unsigned char shape[]{0x48,0x8b,0x05,0,0,0,0,0x8b,0xca,0x48,0x8b,0x84,0xc8,0xc8,0x02,0,0,0xc3};
    UINT_PTR found=0;
    for(size_t i=0;i+sizeof(shape)<=size;++i) {
        bool match=true;
        for(size_t j=0;j<sizeof(shape);++j)if((j<3 || j>6) && text[i+j]!=shape[j]){match=false;break;}
        if(!match)continue;
        if(found)return 0; // Ambiguous builds are deliberately unsupported.
        INT32 displacement=0;std::memcpy(&displacement,text+i+3,4);
        found=address+i+7+displacement;
    }
    return found;
}
inline UINT_PTR Resolve() {
    auto base=reinterpret_cast<UINT_PTR>(GetModuleHandleW(nullptr));
    IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
    if(!Read(base,&dos,sizeof(dos)) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<=0 || dos.e_lfanew>0x100000)return 0;
    if(!Read(base+dos.e_lfanew,&nt,sizeof(nt)) || nt.Signature!=IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.FileHeader.NumberOfSections>96)return 0;
    const auto sections=base+dos.e_lfanew+sizeof(DWORD)+sizeof(IMAGE_FILE_HEADER)+nt.FileHeader.SizeOfOptionalHeader;
    for(UINT i=0;i<nt.FileHeader.NumberOfSections;++i) {
        IMAGE_SECTION_HEADER s{};if(!Read(sections+i*sizeof(s),&s,sizeof(s)))return 0;
        if(std::memcmp(s.Name,".text\0",6))continue;
        const size_t size=s.Misc.VirtualSize;
        if(size>128*1024*1024 || s.VirtualAddress>=nt.OptionalHeader.SizeOfImage || size>nt.OptionalHeader.SizeOfImage-s.VirtualAddress)return 0;
        std::vector<unsigned char> bytes(size);if(!Read(base+s.VirtualAddress,bytes.data(),size))return 0;
        auto global=FindAccessor(bytes.data(),size,base+s.VirtualAddress);
        return global>=base && global-base<=nt.OptionalHeader.SizeOfImage-sizeof(UINT_PTR)?global:0;
    }
    return 0;
}
struct Entry {UINT_PTR identity=0;UINT width=0,height=0;};
struct Snapshot {std::array<Entry,20> entries{};};
inline Snapshot ReadSnapshot(UINT_PTR global) {
    Snapshot result{};UINT_PTR manager=0;
    std::array<unsigned char,0x400> data{};
    if(!Read(global,&manager,sizeof(manager)) || !Read(manager,data.data(),data.size()))return result;
    for(UINT i=0;i<result.entries.size();++i) {
        const UINT offset=i<8?0x2c8+i*8:i<13?0x308+(i-8)*8:i<18?0x378+(i-13)*8:0x360+(i-18)*8;
        UINT_PTR texture=0;std::memcpy(&texture,data.data()+offset,sizeof(texture));
        std::array<unsigned char,0x70> native{};
        if(!Read(texture,native.data(),native.size()))continue;
        auto& entry=result.entries[i];
        std::memcpy(&entry.width,native.data()+0x40,4);std::memcpy(&entry.height,native.data()+0x44,4);
        std::memcpy(&entry.identity,native.data()+0x68,sizeof(entry.identity));
        if(!entry.width || !entry.height || entry.width>16384 || entry.height>16384)entry={};
    }
    return result;
}
inline const Snapshot& Current() {
    static const UINT_PTR global=[] {
        const auto address=Resolve();
        LOG_INFO("CharaView native lookup: verified accessor available {}",address!=0);
        return address;
    }();
    thread_local Snapshot snapshot{};thread_local UINT64 time=0;
    const auto now=GetTickCount64();
    if(!time || now-time>=100){snapshot=ReadSnapshot(global);time=now;}
    return snapshot;
}
inline int Match(const Snapshot& snapshot,const void* bound,UINT width,UINT height) {
    for(UINT i=0;i<snapshot.entries.size();++i) {
        const auto& e=snapshot.entries[i];
        if(e.identity && e.identity==reinterpret_cast<UINT_PTR>(bound) && e.width==width && e.height==height)return int(i);
    }
    return -1;
}
inline const char* Role(int index) {return index<8?"output":index<13?"GBuffer":index<18?"transparent GBuffer":"depth candidate";}
}
