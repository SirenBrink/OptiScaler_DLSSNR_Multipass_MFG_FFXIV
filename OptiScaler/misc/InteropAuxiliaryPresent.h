#pragma once

// The bridge still calls the hidden DX11 present through ReShade/DXGI. It is
// not a visible output frame: nested OptiScaler wrappers must not draw another
// menu, retarget input, or feed it into visible-frame/latency accounting.
namespace InteropAuxiliaryPresent {
inline thread_local bool active=false;
inline bool Active() noexcept { return active; }
class Scope {
    bool previous=active;
public:
    Scope() noexcept { active=true; }
    ~Scope() { active=previous; }
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
};
}
