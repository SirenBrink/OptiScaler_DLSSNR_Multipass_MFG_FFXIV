#include "../OptiScaler/dlssnr/RecordedListIndex.h"
#include <array>
#include <atomic>
#include <thread>
#include <stdexcept>
#include <cstdio>
void require(bool v){if(!v)throw std::runtime_error("recorded-list index assertion failed");}
int main() try {
    std::array<int,65> objects{}; DlssNr::RecordedListIndex<> index;
    require(!index.MayContain(&objects[0])); require(!index.MayContain(nullptr));
    for(unsigned i=0;i<64;++i){ index.Add(&objects[i]);require(index.MayContain(&objects[i])); }
    require(!index.MayContain(&objects[64]));
    index.Reset(&objects[20]);require(!index.MayContain(&objects[20]));
    index.Add(&objects[64]);require(index.MayContain(&objects[64]));
    index.Add(&objects[20]);require(index.MayContain(&objects[20]));
    // Overflow must be permanently conservative, even after reset.
    for(auto& object:objects)index.Reset(&object);
    require(index.MayContain(&objects[0]));require(!index.MayContain(nullptr));
    DlssNr::RecordedListIndex<> concurrent;
    concurrent.Add(&objects[0]);std::atomic<bool> stop=false;
    std::thread reader([&]{while(!stop.load())require(concurrent.MayContain(&objects[0]));});
    for(unsigned i=0;i<100000;++i){concurrent.Add(&objects[1]);concurrent.Reset(&objects[1]);}
    stop=true;reader.join();
    puts("PASS: recorded-list index retains unrelated live entries during concurrent resets, reuses slots, and fails closed on overflow");return 0;
} catch(const std::exception& e){puts(e.what());return 1;}
