# Community integration — 2026-09-25, first test batch

Base: 2679fb17535b6dc44ba69a99af9f87cc5a747c67, plus the existing local HUD/diagnostic cleanup.
This is a staged test build, not the complete requested fork integration.

## Included

- Janblade 9951a3a7: serialize this file's Streamline Detours transactions. Other hook modules are not covered by that mutex.
- Janblade d757178f: align resampled DX12 NR model inputs to 16 pixels; leave native inputs exactly native. Menu displays model-input and half-resolution network dimensions.
- Janblade 056e1cf1, adapted: optional interpass feedback. Default 1 bypasses the new dispatch and retains the original output; lower values bound and damp intermediate edits. Single-pass output is unchanged. Allocation failure logs a fallback to full feedback until rebuild.
- Janblade 34e33c6a, restricted: optional ViT bottleneck reuse on every second evaluation. Default off. Only the existing hash-verified NVIDIA module is eligible; hybrid precision always computes fully. Unsupported runtimes remain unchanged. Cache is invalidated on reset, errors and module/function destruction. Actual NVIDIA kernel reuse and visual quality still need in-game testing.
- ShyVortex b26368e5, adapted: NR creation is ready only after the creation command recording has a completed GPU fence, not merely a new CPU frame. Does not block the render thread to wait.
- Scottmudge b4473abd: preserve 64-bit frame IDs across low-latency sleep interfaces. The XeLL SDK's own 32-bit boundary remains explicit. Unrelated SDK/build changes were not imported.
- Existing NR preset files automatically receive defaults for the two new settings.

Controls are under **DLSS Neural Rendering**, near **Model precision**:
`Reuse NR bottleneck every second evaluation (experimental)` and `Multipass feedback`.
INI keys in `[DlssNr]`: `VitEvery=1` (2 enables reuse), `PassFeedback=1.0` (0–1).
These additions currently target the DX12 NR path used by FFXIV, including the DX11 bridge.

## Validation

Release x64 build succeeded. Host/WARP tests passed for model-size alignment, ViT decision logic, feedback shader, old/new saved presets, delayed/unsubmitted/replayed GPU work, PreSR bounds and skin/residual composition, motion metadata, pan cadence, and bridge/CRT shutdown. Runtime Streamline hook stress test is included but has not been run. No FFXIV gameplay validation yet.

## Next stages

ShyVortex SM86 detection/live controls require a new companion-runtime loader integration. Peripheral spatial compression requires adapting the color, depth/motion-guide and inverse-composition paths together; do not transplant only its menu or color warp. Keep existing targeted Ada correction, and do not combine two runtime patch owners.

Smooth Motion is excluded. Mattjaas exposure changes are deferred. Existing FFXIV native exposure and alternating PreSR behavior remain in place.

Test first with existing settings: startup, NR off/on, DLSS quality changes and exit. Then compare ViT reuse on/off during camera movement. Test feedback separately with two passes and a saved-preset round trip. A `reused` count of zero means reuse has not taken effect; do not infer a performance gain from enabling the checkbox alone.

Assisted-by: GPT-6 Astra
