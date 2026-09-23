# Softer interpolated-NR rejection

Test: a056ffc5-presr-soft-rejection-test, 2026-09-23.

User reported minor movement flicker with otherwise acceptable ghosting and latency. Previous log archived under outputs/presr-latency-result-20260923-175424. It started in one-frame mode, switched to two-frame split mode at 17:50:26, and later used matching age-2 DLSS-G guides. Do not attribute the entire result to one-frame scheduling. Saved INI still has SplitFrameWork=false.

Change: replace the single 0.15..0.50 scene-change rejection ramp with two smooth ramps. Moderate differences reject at most 35% of the interpolated edit; the remaining 65% is rejected across stronger 0.50..0.90 differences. Stable edits remain full strength. Invalid references and large mismatches retain complete rejection. A shared RGB weight preserves hue, alpha and the clean raster. There is no temporal smoothing, added history, or buffering; this only reduces strength contrast against adjacent NR anchors for moderate changes. It cannot bound arbitrary temporal changes or guarantee flicker removal. Looser rejection may allow more ghosting.

Validation: actual HLSL executed on WARP, including a 91-point monotonic confidence sweep, bounded moderate rejection, full strong rejection, unchanged real anchors, exposure compensation, alpha/hue and invalid-reference tests. Existing PreSR shader/buffer, guide metadata, reset and shutdown regressions passed. DX12/Vulkan shader builds and Release x64 passed with existing warnings.

Deploy DLL only; retain saved INI. Compare camera movement, flicker and trails in the same scheduling mode as before. Both one- and two-frame paths use the revised guard. Previous DLL, INI and log are backed up.

Live result: user approved the softer-curve test ("That's great, actually") and requested publication. No objective end-to-end latency measurement was made.
