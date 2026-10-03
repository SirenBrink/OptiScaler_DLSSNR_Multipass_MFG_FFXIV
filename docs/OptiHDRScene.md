# OptiHDR methods

`[HDR] Mode=0` retains the existing SDR expansion. `Mode=1` selects the
experimental RenoDX scene branch. The menu calls these **SDR expansion
(original)** and **RenoDX scene HDR (experimental)**. The method can change live
once the HDR presenter is enabled. Save Settings persists the selection.

The second method is a native OptiScaler adaptation of the MIT-licensed RenoDX
FFXIV shaders, not the external RenoDX addon. No additional addon is required.
Sources, attribution and the pinned upstream revision are in
`OptiScaler/shaders/hdr/renodx/NOTICE.md` and `LICENSE`.

## Scene path and compatibility

- The exact native ToneMapping shader receives the exposed FP16 scene. Its
  RenoDX counterpart is replayed into an independent FP16 target; the original
  draw and original render resources are unchanged.
- Recognised post-tonemap, LUT and gamma passes continue that branch when their
  source is linked to a captured scene. Integer and half-pixel-origin viewports inside larger pooled
  textures are carried as a common HDR/reference crop; padding is not sampled. Full copies propagate the link; unknown
  writes break forwarding. An immutable original reference is captured too.
- By default NR, upscaling and ReShade retain their existing SDR input. At presentation the
  processed SDR image is bridged against the captured scene/reference pair.
  Actual scene highlight excess is forward-mapped with RenoDX's Reinhard curve.
  It is not inferred from an inverse curve applied to the finished SDR image.
- Significant differences from the scene reference suppress the HDR residual
  to protect changed overlays. This is a conservative pixel comparison, **not
  an exact native HUD alpha mask**. Strong colour grades and spatial effects
  can also trigger it. Arbitrary presets are not guaranteed visually identical
  to a fully HDR-aware rendering chain.
- Missing or aspect-mismatched scene data gives paper-white SDR output in the
  HDR container, with an explicit waiting status. It does not silently use
  highlight expansion. Previous-frame snapshots are never substituted.
- Native passes which do not forward through recognised shaders remain part of
  the final SDR reference. This port does not claim complete reconstruction of
  every game shader, saturated colour or ReShade effect.

FP16 intermediates are shared through the existing DX11/DX12 fence boundary.
Their owners remain retained until the conversion recording is reusable.
Original swapchain resources are not retained in the reusable snapshot pool.

## Frame generation and output

DLSS-G's official HDR guide requires RGB10/PQ and explicitly excludes FP16/scRGB
output. Consequently both methods retain **10-bit PQ/BT.2020 presentation**.
FP16 intermediates preserve scene range without exposing unsupported output to
DLSS-G. A 16-bit final presentation mode has not been enabled.

For scene HDR, the original HUD-less SDR image is copied at the upscaler stage.
After the bridge's DX11 fence wait, both final colour and HUD-less colour are
converted using the same scene pair and settings. The late HDR resource is
tagged for DLSS-G or XeFG with immediate copy validity, without double encoding.
Actual SDK pacing and image quality require an in-game test for each provider.

## Validation

`tests/compile_scene_hdr.py` regenerates embedded shaders using the bundled FXC.
`tests/run_hdr10.ps1` tests original expansion, scene highlight differentiation,
SDR fallback, screenshots and completion-gated DX12 resource reuse on WARP.
`tests/run_scene_hdr.ps1` tests the actual RenoDX DX11 replay, restoration of
native bindings, shared texture import, rejection of stale/wrong-aspect data,
in-flight ownership and saved settings.

In-game acceptance remains necessary: check status after entering the world,
bright effects against dark scenery, camera movement with NR, existing ReShade
presets, DLSS-G/XeFG transitions, resolution changes, zoning and exit. A
successful synthetic test is not proof that every live shader path is covered.

## Gameplay acceptance run

Keep the usual SDR ReShade preset and scene HDR mode selected. With DLSS-G
active, spend 30 seconds each in a dark location and a bright location, then
use bright combat effects against dark scenery. Capture representative HDR
screenshots. Turn DLSS-G off and on, switch Quality to Performance and back,
teleport once, and exit. Watch for brightness jumps, misplaced highlights,
washed-out UI, new ghosting or flicker. XeFG is a separate follow-up run.

The log reports cumulative scene import and fallback counts every 600
presentation requests, including crop/output dimensions and the latest
recognised stage. Missing scenes during loading may be expected; sustained
fallbacks, aspect mismatches or import failures during normal gameplay need
investigation. These counters establish resource availability, not proof that
all highlight pixels survived ReShade grading or that image quality is correct.

## ReShade highlight preservation

