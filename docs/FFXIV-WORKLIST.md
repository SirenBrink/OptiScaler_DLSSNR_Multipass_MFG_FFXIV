# FFXIV worklist

Updated 2026-09-23.

- Finish NR and PreSR history optimization. The user confirms [current-edit bounds](FFXIV-PRESR-CURRENT-EDIT-BOUNDS.md) resolve ghosting and disabling broad pan resets while bounds are enabled resolves flashing. Cuts/lighting resets and the legacy pan policy when bounds are off remain.
- Resolved in the first live test: the NR/PreSR exit crash reported by Dalamud. The [lifecycle fix](FFXIV-NR-EXIT.md) releases PreSR's completed private DLSS feature before NGX shutdown and prevents late CRT release. The 2026-09-23 test had one completed generation released, none pending, and no reported crash. Historical exit reports may have different causes; the existing exit delay remains.
- Correct guides for alternating-frame NR. The [cost investigation](FFXIV-PRESR-COST.md) shows reduced measured GPU cost with alternation. Restoring pan-onset rejection did not resolve ghosting, including with both FG paths off. The [PreSR guide-layout omission](FFXIV-PRESR-GUIDE-LAYOUT.md) is corrected and the next log confirms the intended crop; ghosting remains. Presentation DLSS-G now receives age-matched depth, motion and camera metadata; the user confirms substantially less smearing. Approximate camera transforms remain a limitation.
- Investigate rendering native game UI at the Reflex or VRR FPS limit independently of scene FPS. HUD interpolation was retired due to its performance cost.

Rejected: [current-scene NR reprojection](FFXIV-PRESR-CURRENT-SCENE.md) reintroduced whole-screen ghosting and did not improve smoothness, even with presentation DLSS-G disabled. Reverted to the successful bounds/continuity baseline. The [anchor versus skipped-frame completion timing](FFXIV-PRESR-CADENCE.md) then guided split scheduling.

Timing confirms roughly 22/11 ms alternating completion intervals versus ~19 ms steady every-frame NR. The [split-work experiment](FFXIV-PRESR-SPLIT-WORK.md) now stages NR on A and private SR/FG on B with owned anchor guides and two-frame scene buffering; the user confirms substantially smoother pacing. The one-frame mode remains available for lower buffering latency, with potentially less even pacing.

Completed baselines: [native scanned lighting and history rejection](FFXIV_NATIVE_LIGHTING_HISTORY.md), and the separately tested [NR/PreSR exit lifetime fix](FFXIV-NR-EXIT.md).

Accepted 2026-09-23: [matched presentation FG guides](FFXIV-PRESR-FG-GUIDES.md), [midpoint rejection and one-frame latency comparison](FFXIV-PRESR-LATENCY-REJECTION.md), and [softer rejection](FFXIV-PRESR-SOFT-REJECTION.md). The user reported acceptable remaining ghosting/latency, then approved the softer curve after movement testing. The preceding log used both scheduling modes, so this is not proof of a particular latency reduction. No additional history is introduced by soft rejection. Experimental bounds and split-work settings remain opt-in in the shipped config.

Also included in the tested build: cached observed DMFG factor display and the Reflex limiter transition fix previously confirmed working by the user. HUD interpolation remains retired. Future work includes remaining NR/PreSR artifacts, approximate camera guides, and native UI cadence.
