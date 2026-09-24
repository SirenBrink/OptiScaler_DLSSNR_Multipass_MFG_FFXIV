# XeMFG and alternating PreSR NR

## Accepted configuration (2026-09-23)

4x is the current practical maximum from local gameplay testing. Dynamic
XeMFG is capped at 4x; manual 5x–8x remain experimental with poorer latency
and smoothness. The accepted build includes native timing-clamp preservation,
scheduler and generated-present accounting, rebuild-stall filtering, recovery
resets, camera fallback, and shared delayed PreSR guides.

The final dynamic test confirmed 2x → 3x → 4x at a 138 FPS target,
4x → 3x → 2x at 60 FPS, and 3x at 78 FPS. All observed dynamic selections
stayed at or below 4x, without rapid oscillation. Release x64 and the controller,
camera, pacing and patch regression tests passed. This is single-system
gameplay validation, not a claim of universal hardware compatibility.

Known issues: higher manual factors need further latency/pacing work; capped
real-frame timing can hide downshift headroom; the existing XeFG
`XEFG_SWAPCHAIN_RESULT_ERROR_POINTER_STILL_IN_USE` error remains during shutdown.
The sections below document the integration and successive test changes.

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

## First gameplay result and pacing follow-up

The first test reported usable 2x–4x, excessive latency at 5x, and poor
throughput at 6x–8x. At 8x the pacing log measured about 12–13 real fps.
This does not establish a runtime factor limit or identify the sole bottleneck.
Faint distant-object ghosting remains unresolved.

The follow-up includes native final-frame scheduler waits in the render-time
estimate; previously only our inserted waits were removed. It also subtracts
each wait from its own measured interval instead of a median of other frames.
The estimate is still CPU timing, not GPU work or end-to-end latency. Existing
deadline spacing and NR history are unchanged. Regression coverage verifies
native final-frame accounting, the 2x-only burst start, no accounting on
unrelated scheduler indices, and subtraction independent of the period median.
Compare at a fixed 4x before testing higher factors again.

The next log captured resolution rebuilds exported as 3139 ms and 2980 ms
frame-time estimates, followed by much slower pacing. This demonstrates
contamination by pauses, but does not prove a leak or explain all NR-on cost.
The stall-filter test rejects measured intervals over 250 ms, clears our timing
window on such gaps, and uses a five-sample median of valid work estimates.
Submission also rejects nonfinite, negative or over-250-ms timing (including
fallback timing), uses the existing zero/automatic mode and requests history
reset unless the user explicitly disabled resets. This is a discontinuity
filter, not an FPS cap. Native provider timing history is not directly edited.
Regression tests cover the observed multi-second pause, fresh recovery and
an isolated outlier. Test fixed quality and multiplier with NR off/on/off,
then test a resolution change separately. Sustained slowdown under unchanged
settings still requires investigation.

The fixed-settings test stayed around 26–28 real fps with NR off, 20–21
with NR on, and recovered with NR off. Resolution changes still caused
temporary slowdowns lasting tens of seconds, despite rejected timing spikes.

The recovery test clears our pacing window at a burst boundary following
XeFG activation or a measured interval over 250 ms. Activation requests are
atomic; only the present thread edits scheduling state. For 32 new bursts,
the timestamp hook preserves Intel's native clamped interval rather than
extending it using the provider's older median. Subsequent generated frames
retain equal spacing within each burst. Normal scheduling resumes afterward.
Tests cover deferred reset, stale-estimate suppression, recovery spacing,
the last recovery burst and return to ordinary scheduling. GPU waits and NR
image history are unchanged. This is an experimental recovery policy; test
quality changes with NR on at fixed 5x and check pacing during recovery.

