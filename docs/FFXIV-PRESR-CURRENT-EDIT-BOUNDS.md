# PreSR current-edit bounds test

2026-09-23, build `a056ffc5-presr-bounds-test`.

The preceding guide-layout test logged correctly cropped 2257x1270 depth/motion
guides and working pan-onset resets. The user reports no ghosting with PreSR
disabled and ghosting with it enabled, with presentation FG sometimes masking
the effect slightly. This isolates the problematic rendering path; it does not
prove a particular NVIDIA history algorithm is at fault.

## Candidate

PreSR encodes NR's signed contribution, reconstructs it with a private DLSS
feature, then adds it to clean SR output. Previously the reconstructed edit was
only checked for nonfinite values and inverse-encoding poles. There was no check
that a reconstructed edit still existed in the current NR result.

`[DlssNr] PreSRTrailGuard` (default false) adds a live A/B experiment:

- At each evaluated NR anchor, compute the current encoded contribution's local
  3x3 RGB bounds at the matching normalized image position.
- Preserve reconstructed edits inside those bounds. Clip unsupported edits along
  a single RGB direction toward the current bilinear reference. This avoids
  independently clipping the channels; it is not a claim of exact perceptual
  hue preservation through the nonlinear carrier encoding.
- Run before residual FG. Ordinary composition and alternate-frame FG therefore
  consume the same bounded anchor; skipped frames do not use newer input to
  clamp an older anchor.
- Toggling invalidates private SR/held/residual-FG history once. It never resets
  main-game DLSS or presentation FG. See the follow-up below for pan-reset policy.
- Reuse one lazily allocated RGBA16F output texture by swapping ownership after
  successful dispatch. Both resources return to UAV state. The generation's GPU
  completion markers protect their lifetime, including shutdown and live toggles.
- Allocation/dispatch failure retains the clean SR frame and reports failure.

The menu checkbox is **DLSS Neural Rendering → Reduce PreSR trailing
(experimental)**, immediately below the PreSR status. The GPU cost line and log
include a separate `bounds` measurement per evaluated call. The allocation costs
about 63.3 MiB at 3840x2160 and is retained until the generation retires. The
experimental control is stored in the INI, not added to version-1 artistic NR
presets (which require an exact field schema).

This does not clear or modify DLSS's internal history. It limits the visible
contribution afterward. A one-input-pixel neighbourhood remains permissive, and
local NR changes may still flicker, lose fine detail, or appear blotchy. The
synthetic tests do not establish that it fixes FFXIV's observed artifact.

## Validation

- `tests/run_nr_presr_bounds.ps1`: actual shared HLSL executed on D3D11 WARP.
  Moving/departed edits lose unsupported trails; flat and supported edits remain;
  one RGB clipping fraction, image borders, noninteger scaling, and nonfinite
  inputs pass. Existing carrier/composition/motion shader regressions pass.
- Extracted production bounds function: repeated anchor ownership/state
  transitions, disabled bypass, allocation/dispatch failure, and re-enable pass.
- Production pan-reset cadence, guide metadata, WARP buffer/timestamp
  differentials, and bridge shutdown lifecycle regressions pass. Their negative
  controls still reproduce the original guide-layout and late-NGX-release bugs.
- DX12 and Vulkan shaders rebuilt; Release x64 build succeeds. Existing compiler
  and linker warnings remain. No in-game visual or GPU-cost confirmation yet.

## Live test

Start with NR + PreSR + the new bounds toggle enabled, presentation DLSS-G off,
and NR every second frame / approximate camera guides off. Keep quality and
scene fixed, pan the camera, then switch only **Reduce PreSR trailing** off and
on. Allow a few seconds after each toggle. Compare long trails, fine detail,
blotching, and flicker. Keep the next test focused on this pair; only revisit
alternate-frame NR once ordinary PreSR is acceptable.

## Follow-up: camera movement flashes

The user reports completely resolved ghosting, but occasional flashes resembling
one frame without NR. The 09:04–09:10 log (archived in workspace
`outputs/presr-flash-20260923-091040`) confirms repeated pan-onset resets with
bounds enabled, both ordinary and alternate-frame NR. No bounds/evaluation/
composition failure or GPU-slot fallback was logged. The bounds pass costs
approximately 0.15–0.16 ms per evaluated anchor in this run.

The old broad pan reset still resets private SR and residual FG. Mode 10 returns
the clean raster if NVIDIA suppresses that FG result, so these resets are a
plausible cause of the reported flash. The log does not record individual
suppression flags and cannot prove exact correlation with the observed flashes.

`a056ffc5-presr-bounds-continuity-test` leaves the successful bounds shader
unchanged and disables pan sampling/resetting while bounds are enabled. Turning
bounds off restores the legacy pan policy. Toggle transitions clear stale pan
samples/pending onset and reset the private histories once; scene cuts, lighting
events, gaps and failures still reset normally. Production-policy regression
tests cover both modes and preservation of cut/lighting reset propagation.

The next live test repeats the camera movements with bounds enabled, using the
user's saved regular FG and alternate-frame NR settings. Visual confirmation
that flashes are resolved and ghosting remains absent is pending.
