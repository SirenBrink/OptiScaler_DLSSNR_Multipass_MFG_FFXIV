# FFXIV worklist

Updated 2026-09-23.

- Finish NR and PreSR history optimization. Native scanned lighting and conservative lighting-cut rejection are validated; broader movement/history improvements remain open. Retain the accepted motion rejection baseline.
- Resolve the Dalamud exit crash when NR has been active. Investigate an early, reliable exit signal, stop new NR/PreSR work, and respect submitted GPU work before releasing resources. NR off from startup avoids the reported crash; that alone does not establish the cause or prove toggling it off before exit is sufficient.
- Correct guides for alternating-frame NR. Performance investigation follows guide correctness; approximate camera opt-in currently has an unacceptable cost.
- Investigate rendering native game UI at the Reflex or VRR FPS limit independently of scene FPS. HUD interpolation was retired due to its performance cost.

Completed baseline: [native scanned lighting and history rejection](FFXIV_NATIVE_LIGHTING_HISTORY.md). These results do not establish a fix for the separate NR-dependent exit crash.
