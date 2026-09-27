# Exposure recovery and OptiScaler-owned HDR investigation — 2026-09-26

## Implemented test
Native DX11 lighting scan no longer permanently fails on a two-second pending query. It invalidates old readings/history generation, retains all pending resources, issues no new samples while waiting, and resumes only after every pending slot completes. Failed GPU queries still stop detection. Production scheduling test uses WARP resources and deterministic pending/completed/failed query results. Release x64 builds successfully (existing linker warnings unchanged).

This fixes a demonstrated code path, not a confirmed diagnosis for every affected user. Exact shader signatures/resource links, alternate tone-map permutations and context changes remain possible causes. No affected-user log was supplied for this test.

## Mattjaas review
Public fork: https://github.com/mattjaas/OptiScaler-DLSSNR-PreSR-Multipass_Autoexposure
Reviewed HEAD ec5306c2, release patch e8d25426 and current tools/apply_dlssnr_autoexposure.py plus tools/apply_dlssnr_highlight_protected_autoexposure.py.
Exposure is built from scene luminance tiles into a GPU 1x1 exposure texture. Highlight protection compresses bright outliers relative to log luminance. This belongs on genuinely linear HDR input. FFXIV currently supplies tone-mapped SDR to NR; our bypass is intentional. Do not apply the HDR normalization to SDR or change NGX input flags to pretend it is HDR. No Mattjaas release scripts executed against this checkout; their mode IDs and constants conflict with our PreSR/multipass shader layouts.

## HDR target
User wants OptiScaler to replace RTX HDR, with display black floor 0 nits and peak approximately 1100 nits. Keep peak adjustable and separate scene paper white from UI brightness. Preserve exact black; do not forcibly crush nonzero artistic shadows.

References:
- https://github.com/clshortfuse/renodx/tree/main/src/games/ffxiv
- https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range

RenoDX FFXIV uses tone-map/LUT/gamma/copy/bloom shader replacements, FP16 swapchain upgrades, and a final output pass. Our current DX11/DX12 bridge uses CopyResource from SDR shared buffers to the presentation buffer; changing only the latter format is invalid. Existing ForceHDR is not a complete FFXIV HDR conversion.

Required implementation and validation:
1. Preserve and verify the accepted SDR exposure path, including loading-transition recovery.
2. Validate current game shader permutations before intercepting tone-map/LUT stages; port only compatible algorithms with license attribution.
3. Preserve scene dynamic range through DLSS, NR and clean FG guides with correct HDR flags and exposure units.
4. Separate scene and UI luminance and composite safely with Dalamud; retain SDR fallback.
5. Use FP16 only for internal scene/NR work. Convert the finished scene and matching HUD-less color to RGB10 HDR10 (PQ/BT.2020) before DLSS-G and present through R10G10B10A2_UNORM with DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020. NVIDIA explicitly excludes FP16/scRGB presentation for DLSS-G. Preserve adequate UI alpha precision in its separate buffer, never the two-bit alpha of RGB10A2. Validate all resource tagging and flags; handle resize, fullscreen, FG off/on, display-HDR checks, metadata and shutdown.
6. Add adjustable peak/paper-white controls, test black/white ramps and highlight roll-off, then validate on the user's HDR display with RTX HDR disabled for that test.

No HDR output enabled in this test build. RTX HDR settings remain unchanged. No automatic exposure changes imported yet because the current FFXIV input is SDR. HDR requires separate in-game validation; exposure recovery is the first test gate.

DLSS-G format requirement verified against https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md section 11. User correctly identified the initial scRGB presentation proposal as incompatible. HDR10 is the presentation target regardless of whether FG is currently active, to avoid format switching on FG toggles.
