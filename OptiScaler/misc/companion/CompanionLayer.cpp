#include "pch.h"
#include "CompanionLayer.h"
#include "CompanionNative.h"
#include "CompanionGpu.h"
#include "CompanionRuntime.h"
#include "CompanionVisibilityQueue.h"
#include "CompanionContextIdentity.h"
#include "replay/FfxivNameplateGpu.h"
#include <imgui/imgui.h>
#include <Config.h>
#include <State.h>
#include <hooks/FG_Hooks.h>

namespace FfxivCompanion::Layer
{
static std::atomic<bool> captureNext {false};
static thread_local uint64_t beforeSequence = 0;
static std::atomic<std::shared_ptr<Visibility::Ring>> visibilityRing;
// Draw submission and Present may run on different game threads. Protect the
// frame handoff; the independent presentation worker never enters this scope.
static std::mutex visibilityFrameMutex;
static std::shared_ptr<Visibility::Ring> frameRing;
static std::unique_ptr<Visibility::Producer> frameProducer;
static Microsoft::WRL::ComPtr<ID3D11Buffer> visibilityIndices;
static UINT visibilityIndexCapacity=0;
static std::atomic<uint64_t> visibilityCpuUs{0},visibilityDraws{0};
static thread_local uint64_t boundContextToken=0;
static std::array<std::atomic<uint64_t>,5> visibilityRejects{};
static std::atomic<uint64_t> forwardedContexts{0};
static bool managedRequested=false; // Updated by the rendering-thread Tick only.
static ULONGLONG nextRetry=0;
static thread_local std::shared_ptr<const Snapshot> visibilitySnapshot;
static bool HasHudFgPresenter()
{
    const auto& state=State::Instance();
    return (state.activeFgOutput==FGOutput::DLSSG || state.activeFgOutput==FGOutput::XeFG) &&
        state.currentFGSwapchain && !FGHooks::IsDx12InteropPresentSC(state.currentFGSwapchain);
}
void BeginSnapshot()
{
    if (!FfxivNameplateLive::WantFrame()) return;
    Snapshot snapshot; Status status;
    mailbox.Read(snapshot,status,Now()); beforeSequence=status.sequence;
}
void Stop()
{
    VisibilityQueue::tracking=false;
    if(auto ring=visibilityRing.load())ring->validSerial=0;
    captureNext.store(false);
    FfxivNameplateLive::Stop();
    FfxivNameplateGpu::Stop();
    FfxivNameplateLive::Status("Native drawing enabled; replacement stopped");
}
void InvalidateFrame()
{
    if(auto ring=visibilityRing.load())ring->validSerial=0;
    if(FfxivNameplateLive::WantFrame()) FfxivNameplateLive::BatchFilter::Cancel();
}
void TagLayout(ID3D11InputLayout* layout,const D3D11_INPUT_ELEMENT_DESC* desc,UINT count,const void* code,SIZE_T size)
{ FfxivNameplateRenderer::TagLayout(layout,desc,count,code,size); }
void TagPixel(ID3D11PixelShader* shader,const void* code,SIZE_T size)
{
    if (shader && Gpu::Identify(code,size)) shader->SetPrivateData(FfxivNameplateGpu::shaderCodeTag,static_cast<UINT>(size),code);
}
void Capture(const Packets::Queue& before)
{
    namespace Live = FfxivNameplateLive;
    if(Live::visibilityMode.load())
    {
        visibilitySnapshot.reset();
        if(Live::preparing.load() || Live::WantFrame())
        {
            auto next=std::make_shared<Snapshot>();Status status;
            if(mailbox.Read(*next,status,Now()) && status.ageQpc<=Frequency()/20)visibilitySnapshot=std::move(next);
        }
        return;
    }
    if (!Live::preparing.load() && !Live::WantFrame()) { captureNext.store(false); return; }
    if (!Live::WantFrame() && !captureNext.load()) return;
    try
    {
        auto after=FfxivNameplatePackets::ReadQueue(before.address);
        auto begin=after; begin.address=before.address; begin.count=before.count; begin.head=before.head;
        auto data=FfxivNameplatePackets::Copy(begin,after);
        if (captureNext.load() && data.ready && !data.rejected && !data.packets.empty())
        {
            captureNext.store(false);
            FfxivNameplateGpu::Arm(data);
            const auto folder=std::filesystem::path(L"OptiScaler_CompanionLayer") /
                (std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
            // Absolute game directory; never rely on a launcher-provided cwd.
            wchar_t exe[32768]{}; GetModuleFileNameW(nullptr,exe,32768);
            auto directory=std::filesystem::path(exe).parent_path()/folder;
            std::filesystem::create_directories(directory);
            FfxivNameplateGpu::SetDirectory(directory);
            LOG_INFO("Companion layer: preparing {} native packets in {}",data.packets.size(),directory.string());
        }
        if (Live::WantFrame())
        {
            Snapshot snapshot; Status status;
            uint64_t sequence=0, identity=0;
            // Companion 0.1.2 submits the same draw's metadata BEFORE EndNamePlate.
            // With older plugins the sequence does not advance here: replacement
            // still works, but interpolation is conservatively disabled.
            if (mailbox.Read(snapshot,status,Now()) && status.sequence>beforeSequence && snapshot.frame.count)
            {
                sequence=status.sequence; identity=14695981039346656037ull;
                auto hash=[&](const void* value,size_t size){auto p=static_cast<const unsigned char*>(value);while(size--){identity^=*p++;identity*=1099511628211ull;}};
                hash(&snapshot.frame.count,sizeof(snapshot.frame.count));
                for(UINT i=0;i<snapshot.frame.count;++i)
                {
                    const auto& plate=snapshot.plates[i];
                    hash(&plate.objectId,sizeof(plate.objectId));hash(&plate.slot,sizeof(plate.slot));
                    hash(&plate.flags,sizeof(plate.flags));hash(&plate.nameIcon,sizeof(plate.nameIcon));
                    hash(&plate.markerIcon,sizeof(plate.markerIcon));hash(plate.name,sizeof(plate.name));
                }
            }
            Live::Stage(data,sequence,identity);
        }
    }
    catch (const std::exception& e) { Stop(); LOG_WARN("Companion layer capture failed: {}",e.what()); }
}
bool ReplaceBatch(void(__fastcall* original)(uintptr_t,char),uintptr_t renderer,char mode)
{
    if(FfxivNameplateLive::visibilityMode.load())return false;
    if (!FfxivNameplateLive::WantFrame()) return false;
    std::unique_ptr<FfxivNameplateLive::BatchFilter> filter;
    try { filter=std::make_unique<FfxivNameplateLive::BatchFilter>(renderer); }
    catch (...) { FfxivNameplateLive::Stop(); return false; }
    if (!filter->owner) return false;
    original(renderer,mode);
    return true;
}
bool BeginVisibilityNative(uintptr_t base,uintptr_t renderer,UINT bytes,const std::vector<Packets::Packet>& packets,ULONGLONG capturedAt)
{
    namespace Live=FfxivNameplateLive;
    if(!Live::visibilityMode.load() || (!Live::preparing.load() && !Live::WantFrame()) || GetTickCount64()-capturedAt>50)return false;
    try{
        if(!VisibilityQueue::Begin(base,renderer,bytes,packets))return false;
        VisibilityQueue::batch.snapshot=visibilitySnapshot;return true;
    }catch(...){++VisibilityQueue::rejected;return false;}
}
void EndVisibilityNative(){VisibilityQueue::End();}
void EmitVisibilityCommand(uintptr_t command)
{try{VisibilityQueue::Emit(command);}catch(...){++VisibilityQueue::rejected;}}
void BindVisibilityCommand(uintptr_t context,uintptr_t state)
{
    if(!FfxivNameplateLive::preparing.load() && !FfxivNameplateLive::WantFrame()){VisibilityQueue::bound.reset();return;}
    boundContextToken=0;
    try{
        VisibilityQueue::Bind(context,state);
        if(VisibilityQueue::bound)
            boundContextToken=ContextIdentity::Stamp(reinterpret_cast<ID3D11DeviceContext*>(VisibilityQueue::boundContext));
    }catch(...){VisibilityQueue::bound.reset();++VisibilityQueue::rejected;}
}
bool InterceptVisibilityIndexed(ID3D11DeviceContext* c,UINT n,UINT start,INT baseVertex,void(STDMETHODCALLTYPE* original)(ID3D11DeviceContext*,UINT,UINT,INT))
{
    namespace Live=FfxivNameplateLive;
    auto command=std::move(VisibilityQueue::bound);
    if(!command || (!Live::preparing.load() && !Live::WantFrame()))return false;
    if(reinterpret_cast<uintptr_t>(c)!=VisibilityQueue::boundContext)
    {
        if(!ContextIdentity::Matches(c,boundContextToken)){++visibilityRejects[0];return false;}
        ++forwardedContexts;
    }
    UINT savedCount=0,savedStart=0;INT savedBase=0;
    memcpy(&savedCount,command->command.data()+0x18,4);memcpy(&savedStart,command->command.data()+0x14,4);memcpy(&savedBase,command->command.data()+8,4);
    if(n!=savedCount || start!=savedStart || baseVertex!=savedBase || GetTickCount64()-command->time>100){++VisibilityQueue::rejected;return false;}
    const double begin=Live::Seconds();
    try
    {
        using Microsoft::WRL::ComPtr;
        std::lock_guard frameLock(visibilityFrameMutex);
        ComPtr<ID3D11PixelShader> ps;c->PSGetShader(&ps,nullptr,nullptr);UINT shader=0,size=sizeof(shader);
        if(!ps || FAILED(ps->GetPrivateData(Gpu::shaderTag,&size,&shader)) || (shader!=1 && shader!=5 && shader!=6 && shader!=7)){++visibilityRejects[1];return false;}
        UINT layout=shader-1;ComPtr<ID3D11ShaderResourceView> srv;c->PSGetShaderResources(0,1,&srv);
        ComPtr<ID3D11Resource> resource;if(!srv){++visibilityRejects[2];return false;}srv->GetResource(&resource);
        std::vector<UINT> selected,remaining;
        if(!VisibilityQueue::Partition(*command,reinterpret_cast<uintptr_t>(resource.Get()),layout,selected,remaining)){++visibilityRejects[3];return false;}
        ComPtr<ID3D11Buffer> nativeIb;DXGI_FORMAT format;UINT offset;c->IAGetIndexBuffer(&nativeIb,&format,&offset);
        if(!nativeIb || format!=(command->indexBytes==2?DXGI_FORMAT_R16_UINT:DXGI_FORMAT_R32_UINT)){++visibilityRejects[4];return false;}
        D3D11_BUFFER_DESC desc{};nativeIb->GetDesc(&desc);
        if(uint64_t(offset)+(uint64_t(start)+n)*command->indexBytes>desc.ByteWidth)return false;
        ComPtr<ID3D11Device> device;c->GetDevice(&device);
        if(!visibilityIndices || visibilityIndexCapacity<n*4)
        {
            D3D11_BUFFER_DESC d{};d.ByteWidth=n*4;d.BindFlags=D3D11_BIND_INDEX_BUFFER;d.Usage=D3D11_USAGE_DYNAMIC;d.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
            visibilityIndices.Reset();if(FAILED(device->CreateBuffer(&d,nullptr,&visibilityIndices)))return false;visibilityIndexCapacity=d.ByteWidth;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};if(FAILED(c->Map(visibilityIndices.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped)))return false;
        memcpy(mapped.pData,selected.data(),selected.size()*4);if(!remaining.empty())memcpy(static_cast<char*>(mapped.pData)+selected.size()*4,remaining.data(),remaining.size()*4);c->Unmap(visibilityIndices.Get(),0);
        if(!frameProducer){frameRing=visibilityRing.load();frameProducer=std::make_unique<Visibility::Producer>(frameRing);}
        frameProducer->failed=false;
        struct Restore {ID3D11DeviceContext* c;ID3D11Buffer* b;DXGI_FORMAT f;UINT o;~Restore(){c->IASetIndexBuffer(b,f,o);}} restore{c,nativeIb.Get(),format,offset};
        c->IASetIndexBuffer(visibilityIndices.Get(),DXGI_FORMAT_R32_UINT,0);
        struct Args {ID3D11DeviceContext* c;UINT count;INT base;decltype(original) call;} args{c,static_cast<UINT>(selected.size()),baseVertex,original};
        const bool captured=frameProducer->Draw(c,[](void* p){auto& a=*static_cast<Args*>(p);a.call(a.c,a.count,0,a.base);},&args);
        if(captured && frameProducer->slot)
        {
            auto& snapshot=frameProducer->slot->snapshot;
            if(frameProducer->draws==1)snapshot=command->snapshot;
            else if(!command->snapshot || !snapshot || snapshot->frame.sequence!=command->snapshot->frame.sequence)snapshot.reset();
        }
        visibilityRing.store(frameRing);
        if(!captured)
        {
            static thread_local std::string lastReason;
            if(lastReason!=frameProducer->reason){LOG_WARN("Companion visibility draw fallback: {}",frameProducer->reason);lastReason=frameProducer->reason;}
            ++Live::fallbacks;return false;
        }
        if(!Live::WantFrame() || GetTickCount64()-Live::heartbeat.load()>100)return false; // shadow warmup, keep native visible
        if(!remaining.empty())original(c,static_cast<UINT>(remaining.size()),static_cast<UINT>(selected.size()),baseVertex);
        ++VisibilityQueue::partitioned;++visibilityDraws;
        visibilityCpuUs+=static_cast<uint64_t>((Live::Seconds()-begin)*1000000);
        return true;
    }
    catch(const std::exception& e){LOG_WARN("Companion visibility draw failed: {}",e.what());++Live::fallbacks;return false;}
}
void Observe(ID3D11DeviceContext* c)
{
    if (!FfxivNameplateGpu::armed.load() || FfxivNameplateRenderer::executing) return;
    try { FfxivNameplateGpu::Observe(c); }
    catch (const std::exception& e) { Stop(); LOG_WARN("Companion layer materials failed: {}",e.what()); }
}
// Called before publishing the pending frame, on the render thread, so lost
// readiness prevents another launch. The menu changes intent, not GPU resources.
static void SyncControls()
{
    namespace Live=FfxivNameplateLive;
    auto* config=Config::Instance();
    Live::midpointEnabled=config->CompanionHudInterpolation.value_or_default();
    // Also protect diagnostic/timed sessions if their presenter disappears.
    if(!HasHudFgPresenter() || State::Instance().isShuttingDown)
    {
        if(managedRequested || Live::preparing.load() || Live::WantFrame())
        {Native::SetRequested(false);managedRequested=false;nextRetry=0;}
        return;
    }
    const bool wanted=config->CompanionHudReplacement.value_or_default();
    if(!wanted)
    {
        if(managedRequested){Native::SetRequested(false);managedRequested=false;nextRetry=0;}
        return;
    }
    Snapshot snapshot;Status status;
    const bool gameplayReady=mailbox.Read(snapshot,status,Now()) && status.ageQpc<=Frequency()/4 &&
        (snapshot.frame.flags & GameplayReady)!=0;
    if(!gameplayReady)
    {
        if(managedRequested)
        {
            Native::SetRequested(false);managedRequested=false;nextRetry=0;
            visibilitySnapshot.reset();
            LOG_INFO("Companion HUD suspended: waiting for stable gameplay from Companion 0.1.3+");
        }
        return;
    }
    if(Live::preparing.load() || Live::WantFrame() || GetTickCount64()<nextRetry)return;
    if(!Native::GetStatistics().installed || !snapshot.frame.count)return;
    DWORD foreground=0;GetWindowThreadProcessId(GetForegroundWindow(),&foreground);
    if(foreground!=GetCurrentProcessId())return;
    nextRetry=GetTickCount64()+5000;
    if(!Live::Request(0))return;
    managedRequested=true;
    LOG_INFO("Companion HUD preparation authorized by stable gameplay, snapshot {}",snapshot.frame.sequence);
    LOG_INFO("Companion HUD presenter: provider {}, FG swapchain {:X}, dedicated HUD hook bypass enabled",
        State::Instance().activeFgOutput==FGOutput::DLSSG?"DLSS-G":"XeFG",
        reinterpret_cast<uintptr_t>(State::Instance().currentFGSwapchain));
    Native::SetRequested(true);Gpu::requested=false;
    visibilityRing.store(nullptr);
    VisibilityQueue::tracking=true;
    Live::Status("Preparing HUD replacement; native nameplates active");
}
void Tick(ID3D11DeviceContext* c)
{
    FfxivNameplateLive::Poll();
    if(FfxivNameplateLive::visibilityMode.load())
    {
        namespace Live=FfxivNameplateLive;
        std::lock_guard frameLock(visibilityFrameMutex);
        // Revoke readiness before publishing or launching a layer during zoning.
        SyncControls();
        if(frameProducer)
        {
            const bool visible=Live::WantFrame() && GetTickCount64()-Live::heartbeat.load()<=100;
            // Failed individual draws remain native. Earlier successful draws in
            // this same frame still need their visibility layer published.
            frameProducer->failed=false;
            const bool published=frameProducer->Finish(visible);
            if(published)++Live::updates;
            else if(frameRing)frameRing->validSerial=0;
            if(frameRing && Live::preparing.load() && !Live::LaunchVisibility(frameRing))
            {Live::Stop();Live::Status("Shared visibility presentation could not start; native drawing retained");}
            frameProducer.reset();frameRing.reset();
        }
        else if(auto ring=visibilityRing.load())ring->validSerial=0;
        if(!Live::preparing.load() && !Live::WantFrame())
        {VisibilityQueue::tracking=false;visibilityIndices.Reset();visibilityIndexCapacity=0;visibilityRing.store(nullptr);std::lock_guard lock(VisibilityQueue::mutex);VisibilityQueue::queued.clear();}
        static thread_local ULONGLONG reportAt=0;
        if((Live::preparing.load() || Live::WantFrame()) && GetTickCount64()-reportAt>=1000)
        {
            reportAt=GetTickCount64();LOG_INFO("Companion visibility queue: emitted {} bound {} split {} rejected {} CPU-total {} us",
                VisibilityQueue::emitted.load(),VisibilityQueue::boundCount.load(),VisibilityQueue::partitioned.load(),VisibilityQueue::rejected.load(),visibilityCpuUs.load());
            LOG_INFO("Companion visibility handoff: forwarded-context {} rejects context/shader/texture/partition/index {}/{}/{}/{}/{}",
                forwardedContexts.load(),visibilityRejects[0].load(),visibilityRejects[1].load(),visibilityRejects[2].load(),visibilityRejects[3].load(),visibilityRejects[4].load());
        }
        return;
    }
    if (!FfxivNameplateGpu::armed.load()) return;
    try { FfxivNameplateGpu::Tick(c); }
    catch (const std::exception& e) { Stop(); LOG_WARN("Companion layer preparation failed: {}",e.what()); }
}
void DrawSettings()
{
    namespace Live = FfxivNameplateLive;
    auto* config=Config::Instance();
    bool replacement=config->CompanionHudReplacement.value_or_default();
    const bool supported=HasHudFgPresenter();
    ImGui::BeginDisabled(!supported);
    if(ImGui::Checkbox("HUD replacement (nameplates and icons)",&replacement))
    {
        config->CompanionHudReplacement=replacement;
        if(!replacement)Stop();
    }
    ImGui::EndDisabled();
    if(!supported)ImGui::TextWrapped("HUD replacement requires an OptiFG DLSS-G or XeFG presenter. Select the provider and restart the game.");
    bool midpoint=config->CompanionHudInterpolation.value_or_default();
    ImGui::BeginDisabled(!supported);
    if(ImGui::Checkbox("HUD interpolation (2x)",&midpoint))
    {config->CompanionHudInterpolation=midpoint;Live::midpointEnabled=midpoint;}
    ImGui::EndDisabled();
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Adds one intermediate nameplate position. Requires HUD replacement.\nDoes not interpolate the rest of the HUD; may add one overlay refresh of delay.");
    if(replacement && supported)
    {
        if(!Native::GetStatistics().installed)ImGui::TextWrapped("Unavailable for this game build; native nameplates remain active.");
        else ImGui::TextUnformatted(Live::WantFrame()?"Active":Live::preparing.load()?"Preparing; native nameplates active":"Waiting for stable gameplay (Companion 0.1.3+); native nameplates active");
    }
    ImGui::Text("Updates: %llu | Interpolated: %llu | Fallbacks: %llu",
        Live::updates.load(),Live::midpointFrames.load(),Live::fallbacks.load());
}
void DrawDiagnostics()
{
    namespace Live = FfxivNameplateLive;
    ImGui::TextWrapped("Native depth-tested nameplate layer. Optional midpoint interpolation moves isolated nameplate regions between observed positions; no prediction or image blending.");
    ImGui::Text("Midpoints shown: %llu | Bypassed: %llu",Live::midpointFrames.load(),Live::midpointBypass.load());
    ImGui::TextWrapped("One midpoint, then the latest position next refresh. Requires Companion 0.1.2; skips overlapping plates, changed labels, jumps, missed samples and observed layer intervals above 8 ms. Integer-pixel movement preserves glyph sharpness. Can add one overlay refresh of visual delay. Occlusion edges may shift briefly with the current image; input is unchanged.");
    ImGui::BeginDisabled(Config::Instance()->CompanionHudReplacement.value_or_default() || !HasHudFgPresenter());
    if (ImGui::Button("Start depth-tested nameplate replacement (30s)"))
    {
        if (HasHudFgPresenter() && Native::GetStatistics().installed && Live::Request())
        {
            Native::SetRequested(true);Gpu::requested.store(false);
            visibilityRing.store(nullptr);visibilityCpuUs=0;visibilityDraws=0;
            forwardedContexts=0;for(auto& count:visibilityRejects)count=0;
            VisibilityQueue::emitted=0;VisibilityQueue::boundCount=0;VisibilityQueue::partitioned=0;VisibilityQueue::rejected=0;
            VisibilityQueue::tracking=true;
            Live::Status("Preparing live GPU visibility layer; native nameplates active");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Restore native nameplates")) Stop();
    ImGui::EndDisabled();
    ImGui::TextWrapped("%s",Live::Status().c_str());
    ImGui::Text("Layer presents: %.1f/s | Native position updates: %.1f/s",Live::layerRate.load(),Live::gameRate.load());
    ImGui::Text("Replacement frames: %llu | Fallbacks: %llu",Live::updates.load(),Live::fallbacks.load());
    if(auto ring=visibilityRing.load())ImGui::Text("Visibility span GPU: %.3f ms | Split draws: %llu | Split CPU total: %.1f ms",ring->gpuMs.load(),visibilityDraws.load(),visibilityCpuUs.load()/1000.0);
    ImGui::TextWrapped("GPU span includes native HUD work between captured draws; compare game FPS with the test stopped to judge its total cost.");
    ImGui::TextWrapped("No CPU depth readback or waiting for shared textures. Desktop composition may differ in HDR, other UI stacking or FG timing. Use borderless/windowed mode; stop restores native drawing.");
}
}
