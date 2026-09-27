#include "../OptiScaler/misc/companion/CompanionMidpoint.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <limits>
using namespace FfxivCompanion::Midpoint;
struct Vertex { float x,y,z; uint32_t colour; float u,v; };
int main()
{
    std::array<Vertex,4> a {{{10,10,0,0xff123456,0,0},{20,10,0,0xff123456,1,0},
                             {20,20,0,0xff123456,1,1},{10,20,0,0xff123456,0,1}}};
    auto b=a;for(auto& v:b){v.x+=6;v.y-=4;}
    auto bytes=[](const auto& v){return std::span(reinterpret_cast<const unsigned char*>(v.data()),sizeof(v));};
    std::vector<unsigned char> output;
    assert(Positions(bytes(a),bytes(b),sizeof(Vertex),0,output));
    std::array<Vertex,4> result;memcpy(result.data(),output.data(),output.size());
    for(size_t i=0;i<a.size();++i)
    {assert(result[i].x==a[i].x+3 && result[i].y==a[i].y-2);assert(memcmp(&result[i].z,&b[i].z,16)==0);}
    auto changed=b;changed[0].colour=0;assert(!Positions(bytes(a),bytes(changed),24,0,output));
    changed=b;changed[0].u=0.5;assert(!Positions(bytes(a),bytes(changed),24,0,output));
    changed=b;changed[0].z=1;assert(!Positions(bytes(a),bytes(changed),24,0,output));
    changed=b;changed[0].x+=1;assert(!Positions(bytes(a),bytes(changed),24,0,output));
    changed=b;for(auto& v:changed)v.x+=100;assert(!Positions(bytes(a),bytes(changed),24,0,output));
    changed=b;changed[0].x=std::numeric_limits<float>::quiet_NaN();assert(!Positions(bytes(a),bytes(changed),24,0,output));
    assert(!Positions(bytes(a),bytes(b),0,0,output));
    assert(!Positions(bytes(a),bytes(b),24,UINT32_MAX,output));
    assert(!Positions(bytes(a).first(24),bytes(b).first(24),24,0,output));
    Gate gate;
    assert(!gate.Offer(10,9,0.030,0.007,0.001,true)); // no visible endpoint to interpolate from
    gate.Presented(10,false);
    assert(gate.Offer(11,10,0.030,0.007,0.001,true));
    gate.Presented(11,true);
    assert(!gate.Offer(11,10,0.030,0.007,0.008,true)); // at most one midpoint
    assert(!gate.Offer(12,11,0.030,0.007,0.001,true)); // must display endpoint before another midpoint
    gate.Presented(12,false);
    assert(!gate.Offer(14,13,0.030,0.007,0.001,true)); // missed native endpoint
    gate.Presented(14,false);
    assert(!gate.Offer(15,14,0.030,0.017,0.001,true)); // latency gate at low refresh
    gate.Presented(15,false);
    assert(!gate.Offer(16,15,0.010,0.007,0.001,true)); // too little display headroom
    gate.Presented(16,false);
    assert(!gate.Offer(17,16,0.030,0.007,0.020,true)); // stale source
    gate.Presented(17,false);
    assert(!gate.Offer(18,17,0.030,0.007,0.001,false)); // live disable
    gate.Reset();assert(!gate.endpoint && !gate.seen);
    puts("PASS: midpoint stays within known positions; UV/alpha/depth unchanged; jumps/deformation/visibility rejected; one midpoint then endpoint; cadence, latency, stale and missed-frame gates");
}
