# PreSR motion-guide layout correction

2026-09-23, following `a056ffc5-presr-pan-test`.

The 08:27–08:31 test still showed heavy ghosting with presentation FG off and
approximate residual-FG guides off. The log confirms those switches worked:
normal FG was disabled at 08:30:05; residual FG stopped at 08:30:26 and NR returned
to every-frame operation. Pan-onset resets were sampled and consumed. Their
absence cannot explain the remaining result. Shutdown released one completed
generation with none pending.

## Confirmed metadata error

The ordinary NR entry point sets `MotionVectorsLowResolution` and the DLSS
output extent in `DlssNrFrameInfo`. The separate PreSR entry point omitted both,
leaving false/zero defaults. `ResolveGuideRegions` then treated motion as
output-resolution and, with no output extent, used the entire allocation.

At the user's Balanced setting, the log reports a 2257x1270 active frame and
depth guide but a 3840x2160 motion region, despite the bridge reporting
`lowResMV=true`. The correct motion region is the active 2257x1270 rectangle.
Sampling the full allocation maps guides to the wrong scene positions and can
include stale padding. Vector scale (3840x2160 in this game) is an encoding factor
and is deliberately retained; it does not determine the valid guide rectangle.

The correction propagates the same flag and output extent as ordinary NR. Compact
normalized alternate-frame motion remains compact. Explicit synthetic full-frame
guide extents remain honored. No extra GPU copies or history flushing are added.

## Validation and limits

`tests/run_nr_presr_guide_metadata.ps1` extracts the actual PreSR metadata
assignments and runs them through the production guide-region resolver. It covers
the observed Balanced dimensions, DLAA/Quality/Performance, padded and compact
motion, real display-resolution motion, and synthetic source-frame overrides.
The negative control removes the two assignments and reproduces the old error.

This is a confirmed input-contract correction, not yet a verified visual fix.
It principally affects ordinary PreSR using padded guides; it does not establish
the root cause of all alternate-frame ghosting. Approximate camera transforms and
the delayed-color/current-guide interaction with presentation FG remain open.

## Live test

Start with NR and PreSR enabled, approximate guides disabled, and presentation
DLSS-G disabled. At a fixed in-game quality setting, check camera movement.
If the baseline is improved, enable approximate guides and compare the same pan.
If the baseline is still poor, disable **Generate before SR, apply after SR
(DLSS)** to compare ordinary before-SR NR. Exit normally and retain the log.

Outcome: the 08:40–08:45 run (`outputs/presr-isolation-20260923-084549` in the
workspace) confirms the corrected active guide region. The user still reports
ghosting with PreSR on and none with it off. Keep the metadata correction, but
do not count it as a solution to the remaining artifact. The next isolated test
is [current-edit bounds](FFXIV-PRESR-CURRENT-EDIT-BOUNDS.md).
