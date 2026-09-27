#include "../OptiScaler/misc/companion/CompanionCore.h"
#include <cassert>
#include <cstdio>
#include <limits>
#include <thread>

using namespace FfxivCompanion;
int main()
{
    Mailbox m;
    auto open = [&](int64_t now) { return m.Open(Version, sizeof(Frame), sizeof(Plate), sizeof(Status), 1000, 1000, now); };
    assert(!m.Open(2, sizeof(Frame), sizeof(Plate), sizeof(Status), 1000, 1000, 100));
    assert(!m.Open(1, 159, sizeof(Plate), sizeof(Status), 1000, 1000, 100));
    assert(!m.Open(1, sizeof(Frame), sizeof(Plate), sizeof(Status), 999, 1000, 100));
    auto token = open(100);
    assert(token && !open(100));
    Frame f {1, 100, 3840, 2160, 1, Preview};
    Plate p {}; p.objectId = 42; p.slot = 3; p.anchorX = 123; p.anchorY = 456;
    assert(m.Submit(token, f, &p, sizeof(p), 100));
    Snapshot s; Status status;
    assert(m.Read(s, status, 200) && s.plates[0].objectId == 42 && status.accepted == 1);
    // Mailbox owns the copy, not a caller's stack or a game node pointer.
    p.objectId = 43;
    assert(m.Read(s, status, 200) && s.plates[0].objectId == 42);
    assert(!m.Read(s, status, 351));
    f.sequence++;
    assert(!m.Submit(token, f, &p, sizeof(p) - 1, 101));
    assert(!m.Read(s, status, 101));
    assert(m.Submit(token, f, &p, sizeof(p), 101));
    assert(!m.Submit(token, f, &p, sizeof(p), 101)); // reordered/duplicate frame
    f.sequence++; f.qpc = 102;
    p.anchorX = std::numeric_limits<float>::quiet_NaN();
    assert(!m.Submit(token, f, &p, sizeof(p), 102));
    p.anchorX = 123; p.slot = 50;
    assert(!m.Submit(token, f, &p, sizeof(p), 102));
    p.slot = 3; std::memset(p.name, 'x', sizeof(p.name));
    assert(!m.Submit(token, f, &p, sizeof(p), 102));
    p.name[127] = 0;
    assert(m.Submit(token, f, &p, sizeof(p), 102));
    Frame empty {++f.sequence, 103};
    assert(m.Submit(token, empty, nullptr, 0, 103));
    assert(!m.Read(s, status, 103)); // zoning must not retain the last plate
    m.Close(token);
    assert(!m.Query(token, status, 104));
    auto reloaded = open(104);
    assert(reloaded && reloaded != token);
    m.Close(token); // a late old-plugin Dispose must not disconnect the replacement
    assert(m.Query(reloaded, status, 104));
    assert(!m.Submit(token, f, &p, sizeof(p), 104));
    // Concurrent publisher/renderer copies must never expose a partially updated frame.
    std::thread producer([&] {
        for (uint64_t i = 1; i <= 2000; ++i) {
            Frame a {i, 110, 3840, 2160, 1, Preview}; Plate b {};
            b.objectId = i; b.slot = 0;
            assert(m.Submit(reloaded, a, &b, sizeof(b), 110));
        }
    });
    for (int i = 0; i < 2000; ++i)
        if (m.Read(s, status, 110)) assert(s.frame.sequence == s.plates[0].objectId);
    producer.join();
    assert(m.Read(s, status, 110) && status.accepted == 2000);
    puts("PASS: ABI, bounds, ownership, stale data, empty frames, session reload and concurrent snapshots");
}
