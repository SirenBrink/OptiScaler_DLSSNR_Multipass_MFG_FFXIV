#pragma once
#include "MipBiasPolicy.h"
#include <atomic>
#include <Config.h>
#include <State.h>
#include <Logger.h>
namespace SamplerMipBias {
inline std::atomic<bool> automaticDx11Available {false};
inline std::atomic<bool> automaticDx11Active {false};
template<class Sampler> void Apply(Sampler& s) {
    const auto& cfg=*Config::Instance();
    // The native FFXIV binding path owns bias in automatic mode. In particular,
    // do not leak a stale manual value into internal DX12/NGX samplers.
    if (automaticDx11Active.load(std::memory_order_relaxed)) return;
    const std::optional<float> value=cfg.MipmapBiasOverride.has_value()?std::optional<float>(cfg.MipmapBiasOverride.value()):std::nullopt;
    const float before=s.MipLODBias;
    s.MipLODBias=MipBiasPolicy::Apply(before,s.MinLOD,s.MaxLOD,unsigned(s.Filter),value,
        cfg.MipmapBiasFixedOverride.value_or_default(),cfg.MipmapBiasScaleOverride.value_or_default(),
        cfg.MipmapBiasOverrideAll.value_or_default());
    if (s.MipLODBias!=before) LOG_DEBUG("Sampler mip bias: {} -> {}",before,s.MipLODBias);
    if (s.MinLOD<s.MaxLOD && (unsigned(s.Filter)&0x180u)==0 && std::isfinite(s.MipLODBias)) {
        auto& state=State::Instance();
        state.lastMipBias=std::min(state.lastMipBias,s.MipLODBias);
        state.lastMipBiasMax=std::max(state.lastMipBiasMax,s.MipLODBias);
    }
}
}