The separate **Preserve HDR highlights through ReShade (experimental)** checkbox
is off by default and can change live. It relaxes the original colour-difference
rejection where both the scene reference and final SDR output are bright. A
single brightness gain preserves the post-effect RGB ratios, using only actual
captured scene excess. It does not infer missing highlights from SDR or give
ReShade HDR input. Dark changed overlays retain the original rejection; bright
UI overlapping native highlights may also brighten. Strong blur, displaced
bloom and highlights clipped by an effect remain limitations. Save Settings
persists `[HDR] ReShadeHighlights` independently of the HDR method.

## Optional NR scene input test

**Use scene HDR input for NR (experimental)**, saved as `[HDR] NRSceneInput`,
is a live opt-in under the HDR controls. It supports ordinary NR before
upscaling, PreSR, and alternating NR anchors. Unsupported formats, missing
same-frame captures and busy GPU packets retain the SDR NR path. Changes
between HDR and SDR input reset NR histories; PreSR also clears its held
residual and alternating history. Status messages distinguish the active path
from missing scenes, busy packets, unavailable hooks and disabled settings.

The adapter combines native SDR colour with captured unclipped scene brightness
into a linear FP16 work image, then uses NR's existing reversible HDR proxy and
restoration. Already-exposed scene units use normalization 1 rather than applying
game exposure a second time. Its bounded per-channel edit returns to the native
SDR input before DLSS. Alpha, black, padding, resource state and identity output
are retained. The immutable native HDR snapshot is not overwritten, so this is
an HDR-input experiment, not a full HDR NR/SR rendering chain: NR edits to
clipped highlight detail are not yet preserved independently in the final HDR
branch. SDR ReShade presets retain their input contract.

PreSR returns the NR edit to its SDR working copy before encoding the residual
for private DLSS. Alternating NR uses the matching scene only on NR anchors;
skipped frames retain the existing residual schedule. This does not introduce
linear HDR into the private DLSS pass or borrow a newer capture for an older job.

Idle capture, conversion and NR adapter buffers are reclaimed only after their
owners release them and any recorded DX12 work has both completed and retired.
Completed command lists that have not been reset remain protected. Active pools
retain recently used buffers for two seconds to avoid repeated allocation.

`tests/run_nr_scene_input.ps1` exercises the production adapter on WARP with
identity and simulated model edits, cropped scenes, padding, alpha/black and
completion-plus-reset ownership. It does not execute NVIDIA's actual NR model.
For combined acceptance, select XeFG at up to 4X with scene HDR and ReShade
highlight preservation enabled. Compare the NR scene input checkbox off/on in
bright effects, dark locations and camera motion; change resolution, zone and
exit. Check the active status and log, not just the saved checkbox.

The PreSR/alternating extension has passed local adapter, lifetime and residual
scheduling tests, but still needs NVIDIA-model gameplay acceptance. With DLSS-G
selected, compare HDR NR input off/on with PreSR, then enable alternating NR.
Check bright effects, camera motion, resolution changes, zoning and exit for
new flicker, ghosting or pacing changes. Earlier acceptance below predates this
extension and does not validate its visuals.

### Combined XeFG / HDR NR acceptance (2026-10-03)

The user reports that XeFG and the optional scene-HDR NR input work together,
and that the NR input option improves highlights. The 02:59:16–03:05:36 log
confirms linear HDR NR processing, two NR passes, and recovery after the scene
size changes from 2560x1440 to 2257x1270. All 17,226 logged XeFG resource tags
returned success. From 6000 to 8400 presentation requests, every additional
request imported the scene; there were no aspect mismatches or import failures.
There are on/off transitions during the test, plus scene gaps around loading
and shutdown; transition messages alone do not establish whether these were
user toggles or fallbacks.

Six of seven new screenshots were captured with HDR NR input active and one
with it off. All had ReShade highlight preservation on. The screenshots retain
extended highlight range, but their effects are at different animation times;
they are not a controlled image-quality comparison. The user's live comparison
is the acceptance evidence for the highlight improvement.

Shutdown reached DLL_PROCESS_DETACH. XeFG reported an outstanding swapchain
reference during destruction (POINTER_STILL_IN_USE); this remains a cleanup
issue to investigate, rather than a clean SDK teardown. Native exposure was
absent at startup and at the resolution rebuild, with the existing automatic
exposure recovery. This acceptance covers ordinary pre-SR NR, not PreSR or
alternating NR, which were disabled for this run.

Acceptance clarification: the user selected DLSS-G before exiting this XeFG
run, after which all FG stopped until restart. This is a pending provider
selection, not a successful live handoff: the menu marks FG Output changes as
requiring Save Settings and restart, and the log continues calling the XeFG
presenter until teardown with no DLSS-G initialization. This run validates
XeFG plus HDR NR; it does not validate a live provider transition. The XeFG
outstanding-reference result was still emitted at destruction, after the
pending selection. The log alone does not establish a causal connection.
