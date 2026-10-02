#pragma once
#include <string>
namespace DlssNrNative {
void* WrapNvapi(unsigned id,void* original);
void SetEnabled(bool enabled);
void SetPrecision(unsigned precision);
void BeginEvaluate(const void* feature, bool reset, unsigned every, bool allowUnverified = false);
void EndEvaluate(bool success = true);
// The stable policy recomputes downstream passes; disabling it restores all-pass reuse.
inline unsigned ReuseEveryForPass(unsigned pass, unsigned requestedEvery, bool firstPassOnly = true)
{
    return (!firstPassOnly || pass == 0) && requestedEvery > 1 ? 2u : 1u;
}
std::string VitStatus();
std::string Status();
}
