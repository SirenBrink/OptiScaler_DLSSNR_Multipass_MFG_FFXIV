#pragma once
#include "CompanionCore.h"
#include <d3d11_1.h>
#include <algorithm>
#include <vector>

namespace FfxivCompanion::ImageMidpoint
{
struct Move { D3D11_RECT source; int x,y; };
inline bool Overlap(const D3D11_RECT& a,const D3D11_RECT& b)
{return a.left<b.right && b.left<a.right && a.top<b.bottom && b.top<a.bottom;}
inline D3D11_RECT Bounds(const Plate& p)
{return {LONG(std::floor(p.left))-4,LONG(std::floor(p.top))-4,LONG(std::ceil(p.right))+4,LONG(std::ceil(p.bottom))+4};}
inline bool Inside(const D3D11_RECT& r,UINT w,UINT h)
{return r.left>=0 && r.top>=0 && r.right>r.left && r.bottom>r.top && r.right<=LONG(w) && r.bottom<=LONG(h);}
inline bool Valid(const Plate& p)
{return (p.flags&BoundsValid) && std::isfinite(p.left) && std::isfinite(p.top) && std::isfinite(p.right) && std::isfinite(p.bottom) &&
    std::abs(p.left)<65536 && std::abs(p.top)<65536 && std::abs(p.right)<65536 && std::abs(p.bottom)<65536;}
inline std::vector<Move> Build(const Snapshot& old,const Snapshot& now)
{
    std::vector<Move> moves;
    if(!old.frame.sequence || now.frame.sequence!=old.frame.sequence+1 || old.frame.count>MaxPlates || now.frame.count>MaxPlates ||
       old.frame.width!=now.frame.width || old.frame.height!=now.frame.height || old.frame.count!=now.frame.count)return moves;
    for(UINT i=0;i<now.frame.count;++i)if(!Valid(now.plates[i]) || !Valid(old.plates[i]))return moves;
    for(UINT i=0;i<now.frame.count;++i)
    {
        const auto& p=now.plates[i];if(!Valid(p))continue;
        const Plate* previous=nullptr;
        for(UINT j=0;j<old.frame.count;++j)if(old.plates[j].objectId==p.objectId && old.plates[j].slot==p.slot){previous=&old.plates[j];break;}
        if(!previous || !Valid(*previous))continue;const auto& q=*previous;
        if(p.nameIcon!=q.nameIcon || p.markerIcon!=q.markerIcon || p.textColor!=q.textColor || p.edgeColor!=q.edgeColor ||
           memcmp(p.name,q.name,sizeof(p.name)) || p.flags!=q.flags)continue;
        const float dx=q.left-p.left,dy=q.top-p.top;
        if(!std::isfinite(dx) || !std::isfinite(dy) || dx*dx+dy*dy>24*24 ||
           std::abs((q.right-p.right)-dx)>.25f || std::abs((q.bottom-p.bottom)-dy)>.25f)continue;
        // Integer translations retain the exact native glyph pixels; never blend images.
        const int x=int(std::round(dx*.5f)),y=int(std::round(dy*.5f));if(!x && !y)continue;
        auto r=Bounds(p);auto end=r;end.left+=x;end.right+=x;end.top+=y;end.bottom+=y;
        if(!Inside(r,now.frame.width,now.frame.height) || !Inside(end,now.frame.width,now.frame.height))continue;
        auto sweep=r;sweep.left=std::min(r.left,end.left);sweep.top=std::min(r.top,end.top);
        sweep.right=std::max(r.right,end.right);sweep.bottom=std::max(r.bottom,end.bottom);
        bool isolated=true;
        for(UINT j=0;j<now.frame.count;++j)if(i!=j && Valid(now.plates[j]) && Overlap(sweep,Bounds(now.plates[j])))isolated=false;
        // Also reject movement through another plate's previous/midpoint footprint.
        for(UINT j=0;j<old.frame.count;++j)if((old.plates[j].objectId!=p.objectId || old.plates[j].slot!=p.slot) &&
            Valid(old.plates[j]) && Overlap(sweep,Bounds(old.plates[j])))isolated=false;
        if(isolated)moves.push_back({r,x,y});
    }
    // Crossed paths can overlap even if neither endpoint overlaps: skip both.
    std::vector<bool> conflict(moves.size());
    for(size_t i=0;i<moves.size();++i)for(size_t j=i+1;j<moves.size();++j)
    {
        auto sweep=[](const Move& m){auto r=m.source;r.left+=std::min(0,m.x);r.right+=std::max(0,m.x);r.top+=std::min(0,m.y);r.bottom+=std::max(0,m.y);return r;};
        if(Overlap(sweep(moves[i]),sweep(moves[j])))conflict[i]=conflict[j]=true;
    }
    std::vector<Move> safe;for(size_t i=0;i<moves.size();++i)if(!conflict[i])safe.push_back(moves[i]);return safe;
}
inline void Render(ID3D11DeviceContext1* c,ID3D11Texture2D* source,ID3D11Texture2D* target,ID3D11RenderTargetView* view,const std::vector<Move>& moves)
{
    // target already contains the latest visibility image. Source remains immutable.
    FLOAT clear[4]{};
    for(const auto& m:moves)c->ClearView(view,clear,&m.source,1);
    for(const auto& m:moves)
    {
        D3D11_BOX b{UINT(m.source.left),UINT(m.source.top),0,UINT(m.source.right),UINT(m.source.bottom),1};
        c->CopySubresourceRegion(target,0,UINT(m.source.left+m.x),UINT(m.source.top+m.y),0,source,0,&b);
    }
}
}
