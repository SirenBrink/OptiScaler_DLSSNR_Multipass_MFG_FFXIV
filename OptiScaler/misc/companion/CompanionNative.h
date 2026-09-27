#pragma once
#include <cstdint>
namespace FfxivCompanion::Native
{
struct Statistics
{
    bool installed, requested;
    uint64_t captures, replacements, packets, fallbacks;
};
void Install();
void Detach();
void SetRequested(bool value);
Statistics GetStatistics();
}
