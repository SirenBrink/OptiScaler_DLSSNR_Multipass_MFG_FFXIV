# Alternating NR: preserve the current FFXIV scene

**Rejected in live testing and reverted.** The user reports whole-screen ghosting
and no smoothness improvement, including with normal DLSS-G disabled. The
implementation and its dedicated selection test were removed; descriptions below
record the experiment, not current behavior. Do not revive reprojection merely
because synthetic tests passed.

2026-09-23, `a056ffc5-presr-current-scene-test`.

The user confirmed that current-edit bounds remove ghosting and that avoiding
pan-onset resets while bounds are active removes flashing. Alternating NR still
feels like half the expected frame rate.

The log is archived in workspace `outputs/presr-cadence-20260923-092858`.
The half-rate path is actually alternating, not stuck: NR anchor/skipped counts
advance equally. Approximate per-evaluated-call GPU costs are NR 7.1 ms, private
SR 1.7 ms, bounds 0.16 ms, residual FG 2.3 ms, composition 0.32 ms. NR/SR/FG
concentrate on anchors; the log does not measure displayed frame-time variance.

## Confirmed mismatch and candidate

Previously, the steady-state half-rate composition selected the preceding clean
scene frame. Depth, motion, and subsequent game rendering stayed current. The
scene itself was not held at half rate (apart from initial warmup), but this
one-frame mismatch is unsuitable for normal presentation DLSS-G.

FFXIV now always composes onto the **current** clean SR image. Only the previous
frame's NR contribution is moved into current coordinates using the current
one-frame normalized motion field:

- On an anchor, private FG produces the intervening frame's NR contribution.
- On a skipped frame, the preceding anchor's NR contribution is used.
- Both contributions are therefore one rendered frame old and use the same
  one-frame reprojection, not the two-frame anchor field supplied to private FG.
- The prior clean scene is retained only to check correspondence. Large colour
  mismatches smoothly reject the edit, correcting for pre-exposure differences.
  Invalid/offscreen vectors and NVIDIA suppression retain current clean pixels.
- Warmup, scene resets and fallback use current clean output directly. The
  existing successful bounds and pan-reset changes remain. No extra GPU texture,
  full-frame copy, CPU wait, or NGX evaluation is added.

Other games retain the legacy experimental delayed-scene behavior. FFXIV status
now reports `CURRENT scene + reprojected prior NR edit`.

This is a reprojection approximation. Newly revealed surfaces, low-resolution
motion edges, specular/translucent effects, and approximate FG cameras remain
limitations. Colour rejection can locally reduce NR. The contribution is still
one frame old; keeping the scene current does not predict fresh NR details.
Uneven anchor/skip GPU work also remains and may require separate pacing work.

## Validation

`tests/run_nr_presr_current_scene.ps1` extracts the actual composition selection:
scene identity advances every frame; current motion and previous clean reference
match; anchor/midpoint choice, warmup/cut, fallback and non-FFXIV paths pass.

The shared HLSL runs on WARP in `nr_skin_shader_smoke.cpp`: an eight-frame moving
sequence verifies current scene retention and displaced edits, plus midpoint
suppression, offscreen/invalid vectors, mismatched clean colour, alpha and
pre-exposure compensation. Existing bounds, pan-policy, guide metadata, buffer/
timing and bridge shutdown regressions pass. DX12/Vulkan shaders are regenerated.

Live validation remains necessary: compare camera panning and movement with
alternate-frame NR enabled/disabled at fixed quality and FG factor. First test
with the user's saved settings; if roughness remains, one comparison with normal
DLSS-G disabled helps distinguish NR cadence from presentation interpolation.
