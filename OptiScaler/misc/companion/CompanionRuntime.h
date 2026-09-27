#pragma once
#include <Windows.h>
#include "CompanionCore.h"
#include <atomic>
namespace FfxivCompanion
{
inline Mailbox mailbox;
inline std::atomic<void(*)()> beginNativeDraw {nullptr}, endNativeDraw {nullptr};
inline int64_t Now() { LARGE_INTEGER v; QueryPerformanceCounter(&v); return v.QuadPart; }
inline int64_t Frequency() { LARGE_INTEGER v; QueryPerformanceFrequency(&v); return v.QuadPart; }
}
