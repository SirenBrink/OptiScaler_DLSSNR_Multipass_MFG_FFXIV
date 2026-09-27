#pragma once
#include <string>
namespace DlssNrNative {
void* WrapNvapi(unsigned id,void* original);
void SetEnabled(bool enabled);
void SetPrecision(unsigned precision);
void BeginEvaluate(const void* feature, bool reset, unsigned every);
void EndEvaluate(bool success = true);
std::string VitStatus();
std::string Status();
}
