# XeFG OptiHDR test — 2026-09-27

Built separately; NOT installed. The installed game DLL is the Companion handoff test.

Changes:
- Permit XeFG when requesting OptiHDR's RGB10A2 PQ/BT.2020 presentation chain.
- Convert the XeFG HUD-less tag with the same production HDR conversion as the final image. Tag ONLY_NOW so XeFG copies on the same command list before conversion textures can be recycled.
- Explicitly notify the HDR lifetime tracker on FG-owned UI command-list submission/reset, including catch-up and deactivation submissions.
- Keep SDR upscaler, NR, and ReShade processing unchanged.
- Use backbuffer + HUD-less UI extraction. Ignore separate SDR UI textures and bypass manual UI-over-FG and HUD comparison while OptiHDR is active. User settings remain saved unchanged.
- Correct the old XeFG COPY_SOURCE workaround to transition/restore the actual tagged resource only when needed.

Intel SDK reference: https://github.com/intel/xess/blob/main/doc/xess_fg_developer_guide_english.md#hdr-display-support

Validation: Release build succeeded, only existing C4744 and LNK4098 warnings. Production WARP shader ramp/color tests and DX12 converter tests passed, including 256 XeFG-tag preparation/copy cycles and the missing-notification negative control. Real XeFG runtime interpolation/HDR display has NOT been verified.

Next in-game test: Windows HDR on, RTX HDR and Auto HDR off; OptiHDR on; DLSS w/DX12 and XeFG output selected, saved, restart. Check HDR sliders, XeFG off/on, 2X/4X, resolution changes, transparent HUD and ReShade. Expected log: HDR10 output active and XeFG OptiHDR RGB10A2 PQ HUD-less input. This build includes pending Companion changes too, so leave its experimental replacement test off during HDR validation.
