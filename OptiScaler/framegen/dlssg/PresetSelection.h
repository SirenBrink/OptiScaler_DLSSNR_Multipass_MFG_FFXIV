#pragma once
#include <Config.h>
#include <atomic>
namespace DlssgPreset {
inline std::atomic<bool> querySeen{false};
inline int Startup() {
    // NGX and Streamline cache driver settings during initialization.
    static const int preset=Config::Instance()->FGDLSSGRenderPreset.value_or_default();
    return preset;
}
inline const char* Name(int preset) {
    switch(preset) {case 1:return "Preset A";case 2:return "Preset B";case 3:return "Preset C";default:return "Automatic";}
}
}
