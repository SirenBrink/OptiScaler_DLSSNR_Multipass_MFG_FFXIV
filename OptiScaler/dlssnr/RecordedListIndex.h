#pragma once
#include <array>
#include <atomic>
#include <cstddef>

namespace DlssNr
{
// Adapted from janblade f1317dbe's watched-list filter. Writers are serialized
// by the owning lifetime tracker; readers never take the NR recording lock.
// Keep entries through submission: closed lists can execute repeatedly until
// Reset, and every execution must retain completion proof. Overflow only
// disables the optimization, never ownership tracking.
template <size_t Capacity = 64> class RecordedListIndex
{
    std::array<std::atomic<const void*>, Capacity> entries {};
    std::atomic<bool> overflow { false };
  public:
    void Add(const void* list) noexcept
    {
        if (!list) return;
        for (auto& entry : entries) if (entry.load() == list) return;
        for (auto& entry : entries)
        {
            const void* empty = nullptr;
            if (entry.compare_exchange_strong(empty, list)) return;
        }
        overflow.store(true);
    }
    bool MayContain(const void* list) const noexcept
    {
        if (!list) return false;
        if (overflow.load()) return true;
        for (const auto& entry : entries) if (entry.load() == list) return true;
        return false;
    }
    void Reset(const void* list) noexcept
    {
        if (!list) return;
        for (auto& entry : entries)
        {
            const void* expected = list;
            entry.compare_exchange_strong(expected, nullptr);
        }
    }
};
}
