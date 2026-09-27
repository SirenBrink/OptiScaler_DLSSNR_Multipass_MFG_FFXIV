#include "../OptiScaler/misc/companion/CompanionGpu.h"
#include <d3dcompiler.h>
#include <cassert>
#include <cstdio>
#include <vector>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
using namespace FfxivCompanion;
using Microsoft::WRL::ComPtr;
int main()
{
    ComPtr<ID3D11Device> d; ComPtr<ID3D11DeviceContext> c;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,nullptr,&c)));
    const char* vsCode = "struct V{float4 p:SV_Position;float4 c:COLOR;};V main(float2 p:POSITION,float4 c:COLOR){V v;v.p=float4(p,0,1);v.c=c;return v;}";
    const char* psCode = "float4 main(float4 p:SV_Position,float4 c:COLOR):SV_Target{return c;}";
    ComPtr<ID3DBlob> vsBlob, psBlob;
    assert(SUCCEEDED(D3DCompile(vsCode,strlen(vsCode),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&vsBlob,nullptr)));
    assert(SUCCEEDED(D3DCompile(psCode,strlen(psCode),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&psBlob,nullptr)));
    ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps;
    assert(SUCCEEDED(d->CreateVertexShader(vsBlob->GetBufferPointer(),vsBlob->GetBufferSize(),nullptr,&vs)));
    assert(SUCCEEDED(d->CreatePixelShader(psBlob->GetBufferPointer(),psBlob->GetBufferSize(),nullptr,&ps)));
    D3D11_INPUT_ELEMENT_DESC elements[] = {{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA,0}};
    ComPtr<ID3D11InputLayout> layout;
    assert(SUCCEEDED(d->CreateInputLayout(elements,2,vsBlob->GetBufferPointer(),vsBlob->GetBufferSize(),&layout)));
    c->IASetInputLayout(layout.Get()); c->VSSetShader(vs.Get(),nullptr,0); c->PSSetShader(ps.Get(),nullptr,0);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_RASTERIZER_DESC rd {}; rd.FillMode=D3D11_FILL_SOLID; rd.CullMode=D3D11_CULL_NONE;
    ComPtr<ID3D11RasterizerState> raster; assert(SUCCEEDED(d->CreateRasterizerState(&rd,&raster))); c->RSSetState(raster.Get());
    D3D11_TEXTURE2D_DESC td {}; td.Width=td.Height=32; td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM; td.BindFlags=D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> target, staging, texture;
    assert(SUCCEEDED(d->CreateTexture2D(&td,nullptr,&target)));
    assert(SUCCEEDED(d->CreateTexture2D(&td,nullptr,&texture)));
    ComPtr<ID3D11ShaderResourceView> srv; assert(SUCCEEDED(d->CreateShaderResourceView(texture.Get(),nullptr,&srv)));
    auto* srvRaw=srv.Get(); c->PSSetShaderResources(0,1,&srvRaw);
    ComPtr<ID3D11RenderTargetView> rtv; assert(SUCCEEDED(d->CreateRenderTargetView(target.Get(),nullptr,&rtv)));
    auto* rt=rtv.Get(); c->OMSetRenderTargets(1,&rt,nullptr);
    td.BindFlags=0; td.Usage=D3D11_USAGE_STAGING; td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    assert(SUCCEEDED(d->CreateTexture2D(&td,nullptr,&staging)));
    D3D11_VIEWPORT vp {0,0,32,32,0,1}; c->RSSetViewports(1,&vp);
    const float vertices[] = {0,0,0,0,0,0, -1,-1,1,0.5f,0.25f,1, -1,1,1,0.5f,0.25f,1, 1,-1,1,0.5f,0.25f,1};
    const float zero[24] {};
    D3D11_BUFFER_DESC bd {}; bd.ByteWidth=sizeof(vertices); bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data {vertices,0,0}; ComPtr<ID3D11Buffer> vb;
    assert(SUCCEEDED(d->CreateBuffer(&bd,&data,&vb)));
    UINT stride=24, offset=24; auto* vbRaw=vb.Get(); c->IASetVertexBuffers(0,1,&vbRaw,&stride,&offset);
    auto pixels = [&] {
        c->CopyResource(staging.Get(),target.Get()); D3D11_MAPPED_SUBRESOURCE m {};
        assert(SUCCEEDED(c->Map(staging.Get(),0,D3D11_MAP_READ,0,&m)));
        std::vector<uint32_t> out(32*32);
        for(unsigned y=0;y<32;++y) memcpy(out.data()+y*32,static_cast<char*>(m.pData)+y*m.RowPitch,128);
        c->Unmap(staging.Get(),0); return out;
    };
    const float clear[4] {};
    for (auto format : {DXGI_FORMAT_R16_UINT,DXGI_FORMAT_R32_UINT})
    {
        const UINT vertexSlot = format == DXGI_FORMAT_R16_UINT ? 0 : 3;
        ID3D11Buffer* nullBuffer=nullptr; UINT nullValue=0;
        c->IASetVertexBuffers(0,1,&nullBuffer,&nullValue,&nullValue);
        c->IASetVertexBuffers(vertexSlot,1,&vbRaw,&stride,&offset);
        elements[0].InputSlot=elements[1].InputSlot=vertexSlot;
        layout.Reset();
        assert(SUCCEEDED(d->CreateInputLayout(elements,2,vsBlob->GetBufferPointer(),vsBlob->GetBufferSize(),&layout)));
        c->IASetInputLayout(layout.Get());
        // Exercise a nonzero slot plus a second alias of the same allocation.
        if (vertexSlot) c->IASetVertexBuffers(7,1,&vbRaw,&stride,&nullValue);
        const uint16_t shorts[] {999,999,0,1,2}; const uint32_t longs[] {999,999,0,1,2};
        const UINT width=format==DXGI_FORMAT_R16_UINT?2:4;
        bd.ByteWidth=width*5; bd.BindFlags=D3D11_BIND_INDEX_BUFFER;
        data.pSysMem=width==2?static_cast<const void*>(shorts):longs;
        ComPtr<ID3D11Buffer> ib; assert(SUCCEEDED(d->CreateBuffer(&bd,&data,&ib)));
        c->IASetIndexBuffer(ib.Get(),format,width);
        auto restored = [&] {
            ComPtr<ID3D11Buffer> v,i; UINT s,o,io; DXGI_FORMAT f;
            c->IAGetVertexBuffers(vertexSlot,1,&v,&s,&o); c->IAGetIndexBuffer(&i,&f,&io);
            assert(v.Get()==vb.Get() && i.Get()==ib.Get() && s==24 && o==24 && io==width && f==format);
            if (vertexSlot)
            {
                ComPtr<ID3D11Buffer> alias, empty;
                c->IAGetVertexBuffers(7,1,&alias,&s,&o); assert(alias.Get()==vb.Get() && s==24 && o==0);
                c->IAGetVertexBuffers(0,1,&empty,&s,&o); assert(!empty);
            }
        };
        c->UpdateSubresource(vb.Get(),0,nullptr,vertices,0,0);
        c->ClearRenderTargetView(rtv.Get(),clear); c->DrawIndexed(3,1,0); auto reference=pixels();
        assert(reference[24*32+8]!=0);
        c->ClearRenderTargetView(rtv.Get(),clear);
        {
            Geometry::Scope scope;
            assert(scope.Enter(c.Get(),3,1,1024)==Geometry::Result::Ready);
            assert(scope.bytes==sizeof(vertices)+5*width); // aliases copied once
            assert(scope.slotMask==(vertexSlot?((1u<<3)|(1u<<7)):1u));
            if (vertexSlot)
            {
                ComPtr<ID3D11Buffer> owned,alias; UINT s,o;
                c->IAGetVertexBuffers(3,1,&owned,&s,&o); c->IAGetVertexBuffers(7,1,&alias,&s,&o);
                assert(owned && owned.Get()!=vb.Get() && owned.Get()==alias.Get());
            }
            // Changing the original after the copy must not affect the owned draw.
            c->UpdateSubresource(vb.Get(),0,nullptr,zero,0,0);
            c->DrawIndexed(3,1,0);
        }
        restored(); assert(pixels()==reference);
        c->ClearRenderTargetView(rtv.Get(),clear); c->DrawIndexed(3,1,0);
        assert(pixels()!=reference); // restored source really contains the update
        c->UpdateSubresource(vb.Get(),0,nullptr,vertices,0,0);
        try { Geometry::Scope scope; assert(scope.Enter(c.Get(),3,1,1024)==Geometry::Result::Ready); throw 1; }
        catch (int) {} restored();
        { Geometry::Scope s; assert(s.Enter(c.Get(),3,1,1024)==Geometry::Result::Ready); assert(s.observedStride==24); } restored();
        { Geometry::Scope s; assert(s.Enter(c.Get(),3,1,1)==Geometry::Result::Budget); } restored();
        { Geometry::Scope s; assert(s.Enter(c.Get(),3,UINT_MAX,1024)==Geometry::Result::Range); } restored();
        { Geometry::Scope s; assert(s.Enter(c.Get(),0,1,1024)==Geometry::Result::Unsupported); } restored();
        // Exercise the production material gate, exactly-once fallback and reentrancy.
        UINT tag=format==DXGI_FORMAT_R16_UINT?5:6; ps->SetPrivateData(Gpu::shaderTag,sizeof(tag),&tag);
        {
            std::lock_guard lock(Gpu::mutex); Gpu::identityCount=1;
            Gpu::identities[0]={reinterpret_cast<uintptr_t>(texture.Get()),tag-1}; Gpu::publishedAt=GetTickCount64();
        }
        Gpu::requested=true; Gpu::sampling={}; unsigned calls=0; auto before=Gpu::copied.load();
        Gpu::AroundIndexed(c.Get(),3,1,[&]{ ++calls; c->DrawIndexed(3,1,0); });
        assert(calls==1 && Gpu::copied.load()==before+1); restored();
        Gpu::Clear(); Gpu::AroundIndexed(c.Get(),3,1,[&]{++calls;}); assert(calls==2); restored();
        Gpu::processing=true; Gpu::AroundIndexed(c.Get(),3,1,[&]{++calls;}); Gpu::processing=false;
        assert(calls==3); Gpu::requested=false;
    }
    assert(!Gpu::Identify(nullptr,0));
    Gpu::SamplingWindow window;
    assert(window.Allow(1000,1) && !window.Allow(1001,1));
    assert(window.Allow(1001,2) && window.Allow(1001,4) && window.Allow(1001,8));
    window.bytes=1234;
    assert(!window.Allow(1099,8) && window.bytes==1234);
    assert(window.Allow(1100,1) && window.bytes==0);
    puts("PASS: owned GPU geometry pixels, nonzero/multiple slots, preserved aliases, copy ordering, 16/32-bit indices, offsets, exception restoration, guards, material gating and exactly-once draws");
}
