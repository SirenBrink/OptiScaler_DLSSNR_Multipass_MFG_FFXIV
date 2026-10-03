#pragma once
#include "State.h"
template<class T> struct Setting{T v;T value_or_default(){return v;}};struct Config{Setting<int> FfxivHDRMode{0};Setting<bool> FfxivHDR{true},FfxivHDRReShadeHighlights{false}; Setting<::FGOutput> FGOutput{::FGOutput::DLSSG};Setting<float> FfxivHDRPeak{1100},FfxivHDRPaper{203},FfxivHDRExpansion{0},FfxivHDRContrast{1},FfxivHDRSaturation{1},FfxivHDRVibrance{0};static Config* Instance(){static Config c;return &c;}};
