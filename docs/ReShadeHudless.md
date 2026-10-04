# Optional REST HUD isolation

This integration comes from byunjoe's nine-commit v5 addon branch, supplied as
`optiscaler-hudless.bundle` (tip `809e52321d4ea5843ef9bd5d6ce4137dcf98770d`).
The original commit authorship is retained. Integration safeguards and tests
were assisted by GPT-6.1 Sol.

The addon captures a display-ready image before UI drawing at the REST marker.
OptiScaler derives a UI mask from its difference against the final SDR frame.
The optional post-FG paste puts completed real-frame UI over DLSS-G output;
UI-free FG omits that UI from the interpolated scene to avoid doubled edges.
This holds UI at its native real-frame update rate. It does not interpolate
nameplate positions or update game UI independently of the game.

## Requirements and setup

- SDR output, the DX11/DX12 bridge, and OptiFG DLSS-G for post-FG UI paste.
- ReShade with addon support, REST (ReShade Effect Shader Toggler) with its FFXIV configuration, `OptiScalerHudless.addon64`, and the supplied
  `OptiScaler_Hudless.fx` marker.
- Place the marker last among the effects in the REST group that runs directly
  before game UI drawing. A misplaced marker can misclassify scene effects as UI.
- Enable **ReShade REST HUD isolation (experimental)** in OptiFG's detected-UI
  controls and save settings. Its master key is `[FrameGen] ExternalHudless`.
  The master defaults to false. The other addon controls remain inactive without
  both explicit opt-in and the loaded addon. Runtime HDR output disables this
  path while retaining the user's saved preferences.

The author-provided addon binary and FX remain optional files, not dependencies
of ordinary OptiScaler operation. They are not installed or loaded by OptiScaler.
Post-FG paste is DLSS-G only; XeFG post-FG compositing is not validated.
DLSS-G UI recomposition is separate and requires restart. UI-free input bypasses
that recomposition because its input no longer contains UI.

## Safeguards and limitations

UI-free input requires a matching, completed paste image. Loss of readiness
restores the full native frame. Missing captures invalidate the previous paste.
The producer/consumer rings retain their completion fences; teardown does not
release ownership when completion cannot be proved. In that exceptional case,
resources are retained for process lifetime rather than recycled unsafely.
The Companion layer's ReShade-isolating swapchain change is gated to the addon
opt-in; the ordinary Companion path is unchanged otherwise.

Known visual limitations remain: black/white fades can change most pixels and
be mistaken for UI, and nameplates can look displaced at low real FPS because
the held UI and generated scene represent different times. Optional steady
timing adds roughly one real frame of UI latency. This initial integration does
not claim to fix those issues or support HDR capture/paste.

## Validation

`tests/run_external_hudless.ps1` checks the addon status ABI and all activation
combinations, then runs the production extraction and producer shaders on WARP
for threshold, dilation, cleanup and unchanged-scene behavior. The Release
build and existing HDR converter tests pass. Live REST placement, fades,
teleports, Companion overlays and low real FPS still need gameplay acceptance.
