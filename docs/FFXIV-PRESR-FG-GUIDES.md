# Alternating NR: presentation FG guide alignment

Test build: a056ffc5-presr-fg-guides-test, 2026-09-23.

Split alternating NR retains its accepted A/B work distribution and two-frame scene buffer. Previously, presentation DLSS-G tagged current depth/MVs and current camera/jitter metadata against that older scene. The private residual FG guides were already separately snapshotted; this change addresses regular presentation FG.

The NR seam now publishes the actual successful output age once per bridge upscale (including 0/1-frame warm-up). The bridge passes it to DLSS-G. Each active presentation frame records owned depth and motion copies on the existing UI command list. Tagging selects the matching age, with OnlyValidNow lifetime; camera/jitter/MV scales and reset metadata are matched into the current presentation slot. Presentation frame IDs and Reflex scheduling remain current.

History selection requires an exact frame identity and matching texture shape, active dimensions, and offsets. Missing history, gaps, resize, resets, and FG toggles cannot fall back to current guides for a delayed scene: that FG submission is skipped. Successful displayed scene continuity also participates in the existing FG reset policy. Ordinary non-alternating FG retains its existing path. Snapshot textures are kept until the UI fence covers their last possible use, with open-list checks; unresolved teardown work is retained for process cleanup.

Validation: Release x64 build; extracted production guide-history tests with distinctive frame contents and camera/jitter values, wraparound, warm-up, gaps, resets, resizing, toggles, and pending-list retirement; existing WARP PreSR shader/buffer tests and shutdown regressions. These tests do not run NVIDIA's presentation FG runtime or establish visual improvement.

User test: enter gameplay with alternating NR, PreSR and approximate guides enabled. Enable regular DLSS-G, pan the camera, compare FG off/on, then change a DLSS quality preset. Look for reduced smearing without losing NR pacing. Log diagnostic: `DLSSG matched NR guides`, expected steady age 2 with SplitFrameWork. No additional scene buffering is introduced; guide copies add GPU bandwidth/memory cost. NR detail interpolation and downstream game effects may still contribute artifacts.
