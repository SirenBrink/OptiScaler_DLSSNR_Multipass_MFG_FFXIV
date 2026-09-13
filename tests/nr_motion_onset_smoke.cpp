#include "../OptiScaler/shaders/dlssnr/PreSrMotionReset.h"
#include <cassert>
#include <limits>
#include <iostream>
using PreSrMotionReset::Gate;
int main()
{
    Gate g;
    for (uint64_t t=100;t<=400;t+=50) assert(!g.Update(0,t,t));
    assert(g.Update(.3f,450,450));
    for (uint64_t t=500;t<=2000;t+=50) assert(!g.Update(.3f,t,t));
    for (uint64_t t=2050;t<=2350;t+=50) assert(!g.Update(0,t,t));
    assert(g.Update(.3f,2400,2400));
    for (uint64_t t=2450;t<=2750;t+=50) assert(!g.Update(0,t,t));
    assert(!g.Update(.3f,2800,2800)); // cooldown consumes onset
    for (uint64_t t=2850;t<=4000;t+=50) assert(!g.Update(.3f,t,t));
    Gate startup;
    for (uint64_t t=100;t<2000;t+=50) assert(!startup.Update(.3f,t,t));
    assert(!g.Update(.5f,4100,4400)); // stale
    assert(!g.Update(.5f,4500,4400)); // future
    assert(!g.Update(std::numeric_limits<float>::quiet_NaN(),4400,4400));
    Gate gap;
    for(uint64_t t=100;t<=400;t+=50) gap.Update(0,t,t);
    assert(!gap.Update(.3f,900,900)); // don't reset on resumed stale sampling
    std::array<float,9> x{}, y{};
    x[0]=10; x[1]=10; assert(PreSrMotionReset::PanSpeed(x,y)==0);
    x.fill(.3f); assert(PreSrMotionReset::PanSpeed(x,y)>.29f);
    x[0]=std::numeric_limits<float>::infinity(); assert(PreSrMotionReset::PanSpeed(x,y)<0);
    std::cout << "Motion onset, sustained pan, rearm, cooldown, startup, stale/gap, invalid and outlier checks passed\n";
}
