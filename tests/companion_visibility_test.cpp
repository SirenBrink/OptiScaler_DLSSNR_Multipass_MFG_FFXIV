#define NOMINMAX
#include <Windows.h>
#include <cassert>
#include <cstdio>
#include <d3dcompiler.h>
#include "../OptiScaler/misc/companion/CompanionVisibilityQueue.h"
#include "../OptiScaler/misc/companion/CompanionContextIdentity.h"
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"d3dcompiler.lib")
using namespace FfxivCompanion::Visibility;
int main()
{
    ComPtr<ID3D11Device> d,consumerDevice;ComPtr<ID3D11DeviceContext> c,cc;D3D_FEATURE_LEVEL level;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,&level,&c)));
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&consumerDevice,&level,&cc)));
    namespace Identity=FfxivCompanion::ContextIdentity;
    auto contextToken=Identity::Stamp(c.Get());
    assert(contextToken && Identity::Matches(c.Get(),contextToken));
    assert(!Identity::Matches(cc.Get(),contextToken));
    auto nextToken=Identity::Stamp(c.Get());
    assert(nextToken!=contextToken && !Identity::Matches(c.Get(),contextToken) && Identity::Matches(c.Get(),nextToken));
    ComPtr<ID3D11DeviceContext> deferred;assert(SUCCEEDED(d->CreateDeferredContext(0,&deferred)));
    assert(!Identity::Stamp(deferred.Get()));
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=8;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> native;ComPtr<ID3D11RenderTargetView> rtv;assert(SUCCEEDED(d->CreateTexture2D(&td,nullptr,&native)));assert(SUCCEEDED(d->CreateRenderTargetView(native.Get(),nullptr,&rtv)));
    td.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;ComPtr<ID3D11DepthStencilView> dsv;assert(SUCCEEDED(d->CreateTexture2D(&td,nullptr,&depth)));assert(SUCCEEDED(d->CreateDepthStencilView(depth.Get(),nullptr,&dsv)));
    auto r=rtv.Get();c->OMSetRenderTargets(1,&r,dsv.Get());FLOAT background[4]{0,0,1,1};c->ClearRenderTargetView(r,background);c->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,0.5f,0);
    D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=TRUE;dd.DepthFunc=D3D11_COMPARISON_GREATER_EQUAL;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
    ComPtr<ID3D11DepthStencilState> ds;assert(SUCCEEDED(d->CreateDepthStencilState(&dd,&ds)));c->OMSetDepthStencilState(ds.Get(),0);
    D3D11_BLEND_DESC bd{};auto& b=bd.RenderTarget[0];b.BlendEnable=TRUE;b.SrcBlend=D3D11_BLEND_SRC_ALPHA;b.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOp=D3D11_BLEND_OP_ADD;b.SrcBlendAlpha=D3D11_BLEND_ONE;b.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOpAlpha=D3D11_BLEND_OP_ADD;b.RenderTargetWriteMask=15;
    ComPtr<ID3D11BlendState> blend;assert(SUCCEEDED(d->CreateBlendState(&bd,&blend)));c->OMSetBlendState(blend.Get(),nullptr,~0u);
    const char* shader="cbuffer C:register(b0){float4 data;} float4 vs(uint i:SV_VertexID):SV_Position{float2 p=float2((i<<1)&2,i&2);return float4(p*float2(2,-2)+float2(-1,1),data.x,1);} float4 ps():SV_Target{return float4(1,0,0,.5);}";
    ComPtr<ID3DBlob> vsCode,psCode,error;assert(SUCCEEDED(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&vsCode,&error)));assert(SUCCEEDED(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&psCode,&error)));
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;assert(SUCCEEDED(d->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs)));assert(SUCCEEDED(d->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps)));c->VSSetShader(vs.Get(),nullptr,0);c->PSSetShader(ps.Get(),nullptr,0);
    D3D11_BUFFER_DESC cbDesc{};cbDesc.ByteWidth=16;cbDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ComPtr<ID3D11Buffer> cb;assert(SUCCEEDED(d->CreateBuffer(&cbDesc,nullptr,&cb)));auto rawCb=cb.Get();c->VSSetConstantBuffers(0,1,&rawCb);
    D3D11_VIEWPORT viewport{0,0,8,8,0,1};c->RSSetViewports(1,&viewport);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    auto draw=[](void* p){static_cast<ID3D11DeviceContext*>(p)->Draw(3,0);};
    std::shared_ptr<Ring> ring;Producer producer(ring);FLOAT parameters[4]{0.25f,0,0,0};c->UpdateSubresource(cb.Get(),0,nullptr,parameters,0,0);
    assert(producer.Draw(c.Get(),draw,c.Get())); // occluded: must contribute nothing
    parameters[0]=0.75f;c->UpdateSubresource(cb.Get(),0,nullptr,parameters,0,0);assert(producer.Draw(c.Get(),draw,c.Get()));
    auto metadata=std::make_shared<FfxivCompanion::Snapshot>();metadata->frame.sequence=42;
    producer.slot->snapshot=metadata;
    assert(producer.Finish(true));c->Flush();
    ComPtr<ID3D11RenderTargetView> restored;ComPtr<ID3D11DepthStencilView> restoredDepth;c->OMGetRenderTargets(1,&restored,&restoredDepth);assert(restored.Get()==rtv.Get() && restoredDepth.Get()==dsv.Get());
    Consumer consumer;assert(consumer.Init(consumerDevice.Get(),*ring));bool received=false;
    for(int i=0;i<2000 && !received;++i){received=consumer.Update(cc.Get(),*ring);if(!received)Sleep(1);}assert(received);cc->Flush();
    auto pixel=[](ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* texture)
    {D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> staging;assert(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&staging)));context->CopyResource(staging.Get(),texture);D3D11_MAPPED_SUBRESOURCE mapped{};assert(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)));std::array<unsigned char,4> p{};memcpy(p.data(),static_cast<unsigned char*>(mapped.pData)+4*mapped.RowPitch+4*4,4);context->Unmap(staging.Get(),0);return p;};
    auto out=pixel(consumerDevice.Get(),cc.Get(),consumer.image.Get());assert(out[0]==0 && out[1]==0 && out[2]>=127 && out[2]<=128 && out[3]>=127 && out[3]<=128);
    auto nativePixel=pixel(d.Get(),c.Get(),native.Get());assert(nativePixel[0]==255 && nativePixel[1]==0 && nativePixel[2]==0 && nativePixel[3]==255);
    assert(consumer.snapshot==metadata && consumer.snapshot->frame.sequence==42);
    ring->validSerial=0;assert(!consumer.Update(cc.Get(),*ring)); // no stale overlay after native fallback
    assert(!consumer.snapshot && !consumer.previousSnapshot);
    // A consumer holding both slots cannot stall the producer or overwrite them.
    ring->slots[0].state=3;ring->slots[1].state=3;assert(!ring->Acquire(c.Get()));ring->slots[0].state=ring->slots[1].state=0;
    dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;ComPtr<ID3D11DepthStencilState> writing;assert(SUCCEEDED(d->CreateDepthStencilState(&dd,&writing)));c->OMSetDepthStencilState(writing.Get(),0);
    Producer reject(ring);assert(!reject.Draw(c.Get(),draw,c.Get()) && reject.failed && !reject.Finish(true));
    namespace Q=FfxivCompanion::VisibilityQueue;
    Q::Draw command;command.indices={0,1,2, 4,5,6, 2,3,0};command.ranges={{123,4,0,4}};
    std::vector<UINT> selected,remaining;
    assert(Q::Partition(command,123,4,selected,remaining));assert((selected==std::vector<UINT>{0,1,2,2,3,0}));assert((remaining==std::vector<UINT>{4,5,6}));
    assert(!Q::Partition(command,999,4,selected,remaining));assert(!Q::Partition(command,123,5,selected,remaining));
    command.indices={0,1,4};assert(!Q::Partition(command,123,4,selected,remaining));
    command.ranges.push_back({123,4,4,4});assert(!Q::Partition(command,123,4,selected,remaining));
    command.indices={0};assert(!Q::Partition(command,123,4,selected,remaining));
    // Live queue binding accepts only unchanged, recent commands, once.
    std::array<unsigned char,0xb0> bytes{};auto data=std::make_shared<Q::Draw>();data->command=bytes;data->time=GetTickCount64();
    auto address=reinterpret_cast<uintptr_t>(bytes.data());std::array<unsigned char,0x1800> nativeContext{};uintptr_t ctx=reinterpret_cast<uintptr_t>(c.Get());memcpy(nativeContext.data()+0x17b8,&ctx,8);
    Q::queued[address]=data;Q::Bind(reinterpret_cast<uintptr_t>(nativeContext.data()),address+0x20);assert(Q::bound==data && Q::boundContext==ctx);
    Q::Bind(reinterpret_cast<uintptr_t>(nativeContext.data()),address+0x20);assert(!Q::bound);
    Q::queued[address]=data;bytes[0]=1;Q::Bind(reinterpret_cast<uintptr_t>(nativeContext.data()),address+0x20);assert(!Q::bound);bytes[0]=0;
    data->time-=1000;Q::queued[address]=data;Q::Bind(reinterpret_cast<uintptr_t>(nativeContext.data()),address+0x20);assert(!Q::bound);
    Q::queued[address]=data;Q::tracking=true;Q::Emit(address);assert(Q::queued.empty());Q::tracking=false;
    // Native capture -> queued index ownership, including 16/32-bit source data.
    std::array<unsigned char,0x600> renderer{};std::array<unsigned char,0x80> engineIb{},wrapper{};
    std::array<unsigned char,128> table{};std::array<unsigned char,24*12> vertices{};
    auto put=[](auto& bytes,size_t offset,const auto& value){memcpy(bytes.data()+offset,&value,sizeof(value));};
    uintptr_t allocation=reinterpret_cast<uintptr_t>(vertices.data()),ibAddress=reinterpret_cast<uintptr_t>(engineIb.data()),texture=123;
    put(renderer,0x570,allocation);put(renderer,0x1e0,ibAddress);int slot=1;put(table,4*8,slot);UINT stride=24,zero=0;put(renderer,0x510,stride);put(renderer,0x514,zero);put(wrapper,0x68,texture);
    FfxivCompanion::Packets::Packet packet;packet.original=1;UINT kind=0x22,layout=4,vertexCount=4;uintptr_t vertex=allocation+4*24,wrap=reinterpret_cast<uintptr_t>(wrapper.data());
    put(packet.header,0,kind);put(packet.header,0x10,wrap);put(packet.header,0x20,vertexCount);put(packet.header,0x28,vertex);put(packet.header,0x30,layout);
    Q::tracking=true;
    for(UINT width:{2u,4u})
    {
        assert(Q::Begin(reinterpret_cast<uintptr_t>(table.data())-0x217b670,reinterpret_cast<uintptr_t>(renderer.data()),width,{packet}));
        Q::batch.snapshot=metadata;
        assert(Q::batch.ranges[0].first==4 && Q::batch.ranges[0].count==4 && Q::batch.ranges[0].texture==123);
        std::array<uint16_t,9> index16{9,9,9,4,5,6,6,7,4};std::array<UINT,9> large{9,9,9,4,5,6,6,7,4};
        uintptr_t cpu=width==2?reinterpret_cast<uintptr_t>(index16.data()):reinterpret_cast<uintptr_t>(large.data());UINT capacity=9*width;put(engineIb,0x38,capacity);put(engineIb,0x60,cpu);
        bytes={};UINT type=6,count=6,start=3;put(bytes,0,type);put(bytes,0x14,start);put(bytes,0x18,count);Q::Emit(address);Q::End();
        assert(Q::queued.contains(address));auto captured=Q::queued.at(address);assert((captured->indices==std::vector<UINT>{4,5,6,6,7,4}));
        index16.fill(0);large.fill(0);assert(captured->indices[0]==4); // owns data beyond queue storage lifetime
        assert(captured->snapshot==metadata);
        assert(Q::Partition(*captured,123,4,selected,remaining) && remaining.empty());Q::queued.clear();
    }
    Q::tracking=false;
    puts("PASS: native depth occlusion, premultiplied transparency, cross-device GPU transport, untouched native target, OM restoration, busy/stale guards, depth-write rejection queue identity/age checks and mixed-HUD triangle partitioning");
}
