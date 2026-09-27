> Current status (2026-09-27): Companion 0.1.2, native-depth nameplate replacement and lightweight image-region midpoint interpolation have passed local automated checks and user testing. The replacement remains an opt-in 30-second test; interpolation defaults off. See [current test behavior](COMPANION-IMAGE-MIDPOINT-20260927.md) and the [Companion setup guide](https://github.com/SirenBrink/FFXIV-OptiScaler-Companion). The sections below record earlier integration stages; their statements that replacement is not yet implemented are historical.
# FFXIV OptiScaler Companion integration

Companion source: https://github.com/SirenBrink/FFXIV-OptiScaler-Companion

This change adds a versioned, in-process, value-only data bridge and an optional native-nameplate
alignment overlay. There is no HUD suppression, replacement drawing, camera prediction, additional
window, worker or input handling. It does not change NR, FG, HDR, limiter or exposure settings.

The Companion collects post-draw nameplate origins/bounds, object IDs, world anchors and camera
matrices. The mailbox owns the data and rejects invalid frames. A 250 ms timeout and explicit empty
frames invalidate old plates. ABI sizes, QPC frequency and session identity are checked on connection.
The protocol capability mask is 3 (receiver and alignment view), never replacement-ready.

Use `/opticompanion` in Dalamud to enable the test markers. OptiScaler's right settings column has
a `FFXIV OptiScaler Companion` section with receipt counts and an additional marker-disable control.
After the first in-game test exposed drifting boxes, markers moved out of the late ImGui overlay.
They now paint once onto the DX11 bridge's owned HUD/backbuffer copy, before its existing interop
fence, HDR conversion and FG. All generated presentations inherit those marks with the native HUD;
they no longer fetch newer coordinates at each late presentation. The original game backbuffer is
not modified. Source dimensions must match exactly; no guessed rescaling is applied after resize.
The native root origin need not coincide with the center of the name text.

The marker path currently requires the DX11-to-DX12 swapchain bridge. Its rectangle clears use
ClearView and never bind or modify pipeline state. Empty/offscreen geometry must not issue a
zero-rectangle clear, which would clear the whole view. A WARP pixel/state test covers this guard.
The OptiScaler panel and log report marked source frames and skipped draws.

This fixes a plausible late-presentation mismatch, not a proven association between a CPU UI frame
and the render thread's source image. If drift remains, that earlier association must be verified.
It does not modify PreSR, alternating NR or approximate camera guides themselves.

`OptiScaler/misc/companion/CompanionProtocol.h` mirrors the Companion repo's canonical
`protocol/CompanionProtocol.h`; update both alongside `src/Protocol.cs` when changing the ABI.

Validation:

- `tests/run_companion.cmd`: native mailbox validation, copied ownership, expiration, zoning clear,
  plugin/session reload, stale-session protection and concurrent publication/read.
- C# Companion tests: sizes/offsets and real cross-language calls against the production export
  bodies compiled without OptiScaler's game hooks.
- WARP marker test: exact marked/unmarked pixels, original-image and graphics-state preservation,
  resolution mismatch, empty/offscreen lists and partial clipping.
- Release x64 build of OptiScaler and .NET 10 Release build of Companion.

The companion bridge is implemented locally. Native appearance-preserving replacements and independent
HUD refresh remain outstanding; no completion or in-game validation is implied by these builds.

Assisted-by: GPT-6 Astra

## Native submission step

Companion 0.1.1 adds optional BeginNamePlateV1/EndNamePlateV1 exports called from Dalamud's
PreDraw/PostDraw NamePlate callbacks. A test checkbox in OptiScaler enables guarded native command
substitution. No extra native addon-update/draw hook is installed: scope ownership comes from Dalamud.

Native batch hooks at RVAs 0x6e5480 and 0x6e6020 are restricted to PE timestamp 0x6aa84feb,
image size 0x3807000 and matching pristine entry bytes. The local executable was rehashed:
5BBC501DD5C7F22FD61A11D08C25356041D878DB7CD83203ADAE393E4DFACC44.
The old Ghidra batch analyses show that header+0x28 is converted into an index relative to the
native vertex allocation. It is NOT an arbitrary replayable CPU geometry pointer. This stage copies
only ordinary kind 0x22 command headers, retaining all original resource/vertex addresses for the
duration of one synchronous batch. Extension styles 1/2 and unknown kinds/layouts fall back intact.
The archived material capture contains supported style 0 with layouts 0, 4, 5 and 6.

All matching originals must be present exactly once and unchanged before replacing the renderer's
pointer list. The original list, vertices and game UI nodes are not edited. An atomic scoped pointer
exchange restores the original list on normal return or C++ exception. Other UI entries preserve their
ordering and addresses. Pending records expire, and entry/header vector capacity is reused to avoid
steady per-frame heap allocations. There are no additional GPU allocations, draws, waits or frame queues
in this substitution step. It does not improve HUD refresh yet.

Tests cover capture bounds, unsupported extensions, vertex-address preservation, unrelated entry/order
preservation, stale/modified headers, duplicate entries, pointer identity checks and exception restoration.
In-game appearance, clickable targeting, layering and advancing substitution counts still need testing.
