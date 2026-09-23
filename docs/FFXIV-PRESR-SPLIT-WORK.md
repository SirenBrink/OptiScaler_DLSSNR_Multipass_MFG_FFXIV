# Split alternating NR work across two frames

2026-09-23, `a056ffc5-presr-split-work-test`. Experimental, default disabled.

The cadence diagnostic measured steady anchor completions around 22 ms and
skipped completions around 11 ms. NR, private DLSS SR, bounds and private FG were
all executed on the anchor. This option runs NR on A and its private SR/bounds/FG
on B. The successful current-edit bounds and pan-reset policy remain; the rejected
forward reprojection shader is not restored.

## Ownership and sequence

The encoded contribution and private NGX parameter block are retained from A to
B. A copies depth into an owned R32-float resource and composed normalized motion
into an owned RGBA32-float resource, and copies the camera structure. B never
substitutes its current guides or jitter/exposure/frame-time values. Depth capture
currently accepts only the R32/D32 single-plane, single-sample, single-mip family.
Unsupported/allocation failure reports failure and retains clean game SR output.

Full depth copies avoid partial-copy restrictions on depth-stencil resources.
Captured resource states and lifetimes belong to the existing generation, protected
by its completion markers. All work remains on the same direct queue. No CPU/GPU
wait, new queue, reprojection or guessed future anchor is introduced.

Three clean scene buffers hold current and two preceding frames. In steady state:

| Submitted frame | Work | Output scene and NR contribution |
|---|---|---|
| 0 | NR0 and snapshots | Clean scene0 during warm-up |
| 1 | SR/FG0 | Scene0 + NR0 |
| 2 | NR2 and snapshots | Scene0 + NR0 |
| 3 | SR/FG2 | Scene1 + interpolated NR1 |
| 4 | NR4 and snapshots | Scene2 + NR2 |
| 5 | SR/FG4 | Scene3 + interpolated NR3 |

Warm-up repeats the first scene while filling the pipeline. Steady output advances
one scene per real frame. This is **two rendered frames of scene delay**, one more
than legacy alternation. Post-upscale game effects and presentation FG still have
current guides; the existing delayed-scene/current-guide limitation is not solved.
Start validation with presentation DLSS-G off.

The staged anchor is valid only at its immediate next seam epoch. Cuts, lighting
events, gaps, abandoned seams, failures and live setting changes invalidate or
retire the pipeline. A toggle/quality change creates a fresh generation; pending
GPU resources are retired only after completion. Snapshot buffers and the third
scene buffer persist across ordinary anchors, not allocated every frame. Extra
VRAM is one full depth resource, one render-size RGBA32F motion resource, and one
output scene resource.

## UI and configuration

**DLSS Neural Rendering → Spread alternating NR across two frames (experimental)**,
below the approximate-camera option. Requires PreSR, every-second-frame NR,
approximate camera opt-in, and FFXIV. INI: `[DlssNr] SplitFrameWork=true`.
This experimental backend option is not in the strict version-1 artistic preset
schema. Status says `SPLIT work; NR on A, private SR/FG on B; scene delayed 2 frames`.
The log retains anchor/skip GPU completion intervals and adds snapshot cost.

## Validation

WARP executes the production snapshot/capture functions: deferred depth/motion
remain unchanged after source overwrite; the three-scene ring returns matching
images across wrap. Production schedule tests verify phase assignment, anchor/
midpoint identity, warm-up, reset and rejected nonconsecutive epochs. D3D12 debug
layer reports no errors. Existing bounds HLSL, pan policy, guide layout, legacy
buffer/timestamp and shutdown regressions pass. Actual NR/NGX interpolation and
perceived latency still require the user's game test.

Test fixed quality and scene with normal DLSS-G off. Run split mode for ~20 seconds,
then toggle only the split-work checkbox off and repeat. Keep the trailing guard on.
Compare smoothness, response lag, ghosting, flashing and the new cadence log.
