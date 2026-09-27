#pragma once
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

// Owned CPU geometry snapshot, not an executable copy of engine commands.
// Never retain borrowed engine memory or invoke the captured resource pointers.
namespace FfxivNameplatePackets
{
inline std::atomic<bool> requested {false};
inline std::mutex mutex;
struct Queue { uintptr_t address=0, head=0; UINT blocks=0, remaining=0, count=0; std::array<UINT,3> strides{}; };
struct Packet { uintptr_t source=0; UINT key=0, kind=0, layout=0, count=0, stride=0, error=0; std::array<unsigned char,64> header{}; std::vector<unsigned char> vertices; };
struct Capture { Queue before,after; std::vector<Packet> packets; UINT rejected=0; bool ready=false; };
inline Capture pending;
inline void (*onCapture)(const Capture&)=nullptr;
inline void (*onSaved)(const std::filesystem::path&)=nullptr;
template<class T> inline bool Read(uintptr_t address,T& value)
{
    SIZE_T got=0;
    return address && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(address),&value,sizeof(value),&got) && got==sizeof(value);
}
inline bool ReadBytes(uintptr_t address,void* dst,size_t size)
{
    SIZE_T got=0;
    return address && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(address),dst,size,&got) && got==size;
}
inline Queue ReadQueue(uintptr_t queue)
{
    Queue q;
    if(!Read(queue+0x50,q.head) || !Read(queue+0x68,q.remaining) || !Read(queue+0x6c,q.blocks) ||
       !Read(queue+0x34,q.strides) || q.blocks>128 || q.remaining>64) return q;
    // Native reset writes blocks=0, remaining=1 before the first allocation.
    // There is no command to subtract until a block exists (see 14063da30).
    if(!q.blocks)
    {
        if(q.remaining>1) return q;
        q.address=queue; return q;
    }
    if(q.blocks*64<q.remaining) return q;
    q.address=queue; q.count=q.blocks*64-q.remaining;
    return q;
}
inline Queue Snapshot(uintptr_t base)
{
    uintptr_t module=0,renderer=0,queue=0;
    if(!Read(base+0x2aa0590,module) || !Read(module+8,renderer) || !Read(renderer+8,queue)) return {};
    return ReadQueue(queue);
}
// Current executable table at RVA 0x217b670. These are shader variants,
// not direct indices into the three vertex-buffer strides.
inline constexpr std::array<UINT,9> vertexLayouts {0,0,0,0,1,2,2,1,1};
inline Capture Copy(const Queue& before,const Queue& after)
{
    Capture result; result.before=before; result.after=after; result.ready=true;
    if(!before.address || !after.address || before.address!=after.address || after.count<before.count || after.count-before.count>512)
    { ++result.rejected; return result; }
    uintptr_t block=after.head;
    size_t bytes=0;
    for(UINT index=0;index<after.count;++index)
    {
        if(index && index%64==0)
            if(!Read(block+0x3f0,block)) { ++result.rejected; break; }
        if(index<before.count) continue;
        std::array<uintptr_t,2> entry{};
        Packet p;
        if(!Read(block+(index%64)*16,entry) || !Read(entry[1],p.header)) { ++result.rejected; continue; }
        p.source=entry[1]; p.key=static_cast<UINT>(entry[0]);
        std::memcpy(&p.kind,p.header.data(),4);
        UINT format=0; std::memcpy(&format,p.header.data()+0x30,4); p.layout=format&15;
        // Only basic geometry has a verified count/pointer layout. Clip/effect
        // records remain metadata and must never be fed to a generic renderer.
        if(p.kind==0x10 || p.kind==0x11 || (p.kind>=0x20 && p.kind<=0x23))
        {
            uintptr_t vertices=0;
            std::memcpy(&p.count,p.header.data()+0x20,4);
            std::memcpy(&vertices,p.header.data()+0x28,8);
            if(p.layout<vertexLayouts.size()) p.stride=after.strides[vertexLayouts[p.layout]];
            const size_t n=static_cast<size_t>(p.count)*p.stride;
            if(!p.stride || p.stride>128 || p.count>65536 || n>4*1024*1024 || bytes+n>8*1024*1024)
            { ++result.rejected; p.error=1; result.packets.push_back(std::move(p)); continue; }
            p.vertices.resize(n);
            if(n && !ReadBytes(vertices,p.vertices.data(),n)) { ++result.rejected; p.error=2; p.vertices.clear(); result.packets.push_back(std::move(p)); continue; }
            bytes+=n;
        }
        result.packets.push_back(std::move(p));
    }
    return result;
}
inline void Finish(uintptr_t base,const Queue& before)
{
    try { auto result=Copy(before,Snapshot(base)); if(onCapture) onCapture(result); std::lock_guard lock(mutex); pending=std::move(result); }
    catch(...) { LOG_WARN("FFXIV nameplate packets: allocation failed; game rendering unchanged"); }
}
inline void Save()
{
    std::lock_guard lock(mutex);
    if(!pending.ready) return;
    pending.ready=false;
    try
    {
        wchar_t exe[32768]{}; GetModuleFileNameW(nullptr,exe,32768);
        const auto directory=std::filesystem::path(exe).parent_path()/L"OptiScaler_NameplatePackets"/
            (std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(directory);
        if(onSaved) onSaved(directory);
        std::ofstream report(directory/"packets.tsv");
        report<<"# schema=1; CPU snapshot only; pointer-bearing headers are not replayable\n"
              <<"# before_valid="<<(pending.before.address!=0)<<" before_blocks="<<pending.before.blocks<<" before_remaining="<<pending.before.remaining
              <<" after_valid="<<(pending.after.address!=0)<<" after_blocks="<<pending.after.blocks<<" after_remaining="<<pending.after.remaining<<"\n"
              <<"# before="<<pending.before.count<<" after="<<pending.after.count<<" rejected="<<pending.rejected<<"\n"
              <<"index\tkey\tkind\tlayout\tvertices\tstride\tbytes\terror\n";
        UINT i=0;
        for(const auto& p:pending.packets)
        {
            report<<i<<'\t'<<p.key<<'\t'<<p.kind<<'\t'<<p.layout<<'\t'<<p.count<<'\t'<<p.stride<<'\t'<<p.vertices.size()<<'\t'<<p.error<<'\n';
            std::ofstream h(directory/(std::to_string(i)+".header.bin"),std::ios::binary);
            h.write(reinterpret_cast<const char*>(p.header.data()),p.header.size());
            std::ofstream v(directory/(std::to_string(i)+".vertices.bin"),std::ios::binary);
            v.write(reinterpret_cast<const char*>(p.vertices.data()),p.vertices.size()); ++i;
        }
        LOG_INFO("FFXIV nameplate packets: saved {} records, rejected {}, queue {} -> {}, folder {}",pending.packets.size(),pending.rejected,pending.before.count,pending.after.count,directory.string());
        pending.packets.clear();
    }
    catch(const std::exception& e) { LOG_WARN("FFXIV nameplate packets: save failed: {}",e.what()); }
}
}
