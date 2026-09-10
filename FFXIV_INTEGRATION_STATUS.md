# FFXIV integration status

## Working behavior

- The quality preset menu queues an update through FFXIV's native renderer callback path. The game updates its scene textures as well as DLSS sizing. Selecting Game's choice restores native dynamic resolution.
- FFXIV's DX11-to-DX12 swapchain resize suppresses overlay rendering during the transition. XeFG cleanup releases only the reference acquired by its GetBuffer call in FFXIV, rather than releasing other owners' references.
- The OptiFG DLSS-G panel reports its output state and explicitly labels the requested multiplier. It no longer uses the native DX12 input detection counter to incorrectly show OFF for this DX11 bridge.
- NR's split pipeline supports the bridge's generic resource-pointer parameters and preserves their representation during output redirection/restoration.
- Split output dimensions are fixed when the feature is created. A rounded input subrect cannot shrink the output below the dimensions expected by DLSS. The enlargement stage uses the same fixed intermediate dimensions.

- FFXIV upscaler release cancels pending FG command lists and waits for submitted work on the FG and upscaler queues before destroying the feature. A failed wait leaves the feature intact.
- FFXIV NGX shutdown uses the recorded DX11 device when the DX12 bridge is alive, avoiding the legacy shutdown that invalidates NGX on all devices.

## Validation

ReleaseDebug x64 builds passed, including a complete rebuild. Focused compiled harnesses exercised native request gating/deduplication, callback range restoration, resize guard nesting/concurrency, typed/generic pointer redirection/restoration, and fixed split output dimensions under input rounding and feature recreation.

Local FFXIV tests on an RTX 5070 Ti at 3840x2160 confirmed:

- Live Ultra Quality, Balanced and Performance selection; returning to Game's choice resumes DRS.
- Native DLSS/FSR switching with XeFG, without the prior resize crash.
- DLSS-G 2x and 6x, corroborated by Streamline logs; live preset changes also worked in the DLSS-G test.
- NR inside the upscaler at fixed Balanced with DLSS enlargement: NR runs at 2258x1270, guides use the game's rounded 2257x1270 subrect, and the final output is 3840x2160. The output-size rejection is absent in the successful run.

## Limits and remaining work

The native FFXIV hooks are build-specific and require matching instruction signatures. They were investigated against executable MD5 `ca3fe5c8673fe54d1961d856ade51fcc`.

Exit cleanup passed one complete exit without a DirectX dialog after cancelling a pending FG command list and draining both queues. A subsequent settings stress test reproduced a Streamline exception while releasing its NGX FG instance. The device-specific NGX shutdown change addresses the global shutdown path implicated by that trace. On the latest build, the user reported no DirectX dialog and normal gameplay, but Dalamud crashed during framework destruction before the new NGX shutdown completion marker. Full exit validation therefore remains incomplete; the Dalamud exit issue is deferred.

Additional compiled tests passed for pending command-list cancellation, preservation of previously submitted fence values, repeated cleanup, and device-specific shutdown routing (legacy entry point, explicit/null device, unavailable API and other-game behavior). A D3D12 WARP test confirmed that queue draining waits for submitted work.

Known issues for testers:

- NR stalls at DLAA/native resolution with split mode enabled. Avoid that combination for now; automatic fallback is not implemented.
- Settings stress testing produced repeated DLSS-G out-of-memory warnings. The cause and recovery behavior need further investigation.
- FG toggles and quality changes can produce Reflex-not-detected warnings during transitions.
- Intermittent NR flicker was not reproduced in recent successful tests; its original cause is not established.
- Broader split-mode preset/DRS transitions, NR resolution changes, other display resolutions and other GPU generations need further coverage. Treat changing Run inside the upscaler as requiring a feature rebuild/restart.

For bug reports, include GPU and driver, display resolution, DLSS quality, FG type/multiplier, NR split/enlarger/resolution settings, the exact sequence that caused the problem, and whether XIVLauncher/Dalamud was enabled. Preserve OptiScaler.log before another run overwrites it; attach a matching OptiScaler.ini and any crash report after checking them for personal information.

Diagnostic logging remains enabled in code and can be verbose, especially when split and enlargement stages alternate. Existing build warnings, including C4744 and LNK4098, remain. These changes do not claim to resolve them.
