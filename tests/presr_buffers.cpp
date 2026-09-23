// WARP differential regression: original copies versus the production optimized
// capture/rotation fragments, across alternating frames, resets and fallback.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <memory>
#include <vector>
#include <cstdio>
#include <stdexcept>
#include <cmath>
#include <shaders/dlssnr/PreSrTiming.h>
#include <shaders/dlssnr/PreSrCadence.h>
#include <shaders/dlssnr/PreSrSplitSchedule.h>
#include <optional>
using Microsoft::WRL::ComPtr;
void check(HRESULT h) { if (FAILED(h)) throw std::runtime_error("D3D12 failure"); }
void expect(bool v, const char* why) { if (!v) throw std::runtime_error(why); }
void Barrier(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    if (a == b) return;
    D3D12_RESOURCE_BARRIER barrier {}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    c->ResourceBarrier(1, &barrier);
}
struct HalfRate {
    struct Split {ID3D12Resource *depth{},*motion{},*thirdScene{};bool guidesReadable=false,sceneReadable[3]{};float sceneScale[3]{1,1,1};int camera=0;DlssNr::PreSrSplitSchedule schedule;} split;
    int camera=42;
    ID3D12Resource *motion {}, *previousMotion {}, *history[2] {}; bool motionReadable=false, previousReadable=false, historyReadable[2] {}; unsigned writeIndex=0;
};
struct Generation { bool trailGuard=true; std::unique_ptr<HalfRate> half; ID3D12Resource* clean {}; ID3D12Device* device{};unsigned w=4,h=2,outW=4,outH=2;DXGI_FORMAT outputFormat=DXGI_FORMAT_R32G32B32A32_FLOAT; };
struct Config {std::optional<int> DepthResourceBarrier;static Config* Instance(){static Config c;return &c;}};
ID3D12Resource* CreateScratch(ID3D12Device* device,DXGI_FORMAT format,unsigned w,unsigned h) {
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;
    d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=format;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;ID3D12Resource* r=nullptr;
    check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&r)));return r;
}
#include "presr-buffer-production.inl"
int main() try
{
    DlssNr::PreSrCadence cadence;
    expect(cadence.Get().samples==0,"uninitialized cadence returned samples");
    uint64_t tick=1000;cadence.Record(tick,1000000,DlssNr::PreSrCadence::Ordinary);
    for(unsigned i=0;i<300;++i) {
        tick+=i%2?10000:20000;
        cadence.Record(tick,1000000,i%2?DlssNr::PreSrCadence::Skipped:DlssNr::PreSrCadence::Anchor);
    }
    cadence.Record(tick,1000000,DlssNr::PreSrCadence::Skipped); // Duplicate/invalid markers ignored.
    cadence.Record(tick-1,1000000,DlssNr::PreSrCadence::Skipped);
    cadence.Record(tick+1000,0,DlssNr::PreSrCadence::Skipped);
    const auto stats=cadence.Get();
    expect(stats.samples==128 && stats.anchors==64 && stats.skips==64 && stats.anchor==20 && stats.skipped==10 &&
           stats.median==15 && stats.p95==20 && stats.maximum==20,"GPU cadence phase/frequency/rolling-window statistics failed");
    DlssNr::PreSrSplitSchedule schedule;
    for(unsigned cycle=0;cycle<3;++cycle) {
        unsigned anchor=0,midpoint=0;std::array<unsigned,3> scenes{};
        for(unsigned f=0;f<32;++f) {
            bool skip=f%2!=0;auto epoch=uint64_t(cycle*100+f+1);
            expect(DlssNr::PreSrSplitSchedule::Evaluate(true,skip)==skip &&
                   DlssNr::PreSrSplitSchedule::Evaluate(false,skip)!=skip,"SR evaluated on wrong phase");
            if(!skip) schedule.Queue(epoch);
            else {
                expect(schedule.CanResolve(epoch) && !schedule.CanResolve(epoch+1),"Staged anchor accepted stale/missing frame");
                anchor=f-1;midpoint=anchor?anchor-1:0;schedule.Resolved();
            }
            scenes[schedule.write]=f;
            const auto base=scenes[schedule.Output()];
            expect(base==(f<2?0:f-2),"Split output scene order changed");
            if(schedule.HasEdit()) expect((schedule.UseMidpoint(skip)?midpoint:anchor)==base,"Residual belongs to different scene");
            schedule.Advance();
        }
        schedule.Queue(1000);schedule.Reset();
        expect(!schedule.CanResolve(1001) && !schedule.HasEdit() && schedule.write==0,"Reset retained queued work or output history");
    }
    // Execute the production reference-selection block with distinct scene IDs.
    {
        Generation g;g.half=std::make_unique<HalfRate>();
        g.half->history[0]=reinterpret_cast<ID3D12Resource*>(10);
        g.half->history[1]=reinterpret_cast<ID3D12Resource*>(20);
        g.half->split.thirdScene=reinterpret_cast<ID3D12Resource*>(30);
        auto* latest=reinterpret_cast<ID3D12Resource*>(40);
        float exposure=0;
        expect(SelectRejectionProduction(g,false,10,latest,4,exposure)==latest&&exposure==4,
               "One-frame rejection reference is not the clean current anchor");
        for(unsigned write=0;write<3;++write){
            auto& sp=g.half->split;sp.schedule.write=write;
            sp.sceneScale[0]=1;sp.sceneScale[1]=2;sp.sceneScale[2]=3;
            const unsigned anchor=(write+2)%3;
            auto* expected=anchor==2?sp.thirdScene:g.half->history[anchor];
            expect(SelectRejectionProduction(g,true,10,latest,4,exposure)==expected&&exposure==float(anchor+1),
                   "Split rejection reference is not the prior clean A anchor");
        }
        expect(!SelectRejectionProduction(g,true,6,latest,4,exposure),"Real anchor enabled midpoint rejection");
        g.trailGuard=false;expect(!SelectRejectionProduction(g,true,10,latest,4,exposure),"Disabled guard selected a reference");
    }
    ComPtr<ID3D12Debug> debug; check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> adapter; ComPtr<ID3D12Device> device;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory))); check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> messages; check(device.As(&messages));
    ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC q {};
    check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> cmd;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&cmd)));
    std::vector<ComPtr<ID3D12Resource>> owned;
    auto texture = [&]() {
        D3D12_RESOURCE_DESC d {}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=4; d.Height=2;
        d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1; d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
        d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES h {}; h.Type=D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> r;
        check(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&r)));
        owned.push_back(r); return r.Get();
    };
    auto buffer = [&](D3D12_HEAP_TYPE type, UINT64 bytes) {
        D3D12_RESOURCE_DESC d {}; d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; d.Width=bytes;
        d.Height=d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1; d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES h {}; h.Type=type; ComPtr<ID3D12Resource> r;
        check(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,type==D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)));
        owned.push_back(r); return r.Get();
    };
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout {}; UINT64 bytes=0;
    auto* input=texture(); auto desc=input->GetDesc(); device->GetCopyableFootprints(&desc,0,1,0,&layout,nullptr,nullptr,&bytes);
    auto upload = [&](unsigned frame) {
        auto* r=buffer(D3D12_HEAP_TYPE_UPLOAD,bytes); void* mapped=nullptr; check(r->Map(0,nullptr,&mapped));
        for (unsigned y=0;y<2;++y) for(unsigned x=0;x<4;++x) for(unsigned ch=0;ch<4;++ch)
            reinterpret_cast<float*>(static_cast<char*>(mapped)+y*layout.Footprint.RowPitch)[x*4+ch]=float(frame*100+y*20+x*4+ch);
        r->Unmap(0,nullptr); return r;
    };
    auto write = [&](ID3D12Resource* target, ID3D12Resource* source, D3D12_RESOURCE_STATES prior) {
        Barrier(cmd.Get(),target,prior,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION from {},to {}; from.pResource=source; from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; from.PlacedFootprint=layout;
        to.pResource=target; to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cmd->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        Barrier(cmd.Get(),target,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    };
    struct Result { ID3D12Resource* resource; unsigned frame; };
    std::vector<Result> results;
    auto read = [&](ID3D12Resource* source, unsigned frame) {
        auto* r=buffer(D3D12_HEAP_TYPE_READBACK,bytes);
        Barrier(cmd.Get(),source,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from {},to {}; from.pResource=source; from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=r; to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint=layout;
        cmd->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        Barrier(cmd.Get(),source,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        results.push_back({r,frame});
    };
    Generation g; g.clean=texture(); g.half=std::make_unique<HalfRate>();
    auto& h=*g.half; h.motion=texture(); h.previousMotion=texture(); h.history[0]=texture(); h.history[1]=texture();
    auto* oldClean=texture(); auto* oldHistory0=texture(); auto* oldHistory1=texture();
    ID3D12Resource* oldHistory[2] {oldHistory0,oldHistory1}; bool oldReadable[2] {};
    DlssNr::PreSrTiming timer; expect(timer.Init(device.Get(),queue.Get()),"timing init failed");
    bool previous=false; unsigned previousFrame=0;
    for(unsigned f=1;f<=12;++f)
    {
        DlssNr::PreSrTiming::Scope cost(timer,cmd.Get(),f-1,DlssNr::PreSrTiming::Compose);
        // Cover transition into/out of half-rate mode and scene reset.
        bool half=f!=5 && f!=6; if(f==4 || !half || f==7) previous=false;
        auto* pixels=upload(f);
        write(input,pixels,f==1 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmd.Get(),input,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
        // Original two-copy history path.
        Barrier(cmd.Get(),oldClean,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(oldClean,input);
        Barrier(cmd.Get(),oldClean,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if(half) {
            Barrier(cmd.Get(),oldClean,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(cmd.Get(),oldHistory[h.writeIndex],oldReadable[h.writeIndex] ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyResource(oldHistory[h.writeIndex],oldClean);
            Barrier(cmd.Get(),oldHistory[h.writeIndex],D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cmd.Get(),oldClean,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            oldReadable[h.writeIndex]=true;
        }
        auto* cleanTarget=CaptureProduction(g,cmd.Get(),input,half);
        auto* base=half && previous ? h.history[1-h.writeIndex] : cleanTarget;
        auto* oldBase=half && previous ? oldHistory[1-h.writeIndex] : oldClean;
        read(base,half && previous ? previousFrame : f); read(oldBase,half && previous ? previousFrame : f);
        Barrier(cmd.Get(),oldClean,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if(!half) Barrier(cmd.Get(),g.clean,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        else {
            h.historyReadable[h.writeIndex]=true;
            write(h.motion,pixels,h.motionReadable ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            h.motionReadable=true;
            RotateProduction(h);
            read(h.previousMotion,f); // Next frame's guide must be exactly the prior normalized field.
            h.writeIndex=1-h.writeIndex;
        }
        previous=half; previousFrame=f;
        Barrier(cmd.Get(),input,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cost.End(); cost.End(); // Explicit end followed by destructor must resolve only once.
    }
    expect(timer.Get(DlssNr::PreSrTiming::Compose).count==0,"uncompleted samples consumed");
    // Execute the production anchor snapshot, then overwrite the game's inputs.
    // Deferred consumers must still see the original depth/motion.
    Generation splitGen;splitGen.device=device.Get();splitGen.half=std::make_unique<HalfRate>();
    splitGen.half->history[0]=texture();splitGen.half->history[1]=texture();
    ComPtr<ID3D12Resource> depthSource;depthSource.Attach(CreateScratch(device.Get(),DXGI_FORMAT_R32_FLOAT,4,2));
    auto depthDesc=depthSource->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT depthLayout{};UINT64 depthBytes=0;
    device->GetCopyableFootprints(&depthDesc,0,1,0,&depthLayout,nullptr,nullptr,&depthBytes);
    auto writeDepth=[&](float value,D3D12_RESOURCE_STATES prior) {
        auto* data=buffer(D3D12_HEAP_TYPE_UPLOAD,depthBytes);void* mapped=nullptr;check(data->Map(0,nullptr,&mapped));
        for(unsigned y=0;y<2;++y) for(unsigned x=0;x<4;++x)
            reinterpret_cast<float*>(static_cast<char*>(mapped)+y*depthLayout.Footprint.RowPitch)[x]=value;
        data->Unmap(0,nullptr);
        Barrier(cmd.Get(),depthSource.Get(),prior,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=data;from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=depthLayout;
        to.pResource=depthSource.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cmd->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        Barrier(cmd.Get(),depthSource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    };
    writeDepth(7,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    expect(SnapshotSplitGuides(splitGen,cmd.Get(),depthSource.Get(),input),"Production guide snapshot failed");
    expect(splitGen.half->split.camera==42,"Anchor camera not retained");
    writeDepth(99,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    write(input,upload(99),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    read(splitGen.half->split.motion,12); // Previous source, not overwritten frame 99.
    auto* depthReadback=buffer(D3D12_HEAP_TYPE_READBACK,depthBytes);
    Barrier(cmd.Get(),splitGen.half->split.depth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION depthFrom{},depthTo{};
    depthFrom.pResource=splitGen.half->split.depth;depthFrom.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    depthTo.pResource=depthReadback;depthTo.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;depthTo.PlacedFootprint=depthLayout;
    cmd->CopyTextureRegion(&depthTo,0,0,0,&depthFrom,nullptr);
    Barrier(cmd.Get(),splitGen.half->split.depth,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto& splitState=splitGen.half->split;
    for(unsigned f=0;f<12;++f) {
        write(input,upload(200+f),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmd.Get(),input,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
        CaptureProduction(splitGen,cmd.Get(),input,true,true);
        splitState.sceneReadable[splitState.schedule.write]=true;
        unsigned index=splitState.schedule.Output();
        read(index==2?splitState.thirdScene:splitGen.half->history[index],200+(f<2?0:f-2));
        splitState.schedule.Advance();
        Barrier(cmd.Get(),input,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    check(cmd->Close()); ID3D12CommandList* lists[]{cmd.Get()}; queue->ExecuteCommandLists(1,lists);
    ComPtr<ID3D12Fence> fence; check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    HANDLE event=CreateEvent(nullptr,FALSE,FALSE,nullptr); check(queue->Signal(fence.Get(),1)); check(fence->SetEventOnCompletion(1,event));
    expect(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"GPU timeout"); CloseHandle(event);
    for(unsigned i=0;i<12;++i) { timer.Recycle(i); timer.Recycle(i); }
    const auto sample=timer.Get(DlssNr::PreSrTiming::Compose);
    expect(sample.count==DlssNr::PreSrTiming::SampledSlots && std::isfinite(sample.ms) && sample.ms>=0,"invalid timing sample count/value");
    expect(timer.Get(DlssNr::PreSrTiming::Fg).count==0,"skipped FG counted as evaluated");
    void* depthMapped=nullptr;check(depthReadback->Map(0,nullptr,&depthMapped));
    for(unsigned y=0;y<2;++y) for(unsigned x=0;x<4;++x)
        expect(reinterpret_cast<float*>(static_cast<char*>(depthMapped)+y*depthLayout.Footprint.RowPitch)[x]==7,"Deferred depth aliased overwritten game input");
    depthReadback->Unmap(0,nullptr);
    for(const auto& result:results) {
        void* mapped=nullptr; check(result.resource->Map(0,nullptr,&mapped));
        for(unsigned y=0;y<2;++y) for(unsigned x=0;x<4;++x) for(unsigned ch=0;ch<4;++ch)
            expect(reinterpret_cast<float*>(static_cast<char*>(mapped)+y*layout.Footprint.RowPitch)[x*4+ch]==float(result.frame*100+y*20+x*4+ch),"history/motion image differs");
        result.resource->Unmap(0,nullptr);
    }
    for(UINT64 i=0;i<messages->GetNumStoredMessages();++i) {
        SIZE_T n=0; messages->GetMessage(i,nullptr,&n); std::vector<char> storage(n); auto* m=reinterpret_cast<D3D12_MESSAGE*>(storage.data()); check(messages->GetMessage(i,m,&n));
        if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) { std::puts(m->pDescription); throw std::runtime_error("D3D12 validation error"); }
    }
    splitState.depth->Release();splitState.motion->Release();splitState.thirdScene->Release();
    std::puts("PreSR WARP: legacy buffers, split scene sequence, owned depth/motion snapshots, reset/gap schedule, cadence and timing passed; no debug-layer errors");
    return 0;
} catch(const std::exception& e) { std::puts(e.what()); return 1; }
