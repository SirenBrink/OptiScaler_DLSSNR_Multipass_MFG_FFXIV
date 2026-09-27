#pragma once
#include <cstdint>
#include <cstddef>

namespace FfxivCompanion
{
inline constexpr uint32_t Version = 1, MaxPlates = 50;
inline constexpr uint32_t Preview = 1, CameraValid = 2;
inline constexpr uint32_t WorldValid = 1, BoundsValid = 2;
// Capabilities: data receiver = 1, alignment view = 2. There is deliberately NO
// replacement-ready capability. Receipt/preview is not proof that originals may be hidden.
#pragma pack(push, 8)
struct Frame
{
    uint64_t sequence;
    int64_t qpc;
    uint32_t width, height, count, flags;
    float view[16], projection[16];
};
struct Plate
{
    uint64_t objectId;
    uint32_t slot, flags;
    float worldX, worldY, worldZ, anchorX, anchorY;
    float left, top, right, bottom;
    int32_t nameIcon, markerIcon;
    uint32_t textColor, edgeColor;
    char name[128];
    uint32_t reserved;
};
struct Status
{
    uint64_t accepted, rejected, sequence;
    int64_t ageQpc;
    uint32_t count, capabilities;
};
#pragma pack(pop)
static_assert(sizeof(Frame) == 160 && sizeof(Plate) == 200 && sizeof(Status) == 40);
static_assert(offsetof(Plate, name) == 68 && offsetof(Frame, view) == 32);
}
