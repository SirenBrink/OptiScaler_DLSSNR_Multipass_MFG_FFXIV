# Native-depth nameplate visibility test

This 30-second experiment replaces the reconstruction-based start path. In the
Companion section, select **Start depth-tested nameplate replacement (30s)**.
Companion 0.1.2 remains installed. No INI change is needed. Position interpolation
is intentionally disabled; position updates still follow the game's real frames.
This is a functional visibility test, not yet a higher position-refresh solution.

## Why the implementation changed

The previous capture established real D24 depth values for nameplates, with
GREATER_EQUAL depth testing, no depth writes and no stencil. Ignoring that depth
would expose occluded plates. FFXIV also queues native draws rather than issuing
them immediately while building NamePlate batches. Replaying the native batch
builder twice is unsafe and was not shipped.

The current path invokes the original batch builder once. It captures CPU-owned
index arrays as indexed commands enter the native graphics queue, and associates
them with exact Companion-delimited vertex ranges. When the command reaches the
native state binder, its address, complete 0xb0-byte body, age and D3D context are
checked. The D3D draw must also match count, start, base vertex, index format,
known pixel-shader layout and live texture resource identity.

Only complete triangles wholly contained in one selected vertex range move to the
visibility layer. Other triangles in that same native draw remain on the native
target. Partial primitive matches are rejected. A private dynamic index buffer
preserves the original vertex buffers, shaders, constants, samplers and native
alpha/depth states. Original index and output-merger bindings are restored.

The selected pixels are drawn with the live native DSV into a transparent BGRA8
texture. Alpha is the visibility mask; no CPU depth readback, custom approximation
of the depth comparison, or frozen reconstruction materials are used. Unsupported
targets, depth writes, stencil, UAVs, blending and unexpected layouts stay native.

Two keyed-mutex shared textures transfer the result to the existing independent
presentation window on the same adapter. Acquisitions have zero timeout; busy
slots fall back to native rendering. The worker owns a private retained image and
never uses the game's immediate context. Failed/empty frames invalidate the layer.
The producer frame state is protected across game draw and Present threads.

## Current-build guards

Executable SHA256 verified during implementation:
5BBC501DD5C7F22FD61A11D08C25356041D878DB7CD83203ADAE393E4DFACC44

The existing executable timestamp/image-size and batch prologue guards remain.
Additional 16-byte prologue guards cover queue append RVA 0x240a80 and draw-state
binding RVA 0x22d6b0. Layout mapping RVA 0x217b670 and renderer offsets are checked
for supported slots, strides, allocation bounds, packet counts and buffer sizes.
Queue metadata is bounded to 256 records / 8 MiB of indices, expires after 250 ms,
and requires an unchanged command no older than 100 ms at binding. Reused command
addresses are invalidated on all new submissions, including unrelated ones.

Static queue review used the existing September 17 Ghidra export, matching the
current executable hash. Relevant functions: 14063b500 (indexed queue command),
140240a80 (append), 140234920 (dispatch), 14022d6b0 (state binding), 1406e5480 and
1406e6020 (16/32-bit native batches).

## Validation and limitations

WARP tests passed for depth rejection of hidden pixels, premultiplied alpha,
unchanged native target, output binding restoration, cross-device texture transfer,
zero-wait busy fallback, stale-image invalidation, depth-write rejection, mixed-HUD
triangle partitioning, changed/expired/reused queue records, and owned 16/32-bit
native index capture. The existing Companion ABI/geometry/midpoint tests also pass.
The live FFXIV queue hooks and presentation still require in-game validation.

The menu reports split draws, successful replacement frames and fallbacks. Logs
include queued/bound/split/rejected counts, shared image age and presentation rate.
GPU span includes intervening native HUD work; it is not isolated overhead. CPU
split time excludes queue capture. Compare real game FPS with the test stopped
and active for total cost. Submitted overlay FPS does not prove displayed cadence.

Desktop composition can still differ from the game's other UI ordering and HDR
processing; FG frame association and higher-frequency movement remain unresolved.
Test occlusion behind buildings, zero-alpha plates, camera panning and native
click-through. The test automatically ends after 30 seconds; Restore native
nameplates stops it sooner. No claim of zero added latency or final visual parity.

Implementation assistance: GPT-6 Astra.
