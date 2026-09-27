#include "../OptiScaler/misc/companion/CompanionPackets.h"
#include <cassert>
#include <cstdio>
#include <stdexcept>
using namespace FfxivCompanion::Packets;
int main()
{
    alignas(16) std::array<unsigned char, 64> native {};
    uint32_t kind = 0x22, vertices = 4, layout = 4;
    uintptr_t allocation = 0x12345000;
    memcpy(native.data(), &kind, 4); memcpy(native.data()+0x20, &vertices, 4);
    memcpy(native.data()+0x28, &allocation, 8); memcpy(native.data()+0x30, &layout, 4);
    alignas(16) std::array<unsigned char, 0x400> block {};
    Entry name {123, reinterpret_cast<uintptr_t>(native.data())};
    memcpy(block.data(), &name, sizeof(name));
    Queue before {1, reinterpret_cast<uintptr_t>(block.data()), 0}, after = before;
    after.count = 1;
    std::vector<Packet> copied;
    assert(Capture(before, after, copied) && copied.size() == 1);
    uintptr_t copiedAllocation = 0;
    memcpy(&copiedAllocation, copied[0].header.data()+0x28, 8);
    assert(copiedAllocation == allocation && copied[0].header == native);
    std::vector<Entry> original {Entry {7, 0}, name, Entry {8, 0}};
    auto replacement = original;
    assert(Substitute(replacement, copied) == Match::Complete);
    assert(replacement[0] == original[0] && replacement[2] == original[2]);
    assert(replacement[1][0] == original[1][0] && replacement[1][1] != original[1][1]);
    alignas(void*) uintptr_t rendererList = reinterpret_cast<uintptr_t>(original.data());
    const auto expected = rendererList;
    try
    {
        PointerScope scope;
        assert(scope.Enter(reinterpret_cast<uintptr_t>(&rendererList), expected, reinterpret_cast<uintptr_t>(replacement.data())));
        assert(rendererList == reinterpret_cast<uintptr_t>(replacement.data()));
        assert(original[1] == name); // original command and list never edited
        throw std::runtime_error("exercise scope restoration");
    }
    catch (const std::runtime_error&) {}
    assert(rendererList == expected);
    auto duplicate = original; duplicate.push_back(name);
    assert(Substitute(duplicate, copied) == Match::Invalid);
    auto unrelated = std::vector<Entry>{Entry {1, 0}};
    assert(Substitute(unrelated, copied) == Match::None);
    native[8] = 1; replacement = original;
    assert(Substitute(replacement, copied) == Match::Invalid); // recycled/modified command
    native[8] = 0;
    native[4] = 2;
    assert(!Capture(before, after, copied) && copied.empty()); // extended material header
    native[4] = 0;
    kind = 0x21; memcpy(native.data(), &kind, 4);
    assert(!Capture(before, after, copied) && copied.empty());
    after.count = 513;
    assert(!Capture(before, after, copied));
    {
        PointerScope scope;
        assert(!scope.Enter(reinterpret_cast<uintptr_t>(&rendererList), expected + 1, 0x1000));
        assert(rendererList == expected);
    }
    puts("PASS: copied commands preserve vertex allocation, list/order/resources; guards and exception restoration");
}
