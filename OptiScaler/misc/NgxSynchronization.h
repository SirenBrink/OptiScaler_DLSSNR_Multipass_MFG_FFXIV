#pragma once
#include <mutex>
namespace NgxSynchronization {
// Native scene resize callbacks can release an upscaler on the update thread
// while the render thread is evaluating it. Protect the complete bridge lifetime,
// not merely the runtime call or the GPU fence at the end of that lifetime.
inline std::recursive_mutex dx11Features;
using FeatureGuard = std::lock_guard<std::recursive_mutex>;
// Kept separate: NR holds its own state lock before entering the runtime. Never
// acquire dx11Features from a runtime wrapper, which would invert that order.
inline std::recursive_mutex dx12Calls;
template<auto* Function> struct Serialized;
template<class R, class... A, R (**Function)(A...)>
struct Serialized<Function> {
    static R Call(A... args) {
        const std::lock_guard lock(dx12Calls);
        return (*Function)(args...);
    }
};
}
