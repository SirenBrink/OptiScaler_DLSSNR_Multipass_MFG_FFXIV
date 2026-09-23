# FFXIV worklist

Updated 2026-09-23.

- Finish NR and PreSR history optimization. Native scanned lighting and conservative lighting-cut rejection are validated; broader movement/history improvements remain open. Retain the accepted motion rejection baseline.
- Resolved in the first live test: the NR/PreSR exit crash reported by Dalamud. The [lifecycle fix](FFXIV-NR-EXIT.md) releases PreSR's completed private DLSS feature before NGX shutdown and prevents late CRT release. The 2026-09-23 test had one completed generation released, none pending, and no reported crash. Historical exit reports may have different causes; the existing exit delay remains.
- Correct guides for alternating-frame NR. Performance investigation follows guide correctness; approximate camera opt-in currently has an unacceptable cost.
- Investigate rendering native game UI at the Reflex or VRR FPS limit independently of scene FPS. HUD interpolation was retired due to its performance cost.

Completed baselines: [native scanned lighting and history rejection](FFXIV_NATIVE_LIGHTING_HISTORY.md), and the separately tested [NR/PreSR exit lifetime fix](FFXIV-NR-EXIT.md).
