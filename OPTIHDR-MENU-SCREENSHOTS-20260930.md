# OptiHDR menu and screenshot test

The DX12 presentation menu first composites its theme and glyph coverage into
an RGBA8 SDR layer using ImGui's original blending. The completed layer is then
mapped to PQ/BT.2020 at 203-nit reference white and composited with a copy of
the HDR scene in linear light. Previously each individual primitive was PQ
encoded before fixed-function blending, changing the appearance of text edges
and translucent controls. Empty pixels preserve the scene exactly. HDR menu
panels are opaque to keep bright scenery from washing out their backgrounds;
the saved SDR transparency preference remains unchanged.
The legacy CPU-side Reinhard UI mapper is bypassed while OptiHDR is active,
for the style palette, explicit status/overlay colours, and notification text.
It had converted white to 0.5 before the HDR renderer received it, yielding
about 43 nits instead of 203 and compressing contrast. If legacy mapping was
already applied, the saved original palette is restored. Other HDR paths keep
their previous CPU mapping; the palette itself has not been redesigned.
There is no menu brightness slider. Old MenuNits values are ignored and removed
on Save Settings. The game's highlight expansion, contrast and saturation do
not affect the menu. SDR menu targets remain unchanged.

The two intermediate textures are reused after the overlay's existing GPU fence
retires; size/device changes recreate them only after completion. Cleanup also
uses that fence and does not destroy in-flight resources during shutdown.
The extra copy and composite run only when there is UI geometry, on real and
generated presents alike. At 4K the textures require approximately 64 MiB;
in-game performance and visual acceptance remain to be tested.

With OptiHDR active, assign an optional single-key screenshot shortcut in the
HDR controls and select SDR PNG (default) or HDR PNG. No key is assigned by
default; Backspace in the key picker unbinds it. Save Settings persists both
choices. The old fixed Ctrl+Alt+F12 shortcut is removed, including for existing
INIs. Press/release the chosen key while the game is focused to save one PNG
in `game/OptiScaler/Screenshots`. `-HDR.png` is 16-bit RGB with PNG Third Edition cICP 9/16/0/1
(PQ/BT.2020), preserving every original 10-bit HDR sample. Use an HDR-aware
viewer. `-SDR.png` is an 8-bit sRGB copy of the pre-OptiHDR image, including
ReShade's grading. Its RGB bytes are preserved without HDR expansion or a
second tone curve. No JXR is saved.
FFXIV's own screenshot function remains unchanged.

HDR capture occurs after SDR-to-HDR conversion; SDR capture uses the source
before conversion. Both precede the presentation menu and frame generation.
ReShade effects already applied to the SDR source are
included. The external Companion replacement-nameplate window is not part
of this buffer and is not captured. Disable HUD replacement if a screenshot
needs those native nameplates. Effects injected after the capture point are
not included; verify ReShade appearance with the user's actual preset.

The GPU copy uses a dedicated readback allocation and queue completion fence.
Only one screenshot can be pending. A module-retaining worker waits for GPU
completion and writes the image without blocking Present. No resources or
worker are allocated during normal frames without a screenshot request.
Failure to establish completion retains the pending resources and disables
further captures instead of freeing GPU resources early.

Validation:
- `tests/run_hdr_menu_layer.cmd`: production DX12 layer and composite on WARP;
  SDR glyph coverage blending, original palette bypass versus legacy HDR mapping,
  black/grey/white, BT.2020 red, translucent layer,
  exact untouched scene, 32 fenced reuse/resize cycles, clean DX12 debug layer.
- `tests/run_hdr_menu_screenshot.cmd`: actual menu pixel shader on WARP at
  SDR/automatic HDR; preserved alpha/black and dark-to-white theme luminance; HDR PNG encode/decode retaining
  every original 10-bit sample, padded rows and image orientation; PNG colour
  chunk placement/CRCs; exact SDR bytes for RGBA/BGRA UNORM/sRGB/typeless sources.
- `tests/run_hdr10.ps1`: existing tone-curve tests, 256 DX12 submission cycles,
  XeFG tag path and negative lifetime control; also exercises the production
  HDR and pre-HDR screenshot readback/copy/fence, state restoration and
  rejection of source formats inconsistent with the requested capture mode.
- Release x64 build. Existing C4744/LNK4098 warnings remain.

In-game validation remains necessary: menu legibility, an HDR screenshot with
ReShade enabled/disabled, DLSS-G/XeFG/FG-off, resolution changes and exit.
No shadow/history/NR settings were changed for the reported shadow flicker.

Audit follow-up: menu copy/clear/composite work is bounded by actual UI geometry,
with a full-surface fallback for callbacks. PNG encoding avoids a second copy
of the encoded stream. Screenshot pending ownership is established before
post-submission allocations. See FORK-AUDIT-20260930.md for validation and limits.
