# Companion owned GPU geometry test

This extends the accepted Companion 0.1.1 native-command substitution test. It is
an opt-in, sampled replacement of bound vertex/index buffers at actual DX11 indexed
draw calls, including indexed instancing. It is not a higher-refresh renderer.

## In-game test

Under **OptiScaler -> FFXIV OptiScaler Companion**, enable **Test owned GPU geometry
(sampled)**. This also enables copied native submissions. Both reset off on launch.
The existing Companion 0.1.1 plugin is sufficient. Alignment markers are optional.

In one session:

1. Watch Striking Dummy and other nameplates/icons while rotating, zooming and moving.
2. Click targets, change targets, open menus and inspect text/icon transparency.
3. Toggle the geometry test off/on; appearance should remain identical.
4. Try FG off/on and a quality change; zone or teleport if convenient.
5. Finish with a normal exit. Keep the log before restarting.

**Matching draws** and **Owned geometry draws** should increase. Budget skips are
expected: each of four supported layouts gets at most one sample per 100 ms, with
a total 32 MiB copy budget. Unsupported draws and allocation failures remain native.

Every five seconds, `Companion GPU geometry` logs counters, material age, layout
coverage, shader recognition, CPU time and rejection reasons. CPU timing includes
the sampled native draw plus allocation/copy submission/restoration; it is not a
GPU timing or end-to-end input-latency measurement. Material age is the age of the
latest texture/layout association, not proof of presentation-frame association.

## Scope and safeguards

- Uses existing exposure draw hooks; exposure processing remains in its original
  before/after order. No additional native RVA hooks.
- Matches verified native pixel shader CRCs and API-owned texture references against
  the Companion-bracketed command captures. The captured pointer is an identity hint
  only; no COM method is called through an engine pointer.
- Native materials can be shared with other HUD draws. A match is **not** exclusive
  nameplate ownership and must never authorize draw suppression or late replay.
- Copies the complete supported vertex/index allocations into owned default buffers,
  preserving strides, offsets, index format, base vertex and other draw arguments.
- Original draw executes exactly once. Bindings restore on scope exit, including
  C++ exceptions. Unsupported/over-budget/allocation-failed samples use native buffers.
- No shader, constant, texture, target, blend, scissor, input or camera modification.
- No runtime GPU readback, explicit GPU wait, worker presentation, frame queue or
  retained idle COM references. Owned allocations are released after their draw;
  the graphics runtime handles queued command resource lifetime.
- Disable copied native submissions to disable this test too. No INI change.

## Validation

`tests/run_companion.cmd` now includes a WARP GPU test that compares exact rendered
pixels between native and copied geometry. It verifies R16/R32 indices, nonzero
buffer offsets/start index, modifying the source after copying, restoration on
exceptions, byte/index-range guards, material matching, exactly-once fallback,
reentrancy and sampling-window reset. Existing bridge, marker and command tests pass.

Independent geometry/material replay and per-presentation camera association are
still needed before replacing/hiding the native layer or increasing its cadence.

API references: [CopyResource](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copyresource),
[IAGetVertexBuffers](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-iagetvertexbuffers).

Implementation assistance: GPT-6 Astra.

## Follow-up: all vertex slots

The 00:14:04 run reported 118 successful copies, all layout 0, and 1,524 missing-buffer
rejections for other sampled draws. The stride hypothesis was not supported. The
original test queried only IA slot 0; it could not handle native layouts using other
slots. The new scope queries all 32 DX11 IA slots and copies each unique bound vertex
allocation once, preserving aliases, offsets, strides and empty slots. It restores
the entire binding set after the original draw. Total copy budget remains unchanged.
Logs now include active slot masks by material layout. Tests cover slots 3 and 7
sharing an allocation with different offsets, empty slot 0, exact rendered pixels,
copy deduplication and restoration. No original draw suppression is enabled.
