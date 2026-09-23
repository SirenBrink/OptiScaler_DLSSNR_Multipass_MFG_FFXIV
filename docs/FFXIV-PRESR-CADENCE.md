# Alternating NR cadence diagnostic

2026-09-23: `a056ffc5-presr-cadence-diagnostic` restores the rendering behavior
of the user-confirmed ghosting/flashing-free `presr-bounds-continuity-test`.
The subsequent current-scene reprojection experiment caused whole-screen
ghosting and failed to improve smoothness, including without normal DLSS-G.
The rejected test log is archived in workspace
`outputs/presr-reprojection-rejected-20260923-093909`.

NR + private SR + bounds + residual FG take roughly 11 ms on an evaluated anchor,
while all four are skipped on the intervening frame. That is a plausible source
of uneven frame pacing, but per-stage average cost alone cannot establish it.

This diagnostic records differences between the existing GPU completion markers
at successive successful After seams. Slots are read only on completed reuse;
no new GPU commands, queries, waits, copies or image operations are introduced.
A 128-interval rolling window reports anchor/skipped means and overall median,
p95 and maximum every 120 composed frames. Timestamp frequency is from the same
owning direct queue. Stale/duplicate timestamps and unavailable frequencies are
ignored; generation retirement resets the collection.

These are real-frame **GPU completion intervals**, not presented/display FPS,
input latency, or pure GPU busy time. They include intervening queue work and
queue starvation. Gaps or mode transitions can produce outliers; use steady
windows several seconds after a toggle. A gap after failed composition remains
visible rather than being silently trimmed.

Tests cover timestamp frequency conversion, phase attribution, rolling-window
wrap, invalid/duplicate data and quantiles. Existing shader, GPU-buffer, pan policy,
guide metadata and shutdown regressions pass. Release build is required before
deployment; live pacing evidence is still pending.

Test with normal DLSS-G off, NR + PreSR + trailing guard on. Keep quality/camera
conditions fixed. Pan for about 20 seconds with alternating NR and approximate
guides on, then disable only **NR every second frame** and repeat for 20 seconds.
Close normally to preserve the complete log. This test is diagnostic, not a claim
that alternating-frame smoothness has been fixed.

## Live result

Archived `outputs/presr-cadence-result-20260923-095106/OptiScaler.log`.
Steady in-game alternating windows from 09:46:32 through 09:46:55 show anchor
means about 21.3–21.9 ms and skipped means 10.6–11.1 ms. The second alternating
segment from 09:47:43 through 09:48:04 reproduces the pattern. Startup, mode
transitions and exit outliers are excluded from this comparison.

Ordinary every-frame PreSR from 09:47:02 through 09:47:27 has medians about
19.2–19.5 ms, p95 about 20.2–21.1 ms. Alternating saves total work but produces a
repeatable roughly 11 ms long/short difference. The user independently reports
choppiness with normal presentation DLSS-G disabled. These observations strongly
support uneven work scheduling as a contributor; completion spacing remains
distinct from measured scanout cadence.

Next design target: redistribute work, preserving the accepted contribution and
history behavior. Merely limiting presentation to the slow anchor interval would
discard the throughput benefit. Moving private SR/FG onto the intervening frame
requires retaining its anchor colour/guides/parameters until evaluation and a
correctly matched output schedule; it cannot be done by moving the Evaluate call
alone. Interpolating between anchors requires the later anchor to exist, so this
may need additional buffering/latency. The rejected forward-reprojection path
must not be used to hide that dependency. No scheduling change is included yet.
