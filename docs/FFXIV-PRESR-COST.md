# Alternate-frame PreSR cost investigation

2026-09-23: cost test reported better performance but heavy ghosting with normal
DLSS-G enabled. The follow-up pan-history correction still needs live validation.

“Allow approximate FG camera guides (experimental)” permits a separate NVIDIA
frame-generation instance for the NR residual. Its cost includes FG inference,
motion preparation, and delayed raster history; it is not just camera arithmetic.
Skipping every second NR/private-DLSS evaluation may not recover that cost.
Actual FFXIV camera transforms remain unavailable in this path.

## Changes

- Rotate the two normalized motion textures with their resource-state flags,
  eliminating a motion-field copy each active frame.
- Capture the clean raster directly into its history slot, eliminating the
  intermediate full-resolution image copy in alternate-frame mode.
- Compose two-frame motion only on NR anchor frames, where it is consumed.
- Apply the existing pan-onset rejection to both ordinary and alternate-frame
  PreSR. An onset detected on a skipped frame stays pending until private DLSS
  successfully evaluates. Reset residual FG on the same anchor as its carrier.
  Rejection thresholds, quiet rearm, and cooldown are unchanged.
- Sample GPU cost for guides, NR, private residual DLSS, residual FG, and
  composition. Four consecutive slots per 16-slot cycle cover both anchor and
  skipped frames. Results are consumed only after existing GPU completion
  markers, without frame-path waits or new map operations.

Cadence, history delay, image formats, and the accepted NR exit fix are retained.
This does not provide exact camera guides or promise that alternate-frame NR is
faster on every configuration.

The menu's **Residual DLSS** status and periodic `PreSR GPU cost per call` log
entries show smoothed milliseconds per invocation. Skipped evaluations are not
counted as zero-cost calls. These values are not total frame time; some conversion
work lies outside the measured stages. NR anchors/skipped counters establish
whether alternation actually ran. With a steady 1:1 cadence, NR/private SR/FG
invocation costs are each incurred approximately every other real frame.

## Validation

- Release x64 build passed (existing linker warnings remain).
- `tests/run_presr_buffers.ps1`: WARP differential readback compares original and
  optimized raster histories through cadence, resets, and half-rate fallback;
  verifies motion rotation and sampled timestamp counts. No D3D12 debug-layer
  errors. Extracts the actual capture/rotation statements from production.
- NR bridge shutdown regression passed, including the negative control that
  reproduces the original late NGX release; NR GPU lifetime WARP test passed.
- `tests/run_nr_presr_pan_cadence.ps1`: extracted production policy/parameter/
  acknowledgement code retains a skipped-frame onset, resets SR and residual FG
  together, avoids repeated flushes in a sustained pan, and retains requests on
  failed evaluation. Ordinary mode and explicit scene-reset behavior pass.

## First live result and remaining risk

The 08:15–08:18 log confirms steady 1:1 NR anchors/skipped frames and successful
shutdown (one generation released, none pending). At the in-game 2257x1270 input
and 3840x2160 output, NR costs about 6.8 ms per invocation, private SR about
1.6–1.7 ms, and residual FG about 2.2 ms. Accounting for their cadence, measured
stages total roughly 5.7–5.8 ms per real frame with alternation versus 8.7 ms
without. These are partial GPU costs, not end-to-end FPS or a before/after
measurement of the copy optimization.

No pan-onset samples were taken while approximate guides were on. With guides
off, the accepted policy detected and rejected several onsets. This is a concrete
history-policy gap addressed by the follow-up build.

The alternate-frame composition still delays scene color by one real frame;
ordinary presentation FG is not taught that delay by this path. Current-frame
guides combined with delayed color are a separate suspected contributor. The
approximate camera transforms and this downstream timing issue remain open.

## Live comparison

Under **DLSS Neural Rendering**, retain **Generate before SR, apply after SR
(DLSS)** and **NR every second frame (Nvidia FG, experimental)**. In a fully
loaded in-game scene, compare **Allow approximate FG camera guides
(experimental)** off and on for 15–20 seconds each, keeping DLSS quality, model
settings, and normal FG factor fixed. Pan the camera to check ghosting and
responsiveness, then exit normally. The new status line appears below
**Residual DLSS**; the log records its cost breakdown automatically.

For the pan-history follow-up, leave NR, PreSR, and approximate guides enabled.
First pan with normal DLSS-G **off**; then turn normal DLSS-G **on** and repeat
in the same scene. Stop the camera for at least a second between pans to let the
existing onset gate rearm. Compare against guides off if either is still poor.
The log records when the pending onset is consumed by a private SR/FG anchor.
