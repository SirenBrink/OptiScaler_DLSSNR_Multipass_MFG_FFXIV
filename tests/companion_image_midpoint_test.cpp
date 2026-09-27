#define NOMINMAX
#include <Windows.h>
#include <wrl/client.h>
#include <cassert>
#include <cstdio>
#include "../OptiScaler/misc/companion/CompanionImageMidpoint.h"
#pragma comment(lib,"d3d11.lib")
using namespace FfxivCompanion;
using Microsoft::WRL::ComPtr;
int main()
{
    Snapshot old{},now{};old.frame.sequence=1;old.frame.width=64;old.frame.height=32;old.frame.count=1;
    old.plates[0].objectId=12;old.plates[0].flags=BoundsValid;old.plates[0].left=12;old.plates[0].right=20;old.plates[0].top=12;old.plates[0].bottom=16;
    now=old;now.frame.sequence=2;now.plates[0].left+=4;now.plates[0].right+=4;
    auto moves=ImageMidpoint::Build(old,now);assert(moves.size()==1 && moves[0].x==-2 && moves[0].y==0);
    auto bad=now;bad.plates[0].name[0]='x';assert(ImageMidpoint::Build(old,bad).empty());
    bad=now;bad.frame.sequence=4;assert(ImageMidpoint::Build(old,bad).empty());
    bad=now;bad.plates[0].right+=2;assert(ImageMidpoint::Build(old,bad).empty());
    bad=now;bad.plates[0].left+=30;bad.plates[0].right+=30;assert(ImageMidpoint::Build(old,bad).empty());
    bad=now;bad.plates[0].left=0;bad.plates[0].right=8;assert(ImageMidpoint::Build(old,bad).empty());
    auto crowdedOld=old,crowdedNow=now;crowdedOld.frame.count=crowdedNow.frame.count=2;
    crowdedOld.plates[1]=old.plates[0];crowdedOld.plates[1].objectId=13;crowdedOld.plates[1].slot=1;
    crowdedNow.plates[1]=crowdedOld.plates[1];assert(ImageMidpoint::Build(crowdedOld,crowdedNow).empty());
    bad=now;bad.plates[0].textColor=1;assert(ImageMidpoint::Build(old,bad).empty());
    ComPtr<ID3D11Device> d;ComPtr<ID3D11DeviceContext> c;D3D_FEATURE_LEVEL level;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,&level,&c)));
    ComPtr<ID3D11DeviceContext1> c1;assert(SUCCEEDED(c.As(&c1)));
    UINT pixels[64*32]{};for(UINT y=12;y<16;++y)for(UINT x=16;x<24;++x)pixels[y*64+x]=0x80808080;
    pixels[30*64+60]=0xff123456;
    D3D11_TEXTURE2D_DESC td{};td.Width=64;td.Height=32;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    D3D11_SUBRESOURCE_DATA initial{pixels,64*4,0};ComPtr<ID3D11Texture2D> source,target,read;
    assert(SUCCEEDED(d->CreateTexture2D(&td,&initial,&source)));assert(SUCCEEDED(d->CreateTexture2D(&td,nullptr,&target)));
    ComPtr<ID3D11RenderTargetView> view;assert(SUCCEEDED(d->CreateRenderTargetView(target.Get(),nullptr,&view)));
    c->CopyResource(target.Get(),source.Get());ImageMidpoint::Render(c1.Get(),source.Get(),target.Get(),view.Get(),moves);
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;assert(SUCCEEDED(d->CreateTexture2D(&td,nullptr,&read)));
    c->CopyResource(read.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE map{};assert(SUCCEEDED(c->Map(read.Get(),0,D3D11_MAP_READ,0,&map)));
    for(UINT y=0;y<32;++y)for(UINT x=0;x<64;++x)
    {
        const UINT expected=(y>=12 && y<16 && x>=14 && x<22)?0x80808080:(y==30 && x==60)?0xff123456:0;
        assert(*reinterpret_cast<UINT*>(static_cast<char*>(map.pData)+y*map.RowPitch+x*4)==expected);
    }
    c->Unmap(read.Get(),0);
    c->CopyResource(read.Get(),source.Get());assert(SUCCEEDED(c->Map(read.Get(),0,D3D11_MAP_READ,0,&map)));
    for(UINT y=0;y<32;++y)assert(!memcmp(static_cast<char*>(map.pData)+y*map.RowPitch,pixels+y*64,64*4));c->Unmap(read.Get(),0);
    puts("PASS: isolated midpoint positions; overlap, appearance, jump and stale guards; exact GPU translation; old footprint cleared; alpha/source/unrelated pixels preserved");
}
