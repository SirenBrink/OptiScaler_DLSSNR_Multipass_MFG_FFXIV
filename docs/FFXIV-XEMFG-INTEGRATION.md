# XeMFG and alternating PreSR NR

Integrated Citrus333's XeFG MFG implementation from commit
`e1a0f2187f57b7aeb13ee480602c02e3de4555d6` in
https://github.com/Citrus333/OptiScaler_DLSSNR_Multipass_MFG_FFXIV.
The implementation exposes up to seven interpolated frames (8x total), with
optional per-frame pacing. Citrus's accompanying pacing/unlock READMEs describe
the original reverse engineering.

## FFXIV integration

XeFG and DLSS-G now share the tested presentation-guide snapshot code. When
alternating NR displays a delayed scene, XeFG receives depth, motion, jitter,
and camera metadata from that scene's frame. Missing or mismatched history
skips FG until matching guides are available. Existing PreSR soft rejection
and split-frame scheduling are preserved.

This changes the presentation FG backend only. Alternating NR's private
residual-generation path still uses NVIDIA FG; this does not make that feature
available on other GPU vendors.

The upscaler-change reset option affects XeFG only. FFXIV pacing uses fresh
provider timing estimates where available; stale estimates expire. Pacing
index validation supports the entire advertised 2x–8x range.

## Runtime checks

The unlock targets libxess_fg.dll 1.3.1.78, PE timestamp `0x69CB0F4D`, image
size `0x015ED000`. Other builds remain unmodified. All five patch sites are
checked before writing; a failed write triggers rollback. Pacing hooks also
check their expected thunk bytes. Runtime files on disk are not patched.

The local installed runtime matches all five unlock sites and three pacing
thunks. This is compatibility evidence, not proof of gameplay stability.

## Validation and testing

- Release x64 build passed.
- Production-header regression tests passed for patch preflight, rollback,
  repeat application, thunk installation, 2x–8x pacing and stale timing.
- Shared delayed-guide tests passed for warmup, scene gaps, resets, dimensions,
  metadata matching and resource retirement.
- Existing PreSR WARP/shader, scene-order and shutdown regressions passed.

Gameplay validation remains necessary. Start with XeFG at 2x, then try 3x and
4x under OptiFG's XeFG section. Compare alternating NR off/on while moving the
camera, and test a quality change and FG off/on. Higher factors can follow once
these are stable. Check pacing, ghosting, HUD appearance and shutdown.
