#pragma once
#include <array>
#include <cstdint>
#include <cstddef>

// Advances exactly with ImGui's RenderDrawData upload-ring index, not the
// swapchain index. Busy frames do not advance either ring or block the presenter.
template<std::size_t Count> class OverlaySubmissionRing
{
    std::array<uint64_t, Count> values{};
    uint64_t cursor = 0;
public:
    std::size_t Index() const { return cursor % Count; }
    bool Ready(uint64_t completed) const
    {
        return completed != UINT64_MAX && values[Index()] != UINT64_MAX && values[Index()] <= completed;
    }
    void DrawRecorded() { ++cursor; }
    void Submitted(std::size_t index, uint64_t value) { values[index] = value; }
    void Reset() { values = {}; cursor = 0; }
};
