# Native DX11 lighting capture (diagnostic)

This diagnostic records native FFXIV lighting data. It does not select or apply NR exposure. It remains idle until a capture is requested from **DLSS Neural Rendering -> Colour -> Native DX11 lighting capture (diagnostic)**. This test uses the existing DX11-to-DX12 swapchain bridge to service readbacks at Present.

## Test procedure

1. Disable NR for the initial set. Keep the same DLSS quality and other graphics settings throughout.
2. Fully enter the game. Face a brightly lit scene, select **Capture bright view**, and hold the camera still until the status says **Finished** (about 12 seconds).
3. Move to a dark scene, select **Capture dark view**, and wait for **Finished** again.
4. Start **Capture transition** before moving from shade into a bright area, or vice versa. Keep the menu closed during movement if convenient. Avoid teleport/loading transitions for this first set.
5. Exit normally after all captures finish. Each session gets a unique folder inside `OptiScaler_LightingCaptures` beside the game executable. Existing captures are never overwritten.

The shader count reports matching shaders created, not whether those passes ran. A zero-draw result is useful evidence of limited coverage, not proof that exposure is absent. The diagnostic counts deferred command-list execution separately; this version observes immediate-context draws and does not instrument recorded command lists.

## Capture contents and limits

The second diagnostic build also records the native DX11 colour resource at the DLSS input boundary, before the bridge substitutes DX12 resources. It records input/output identity, the flags visible to OptiScaler, render dimensions and supplied exposure parameters. Matching these samples against tone-mapping output establishes the colour processing already applied to NR's input. The first bright/dark/transition set is already sufficient to identify live adaptation; the follow-up needs only one capture in any convenient scene, with NR off.

- Exact bytecode length, DXBC checksum and CRC32 identify nine shaders from the installed game's SqPack assets: ToneMapping, ToneAdjust, MeasureLumInitial/Iterative/Final, AdaptLum, BrightPassFilter/Update, and ToneMapLut. No game executable addresses are hooked.
- One sample frame approximately every 500 ms for 12 seconds. Up to one draw per shader per sample frame; up to four draws for the repeated luminance reduction shader.
- Complete visible pixel-shader constant-buffer ranges (up to the DX11 64 KiB limit), including DX11.1 binding offsets, are copied before the draw.
- t0/t1 resource identities, texture/view formats, selected mip and array slice, viewport and draw-call stack addresses are recorded. All verified shaders use only these two texture slots. Bindings can be stale/unused; consult shader disassembly before interpreting values.
- Small input textures, 3D LUTs up to 4096 texels, and outputs before/after the draw are copied. Large 2D images get a centre crop up to 256x144 on sample windows 0, 12 and 23 only. These crops are not a whole-screen brightness meter.
- Readbacks use a GPU event query followed by nonblocking Map. No forced Flush, blocking GPU wait, or guessed frame-age completion is used. Timed-out/incomplete samples are labelled. No shader/resource binding or game output is changed.
- Hard limits: 64 MiB queued payload per session, 32 pending draws, 512 draws per session, and a 3-second readback timeout. Filesystem or resource failures stop/skip diagnostics rather than interfering with the original draw. Each draw is forwarded exactly once.
- Each draw's `.tsv` describes its `.bin` files. Binary rows and depth slices are tightly packed with GPU pitch removed. Floats retain the resource's native format; they have not been converted into an exposure or paper-white value.

## Verified offline findings (2026-09-23)

The existing Ghidra export matches the current executable SHA256:
`5bbc501dd5c7f22fd61a11d08c25356041d878db7cd83203adae393e4dfacc44`.

At preferred image base 0x140000000, the analysed renderer constructor is `FUN_140353e90`. Adaptation initialization `FUN_140365c80` allocates two 1x1 targets plus a 1024x1 lookup texture and loads ToneMapLut. `FUN_140365f70` prepares `cAdaptLumParam` and `cToneMapParam`, uses `sAdaptedLum`, submits the passes, and swaps its two adaptation resources. These are analysis anchors for this executable only, not runtime patch addresses.

AdaptLum's bytecode reads previous adapted data and current measured data, blends them using a supplied coefficient, applies a reciprocal with a supplied scale, and clamps the result. ToneMapLut uses adapted data to build a nonlinear lookup; ToneMapping samples that lookup. ToneAdjust instead applies a power and optional 3D colour LUT. Asset existence and static analysis do not establish which paths are active during current gameplay.

No value should be routed into NR until captures establish live use, units, direction, and its relationship to the image passed into NR.

## Live findings (2026-09-23)

Three bright/dark/transition sessions recorded 72 complete sample windows. Resource identities and exact bytes linked luminance reduction to adaptation, adaptation to the lookup texture, and that lookup to ToneMapping. AdaptLum matched its inspected shader formula within 4.38e-7. These values are native shader units, not nits or a simple average luminance.

A follow-up recorded 24 DLSS input samples. In every sample, ToneMapping writes the texture later supplied as DLSS colour input. The flags visible to OptiScaler are 0x4A (IsHDR clear), with no exposure texture and unit pre-exposure/exposure scale. All 216 follow-up snapshots completed without dropped readbacks.

The three sampled colour crops differ between the two boundaries. Additional conditional post-processing occurs between them in the inspected renderer; the diagnostic does not identify the particular intervening writes. The result establishes resource continuity and a non-HDR input declaration, not byte-identical image contents.

The current regular and PreSR NR paths use that HDR declaration to bypass colour encoding and white-point normalization on already-tone-mapped input. Automatic exposure and manual paper white therefore do not adjust the ordinary NR image on this path. The debug comparison divider can still use white point. A floating-point texture alone does not imply linear HDR input.

FFXIV's adaptation is already upstream of NR. Routing its gain directly into NR's HDR normalization would require a new, validated colour transform; the captures do not justify doing so. The existing DX12 exposure scanner cannot see the native DX11 adaptation resources. UI availability messages should distinguish that limitation from whether the game has internal adaptation.

## Validation

Local WARP DX11 test, with the Microsoft debug layer enabled, exercised the actual installed ToneMapping and AdaptLum shader bytecode, real draw detours, exact signature rejection, constant-buffer subranges, 1x1 adaptation output, 2D/3D asynchronous readbacks, preserved shader/CB/RT bindings, and idle/capture/completion behavior. Expected pixels and captured bytes matched; no debug-layer errors were reported. This does not replace an FFXIV gameplay test.

SqPack read-only extraction followed the layouts in [Lumina's index structures](https://github.com/NotAdam/Lumina/blob/master/src/Lumina/Data/Structs/SqPackIndexHeader.cs), [file structures](https://github.com/NotAdam/Lumina/blob/master/src/Lumina/Data/Structs/SqPackFileInfo.cs), and [stream reader](https://github.com/NotAdam/Lumina/blob/master/src/Lumina/Data/SqPackStream.cs). Extracted game shaders remain local analysis artifacts and are not included in this repository.

GPU completion and copy handling follow Microsoft's [GetData](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-getdata), [CopySubresourceRegion](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copysubresourceregion), and [PSGetConstantBuffers1](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-psgetconstantbuffers1) documentation.
