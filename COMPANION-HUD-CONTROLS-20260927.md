# Companion HUD controls — 2026-09-27

The normal FFXIV OptiScaler Companion panel contains:
- HUD replacement (nameplates and icons)
- HUD interpolation (2x)
- A short connection/runtime status and nameplate, update, interpolation and fallback counters.

Both options default off. The standard OptiScaler INI save/reload path persists them in [Companion], as HudReplacement and HudInterpolation. Interpolation requires replacement and preserves its saved preference while replacement is off. This replaces only world nameplates and associated icons, not menus, chat, or the full HUD. Persistent controls require Companion 0.1.3 or newer for the gameplay-readiness signal; older plugins cannot authorize replacement.

Enabled replacement has no 30-second deadline. Preparation, stale-data, heartbeat and visibility safeguards remain. The render thread prepares the layer once current nameplate data is available; on window focus/size changes or other layer stops, native drawing is retained and preparation is retried with a five-second backoff while enabled. Shutdown and disabling stop replacement; disabling also stops managed native submission copying. HUD replacement now requires a created OptiFG DLSS-G or XeFG swapchain. The plain OptiHDR/no-FG presenter is blocked after reproducible activation crashes. DLSS-G and XeFG are confirmed in-game; the XeFG test also confirmed OptiHDR output. Interpolation stays editable while replacement is off.

Detailed counters, timings, alignment markers, copied-submission/geometry tests, and the old timed test are under a collapsed Companion diagnostics tree. It appears only with an enabled logging destination and a log level other than Off. Alignment markers default off; switching logging off disables diagnostic geometry/submission work. Diagnostic native-submission controls are disabled while managed HUD replacement is enabled.

Validation: full Companion native/WARP suite passed. Added untimed-lifetime coverage for startup, repeat requests during preparation, retained stale-heartbeat/native-draw guards and stop behavior. The test runner now treats all nonzero process exits, including Windows assertion exits, as failures. Release x64 build passed with existing linker warnings. Continuous gameplay, automatic restart after alt-tab/resize, and saved-setting behavior still require live stress testing.

Stress test: enable HUD replacement then HUD interpolation in the Companion panel; run longer than 30 seconds, pan the camera, overlap/occlude nameplates, click through them, zone, alt-tab, change resolution/quality, toggle both controls, then exit. Compare FPS and input responsiveness with replacement disabled. Existing interpolation still supplies at most one intermediate position and can add one overlay refresh of delay; it does not track the scene's MFG multiplier.

Successful DLSS-G run 04:19:11–04:20:21: 7,418 layer presents, 2,754 native updates, 190 midpoints, 2,456 midpoint bypasses, 47 fallback events. Layer cadence periodically fell to native cadence (about 43–50 Hz), triggering the 8 ms interpolation latency cap. Some 144 Hz samples also bypassed; new bounded logging separates sequence, endpoint, cadence, age, missing guides and no safe movement without relaxing visual guards. The logs do not prove why FG setup prevents the no-FG native crash; no global Dalamud setting change is required or retained.

Final acceptance: user reports HUD replacement/interpolation working with XeFG and confirms XeFG HDR. Both checkboxes are disabled without an eligible OptiFG presenter, with an explanatory warning; preferences are retained. Dalamud hook mode was restored to its original setting. No diagnostic configuration changes are shipped.
