#include "pch.h"
#include "FrameLimit.h"
#include "FrameLimitTiming.h"
#include <chrono>

#include "Config.h"
// #include "hooks/D3D11Hooks.h"

inline uint64_t FrameLimit::get_timestamp()
{
    // Wall-clock corrections must never become a multi-second frame delay.
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// https://learn.microsoft.com/en-us/windows/win32/sync/using-waitable-timer-objects
inline int FrameLimit::timer_sleep(int64_t hundred_ns)
{
    // Separate presentation threads must not reset each other's waitable timer.
    struct Timer
    {
        HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        ~Timer() { if (handle) CloseHandle(handle); }
    };
    static thread_local Timer localTimer;
    const auto timer = localTimer.handle;
    LARGE_INTEGER due_time;

    due_time.QuadPart = -hundred_ns;

    if (!timer)
        return 1;

    if (!SetWaitableTimerEx(timer, &due_time, 0, NULL, NULL, NULL, 0))
        return 2;

    if (WaitForSingleObject(timer, 1000) != WAIT_OBJECT_0)
        return 3;

    return 0;
};

inline int FrameLimit::busywait_sleep(int64_t ns)
{
    auto current_time = get_timestamp();
    auto wait_until = current_time + ns;
    while (current_time < wait_until)
    {
        current_time = get_timestamp();
    }
    return 0;
}

inline int FrameLimit::combined_sleep(int64_t ns)
{
    constexpr int64_t busywait_threshold = 2'000'000; // 2ms
    int status {};
    auto current_time = get_timestamp();
    if (ns <= busywait_threshold)
        status = busywait_sleep(ns);
    else
        status = timer_sleep((ns - busywait_threshold) / 100);

    if (int64_t sleep_deviation = ns - (get_timestamp() - current_time); sleep_deviation > 0 && !status)
        status = busywait_sleep(sleep_deviation);

    return status;
}

void FrameLimit::sleep(bool fgActive)
{
    static thread_local uint64_t previous_frame_time = 0;
    const auto interval = FrameLimitTiming::Interval(Config::Instance()->FramerateLimit.value_or_default(), fgActive);
    if (!interval)
    {
        previous_frame_time = 0;
        return;
    }
    const auto current_time = get_timestamp();
    if (previous_frame_time && current_time >= previous_frame_time)
    {
        const auto frame_time = current_time - previous_frame_time;
        if (frame_time < interval)
            if (const auto res = combined_sleep(int64_t(interval - frame_time)); res)
                LOG_ERROR("Sleep command failed: {}", res);
    }
    previous_frame_time = get_timestamp();
}
