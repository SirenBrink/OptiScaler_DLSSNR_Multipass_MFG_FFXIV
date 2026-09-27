#pragma once
#include "FfxivNameplateRenderer.h"
#include "FfxivNameplateLiveScope.h"
#include "../CompanionPackets.h"
#include "../CompanionMidpoint.h"
#include "../CompanionVisibility.h"
#include "../CompanionImageMidpoint.h"
#include <dxgi1_3.h>
#include <dcomp.h>
#include <memory>
#include <cmath>
#include <chrono>
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"dcomp.lib")
#pragma comment(lib,"ole32.lib")

// An optional presentation layer. Only owned geometry/resources cross threads.
// Never calls game UI updates or uses the game's immediate context from the worker.
namespace FfxivNameplateLive
{
using namespace FfxivNameplateRenderer;
struct TextureSource { uintptr_t identity=0; D3D11_TEXTURE2D_DESC desc{}; std::filesystem::path file; };
struct CpuMaterial
{
    std::pair<uintptr_t,UINT> key;
    Layout layout; std::vector<unsigned char> vs,ps;
    TextureSource texture; D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    D3D11_SAMPLER_DESC sampler{}; D3D11_BLEND_DESC blend{}; D3D11_RASTERIZER_DESC raster{};
    D3D11_VIEWPORT viewport{}; std::vector<D3D11_RECT> scissors;
    FLOAT factor[4]{}; UINT mask=~0u; bool defaultBlend=false,defaultRaster=false;
    std::array<std::filesystem::path,3> vcb; std::array<std::filesystem::path,2> pcb;
};
struct Package { std::vector<CpuMaterial> materials; UINT width=0,height=0; HWND game=nullptr; LUID adapter{}; HMODULE module=nullptr; std::shared_ptr<FfxivCompanion::Visibility::Ring> visibility; };
struct Frame { std::vector<Draw> draws; double time=0; uint64_t sequence=0,identity=0; };
inline std::atomic<bool> midpointEnabled{false},visibilityMode{true};
inline std::atomic<uint64_t> midpointFrames{0},midpointBypass{0},midpointCpuUs{0};
inline std::atomic<bool> requested{false},preparing{false},ready{false},stop{false};
inline std::atomic<ULONGLONG> heartbeat{0},endTime{0},prepareEnd{0};
inline std::atomic<ULONGLONG> durationMs{30000}; // Zero means enabled until stopped.
inline std::atomic<UINT64> updates{0},presents{0},fallbacks{0};
inline std::atomic<double> layerRate{0},gameRate{0};
inline std::mutex mutex;
inline HANDLE worker=nullptr;
inline std::shared_ptr<const Frame> latest,previous;
inline std::vector<std::pair<uintptr_t,UINT>> supported;
inline std::string status="Not running";
inline double Seconds() { LARGE_INTEGER t,f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f); return double(t.QuadPart)/f.QuadPart; }
inline void Status(std::string value) { std::lock_guard lock(mutex); status=std::move(value); }
inline std::string Status() { std::lock_guard lock(mutex); return status; }
inline void Stop() { stop.store(true); ready.store(false); preparing.store(false); requested.store(false); }
inline bool WantFrame() { return ready.load() && !stop.load() && (!endTime.load() || GetTickCount64()<endTime.load()); }
inline bool Request(ULONGLONG duration=30000)
{
    std::lock_guard lock(mutex);
    if(preparing.load()) return false; // Repeated clicks must not restart material capture.
    if(worker && WaitForSingleObject(worker,0)!=WAIT_OBJECT_0)
    {status="Previous layer is still stopping; retry shortly";return false;}
    if(worker){CloseHandle(worker);worker=nullptr;}
    latest.reset();previous.reset();supported.clear();
    updates=0;presents=0;fallbacks=0;layerRate=0;gameRate=0;
    midpointFrames=0;midpointBypass=0;midpointCpuUs=0;
    durationMs=duration;endTime=0;
    stop=false;ready=false;requested=true;preparing=true;prepareEnd=GetTickCount64()+15000;
    status="Preparing native geometry and materials; native drawing remains active";
    return true;
}
inline void Poll()
{
    if(preparing.load() && GetTickCount64()>prepareEnd.load()) {Stop();Status("Preparation timed out; native nameplates active");}
}
inline std::vector<unsigned char> ReadFile(const std::filesystem::path& path)
{
    std::ifstream file(path,std::ios::binary); if(!file) throw std::runtime_error("Missing captured material file");
    file.seekg(0,std::ios::end); auto size=file.tellg(); if(size<0 || size>64*1024*1024) throw std::runtime_error("Invalid material file size");
    std::vector<unsigned char> bytes(static_cast<size_t>(size)); file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
    if(!file) throw std::runtime_error("Material read failed");return bytes;
}
inline void Check(HRESULT hr) { if(FAILED(hr)) throw std::runtime_error("Live layer DirectX failure: "+std::to_string(static_cast<unsigned>(hr))); }
inline HWND FindGameWindow()
{
    struct Find { HWND result=nullptr; LONG area=0; } find;
    EnumWindows([](HWND w,LPARAM data)->BOOL
    {
        DWORD pid=0;GetWindowThreadProcessId(w,&pid);RECT r{};auto& f=*reinterpret_cast<Find*>(data);
        if(pid==GetCurrentProcessId() && IsWindowVisible(w) && !GetWindow(w,GW_OWNER) && GetClientRect(w,&r))
        { LONG area=(r.right-r.left)*(r.bottom-r.top); if(area>f.area) {f.result=w;f.area=area;} }
        return TRUE;
    },reinterpret_cast<LPARAM>(&find));return find.result;
}
// The game still builds NamePlate packets once per real frame. At the verified
// native batch consumer, substitute a temporary pointer list with just those
// packets omitted. No cached vertices, packet headers or queue counts are edited.
inline thread_local std::shared_ptr<Frame> pending;
inline thread_local std::vector<uintptr_t> pendingSources;
inline void Stage(const FfxivNameplatePackets::Capture& capture,uint64_t sequence=0,uint64_t identitySignature=0)
{
    pending.reset();pendingSources.clear();
    if(!WantFrame())return;
    auto frame=std::make_shared<Frame>();frame->time=Seconds();frame->sequence=sequence;frame->identity=identitySignature;
    std::lock_guard lock(mutex);
    if(!capture.ready || capture.rejected || capture.packets.size()>512) {++fallbacks;latest.reset();return;}
    size_t bytes=0;
    for(const auto& p:capture.packets)
    {
        uintptr_t wrapper=0,identity=0;memcpy(&wrapper,p.header.data()+16,8);
        const UINT stride=p.layout==0?40:p.layout==4?24:32;bytes+=p.vertices.size();
        uint16_t extension=0;memcpy(&extension,p.header.data()+4,2);
        if(!p.source || p.error || p.kind!=0x22 || extension || !p.count || p.count%4 || p.count>65536 || p.stride!=stride || p.vertices.size()!=size_t(p.count)*stride || bytes>8*1024*1024 ||
           !FfxivNameplatePackets::Read(wrapper+0x68,identity) || std::find(supported.begin(),supported.end(),std::make_pair(identity,p.layout))==supported.end())
        {++fallbacks;latest.reset();pendingSources.clear();return;}
        frame->draws.push_back({p,identity});pendingSources.push_back(p.source);
    }
    std::stable_sort(frame->draws.begin(),frame->draws.end(),[](const Draw&a,const Draw&b){return a.packet.key<b.packet.key;});pending=std::move(frame);
}
struct BatchFilter
{
    uintptr_t owner=0,original=0;
    std::vector<std::array<uintptr_t,2>> entries;
    FfxivCompanion::Packets::PointerScope scope;
    BatchFilter(uintptr_t renderer)
    {
        if(!pending)return;
        auto frame=pending;
        UINT count=0;
        if(!WantFrame() || Seconds()-frame->time>0.05 || GetTickCount64()-heartbeat.load()>100 ||
           !FfxivNameplatePackets::Read(renderer+0x580,original) || !FfxivNameplatePackets::Read(renderer+0x588,count) || count>8192)
        {Cancel();return;}
        entries.resize(count);
        if(count && !FfxivNameplatePackets::ReadBytes(original,entries.data(),entries.size()*sizeof(entries[0]))) {Cancel();return;}
        size_t found=0;std::vector<bool> seen(pendingSources.size(),false);
        for(auto& entry:entries)
        {
            const auto source=std::find(pendingSources.begin(),pendingSources.end(),entry[1]);
            if(source==pendingSources.end())continue;
            const auto sourceIndex=static_cast<size_t>(source-pendingSources.begin());
            if(seen[sourceIndex]){Cancel();return;}seen[sourceIndex]=true;
            const auto packet=std::find_if(frame->draws.begin(),frame->draws.end(),[&](const Draw& d){return d.packet.source==entry[1];});
            std::array<unsigned char,64> live{};
            if(packet==frame->draws.end() || !FfxivNameplatePackets::Read(entry[1],live) || live!=packet->packet.header)
            {Cancel();return;}
            entry[1]=0;++found;
        }
        if(!found && !pendingSources.empty()) return; // Different/offscreen renderer; wait for the owning batch.
        if(found!=pendingSources.size()) {Cancel();return;}
        std::lock_guard lock(mutex);
        if(!WantFrame()){latest.reset();pending.reset();return;}
        uintptr_t replacement=reinterpret_cast<uintptr_t>(entries.data());
        if(!scope.Enter(renderer+0x580,original,replacement)) {++fallbacks;latest.reset();return;}
        owner=renderer;pending.reset();
        previous=latest;latest=std::move(frame);++updates;
    }
    BatchFilter(const BatchFilter&)=delete;
    BatchFilter& operator=(const BatchFilter&)=delete;
    static void Cancel(){pending.reset();++fallbacks;std::lock_guard lock(mutex);latest.reset();}
    ~BatchFilter()=default; // PointerScope restores the original list on every exit.
};
inline LRESULT CALLBACK WindowProc(HWND w,UINT message,WPARAM a,LPARAM b)
{
    if(message==WM_NCHITTEST) return HTTRANSPARENT;
    if(message==WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(w,message,a,b);
}
struct Window
{
    HWND value=nullptr; HINSTANCE instance=nullptr;
    ~Window(){if(value)DestroyWindow(value);if(instance)UnregisterClassW(L"OptiScalerNameplateLiveTest",instance);}
};
struct Handle { HANDLE value=nullptr; ~Handle(){if(value)CloseHandle(value);} };
inline bool BuildMidpoint(const Frame& old,const Frame& current,const std::map<std::pair<uintptr_t,UINT>,Material>& materials,
                          std::vector<std::vector<unsigned char>>& vertices)
{
    if(!old.identity || current.identity!=old.identity || old.draws.size()!=current.draws.size() || current.draws.empty())return false;
    vertices.resize(current.draws.size());
    for(size_t i=0;i<current.draws.size();++i)
    {
        const auto& a=old.draws[i];const auto& b=current.draws[i];const auto& p=a.packet;const auto& q=b.packet;
        if(a.identity!=b.identity || p.key!=q.key || p.layout!=q.layout || p.count!=q.count || p.stride!=q.stride ||
           memcmp(p.header.data(),q.header.data(),0x28) || memcmp(p.header.data()+0x30,q.header.data()+0x30,0x10))return false;
        const auto material=materials.find({b.identity,q.layout});if(material==materials.end())return false;
        const auto& layout=material->second.layout;
        const Element* position=nullptr;
        for(UINT e=0;e<layout.count;++e)
            if(layout.elements[e].index==0 && !_stricmp(layout.elements[e].semantic,"POSITION"))
            {if(position)return false;position=&layout.elements[e];}
        if(!position || (position->format!=DXGI_FORMAT_R32G32_FLOAT && position->format!=DXGI_FORMAT_R32G32B32_FLOAT &&
                        position->format!=DXGI_FORMAT_R32G32B32A32_FLOAT))return false;
        if(!FfxivCompanion::Midpoint::Positions(p.vertices,q.vertices,q.stride,position->offset,vertices[i]))return false;
    }
    return true;
}
inline void Run(Package& package)
{
    FfxivNameplateLiveScope::Guard ownLayer;
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ComPtr<IDXGIFactory2> factory;Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> candidate;if(factory->EnumAdapters1(i,&candidate)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 desc{};candidate->GetDesc1(&desc);if(desc.AdapterLuid.HighPart==package.adapter.HighPart && desc.AdapterLuid.LowPart==package.adapter.LowPart){adapter=candidate;break;}}
    if(!adapter)throw std::runtime_error("Game adapter not found");
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level;
    Check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context));
    FfxivCompanion::Visibility::Consumer visibility;
    ComPtr<ID3D11DeviceContext1> regionContext;context.As(&regionContext);
    if(package.visibility && !visibility.Init(device.Get(),*package.visibility))throw std::runtime_error("Cannot open shared native visibility layer");
    std::map<std::pair<uintptr_t,UINT>,Material> materials;
    for(const auto& source:package.materials)
    {
        Material m;m.layout=source.layout;m.viewport=source.viewport;m.scissors=source.scissors;m.sampleMask=source.mask;memcpy(m.blendFactor,source.factor,sizeof(m.blendFactor));
        Check(device->CreateVertexShader(source.vs.data(),source.vs.size(),nullptr,&m.vs));Check(device->CreatePixelShader(source.ps.data(),source.ps.size(),nullptr,&m.ps));
        std::array<D3D11_INPUT_ELEMENT_DESC,16> il{};
        for(UINT i=0;i<source.layout.count;++i){auto&e=source.layout.elements[i];il[i]={e.semantic,e.index,e.format,0,e.offset,D3D11_INPUT_PER_VERTEX_DATA,0};}
        Check(device->CreateInputLayout(il.data(),source.layout.count,source.vs.data(),source.vs.size(),&m.input));
        auto pixels=ReadFile(source.texture.file);auto td=source.texture.desc;td.MipLevels=1;td.ArraySize=1;td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;td.CPUAccessFlags=td.MiscFlags=0;
        if(pixels.size()!=size_t(td.Width)*td.Height*4)throw std::runtime_error("Unexpected atlas dimensions");
        D3D11_SUBRESOURCE_DATA init{pixels.data(),td.Width*4,0};ComPtr<ID3D11Texture2D> texture;Check(device->CreateTexture2D(&td,&init,&texture));
        auto srv=source.view;srv.Texture2D.MipLevels=1;Check(device->CreateShaderResourceView(texture.Get(),&srv,&m.texture));
        Check(device->CreateSamplerState(&source.sampler,&m.sampler));if(!source.defaultBlend)Check(device->CreateBlendState(&source.blend,&m.blend));if(!source.defaultRaster)Check(device->CreateRasterizerState(&source.raster,&m.raster));
        auto cb=[&](const std::filesystem::path& file,ComPtr<ID3D11Buffer>& buffer){if(file.empty())return;auto data=ReadFile(file);D3D11_BUFFER_DESC bd{};bd.ByteWidth=static_cast<UINT>(data.size());bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;bd.Usage=D3D11_USAGE_IMMUTABLE;D3D11_SUBRESOURCE_DATA initial{data.data(),0,0};Check(device->CreateBuffer(&bd,&initial,&buffer));};
        for(UINT i=0;i<3;++i)cb(source.vcb[i],m.vertexConstants[i]);for(UINT i=0;i<2;++i)cb(source.pcb[i],m.pixelConstants[i]);materials.emplace(source.key,std::move(m));
    }
    if(stop.load())return;
    Window window;window.instance=package.module;WNDCLASSW cls{};cls.hInstance=window.instance;cls.lpfnWndProc=WindowProc;cls.lpszClassName=L"OptiScalerNameplateLiveTest";
    if(!RegisterClassW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error("Cannot register layer window");
    RECT rect{};GetClientRect(package.game,&rect);POINT origin{};ClientToScreen(package.game,&origin);
    if(UINT(rect.right)!=package.width || UINT(rect.bottom)!=package.height)throw std::runtime_error("Client size and captured target differ; test declined");
    window.value=CreateWindowExW(WS_EX_LAYERED|WS_EX_NOREDIRECTIONBITMAP|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|WS_EX_TOPMOST,cls.lpszClassName,L"OptiScaler nameplate refresh test",WS_POPUP,origin.x,origin.y,rect.right,rect.bottom,package.game,nullptr,window.instance,nullptr);
    if(!window.value)throw std::runtime_error("Cannot create layer window");
    // HTTRANSPARENT alone only searches windows on the same thread. The game
    // window belongs to another thread: layered + transparent supplies OS mouse
    // pass-through without forwarding/injecting clicks or disabling the window.
    if(!SetLayeredWindowAttributes(window.value,0,255,LWA_ALPHA))
        throw std::runtime_error("Cannot enable layer mouse pass-through");
    DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=package.width;sd.Height=package.height;sd.Format=DXGI_FORMAT_B8G8R8A8_UNORM;sd.SampleDesc.Count=1;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=2;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;sd.AlphaMode=DXGI_ALPHA_MODE_PREMULTIPLIED;sd.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    ComPtr<IDXGISwapChain1> swap;Check(factory->CreateSwapChainForComposition(device.Get(),&sd,nullptr,&swap));ComPtr<IDXGISwapChain2> swap2;Check(swap.As(&swap2));Check(swap2->SetMaximumFrameLatency(1));Handle pacing{swap2->GetFrameLatencyWaitableObject()};if(!pacing.value)throw std::runtime_error("No layer pacing handle");
    ComPtr<IDXGIDevice> dxgi;Check(device.As(&dxgi));ComPtr<IDCompositionDevice> composition;Check(DCompositionCreateDevice(dxgi.Get(),IID_PPV_ARGS(&composition)));
    ComPtr<IDCompositionTarget> target;ComPtr<IDCompositionVisual> visual;Check(composition->CreateTargetForHwnd(window.value,TRUE,&target));Check(composition->CreateVisual(&visual));Check(visual->SetContent(swap.Get()));Check(target->SetRoot(visual.Get()));Check(composition->Commit());
    ComPtr<ID3D11Texture2D> back;Check(swap->GetBuffer(0,IID_PPV_ARGS(&back)));ComPtr<ID3D11RenderTargetView> rtv;Check(device->CreateRenderTargetView(back.Get(),nullptr,&rtv));
    D3D11_DEPTH_STENCIL_DESC dd{};ComPtr<ID3D11DepthStencilState> depth;Check(device->CreateDepthStencilState(&dd,&depth));
    D3D11_BUFFER_DESC vbDesc{};vbDesc.ByteWidth=65536*40;vbDesc.BindFlags=D3D11_BIND_VERTEX_BUFFER;vbDesc.Usage=D3D11_USAGE_DYNAMIC;vbDesc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;ComPtr<ID3D11Buffer> vb;Check(device->CreateBuffer(&vbDesc,nullptr,&vb));
    std::vector<UINT> indices;for(UINT i=0;i<65536;i+=4)for(UINT off:{0u,1u,2u,0u,2u,3u})indices.push_back(i+off);D3D11_BUFFER_DESC ibDesc{};ibDesc.ByteWidth=static_cast<UINT>(indices.size()*4);ibDesc.BindFlags=D3D11_BIND_INDEX_BUFFER;ibDesc.Usage=D3D11_USAGE_IMMUTABLE;D3D11_SUBRESOURCE_DATA ii{indices.data(),0,0};ComPtr<ID3D11Buffer> ib;Check(device->CreateBuffer(&ibDesc,&ii,&ib));
    const auto duration=durationMs.load();auto end=duration?GetTickCount64()+duration:0;
    endTime=end;heartbeat=GetTickCount64();
    {std::lock_guard lock(mutex);supported.clear();for(auto&[key,m]:materials)supported.push_back(key);status="HUD replacement active";}
    LOG_INFO("FFXIV nameplate live: independent replacement active; optional 2x positional midpoint, no extrapolation");
    ShowWindow(window.value,SW_SHOWNOACTIVATE);preparing=false;ready=true;
    UINT64 lastPresents=0,lastUpdates=0;double lastReport=Seconds();
    FfxivCompanion::Midpoint::Gate midpointGate;
    FfxivCompanion::Midpoint::Gate visibilityGate;
    std::array<uint64_t,6> timingSkips{};
    uint64_t missingGuides=0,noSafeMovement=0;
    std::vector<std::vector<unsigned char>> midpointVertices;
    double previousPresentStart=0;
    while(!stop.load() && (!end || GetTickCount64()<end) && IsWindow(package.game))
    {
        if(WaitForSingleObject(pacing.value,50)!=WAIT_OBJECT_0)continue;
        MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
        GetClientRect(package.game,&rect);POINT position{};ClientToScreen(package.game,&position);
        if(UINT(rect.right)!=package.width || UINT(rect.bottom)!=package.height){LOG_INFO("FFXIV nameplate live: resolution changed; restoring native layer");break;}
        if(position.x!=origin.x || position.y!=origin.y)
        {
            SetWindowPos(window.value,HWND_TOPMOST,position.x,position.y,rect.right,rect.bottom,
                         SWP_NOACTIVATE|SWP_NOSENDCHANGING|SWP_ASYNCWINDOWPOS);
            origin=position;
        }
        DWORD fgPid=0;GetWindowThreadProcessId(GetForegroundWindow(),&fgPid);
        if(fgPid!=GetCurrentProcessId() || IsIconic(package.game)){LOG_INFO("FFXIV nameplate live: game lost focus or minimized; restoring native layer");break;}
        heartbeat=GetTickCount64();
        std::shared_ptr<const Frame> now,old;{std::lock_guard lock(mutex);now=latest;old=previous;}
        const double time=Seconds();double age=now?time-now->time:1;
        const double presentInterval=previousPresentStart?time-previousPresentStart:0;previousPresentStart=time;
        bool midpoint=false;
        if(now && old)
        {
            const bool first=midpointGate.seen!=now->sequence;
            if(midpointGate.Offer(now->sequence,old->sequence,now->time-old->time,presentInterval,age,midpointEnabled.load()))
            {
                const auto begin=Seconds();
                midpoint=BuildMidpoint(*old,*now,materials,midpointVertices);
                midpointCpuUs+=static_cast<uint64_t>((Seconds()-begin)*1000000.0);
            }
            if(first && midpointEnabled.load() && !midpoint)++midpointBypass;
        }
        else midpointGate.Reset();
        auto rt=rtv.Get();context->OMSetRenderTargets(1,&rt,nullptr);FLOAT clear[4]{};context->ClearRenderTargetView(rt,clear);context->OMSetDepthStencilState(depth.Get(),0);
        if(package.visibility)
        {
            context->OMSetRenderTargets(0,nullptr,nullptr);
            if(visibility.Update(context.Get(),*package.visibility))
            {
                context->CopyResource(back.Get(),visibility.image.Get());
                const bool first=visibilityGate.seen!=visibility.serial;
                const double imageAge=double(GetTickCount64()-visibility.time)/1000;
                age=imageAge;
                const double interval=visibility.time>=visibility.previousTime?double(visibility.time-visibility.previousTime)/1000:0;
                const bool offered=visibilityGate.Offer(visibility.serial,visibility.previousSerial,interval,presentInterval,imageAge,midpointEnabled.load());
                const bool guides=regionContext && visibility.snapshot && visibility.previousSnapshot &&
                   visibility.snapshot->frame.width==package.width && visibility.snapshot->frame.height==package.height;
                if(offered && guides)
                {
                    const auto begin=Seconds();
                    auto moves=FfxivCompanion::ImageMidpoint::Build(*visibility.previousSnapshot,*visibility.snapshot);
                    if(!moves.empty())
                    {
                        FfxivCompanion::ImageMidpoint::Render(regionContext.Get(),visibility.image.Get(),back.Get(),rtv.Get(),moves);
                        midpoint=true;
                    }
                    else ++noSafeMovement;
                    midpointCpuUs+=static_cast<uint64_t>((Seconds()-begin)*1000000);
                }
                if(first && midpointEnabled.load() && !midpoint)
                {
                    ++midpointBypass;
                    if(!offered)++timingSkips[static_cast<size_t>(visibilityGate.rejection)];
                    else if(!guides)++missingGuides;
                }
            }
            else visibilityGate.Reset();
        }
        else if(now && age<=0.1)
        {
            for(size_t i=0;i<now->draws.size();++i)
            {
                auto& draw=now->draws[i];auto& p=draw.packet;auto& m=materials.at({draw.identity,p.layout});
                const auto& vertices=midpoint?midpointVertices[i]:p.vertices;
                D3D11_MAPPED_SUBRESOURCE mapped{};Check(context->Map(vb.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped));memcpy(mapped.pData,vertices.data(),vertices.size());context->Unmap(vb.Get(),0);
                auto v=vb.Get();UINT offset=0;context->IASetVertexBuffers(0,1,&v,&p.stride,&offset);context->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R32_UINT,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->IASetInputLayout(m.input.Get());
                context->VSSetShader(m.vs.Get(),nullptr,0);context->PSSetShader(m.ps.Get(),nullptr,0);ID3D11Buffer* vc[]{m.vertexConstants[0].Get(),m.vertexConstants[1].Get(),m.vertexConstants[2].Get()};ID3D11Buffer* pc[]{m.pixelConstants[0].Get(),m.pixelConstants[1].Get()};context->VSSetConstantBuffers(0,3,vc);context->PSSetConstantBuffers(0,2,pc);
                auto tex=m.texture.Get();auto sampler=m.sampler.Get();context->PSSetShaderResources(0,1,&tex);context->PSSetSamplers(0,1,&sampler);context->OMSetBlendState(m.blend.Get(),m.blendFactor,m.sampleMask);context->RSSetState(m.raster.Get());context->RSSetViewports(1,&m.viewport);context->RSSetScissorRects(static_cast<UINT>(m.scissors.size()),m.scissors.data());context->DrawIndexed(p.count/4*6,0,0);
            }
        }
        // Present the independent composition chain with a full update. Present1
        // alone does not isolate third-party hooks: Layer also requires the tested
        // OptiFG presenter path. On failure, restore native drawing without retrying
        // through a different presentation entry.
        const DXGI_PRESENT_PARAMETERS presentParameters{};
        const auto presentResult=swap->Present1(1,0,&presentParameters);
        if(presents.load()==0 || FAILED(presentResult))
            LOG_INFO("Companion HUD Present1: result {:X}, first {}",static_cast<unsigned>(presentResult),presents.load()==0);
        Check(presentResult);++presents;
        if(package.visibility && visibility.serial)
        {visibilityGate.Presented(visibility.serial,midpoint);if(midpoint)++midpointFrames;}
        else if(now && age<=0.1) {midpointGate.Presented(now->sequence,midpoint);if(midpoint)++midpointFrames;}
        else midpointGate.Reset();
        if(time-lastReport>=1)
        {
            auto presentCount=presents.load(),updateCount=updates.load();double seconds=time-lastReport;
            layerRate=(presentCount-lastPresents)/seconds;gameRate=(updateCount-lastUpdates)/seconds;
            DXGI_FRAME_STATISTICS stats{};auto feedback=swap->GetFrameStatistics(&stats);
            LOG_INFO("FFXIV nameplate live: layer presents={:.1f}/s native updates={:.1f}/s age={:.1f}ms fallbacks={} displayFeedback={:X} presentCount={} refreshCount={}",(presentCount-lastPresents)/seconds,(updateCount-lastUpdates)/seconds,age*1000,fallbacks.load(),static_cast<unsigned>(feedback),stats.PresentCount,stats.PresentRefreshCount);
            LOG_INFO("Companion 2x midpoint: enabled {} shown {} bypass {} build-CPU-total {} us observed-layer-interval {:.3f} ms ordered-snapshot {}",
                midpointEnabled.load(),midpointFrames.load(),midpointBypass.load(),midpointCpuUs.load(),presentInterval*1000,
                package.visibility?(visibility.snapshot?visibility.snapshot->frame.sequence:0):(now?now->sequence:0));
            if(package.visibility)LOG_INFO("Companion visibility: image {} age {} ms GPU span including intervening HUD {:.3f} ms valid {}",
                visibility.serial,visibility.serial?GetTickCount64()-visibility.time:0,package.visibility->gpuMs.load(),package.visibility->validSerial.load());
            if(package.visibility)LOG_INFO("Companion midpoint skips: sequence {} endpoint {} cadence {} age {} guides {} no-safe-movement {}",
                timingSkips[2],timingSkips[3],timingSkips[4],timingSkips[5],missingGuides,noSafeMovement);
            lastPresents=presentCount;lastUpdates=updateCount;lastReport=time;
        }
    }
    ready=false;if(package.visibility)package.visibility->validSerial=0;ShowWindow(window.value,SW_HIDE);
    Check(visual->SetContent(nullptr));Check(composition->Commit());
}
inline DWORD WINAPI Worker(void* data)
{
    HMODULE module=nullptr;
    {
        std::unique_ptr<Package> package(static_cast<Package*>(data));module=package->module;
        const auto comResult=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        try {Run(*package);Status("Native nameplates active");}
        catch(const std::exception& e){LOG_WARN("FFXIV nameplate live: {}",e.what());Status(std::string("Native nameplates active: ")+e.what());}
        ready=false;preparing=false;
        if(SUCCEEDED(comResult))CoUninitialize();
        LOG_INFO("FFXIV nameplate live: finished layerPresents={} nativeUpdates={} fallbacks={}",presents.load(),updates.load(),fallbacks.load());
    }
    FreeLibraryAndExitThread(module,0);
}
inline bool LaunchVisibility(const std::shared_ptr<FfxivCompanion::Visibility::Ring>& ring)
{
    if(!preparing.load() || stop.load())return false;
    std::lock_guard lock(mutex);
    if(worker)return true;
    auto package=std::make_unique<Package>();package->visibility=ring;package->width=ring->width;package->height=ring->height;
    package->adapter=ring->adapter;package->game=FindGameWindow();
    if(!package->game || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(&Worker),&package->module))return false;
    auto module=package->module;auto ptr=package.release();worker=CreateThread(nullptr,0,Worker,ptr,0,nullptr);
    if(!worker){delete ptr;FreeLibrary(module);return false;}
    requested=false;return true;
}
inline void Launch(ID3D11DeviceContext* context,const Session& session,const std::vector<TextureSource>& textures,const std::filesystem::path& directory,const GUID& pixelCodeTag)
{
    if(!preparing.load() || stop.load())return;
    requested=false;
    try
    {
        if(!session.saved)
        {
            std::string reason=session.status;
            if(!session.failures.empty())reason=session.failures.begin()->second;
            throw std::runtime_error("Replacement unavailable: "+reason+". Native nameplates retained.");
        }
        auto package=std::make_unique<Package>();package->width=session.width;package->height=session.height;package->game=FindGameWindow();if(!package->game)throw std::runtime_error("Game window unavailable");
        ComPtr<ID3D11Device> device;context->GetDevice(&device);ComPtr<IDXGIDevice> dxgi;Check(device.As(&dxgi));ComPtr<IDXGIAdapter> adapter;Check(dxgi->GetAdapter(&adapter));DXGI_ADAPTER_DESC ad{};Check(adapter->GetDesc(&ad));package->adapter=ad.AdapterLuid;
        UINT index=0;
        for(const auto& [key,m]:session.materials)
        {
            CpuMaterial cpu;cpu.key=key;cpu.layout=m.layout;cpu.vs=m.vertexCode;UINT size=0;Check(m.ps->GetPrivateData(pixelCodeTag,&size,nullptr));if(!size || size>256*1024)throw std::runtime_error("Missing pixel shader");cpu.ps.resize(size);Check(m.ps->GetPrivateData(pixelCodeTag,&size,cpu.ps.data()));
            auto texture=std::find_if(textures.begin(),textures.end(),[&](const TextureSource& t){return t.identity==key.first;});if(texture==textures.end())throw std::runtime_error("Missing live atlas");cpu.texture=*texture;
            m.texture->GetDesc(&cpu.view);m.sampler->GetDesc(&cpu.sampler);if(cpu.sampler.MaxLOD>0 && texture->desc.MipLevels>1)throw std::runtime_error("Mipmapped atlas not supported by live test");
            cpu.defaultBlend=!m.blend;cpu.defaultRaster=!m.raster;if(m.blend)m.blend->GetDesc(&cpu.blend);if(m.raster)m.raster->GetDesc(&cpu.raster);cpu.viewport=m.viewport;cpu.scissors=m.scissors;cpu.mask=m.sampleMask;memcpy(cpu.factor,m.blendFactor,sizeof(cpu.factor));
            for(UINT i=0;i<3;++i)if(m.vertexConstants[i])cpu.vcb[i]=directory/("material-"+std::to_string(index)+"-vs-cb"+std::to_string(i)+".bin");for(UINT i=0;i<2;++i)if(m.pixelConstants[i])cpu.pcb[i]=directory/("material-"+std::to_string(index)+"-ps-cb"+std::to_string(i)+".bin");
            package->materials.push_back(std::move(cpu));++index;
        }
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(&Worker),&package->module))throw std::runtime_error("Cannot retain module for layer lifetime");
        auto module=package->module;auto ptr=package.release();
        std::lock_guard lock(mutex);worker=CreateThread(nullptr,0,Worker,ptr,0,nullptr);
        if(!worker){delete ptr;FreeLibrary(module);throw std::runtime_error("Cannot start layer thread");}
        // Keep preparing true until the worker owns a ready presentation layer.
    }
    catch(const std::exception& e){preparing=false;requested=false;Status(e.what());LOG_WARN("FFXIV nameplate live: {}",e.what());}
}
}
