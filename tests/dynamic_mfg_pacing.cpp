#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
#include <limits>
#include "../OptiScaler/framegen/dlssg/DynamicMfgPolicy.h"
#define LOG_DEBUG(...) ((void)0)
#define LOG_INFO(...) ((void)0)
enum class FGOutput { NoFG, DLSSG, FSRFG, XeFG };
enum class LowLatencyMode { Reflex, XeLL };
enum class ForceReflex { ForceEnable, ForceDisable };
namespace sl { enum class DLSSGMode { eOff, eOn, eDynamic }; }
using HANDLE=void*;
struct IUnknown {};
struct Feature {
    bool active=true,paused=false,owns=true;
    bool IsActive(){return active;}
    bool IsPaused(){return paused;}
    bool OwnsFramePacing()const{return owns;}
};
struct State {
    bool externalFrameGeneration=false,reflexLimitsFps=false,reflexShowWarning=false,fakenvapiReloadLowLatency=false;
    FGOutput activeFgOutput=FGOutput::DLSSG;
    sl::DLSSGMode dlssgLastSetMode=sl::DLSSGMode::eDynamic;
    Feature* currentFG=nullptr;
    static State& Instance(){static State s;return s;}
};
template<class T> struct Setting {
    T value{}; T value_or_default() const{return value;}
    void set_volatile_value(T v){value=v;}
};
struct Config {
    Setting<float> FramerateLimit{165};
    Setting<bool> FGDLSSGForceDMFG{true};
    Setting<ForceReflex> FN_ForceReflex{ForceReflex::ForceEnable};
    static Config* Instance(){static Config c;return &c;}
};
namespace fakenvapi {
bool mainNvapi=true; LowLatencyMode mode=LowLatencyMode::Reflex;
bool isUsingAsMainNvapi(){return mainNvapi;}
LowLatencyMode getCurrentMode(){return mode;}
}
struct ReflexHooks {
    inline static uint64_t _updatesWithoutMarker=0;
    inline static bool _inited=true;
    inline static IUnknown device;
    inline static IUnknown* _lastSleepDev=&device;
    inline static HANDLE _lastVkSleepDev=nullptr;
    struct Params {bool bLowLatencyMode=true;};
    inline static Params _lastSleepParams{true},_lastVkSleepParams{true};
    inline static uint8_t _FgNumFramesToGenerate=1;
    inline static std::vector<float> writes;
    static void setFPSLimit(float fps){writes.push_back(fps);}
    static void update(bool,bool);
};
#include "dynamic_mfg_update_production.inl"
static void check(bool value,const char* msg){if(!value)throw std::runtime_error(msg);}
int main()try {
    using namespace DynamicMfgPolicy;
    check(LimitUs(165)==6061 && LimitUs(60)==16667,"Reflex cap units/rounding wrong");
    for(float bad:{0.f,-1.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
        check(Target(bad)==0 && LimitUs(bad)==0,"invalid cap was not disabled");
    check(LimitUs(std::numeric_limits<float>::denorm_min())==UINT32_MAX,"small cap overflow");
    check(LimitUs(std::numeric_limits<float>::max())==1,"large cap underflow");
    check(Target(360)==360,"high refresh target clamped");
    for(int mode=0;mode<2;++mode)for(int dlss=0;dlss<2;++dlss)for(int reflex=0;reflex<2;++reflex)
        check(OwnsPacing(mode,dlss,reflex)==bool(mode&&dlss&&reflex),"failed configuration claimed pacing");
    Feature feature;State::Instance().currentFG=&feature;
    // This is the extracted production update, not a duplicate controller.
    for(int factor:{2,6,3,5,2,6,4}){
        ReflexHooks::_updatesWithoutMarker=0;
        ReflexHooks::_FgNumFramesToGenerate=static_cast<uint8_t>(factor-1);
        ReflexHooks::update(true,false);
        check(State::Instance().reflexLimitsFps,"native pacing not recognised");
        check(ReflexHooks::writes.empty(),"factor swap reprogrammed a second limiter");
    }
    Config::Instance()->FramerateLimit.value=120;
    ReflexHooks::update(true,false);
    check(ReflexHooks::writes.empty(),"live output cap divided by old factor");
    // Pausing FG restores the full-rate cap, even with stale 6X telemetry.
    feature.paused=true;ReflexHooks::_FgNumFramesToGenerate=5;
    ReflexHooks::update(true,false);
    check(!ReflexHooks::writes.empty() && ReflexHooks::writes.back()==120,"pause retained divided cap");
    feature.paused=false;ReflexHooks::writes.clear();
    ReflexHooks::update(true,false);
    check(ReflexHooks::writes.empty(),"resume did not hand pacing back");
    // Runtime rejects dynamic options: recover the legacy fixed-factor cap.
    feature.owns=false;State::Instance().dlssgLastSetMode=sl::DLSSGMode::eOn;
    Config::Instance()->FGDLSSGForceDMFG.value=false;
    ReflexHooks::update(true,false);
    check(ReflexHooks::writes.back()==20,"failed runtime did not restore fixed-factor fallback");
    ReflexHooks::writes.clear();
    ReflexHooks::_FgNumFramesToGenerate=2;
    ReflexHooks::update(true,false);
    check(ReflexHooks::writes.size()==1 && ReflexHooks::writes[0]==40,"fixed swap briefly applied full cap");
    feature.active=false;ReflexHooks::update(false,false);
    check(ReflexHooks::writes.back()==120,"FG off retained old divided cap");
    Config::Instance()->FramerateLimit.value=0;
    ReflexHooks::_FgNumFramesToGenerate=3;ReflexHooks::writes.clear();
    ReflexHooks::update(false,false);
    check(ReflexHooks::writes.size()==1 && ReflexHooks::writes.back()==0,"simultaneous factor swap/cap disable retained old cap");
    puts("PASS: production Reflex handoffs, 2X-6X swaps without limiter writes, pause/off recovery, failed-runtime fallback, live/invalid caps");
    return 0;
}catch(const std::exception& e){puts(e.what());return 1;}
