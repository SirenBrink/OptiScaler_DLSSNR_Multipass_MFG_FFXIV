# Community integration test - 2026-09-26

Continues the first community batch preserved in COMMUNITY-INTEGRATION-20260925.md.

Sources: ShyVortex/OptiScaler-DLSSNR-PreSR-Multipass main a4d82db9; PeripheralWarp by BeliyG3, commit 64902dd6a02460e5f6b778504ec2a4005faf4d9c (MIT license retained).

## Peripheral NR compression
Opt-in under DLSS Neural Rendering -> Performance. Packs color and depth and transforms motion-vector endpoints consistently, then restores model and base proxy to the ordinary grid for matched-residual composition. Defaults: center 80%, work 90% on each axis. This DX12 test supports model resolution through 100%; higher scales retain ordinary NR. Resource/evaluation failures fall back to ordinary NR. Spatial settings are included in NR user presets.

## SM86/SM75 companion runtime
Opt-in restart setting under OptiFG. Detects an installed companion runtime and supports its exported live controls. No runtime binaries included. Limited to Turing/Ampere and excludes simultaneous targeted Ada patch use. Initializes before private Streamline loading. Actual companion runtime and RTX 20/30 hardware verification remain required. Does not import upstream Smooth Motion, broad Ada patch, or automatic Linux fallback ownership.

## Validation
Release x64 build; CPU spatial mapping; actual spatial HLSL on D3D11 WARP (color geometry, padded depth, motion endpoints, proxy/model identity); runtime detection and INI helper tests; NR preset roundtrip/legacy defaults; model sizing, ViT, multipass; PreSR metadata/bounds/cadence; GPU lifetime and shutdown regressions. Runtime binary tests skip when companion binaries are absent. No in-game acceptance yet.

Build retains existing atomic shared_ptr alignment and LIBCMT linker warnings.

Follow-up: moved spatial controls near the top of the NR panel. Fixed module teardown clearing all live ViT function roles; registry now removes only functions owned by the destroyed module. Added a regression proving another module continues counting computed/reused evaluations. Release and community-control tests passed. In-game confirmation pending.

User validated spatial compression and sustained bottleneck reuse through gameplay. Preferred baseline: one pass, spatial compression on, VitEvery=2, PreSR off. Prepared next in-game test with two passes and feedback=0.5, existing second-pass appearance retained; accepted INI and log archived. No additional DLL change required.

Final validation update: user confirmed NR remained active, sustained ViT reuse during gameplay, live multipass feedback, and multiple peripheral spatial compression settings without major ghosting. These supersede the earlier pending-test notes above. SM86/SM75 companion hardware validation remains pending. HDR and native-exposure follow-ups are documented separately in HDR10-TEST-20260926.md.
