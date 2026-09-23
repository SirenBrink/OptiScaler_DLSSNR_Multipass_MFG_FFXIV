# Alternating NR: midpoint rejection and one-frame latency comparison

Test build: a056ffc5-presr-latency-rejection-test (2026-09-23).

The preceding guide-alignment test was reported to reduce smearing substantially, with small residual NR ghosting and input lag. Its log is archived in outputs/presr-fg-guides-result-20260923-174213. It confirms matching age=2 and late steady GPU completion intervals around 30 ms. Those intervals are not input-to-display latency measurements.

New composition guard: when Reduce PreSR trailing is enabled, interpolated NR edits are weighted down where the displayed clean midpoint disagrees strongly with the next clean NR anchor. Comparison uses pre-exposure-normalized RGB, one shared confidence for all channels, and keeps the clean raster and alpha unchanged. Stable edits are preserved. This adds two constants and one texture read in the existing midpoint composition pass, with no extra allocations or passes. The reference is the current clean A for the legacy schedule, or the previous clean A for split B; it is never the newer, unprocessed B.

This is deliberately a conservative screen-space change heuristic. It does not establish true surface correspondence, fix proprietary model history, or eliminate all ghosting. Motion/lighting changes can locally weaken NR on interpolated frames, causing visible variation; gameplay testing is necessary.

Latency experiment: set existing DlssNr/SplitFrameWork=false. This uses the existing one-frame alternating schedule, with the now-correct presentation FG guides. No new low-latency scheduler is claimed: NR/private SR/residual FG again concentrate on A, so uneven pacing may return. Relative to split mode, scene buffering drops by one real frame. Exact input latency improvement is not measured or guaranteed. Restoring Spread alternating NR across two frames preserves the new midpoint guard and returns to the accepted smoother two-frame schedule.

Validation: DX12 and Vulkan shader compilation; Release x64; WARP HLSL checks of stable edits, changed pixels, alpha, common RGB rejection, exposure compensation, invalid references, and unaffected anchors; extracted production reference selection for all three split slots and legacy mode; existing PreSR buffers, guide metadata, history/reset, shutdown, and DLSS-G guide matching regressions. Existing compiler/link warnings remain.

Test in gameplay: first compare camera movement with regular FG off/on. Check ghosting, responsiveness, pacing and any alternating brightness/blotching. If pacing worsens, enable Spread alternating NR across two frames and compare on the same build. Keep Reduce PreSR trailing enabled for the new guard. Installation backs up the previous DLL/INI/log and starts regular FG off.
