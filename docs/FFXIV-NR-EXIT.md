# NR / PreSR exit lifetime fix

Status: Release build, local regression tests, and the first live NR/PreSR/Dalamud exit test passed on 2026-09-23.

## Evidence

The 2026-09-23 07:05 crash is a native access violation in NVIDIA's DLSS runtime, reported by Dalamud's crash handler. Dalamud logs `Session has ended` at 07:05:14.889. OptiScaler's device-specific NGX shutdown succeeds at 07:05:15.485; its DLL detach is logged at 07:05:26.410, just before the native crash.

The matching installed DLL has SHA256 `B59A81249A0A01950D922B1390B56AD90709FAEFB803E230263FE318F5DD4BAF`. Its crash stack contains:

- `nvngx_dlss.dll+2BA38`
- `_nvngx.dll+6870D` (`NVSDK_NGX_D3D12_ReleaseFeature`)
- `WINMM.dll+2A546F`
- `WINMM.dll+3F4EAA`
- CRT exit-table cleanup and `LdrShutdownProcess`

Disassembly of that exact DLL maps the OptiScaler frames to `DeferredSr::Generation::~Generation` releasing its private DLSS feature, invoked by the CRT destructor of the global `current` owner. The feature at structure offset `0xA0`, parameter block at `0x98`, and subsequent resource-release sequence match the source. This is later than normal graphics shutdown. The historical September 10 CLR report describes a different stack and must not be treated as proof of the same cause.

## Change

Before FFXIV's bridge calls the driver's NGX shutdown, suspend new NR/PreSR recording under the NR mutex and explicitly retire PreSR generations while the runtime is available. Completed generations are destroyed; unresolved GPU work remains owned until OS process cleanup. No GPU waits are added. Repeat shutdown is harmless, and NGX reinitialization resumes NR with history reset without changing the saved NR setting. NR/quality toggles do not trigger this path.

PreSR's current/retired owning containers and NR's GPU helpers now follow the existing model state's process lifetime, preventing automatic driver callbacks during CRT teardown if the early cleanup is missed. Normal generation collection during gameplay is unchanged. The main NR model and any unresolved allocations remain intentionally retained at process exit, rather than being force-freed without proof that GPU work is complete.

This uses the existing NGX lifecycle boundary; no additional game executable hook, Dalamud hook, or DLL-detach driver call is added. It addresses the observed late private-DLSS release, not every possible plugin shutdown exception or the existing exit delay.

## Validation

- Release x64 build passed with existing compiler/linker warnings.
- `tests/run_nr_bridge_shutdown.ps1` compiles the production generation owners, destructor, retirement and suspend/resume functions against a simulated NGX runtime. Completed and pending generations, repeated shutdown, reinitialization and history reset pass.
- The same test leaves ownership alive after the simulated runtime shuts down. Current code exits normally; restoring the original two global owning containers produces the expected late NGX callback (exit code 99). This reproduces the ownership-order bug without an NVIDIA GPU.
- Existing production GPU lifetime tests pass on D3D12 WARP, covering pending/replayed submissions, GPU fences, dormant/shared generations and retirement.
- Live verification: the user reports no Dalamud crash on the first test. The log confirms that NR and PreSR ran, the new shutdown path released one completed PreSR generation with zero pending generations, and driver shutdown and DLL detach were reached. No new native crash report was created. This validates the observed PreSR late-release fix; it does not establish that every historical shutdown failure is resolved.

## Successful live run

Build `bf93bb1d-nr-exit-lifetime-test`, DLL SHA256 `0A02A9F3EB198222B2AB0DE0CDB823285F4EC5ED786151D68368C472DC888F3E`, tested 2026-09-23 07:39–07:40 with existing settings unchanged.

- Dalamud session ended at 07:40:25.395.
- NR/PreSR work suspended at 07:40:26.024633.
- One completed PreSR generation released at 07:40:26.037636; zero pending generations retained.
- Device-specific NGX shutdown began afterward and returned success at 07:40:26.079994.
- Swapchain teardown and DLL detach completed in the log at 07:40:37.366848. The existing exit delay remains.
- The crash handler did not record a new `Crash triggered` event or dump for this run. Its final broken-pipe message does not supply a normal process exit code; the confirmed outcome is the user's no-crash report plus the successful early cleanup and absence of a new crash report.

## Live test

Keep NR and PreSR on, play briefly, and exit through the game normally. Keep Dalamud enabled. No setting toggles are needed. Preserve the log before another launch.

Expected log order: `DLSS-NR bridge shutdown: new NR/PreSR work suspended`, `DLSS-NR PreSR shutdown: released ... retained ...`, then `FFXIV device-specific NGX shutdown`. Check whether the crash dialog disappears; the existing exit delay may remain. If a crash remains, compare the new Dalamud crash report with the stack above.
