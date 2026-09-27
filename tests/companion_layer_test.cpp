#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <map>
#include <cassert>
#include <cstdio>
template<class... T> void IgnoreLog(T&&...) {}
#define LOG_INFO(...) IgnoreLog(__VA_ARGS__)
#define LOG_WARN(...) IgnoreLog(__VA_ARGS__)
#include "../OptiScaler/misc/companion/replay/FfxivNameplateGpu.h"
using namespace FfxivNameplateLive;
int main()
{
    // A rejected depth-tested material must stay rejected, but preserve enough
    // bounded data to explain it. Readback is queried asynchronously in production.
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context)));
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=8;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depthTexture;ComPtr<ID3D11DepthStencilView> dsv;
    assert(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&depthTexture)));
    assert(SUCCEEDED(device->CreateDepthStencilView(depthTexture.Get(),nullptr,&dsv)));
    context->OMSetRenderTargets(0,nullptr,dsv.Get());
    D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=TRUE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;dd.DepthFunc=D3D11_COMPARISON_GREATER_EQUAL;
    ComPtr<ID3D11DepthStencilState> depthState;assert(SUCCEEDED(device->CreateDepthStencilState(&dd,&depthState)));
    context->OMSetDepthStencilState(depthState.Get(),0);
    Material rejected;std::string reason;
    assert(!CaptureMaterial(context.Get(),nullptr,4,rejected,1024,reason));
    assert(reason.starts_with("Depth/stencil"));
    std::array<float,16> projection{};projection[0]=projection[5]=projection[10]=projection[15]=1;
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(projection);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{projection.data(),0,0};ComPtr<ID3D11Buffer> cb;
    assert(SUCCEEDED(device->CreateBuffer(&bd,&initial,&cb)));auto rawCb=cb.Get();context->VSSetConstantBuffers(0,1,&rawCb);
    FfxivNameplateGpu::ProbeDepth(context.Get(),123,4);
    FfxivNameplateGpu::ProbeDepth(context.Get(),123,4);
    assert(FfxivNameplateGpu::depthProbes.size()==1);
    auto& probe=FfxivNameplateGpu::depthProbes.front();assert(probe.fence && probe.projection);
    context->Flush();HRESULT completion=S_FALSE;
    for(int i=0;i<2000 && completion==S_FALSE;++i){completion=context->GetData(probe.fence.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH);if(completion==S_FALSE)Sleep(1);}
    assert(completion==S_OK);D3D11_MAPPED_SUBRESOURCE mapped{};
    assert(SUCCEEDED(context->Map(probe.projection.Get(),0,D3D11_MAP_READ,0,&mapped)));
    assert(memcmp(mapped.pData,projection.data(),sizeof(projection))==0);context->Unmap(probe.projection.Get(),0);
    FfxivNameplateGpu::Stop();assert(FfxivNameplateGpu::depthProbes.empty());
    preparing=true;assert(!Request());preparing=false;
    Session incomplete;incomplete.failures[{1,4}]="Depth/stencil test requires scene visibility";
    preparing=true;stop=false;Launch(context.Get(),incomplete,{}, {},FfxivNameplateGpu::shaderCodeTag);
    assert(Status().find("Depth/stencil test requires scene visibility")!=std::string::npos && !ready && !preparing);
    std::array<unsigned char,64> header{};
    std::array<unsigned char,0x600> renderer{};
    std::vector<std::array<uintptr_t,2>> entries {{{10,reinterpret_cast<uintptr_t>(header.data())},{20,123456}}};
    uintptr_t original=reinterpret_cast<uintptr_t>(entries.data()); UINT count=2;
    memcpy(renderer.data()+0x580,&original,8);memcpy(renderer.data()+0x588,&count,4);
    const auto rendererAddress=reinterpret_cast<uintptr_t>(renderer.data());
    auto arm=[&]{
        ready=true;stop=false;endTime=GetTickCount64()+10000;heartbeat=GetTickCount64();
        pending=std::make_shared<Frame>();pending->time=Seconds();
        Draw d;d.packet.source=reinterpret_cast<uintptr_t>(header.data());d.packet.header=header;
        pending->draws.push_back(d);pendingSources={d.packet.source};latest.reset();
    };
    auto pointer=[&]{uintptr_t value=0;memcpy(&value,renderer.data()+0x580,8);return value;};
    arm();
    {
        BatchFilter filter(rendererAddress); assert(filter.owner==rendererAddress && pointer()!=original && latest);
        auto* list=reinterpret_cast<std::array<uintptr_t,2>*>(pointer());
        assert(list[0][1]==0 && list[0][0]==10 && list[1]==entries[1]);
        assert(entries[0][1]==reinterpret_cast<uintptr_t>(header.data()));
    }
    assert(pointer()==original);
    arm();try{BatchFilter filter(rendererAddress);assert(filter.owner);throw 1;}catch(int){}assert(pointer()==original);
    arm();header[3]^=1;{BatchFilter filter(rendererAddress);assert(!filter.owner && !latest);}assert(pointer()==original);
    arm();pending->time-=1;{BatchFilter filter(rendererAddress);assert(!filter.owner);}assert(pointer()==original);
    arm();entries[1][1]=entries[0][1];{BatchFilter filter(rendererAddress);assert(!filter.owner);}assert(pointer()==original);
    entries[1][1]=123456;
    arm();ready=false;{BatchFilter filter(rendererAddress);assert(!filter.owner);}assert(pointer()==original);
    arm();heartbeat=GetTickCount64()-200;{BatchFilter filter(rendererAddress);assert(!filter.owner);}assert(pointer()==original);
    arm();entries[0][1]=987654;{BatchFilter filter(rendererAddress);assert(!filter.owner && pending);}assert(pointer()==original);
    Stop();assert(!WantFrame());
    // Persistent mode has no 30-second deadline, but readiness, stop and stale
    // heartbeat guards must continue to retain native drawing.
    assert(Request(0));assert(durationMs==0 && endTime==0 && preparing);
    assert(!Request(30000));assert(durationMs==0); // No reset during preparation.
    preparing=false;entries[0][1]=reinterpret_cast<uintptr_t>(header.data());arm();endTime=0;assert(WantFrame());
    {BatchFilter filter(rendererAddress);assert(filter.owner);}assert(pointer()==original);
    heartbeat=GetTickCount64()-200;
    {BatchFilter filter(rendererAddress);assert(!filter.owner);}assert(pointer()==original);
    ready=false;assert(!WantFrame());ready=true;Stop();assert(!WantFrame());
    assert(Request(30000));assert(durationMs==30000);Stop();
    // Production pairing: exact object signature and geometry must agree. Only xy changes.
    Frame oldFrame,newFrame;oldFrame.identity=newFrame.identity=123;
    Draw oldDraw,newDraw;oldDraw.identity=newDraw.identity=456;
    oldDraw.packet.layout=newDraw.packet.layout=4;oldDraw.packet.stride=newDraw.packet.stride=24;
    oldDraw.packet.count=newDraw.packet.count=4;
    std::array<float,24> points {10,10,0,1,0,0, 20,10,0,1,1,0, 20,20,0,1,1,1, 10,20,0,1,0,1};
    oldDraw.packet.vertices.resize(sizeof(points));memcpy(oldDraw.packet.vertices.data(),points.data(),sizeof(points));
    for(size_t i=0;i<points.size();i+=6)points[i]+=4;
    newDraw.packet.vertices.resize(sizeof(points));memcpy(newDraw.packet.vertices.data(),points.data(),sizeof(points));
    oldFrame.draws.push_back(oldDraw);newFrame.draws.push_back(newDraw);
    std::map<std::pair<uintptr_t,UINT>,Material> testMaterials;
    auto& material=testMaterials[{456,4}];material.layout.count=1;
    strcpy_s(material.layout.elements[0].semantic,"POSITION");material.layout.elements[0].format=DXGI_FORMAT_R32G32B32_FLOAT;
    std::vector<std::vector<unsigned char>> midpoint;
    assert(BuildMidpoint(oldFrame,newFrame,testMaterials,midpoint));
    float x;memcpy(&x,midpoint[0].data(),4);assert(x==12);
    newFrame.identity=124;assert(!BuildMidpoint(oldFrame,newFrame,testMaterials,midpoint));newFrame.identity=123;
    newFrame.draws[0].packet.header[0]=1;assert(!BuildMidpoint(oldFrame,newFrame,testMaterials,midpoint));
    assert(WindowProc(nullptr,WM_NCHITTEST,0,0)==HTTRANSPARENT);
    assert(WindowProc(nullptr,WM_MOUSEACTIVATE,0,0)==MA_NOACTIVATE);
    pending.reset();latest.reset();previous.reset();
    puts("PASS: exact-command suppression, unrelated UI preservation, stale/header/duplicate/readiness/heartbeat guards, exception restoration, click-through message handling");
}
