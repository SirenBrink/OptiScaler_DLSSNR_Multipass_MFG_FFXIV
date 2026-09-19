# AMD HIP pre-SR

This branch adds an experimental D3D12 AMD HIP backend for Neural Rendering before super resolution. It activates only on an AMD adapter when `RunBeforeSR=true` and the required files are present, so the NVIDIA path is unchanged.

## Requirements

- A compatible AMD GPU and HIP 7 runtime. The loader checks `%HIP_PATH%\bin\amdhip64_7.dll`, the normal DLL search path, and `PATH`; it does not select a GPU architecture by name.
- `dlssnr_amd_pass1.dll` and `dlssnr_on_amd_weights.bin` beside the OptiScaler proxy. Additional pass DLLs are required only when using additional passes.
- D3D12 colour, motion, and depth inputs with a zero colour subrect origin.

The runtime DLL and model weights are not distributed by this repository.

## Behavior and limits

- Dynamic resolution changes invalidate history and wait for a stable input before resuming.
- Command submissions are matched by COM identity and the neural command list is submitted separately when a game batches it with unrelated lists.
- Model scale, pass count, local structure, and skin structure are forwarded. Intensity is intentionally not applied by this AMD backend yet.
- Runtime offsets and the accepted runtime hash target the supported private runtime build. They are not tied to RX 9070 XT or `gfx1201`.
- Hardware runtime behavior on this migrated v0.8.4 branch is unverified.

Build `OptiScaler.sln` as `Release | x64`. The AMD code uses Windows `bcrypt.lib` for runtime verification; no additional source dependency is introduced.
