# Independent nameplate replacement test

Companion 0.1.1 is unchanged. In the OptiScaler Companion section, use
**Start higher-refresh nameplate replacement (30s)**. Preparation takes approximately
five seconds, followed by up to 30 seconds of replacement. **Restore native
nameplates** stops it. The native command hook is enabled automatically; the earlier
sampled GPU-buffer test is disabled to avoid adding its cost.

This test actually suppresses the exact supported native nameplate commands and
draws copies in a click-through DirectComposition layer. It does not just draw boxes
or report a higher FPS. The layer uses a separate device/context on the game's
adapter and a waitable swapchain with maximum frame latency one. Present(1) requests
display cadence; the compositor determines actual delivery. Rates are measured in
the UI/log, not inferred from the game's FG multiplier.

## What this does and does not establish

- **Layer presents/s** measures independent presentation submissions.
- **Native position updates/s** measures accepted replacement snapshots.
- There is no extra camera sampling, extrapolation, image interpolation or positional
  smoothing. New positions still arrive at the game's real-frame rate. A high layer
  rate alone does not prove smoother motion or higher-frequency position updates.
- Generated-frame camera association is unresolved. Timing can differ from the
  displayed 3D scene with FG, although the old input-driven overshoot is not used.
- Use borderless/windowed mode. This is a desktop-composed SDR layer; HDR appearance,
  interaction with ReShade and stacking against other UI are not production-ready.
- Initial font/icon atlases and constants are snapshotted. Test a stable scene first;
  new materials cause native fallback. Same-atlas content changes during the short
  test may not be reflected, so do not treat this as a release-ready renderer.
- Actual depth/stencil-dependent draws are rejected. Native per-vertex colour/alpha
  and command visibility are retained; other HUD windows can nevertheless be covered
  by a desktop layer. Invisible/occluded plates need explicit visual verification.

## Safeguards and validation

Only source command identities captured between Companion PreDraw/PostDraw delimiters
are eligible for suppression. Shared texture/shader identity is used to prepare
materials, never by itself to decide which native commands to hide. Every source
header is revalidated, every packet must match exactly once, and unsupported,
partial, stale, unprepared or nonresponsive layers leave native drawing enabled.
Pointer lists restore through RAII, including exceptions. Native hit-testing remains
untouched. The overlay uses layered/transparent/no-activate window styles plus
transparent hit-test handling, and does not inject mouse input.

The layer stops on timeout, resolution change, lost focus/minimization, explicit stop,
or failure. Device/factory/swapchain creation and presentation on its worker bypass
OptiScaler's game-device/FG wrapping to avoid recursive integration. Only owned CPU
data and independently rebuilt GPU resources enter the worker; it never uses the
game immediate context. Preparation uses bounded asynchronous readbacks with no
forced GPU flush. Files are written once under the game's OptiScaler_CompanionLayer
directory for material preparation and troubleshooting.

The reconstruction/presentation code reuses the retired renderer's native material
handling, with Companion-delimited captures, guarded suppression and native positions
only. Its old disabled entrypoint is replaced by this explicit bounded test. No mouse
prediction, nameplate extrapolation or prior floating-icon motion path was restored.

Automated tests cover exact command suppression, unrelated UI preservation,
header/duplicate/stale/readiness/heartbeat guards, restoration on exceptions,
click-through message handling, and the previously verified GPU geometry copies.
In-game presentation, visibility, clicking and visual equivalence still require testing.

Implementation assistance: GPT-6 Astra.

## Lightweight midpoint test (Companion 0.1.2)

The Companion now publishes metadata before ending its native packet capture, allowing
same-draw identity checks. Older plugins still support replacement but cannot enable
midpoints. No protocol layout changes are required.

In the Companion section, enable **Lightweight 2x position interpolation**, then start
the 30-second replacement test. Compare with the checkbox off in the same scene.
**Midpoints shown** must increase to confirm interpolation actually ran.

One rigid XY midpoint is permitted between consecutive, already received snapshots;
the latest endpoint follows on the next layer refresh. Text, UV, alpha, depth,
material, identity and non-position geometry must match. Jumps above 24 pixels,
missed samples, stale snapshots, and insufficient presentation headroom bypass it.
Observed layer intervals must be 1-8 ms and no more than half the source interval.
The gate is not a hard latency guarantee: desktop scheduling can vary. There is no
future-frame wait or new presentation queue, but a midpoint can add one overlay
refresh of visual delay. Input remains native.

Scratch vertex storage is reused; rendering uses the existing draws and buffers.
There are no additional image passes or runtime readbacks. Tests verify midpoint
bounds, changed-data rejection, endpoint ordering, cadence gates and production
material pairing. Whole-scene/FG frame association remains unresolved.

Latest baseline run: 4,303 submitted layer presentations, 745 accepted position
updates and 240 fallbacks over 30 seconds. Submission rate was approximately 144/s,
usually 32-34 native updates/s; compositor display counters did not establish 144 Hz
actual display delivery. User observed residual wandering, less near the top left.
The captured 3840x2160 viewport and half-pixel-adjusted projection match output size;
no scaling defect has been established. This run did not include midpoint code.

## Depth rejection follow-up

The 00:50-00:52 run could not start replacement: every attempted reconstruction
had zero accepted materials. Its atlas/shader capture succeeded, but materials
were rejected because a DSV was bound with depth testing enabled (GREATER_EQUAL,
no depth writes, stencil disabled). The midpoint algorithm was never reached.
Do not disable this guard merely to make the start button work: the independent
layer has no live scene-depth visibility data.

The UI now displays the actual material rejection reason. Repeated start clicks
do not reset an in-progress preparation. On a depth rejection, one bounded probe
per resource/layout saves the tagged vertex shader, owned packet vertices, DSV
format/dimensions, viewport and an asynchronous copy of the projection constants.
No scene-depth readback, GPU wait or native visibility override is introduced.
Probes are preparatory evidence, not a same-frame visibility proof: native packet
capture can precede the sampled material state. Any future exemption must validate
its assumptions for every replacement frame.

One five-second attempt is sufficient for this diagnostic. The test may still
refuse to start; that is expected until the depth requirement is resolved. Tests
cover rejection with a real WARP depth target, asynchronous projection copy,
probe deduplication/cleanup, precise UI errors, and repeated-click protection.