The burst-timing build adds diagnostics without changing scheduling decisions.
Every five seconds it reports completed-burst average/maximum duration,
scheduler duration by generated-frame index (including the native final call),
generated-present duration and call count, remaining uninstrumented wall time,
and provider median versus scheduled interval. Windows do not mix factors or
include intervals over 250 ms. A flag indicates recovery was included.
These are CPU wall times; scheduler and present calls may contain GPU waits.
The generated-present counts exclude the real frame and cannot be used as a
display FPS counter. Tests cover accounting, factor isolation and pause
exclusion. Compare 4x–5x–4x at fixed quality with NR on, about one minute each.

## Native-clamp and camera comparison test

The burst log measured roughly 38.5 ms total / 25.5 ms scheduler at 4x,
versus 47.2 ms / 35.5 ms at 5x. Scheduler calls may include GPU waits;
these figures alone do not establish excess sleep as the sole cause.
The new test retains the provider's clamped interval in ordinary scheduling
as well as recovery. It keeps the first native deadline and evenly spaces
later generated frames using that interval, without bypassing GPU waits.
Diagnostics remain enabled to compare the result.

The FFXIV bridge supplies estimated projection parameters but no world-space
camera pose. XeFG previously left its view matrix zero in this case. A tested
row-major view helper now uses identity for absent/degenerate pose, handles
valid rotated cameras at the world origin, and retains translation for a
valid pose. Projection construction also uses local near/far values rather
than modifying shared source metadata. This is a stationary approximation,
not recovered native camera matrices. Its visual effect requires gameplay testing.

Depth, motion vectors, jitter, camera metadata, and scene age already use the
shared delayed-guide matcher. Motion-vector scaling follows each API's
different convention. Alternating NR retains its private NVIDIA residual FG
and approximate-camera path independently of the presentation FG backend.
The log confirms XeLL is selected for XeFG. No missing DLSS-G-provided NR
input was established by this comparison. Camera fallback and pacing are
separate code changes included together in this test DLL; an improvement
cannot be attributed to one alone without a follow-up comparison.

## Generated-present accounting experiment

The native-clamp test was reported as much smoother, but 5x remained less
usable than 4x. At steady 5x, diagnostics measured roughly 31–34 ms inside
generated-present calls and 2–4 ms in the scheduler, in a 46–48 ms burst.
The existing render estimate removed only scheduler time. The next experiment
also excludes generated-present call duration, measured after scheduling so
the intervals do not overlap. The estimate now represents CPU time outside
these provider calls, not GPU execution time. Native presentation can include
GPU work, so this can understate actual rendering cost and needs gameplay
validation. No native GPU synchronization is bypassed. Burst diagnostics,
native timing clamp, camera fallback, and NR history remain unchanged.
The regression verifies separate scheduler/present accounting and subtraction
from the same interval. Compare 4x–5x–4x with NR on at fixed quality; try 6x
only after confirming 5x pacing and responsiveness.

## Dynamic XeMFG

OptiFG → XeFG offers Dynamic XeMFG, Target FPS and the currently selected
factor. This is an OptiScaler controller, not an Intel native dynamic mode.
It selects 2x–4x, additionally constrained by the runtime's reported maximum.
Manual factors are preserved and restored when dynamic mode is disabled.
Only successful provider requests update the displayed selected factor.

Selection uses real-present intervals from the existing frame tracker, with
a 500 ms smoothing time constant, 7% undershoot / 10% headroom bands,
one-second upward and two-second downward dwell, and at least three seconds
between changes. Invalid/loading samples restart observation. The effective
target is the lower of DynamicTargetFPS and a positive configured FPS cap.
This target controls factor selection, not an additional frame limiter.
At a frame cap, measured timing can hide headroom for reducing the factor;
the controller conservatively retains its factor in that case.

INI keys: [XeFG] DynamicMFG=false, DynamicTargetFPS=138. Global Save Settings
persists these preferences. Dynamic mode never overwrites InterpolationCount.
Live factor changes retain the existing XeFG toggle/pacing-reset workaround.
Tests cover upward/downward selection, the hard 4x ceiling, runtime limits,
dwell/cooldown, and recovery from loading or invalid samples. Gameplay testing
must check factor transitions, FG toggles and target edits with NR active.
