#pragma once

// Reuse of the ViT bottleneck of NVIDIA's own DLSS-NR model.
//
// One NR evaluation launches its kernels on the game's command list through NvAPI. The ViT bottleneck (blocks 31-38, the
// coarsest and most stable level of the network) is one contiguous run of those launches, from cc_vit_1d_repack_2d_to_1d
// through cc_vit_1d_repack_1d_to_2d, and about a fifth of the evaluation. Leaving that run out on some frames leaves the
// previous frame's result in its output buffer, which the next kernel reads.
//
// Why dropping the run is safe (checked on a real capture, fp8 kernels): the run's sync counters are referenced by no kernel outside
// it, so nothing that is kept can wait on something that was dropped; and its output buffer is written only by its last kernel.
// Anything that does not look like that turns the feature off for the session instead of guessing.
//
// This header is only the decision: which launch of an evaluation is dropped. It knows nothing about NvAPI, so tests/nr_vit_reuse_smoke.cpp
// exercises it on the host.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace DlssNrVitReuse
{
enum class Role : uint8_t
{
    None = 0, // any other kernel
    Start,    // first kernel of the ViT run
    Inner,    // any other kernel of the ViT run
    End,      // last kernel of the ViT run
};

// Callers serialize access with the native-runtime mutex. Module teardown must
// preserve function roles belonging to other, still-live NR features.
template<class Handle> class RoleRegistry
{
    struct Entry { Handle module; Role role; };
    std::unordered_map<Handle, Entry> entries;
public:
    void Add(Handle function, Handle module, Role role) { entries[function] = {module, role}; }
    Role Lookup(Handle function) const {
        auto it = entries.find(function);
        return it == entries.end() ? Role::None : it->second.role;
    }
    void RemoveFunction(Handle function) { entries.erase(function); }
    void RemoveModule(Handle module) {
        for (auto it = entries.begin(); it != entries.end();)
            if (it->second.module == module) it = entries.erase(it);
            else ++it;
    }
};

// Names of both kernel variants (the fp8 ones carry a _fp8 suffix), so the plain fp16 kernels match too.
inline Role RoleOf(const char* name)
{
    constexpr char prefix[] = "cc_vit_1d_";
    constexpr size_t prefixLen = sizeof(prefix) - 1;

    if (name == nullptr || strncmp(name, prefix, prefixLen) != 0)
        return Role::None;

    const char* rest = name + prefixLen;

    if (strncmp(rest, "repack_2d_to_1d", 15) == 0)
        return Role::Start;

    if (strncmp(rest, "repack_1d_to_2d", 15) == 0)
        return Role::End;

    return Role::Inner;
}

// Feed it the launches of one model evaluation, in order, between Begin and End. One Filter serves every feature (every
// pass); the evaluation in progress belongs to the calling thread, so launches from any other thread pass untouched.
class Filter
{
  public:
    // Start of one model evaluation of `feature`. `every` is how often the run is computed (1 = always); `reset` forces it.
    void Begin(const void* feature, bool reset, unsigned every)
    {
        Ctx& c = ctx();
        c = Ctx {};
        c.active = true;
        c.key = feature;
        c.reset = reset;
        c.every = every < 1 ? 1 : every;
    }

    bool Evaluating() const { return ctx().active; }

    // True when this launch is to be dropped. Must be called for every launch of the evaluation, in order.
    bool Drop(Role role)
    {
        Ctx& c = ctx();

        if (!c.active)
            return false;

        switch (role)
        {
        case Role::Start:
        {
            if (c.inRange) // a second start: not the order the checks were made for
            {
                c.anomaly = true;
                return c.skipping;
            }

            c.inRange = true;

            std::lock_guard<std::mutex> lock(mutex_);
            Entry& e = entries_[c.key];
            const bool skip = !disabled_ && c.every > 1 && e.valid && !c.reset && e.skips + 1 < c.every;

            if (skip)
            {
                ++e.skips;
                c.skipping = true;
                return true;
            }

            // computed in full: the cache is valid again only once the run has been seen to the end
            e.skips = 0;
            e.valid = false;
            return false;
        }
        case Role::Inner:
            return c.inRange && c.skipping;

        case Role::End:
        {
            if (!c.inRange)
            {
                c.anomaly = true;
                return false;
            }

            c.inRange = false;

            if (c.skipping)
            {
                c.skipping = false;
                ++reused_;
                return true;
            }

            std::lock_guard<std::mutex> lock(mutex_);
            entries_[c.key].valid = true;
            ++computed_;
            return false;
        }
        default:
            // Another kernel in the middle of a skipped run. It is kept (it is not ours to remove), the rest of the run is
            // still dropped so nothing waits on a half-dropped chain, and the feature goes off afterwards.
            if (c.skipping)
                c.anomaly = true;

            return false;
        }
    }

    // End of the evaluation. False when what was launched did not look like the expected order; the feature is then off
    // for the rest of the session.
    bool End()
    {
        Ctx& c = ctx();

        if (!c.active)
            return true;

        if (c.inRange)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            entries_[c.key].valid = false;
        }

        const bool wellFormed = !c.inRange && !c.anomaly;

        if (!wellFormed)
            disabled_ = true;

        c = Ctx {};
        return wellFormed;
    }

    // The modules were destroyed: no cached result can be trusted any more.
    void Clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
    }

    bool Disabled() const { return disabled_; }
    unsigned long long Computed() const { return computed_; }
    unsigned long long Reused() const { return reused_; }

    std::string Status() const
    {
        char buf[128];

        if (disabled_)
            return "off for this session (unexpected NR launch order)";

        snprintf(buf, sizeof buf, "computed %llu, reused %llu", (unsigned long long) computed_,
                 (unsigned long long) reused_);
        return buf;
    }

  private:
    struct Entry
    {
        bool valid = false;  // the output buffer holds a complete result
        unsigned skips = 0;  // consecutive evaluations that reused it
    };

    struct Ctx
    {
        bool active = false;
        const void* key = nullptr;
        bool reset = false;
        unsigned every = 1;
        bool inRange = false;
        bool skipping = false;
        bool anomaly = false;
    };

    static Ctx& ctx()
    {
        static thread_local Ctx c;
        return c;
    }

    std::mutex mutex_;
    std::unordered_map<const void*, Entry> entries_;
    std::atomic<bool> disabled_ { false };
    std::atomic<unsigned long long> computed_ { 0 };
    std::atomic<unsigned long long> reused_ { 0 };
};
} // namespace DlssNrVitReuse
