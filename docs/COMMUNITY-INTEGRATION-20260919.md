# Community integration — September 19, 2026

This is a selective source integration into the FFXIV branch, not a replacement of its rendering pipeline.

## Sources and changes

- **stegalloway** (`69efe50380818213d140a61f95b36ee0ca9c02d3`): `DlssNr_GpuLifetime`, command-list reset observation, and WARP lifetime tests. Adapted to the existing global NR owner: retired textures and model features now wait for recording retirement and actual queue fences instead of 32 CPU evaluations. Submission/reset failures retain ownership rather than permitting an early release. This does not replace the descriptor ring or import the entire newer NR scheduler.
- **janblade** (`59973853d0ec557e0edd72c5ac84e7fce7dc326e`): NR runtime search paths when Streamline initializes NGX first (`7df67eed`); depth-plane SRV preservation (`733c2db0`); borrowed input/output DX11 texture ownership fixes (`9f4d22c9`, `7fca1b40`). Adapted the bounded allocator wait from `b34d3b52`, with a completed-fence/device-removal check before allocator reset. The broader swapchain redesign and exposure/shader changes are not included in this integration.
- **Ainquisition**, `amd-v084-minimal` (`5b6a53b1`): HIP PreSR backend, runtime validation, guide preparation, submission isolation and prerequisite reporting. Adapted to the existing FFXIV bridge by passing its actual command queue to the upscaler. The replacement color is scoped to the upscaler call and restored even on failure. The NVIDIA NR seam is bypassed on AMD. Initialization failures remain disabled until restart; a changed graphics device cannot reuse the old backend.
- **ShyVortex**: retain the separately prepared broad Ada rewrite and our existing provider-lifetime protections. No competing NVIDIA MFG implementation from these three forks is imported. This integration does not add the absent ShyVortex SM75/SM86 companion loader; RTX 30 NR with the community runtime is distinct from RTX 30 MFG.

Upstream references:

- https://github.com/stegalloway/OptiScaler-DLSSNR-PreSR-Multipass
- https://github.com/janblade/OptiScaler-DLSSNR-PreSR-Multipass
- https://github.com/Ainquisition/OptiScaler-DLSSNR-PreSR-Multipass/tree/amd-v084-minimal
- https://github.com/ShyVortex/OptiScaler-DLSSNR-PreSR-Multipass

Original upstream authorship and attribution are retained in imported files. The AMD implementation also credits MatheusGViana/dlss-5-amd-project (7b9dcb9). All integration commits are tagged `Assisted-by: GPT-6 Astra`.

## AMD prerequisites and limits

See [AMD-HIP-PRESR.md](AMD-HIP-PRESR.md). The matching `dlssnr_amd_pass1.dll`, `dlssnr_on_amd_weights.bin`, and HIP 7 runtime are external prerequisites, not included in this build. The runtime DLL is hash checked before private offsets are used. Place the runtime and weights beside the OptiScaler proxy DLL; additional passes require their runtime DLLs.

Use a DX12-backed upscaler available on the AMD card, enable NR and Apply before Super Resolution. NVIDIA model/style/intensity controls are not implemented by this backend. Existing unsupported configurations do not fall through to NVIDIA NGX. AMD status/prerequisite errors are displayed in the NR panel. No FFXIV AMD hardware validation is claimed.

## Validation and test scope

- Release x64 build.
- Upstream WARP lifetime tests: delayed submissions, multiple queues, replay, abandoned command lists, wrapper identity, concurrent notifications, dormant/shared resource generations.
- Review of FFXIV hook RVAs, the NGX shutdown ABI, PreSR motion-reset code, named presets and MFG source preservation.

The WARP tests validate retirement behavior, not neural inference or game compatibility. First test NVIDIA startup, quality/resolution changes, NR toggles, FG toggles and shutdown. AMD and RTX 40 stability require separate hardware sessions. Alternate-frame NR performance and HUD interpolation remain outside this integration.
