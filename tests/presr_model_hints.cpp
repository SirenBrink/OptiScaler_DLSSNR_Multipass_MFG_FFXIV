#include "../OptiScaler/dlssnr/PreSrModelHints.h"
#include <cassert>
#include <cstdio>
using namespace DlssModelHints;
int main(){
    const std::array<uint32_t,6> game{1,2,3,4,5,6};
    std::array<std::optional<uint32_t>,6> per{};
    auto original=Resolve(2,game,false,11,per);assert(original.presets==game);
    auto forced=Resolve(2,game,true,11,per);for(auto p:forced.presets)assert(p==11);
    auto changed=Resolve(2,game,true,10,per);assert(changed!=forced);
    assert(Resolve(1,game,true,11,per)!=forced); // Quality-only changes require recreation too.
    assert(Resolve(2,game,true,11,per)==forced); // Stable hints retain the existing generation.
    per[2]=0;per[4]=12;auto partial=Resolve(2,game,true,std::nullopt,per);
    assert(partial.presets[2]==0 && partial.presets[4]==12 && partial.presets[0]==1);
    assert(Resolve(2,game,false,std::nullopt,per).presets==game);
    auto automatic=Resolve(2,game,true,0x00FFFFFFu,per);for(auto p:automatic.presets)assert(p==0);
    assert(game[2]==3);assert(NgxPreset(11)==11 && NgxPreset(0x00FFFFFFu)==0);
    puts("PASS: override precedence, explicit Default, quality/model generation identity, source preservation and legacy Latest normalization");
}
