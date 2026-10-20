#pragma once
#include <Config.h>
#include <atomic>
#include <cstdint>
namespace DlssgPreset {
inline std::atomic<bool> querySeen{false};
inline std::atomic<uint64_t> originalPreset{UINT64_MAX};
inline void CaptureOriginal(unsigned value){uint64_t empty=UINT64_MAX;originalPreset.compare_exchange_strong(empty,value,std::memory_order_release);}

inline int Startup() {
    // NGX and Streamline cache driver settings during initialization.
    static const int preset=Config::Instance()->FGDLSSGRenderPreset.value_or_default();
    return preset;
}
inline std::atomic<int>& Active() { static std::atomic<int> preset{Startup()};return preset; }
inline int Current() { return Active().load(std::memory_order_acquire); }
inline std::atomic<int> failedRequest{-1};
enum class Change { Unchanged, Recreated, RestartRequired };
// Only called on OptiFG's serialized dispatch thread, outside SL Present.
// Stop/free failures never publish a new active request or spin every frame.
template<class Stop,class Free> Change Apply(int requested,Stop stop,Free free) {
    if(requested<0 || requested>3)return Change::RestartRequired;
    if(requested==Current()){failedRequest.store(-1);return Change::Unchanged;}
    if(failedRequest.load()==requested)return Change::RestartRequired;
    if(!stop() || !free()){failedRequest.store(requested);return Change::RestartRequired;}
    querySeen.store(false,std::memory_order_relaxed);
    Active().store(requested,std::memory_order_release);failedRequest.store(-1);
    return Change::Recreated;
}
inline const char* Name(int preset) {
    switch(preset) {case 1:return "Preset A";case 2:return "Preset B";case 3:return "Preset C";default:return "Automatic";}
}
}
