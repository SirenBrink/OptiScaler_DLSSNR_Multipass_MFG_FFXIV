# FFXIV integration status

## Working behavior

- The quality preset menu queues an update through FFXIV's native renderer callback path. The game updates its scene textures as well as DLSS sizing. Selecting Game's choice restores native dynamic resolution.
- FFXIV's DX11-to-DX12 swapchain resize suppresses overlay rendering during the transition. XeFG cleanup releases only the reference acquired by its GetBuffer call in FFXIV, rather than releasing other owners' references.
- The OptiFG DLSS-G panel reports its output state and explicitly labels the requested multiplier. It no longer uses the native DX12 input detection counter to incorrectly show OFF for this DX11 bridge.
- NR's split pipeline supports the bridge's generic resource-pointer parameters and preserves their representation during output redirection/restoration.
- Split output dimensions are fixed when the feature is created. A rounded input subrect cannot shrink the output below the dimensions expected by DLSS. The enlargement stage uses the same fixed intermediate dimensions.

## Validation

ReleaseDebug x64 builds passed, including a complete rebuild. Focused compiled harnesses exercised native request gating/deduplication, callback range restoration, resize guard nesting/concurrency, typed/generic pointer redirection/restoration, and fixed split output dimensions under input rounding and feature recreation.

Local FFXIV tests at 3840x2160 confirmed:

- Live Ultra Quality, Balanced and Performance selection; returning to Game's choice resumes DRS.
- Native DLSS/FSR switching with XeFG, without the prior resize crash.
- DLSS-G 2x and 6x, corroborated by Streamline logs; live preset changes also worked in the DLSS-G test.
- NR inside the upscaler at fixed Balanced with DLSS enlargement: NR runs at 2258x1270, guides use the game's rounded 2257x1270 subrect, and the final output is 3840x2160. The output-size rejection is absent in the successful run.

## Limits and remaining work

The native FFXIV hooks are build-specific and require matching instruction signatures. They were investigated against executable MD5 `ca3fe5c8673fe54d1961d856ade51fcc`.

The DirectX/device-hung error on exit remains unresolved. Intermittent NR flicker was not reproduced in the latest successful tests; its original cause is not established. Broader split-mode preset/DRS transitions, checkbox changes across rebuilds, other resolutions and other GPU generations still need validation. Treat Run inside the upscaler as requiring a feature rebuild/restart.

Diagnostic logging remains enabled in code and can be verbose, especially when split and enlargement stages alternate. Existing build warnings, including C4744 and LNK4098, remain. These changes do not claim to resolve them.
