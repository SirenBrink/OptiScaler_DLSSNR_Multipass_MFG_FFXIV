#pragma once
#include "CompanionVisibility.h"
#include <map>
#include <mutex>

namespace FfxivCompanion::VisibilityQueue
{
struct Range {uintptr_t texture=0;UINT layout=0,first=0,count=0;};
struct Draw
{
    std::array<unsigned char,0xb0> command{};
    std::vector<UINT> indices;
    std::vector<Range> ranges;
    ULONGLONG time=0;
    UINT indexBytes=0;
    std::shared_ptr<const Snapshot> snapshot;
};
struct NativeBatch {uintptr_t renderer=0;UINT indexBytes=0;std::vector<Range> ranges;std::shared_ptr<const Snapshot> snapshot;};
inline thread_local NativeBatch batch;
inline thread_local std::shared_ptr<Draw> bound;
inline thread_local uintptr_t boundContext=0;
inline std::mutex mutex;
inline std::map<uintptr_t,std::shared_ptr<Draw>> queued;
inline std::atomic<uint64_t> emitted{0},boundCount{0},partitioned{0},rejected{0};
inline std::atomic<bool> tracking{false};
inline bool Begin(uintptr_t base,uintptr_t renderer,UINT indexBytes,const std::vector<Packets::Packet>& packets)
{
    if(batch.renderer || packets.empty())return false;
    NativeBatch next;next.renderer=renderer;next.indexBytes=indexBytes;
    uintptr_t allocation=0;if(!Packets::Read(renderer+0x570,allocation) || !allocation)return false;
    for(const auto& p:packets)
    {
        if(!Packets::Supported(p))return false;
        UINT flags=0,count=0;uintptr_t vertex=0,wrapper=0,texture=0;
        memcpy(&flags,p.header.data()+0x30,4);memcpy(&count,p.header.data()+0x20,4);memcpy(&vertex,p.header.data()+0x28,8);memcpy(&wrapper,p.header.data()+0x10,8);
        UINT layout=flags&15;int slot=-1;UINT stride=0,offset=0;
        if(!Packets::Read(base+0x217b670+layout*8,slot) || slot<0 || slot>2 ||
           !Packets::Read(renderer+0x500+slot*16,stride) || !Packets::Read(renderer+0x504+slot*16,offset) ||
           stride!=(layout==0?40u:layout==4?24u:32u) || !Packets::Read(wrapper+0x68,texture) || !texture ||
           vertex<allocation+offset || (vertex-allocation-offset)%stride)return false;
        uint64_t first=(vertex-allocation-offset)/stride;
        if(first+count>UINT_MAX)return false;
        next.ranges.push_back({texture,layout,static_cast<UINT>(first),count});
    }
    batch=std::move(next);return true;
}
inline void End(){batch={};}
inline void Emit(uintptr_t command)
{
    if(!tracking.load())return;
    // The command pool reuses addresses. Every new submission invalidates an old
    // record at that address, even when this submission is unrelated to NamePlate.
    {std::lock_guard lock(mutex);queued.erase(command);}
    if(!batch.renderer)return;
    auto draw=std::make_shared<Draw>();
    if(!Packets::Read(command,draw->command))return;
    UINT type=0,count=0,start=0;memcpy(&type,draw->command.data(),4);if(type!=6)return;
    memcpy(&count,draw->command.data()+0x18,4);memcpy(&start,draw->command.data()+0x14,4);
    if(!count || count%3 || count>256*1024)return;
    uintptr_t ib=0,cpu=0;UINT flags=0,capacity=0;
    if(!Packets::Read(batch.renderer+0x1e0,ib) || !Packets::Read(ib+0x40,flags) || !Packets::Read(ib+0x38,capacity) ||
       !Packets::Read(ib+((!(flags&0x11) || (flags&0x40))?0x60:0x68),cpu) || !cpu ||
       (uint64_t(start)+count)*batch.indexBytes>capacity)return;
    draw->indices.resize(count);
    if(batch.indexBytes==4){if(!Packets::ReadBytes(cpu+uint64_t(start)*4,draw->indices.data(),size_t(count)*4))return;}
    else
    {
        std::vector<uint16_t> values(count);if(!Packets::ReadBytes(cpu+uint64_t(start)*2,values.data(),size_t(count)*2))return;
        std::copy(values.begin(),values.end(),draw->indices.begin());
    }
    draw->indexBytes=batch.indexBytes;draw->ranges=batch.ranges;draw->time=GetTickCount64();draw->snapshot=batch.snapshot;
    std::lock_guard lock(mutex);
    // Short-lived metadata only. Never retain an executable command pointer.
    for(auto it=queued.begin();it!=queued.end();)if(draw->time-it->second->time>250)it=queued.erase(it);else ++it;
    size_t bytes=draw->indices.size()*4;
    for(const auto& entry:queued)bytes+=entry.second->indices.size()*4;
    if(queued.size()>=256 || bytes>8*1024*1024){++rejected;return;}
    queued[command]=std::move(draw);++emitted;
}
inline void Bind(uintptr_t nativeContext,uintptr_t state)
{
    bound.reset();boundContext=0;if(state<0x20)return;
    std::shared_ptr<Draw> found;
    {std::lock_guard lock(mutex);auto it=queued.find(state-0x20);if(it==queued.end())return;found=std::move(it->second);queued.erase(it);}
    std::array<unsigned char,0xb0> current{};
    if(GetTickCount64()-found->time>100 || !Packets::Read(state-0x20,current) || current!=found->command ||
       !Packets::Read(nativeContext+0x17b8,boundContext)){++rejected;return;}
    bound=std::move(found);++boundCount;
}
inline bool Partition(const Draw& draw,uintptr_t texture,UINT layout,std::vector<UINT>& selected,std::vector<UINT>& remaining)
{
    selected.clear();remaining.clear();
    if(draw.indices.empty() || draw.indices.size()%3)return false;
    for(size_t i=0;i<draw.indices.size();i+=3)
    {
        int hits=0;
        for(size_t j=0;j<3;++j)
        {
            UINT index=draw.indices[i+j];
            hits+=std::any_of(draw.ranges.begin(),draw.ranges.end(),[&](const auto& r){return r.texture==texture && r.layout==layout && index>=r.first && uint64_t(index)<uint64_t(r.first)+r.count;});
        }
        const bool singleRange=std::any_of(draw.ranges.begin(),draw.ranges.end(),[&](const auto& r)
        {return r.texture==texture && r.layout==layout && std::all_of(draw.indices.begin()+i,draw.indices.begin()+i+3,[&](UINT index){return index>=r.first && uint64_t(index)<uint64_t(r.first)+r.count;});});
        if(hits && (hits!=3 || !singleRange))return false; // Never cut or join unrelated primitives.
        auto& out=hits?selected:remaining;out.insert(out.end(),draw.indices.begin()+i,draw.indices.begin()+i+3);
    }
    return !selected.empty();
}
}
