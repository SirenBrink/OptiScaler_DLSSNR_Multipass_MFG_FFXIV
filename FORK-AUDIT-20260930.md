# Fork review - 2026-09-30

Scope: risk-focused review of the FFXIV fork at e3f36a4d plus the entire current working tree, including the pending HDR menu/screenshot feature. This is not a claim that every inherited upstream line or every driver/runtime combination has been proven correct.

## Confirmed fixes

- Cancel unsubmitted FG UI/swapchain work before invoking provider deactivation. XeFG and FSR deactivation can otherwise submit the very work being cancelled during upscaler teardown.
- Treat the D3D12 removed-device fence sentinel as failure, recheck completion after an event wake, and reject missing completion objects when work is pending. Applies to common FG UI allocator reuse and the DX11/DX12 bridge copy allocator/queue waits.
- Latch bridge copy-queue signal failures. An older completed fence cannot prove that newly submitted work has finished. Retain interop resources if cleanup cannot prove completion, rather than releasing GPU-visible objects early.
- Fence optional swapchain UI/comparison command submissions for DLSS-G, XeFG and FSR. Wait before allocator reuse; assign SC fence values in submission order; stop that path after a close/signal failure. Reject null command lists in the two downstream shaders. This does not change Companion HUD replacement or NR history.
- Use a monotonic clock for the generic frame limiter, separate timer handles/timestamps by calling thread, and reject nonfinite/nonpositive caps before integer conversion. Preserve the existing FG factor policy.
- Harden screenshot ownership against allocation failures after GPU submission. Establish pending ownership before scheduling the worker; finish throwing preparation before recording GPU work. Only one failed/pending screenshot can retain resources.

## Optimizations

- Restrict HDR-menu background copies, layer clears and final compositing to the union of actual menu geometry. Arbitrary draw callbacks/nonfinite geometry conservatively use the whole surface. No change to the accepted HDR palette or 203-nit reference white. Full-size reusable textures remain allocated.
- Encode PNG metadata directly from the WIC stream storage instead of copying the entire encoded PNG into another vector. Screenshot request-free frames return before configuration/foreground checks and allocate no capture resources.

These reduce work and peak memory structurally; no in-game FPS gain is claimed without measurement.

## Reviewed existing areas

Shared frame-generation command submission and presentation guides; DX11/DX12 bridge waits and shutdown; NR GPU lifetime/deferred shutdown; PreSR bounds, guide metadata and cadence; XeMFG scheduling/dynamic selection; Ada compatibility guards; Companion mailbox validation/provider gating; native lighting readback/recovery; NR preset validation and atomic file replacement; pending HDR composition, configuration and PNG capture.

The shadow-flicker investigation remains separate. No NR history thresholds, exposure tuning, graphics settings, or user INI values were changed in this review.

## Validation

- All 15 existing baseline suites passed before changes and after changes: NR GPU lifetime, bridge shutdown, community controls, PreSR bounds/guide metadata/pan cadence/spatial smoke/buffers, XeMFG integration, delayed DLSS-G guides, HDR10, Companion, lighting recovery, HDR menu layer and PNG screenshots.
- New `tests/run_fork_safety.ps1` compiles extracted production functions and checks cancellation order, removed-device and stale-event handling, timeout/event setup failures, missing fences, failed submissions, SC submission signaling and limiter interval edge cases. A negative control restores the old deactivation order and reproduces its unwanted submission.
- HDR menu test uses the actual layer/composite on D3D12 WARP with the debug layer, region/full-surface alternation, resize/reuse and exact untouched pixels. Includes geometry transforms, callbacks and invalid coordinates.
- PNG tests check every 10-bit HDR code value, PQ/BT.2020 metadata and CRCs, and exact SDR bytes across supported source formats/strides. HDR10 integration exercises real GPU readback and restored source states.
- Release x64 build succeeded. Diff whitespace checks passed.

Local test/build evidence: `C:/Games/FF14/fork-audit-20260930/`.

## Remaining verification boundaries

- Real gameplay, resolution/provider switches, exit with Dalamud, long sessions and device-loss recovery still require in-game testing. Software-GPU tests cannot establish visual quality or compatibility with every GPU/driver.
- Existing C4744 low-latency atomic alignment and LNK4098 CRT-link warnings remain. Do not suppress them as a cosmetic cleanup; the include/build ABI needs a dedicated investigation.
- Provider-wide teardown still relies on caller/SDK queue retirement. The new SC fences protect command allocator reuse; they are not a replacement for a full provider-lifetime redesign. Verify the ownership contract before changing generic provider destruction.
- Retaining resources on an unprovable GPU completion is intentional fail-safe behavior, not normal-session memory growth.
- No repository push or public release is part of this review.
