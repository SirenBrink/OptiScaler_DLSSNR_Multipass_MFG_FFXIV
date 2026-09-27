#pragma once
#include "CompanionProtocol.h"
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

namespace FfxivCompanion
{
struct Snapshot
{
    Frame frame {};
    std::array<Plate, MaxPlates> plates {};
};

// A value-only mailbox. It neither owns game pointers nor calls managed code.
// One snapshot is bounded to 10 KB; calls never queue work on the game thread.
class Mailbox
{
    mutable std::mutex mutex;
    uint64_t generation = 0, session = 0, accepted = 0, rejected = 0, lastSequence = 0;
    int64_t frequency = 0, lastContact = 0;
    Snapshot current {};
    bool available = false;

    static bool Finite(float v) { return std::isfinite(v) && std::abs(v) < 10000000.0f; }
    bool Fresh(int64_t now) const
    {
        return available && now >= current.frame.qpc && now - current.frame.qpc <= frequency / 4;
    }
    bool Validate(const Frame& f, const Plate* p, uint32_t bytes, int64_t now) const
    {
        if (f.count > MaxPlates || bytes != f.count * sizeof(Plate) || (f.count && !p) ||
            f.sequence <= lastSequence || f.qpc <= 0 || f.qpc > now || now - f.qpc > frequency / 4 ||
            (f.flags & ~(Preview | CameraValid | GameplayReady))) return false;
        if (f.count && (!f.width || !f.height || f.width > 16384 || f.height > 16384)) return false;
        if (f.flags & CameraValid)
            for (unsigned i = 0; i < 16; ++i)
                if (!Finite(f.view[i]) || !Finite(f.projection[i])) return false;
        uint64_t slots = 0;
        for (uint32_t i = 0; i < f.count; ++i)
        {
            const auto& a = p[i];
            if (!a.objectId || a.objectId == 0xE0000000 || a.slot >= MaxPlates ||
                (slots & (1ull << a.slot)) || (a.flags & ~(WorldValid | BoundsValid)) || a.reserved ||
                !Finite(a.anchorX) || !Finite(a.anchorY) || !std::memchr(a.name, 0, sizeof(a.name))) return false;
            slots |= 1ull << a.slot;
            if ((a.flags & WorldValid) && (!Finite(a.worldX) || !Finite(a.worldY) || !Finite(a.worldZ))) return false;
            if ((a.flags & BoundsValid) && (!Finite(a.left) || !Finite(a.top) || !Finite(a.right) ||
                !Finite(a.bottom) || a.left > a.right || a.top > a.bottom)) return false;
        }
        return true;
    }
public:
    uint64_t Open(uint32_t version, uint32_t frameSize, uint32_t plateSize, uint32_t statusSize,
                  int64_t peerFrequency, int64_t nativeFrequency, int64_t now)
    {
        std::lock_guard lock(mutex);
        if (version != Version || frameSize != sizeof(Frame) || plateSize != sizeof(Plate) ||
            statusSize != sizeof(Status) || peerFrequency <= 0 || peerFrequency != nativeFrequency) return 0;
        if (session && now >= lastContact && now - lastContact <= nativeFrequency * 2) return 0;
        session = ++generation;
        frequency = nativeFrequency;
        lastContact = now;
        accepted = rejected = lastSequence = 0;
        available = false;
        current = {};
        return session;
    }
    bool Submit(uint64_t token, const Frame& f, const Plate* p, uint32_t bytes, int64_t now)
    {
        std::lock_guard lock(mutex);
        if (!token || token != session) return false;
        if (!Validate(f, p, bytes, now)) { ++rejected; available = false; return false; }
        current.frame = f;
        if (f.count) std::memcpy(current.plates.data(), p, bytes);
        available = f.count != 0;
        lastSequence = f.sequence;
        lastContact = now;
        ++accepted;
        return true;
    }
    bool Query(uint64_t token, Status& s, int64_t now)
    {
        std::lock_guard lock(mutex);
        if (!token || token != session) return false;
        lastContact = now;
        s = {accepted, rejected, lastSequence, lastSequence ? now - current.frame.qpc : 0,
             Fresh(now) ? current.frame.count : 0, 3};
        return true;
    }
    bool Read(Snapshot& s, Status& status, int64_t now) const
    {
        std::lock_guard lock(mutex);
        status = {accepted, rejected, lastSequence, lastSequence ? now - current.frame.qpc : 0,
                  Fresh(now) ? current.frame.count : 0, 3};
        if (!session || !Fresh(now)) return false;
        s = current;
        return true;
    }
    void Close(uint64_t token)
    {
        std::lock_guard lock(mutex);
        if (token && token == session) { session = 0; available = false; current = {}; lastSequence = 0; }
    }
};
}
