# Native scanned lighting and NR history

Status: validated in game on 2026-09-23. The native scan and event delivery are retained with conservative thresholds; visual ghosting reduction and GPU cost have not been quantified.

In FFXIV with the DX11-to-DX12 bridge, **DLSS Neural Rendering → Colour → Exposure source → Scanned exposure (native DX11)** now uses the game's native lighting pipeline. Other games retain the existing exposure scanner.

FFXIV supplies already-tone-mapped colour. This option does not apply its exposure again, change paper white, or require calibration anchors. The live adapted gain is reported in native game units. Manual paper-white controls and the unrelated DX12 candidate list are replaced by an explanation and a native status readout on this bridge.

**Reject abrupt lighting history (experimental)** enables NR history resets for detected lighting cuts. Turning it off leaves the scan active for comparison. `[DlssNr] WhitePointSource = 2` selects the native scan; `LightingHistory = true` enables rejection. The latter is a game-specific preference, retained in the INI independently of NR appearance presets. Old appearance presets remain compatible.

## Data and lifetime

- Reuses the exact DXBC size/checksum/CRC shader identification from the diagnostic. No executable addresses, image brightness estimates, or shape-only candidate selection.
- At most ten sampled frames per second. Requires the linked sequence: AdaptLum 1x1 R32_FLOAT output → ToneMapLut input → 1024x1 RGBA16F LUT → ToneMapping input → matching DLSS colour input. Unknown/HDR input flags or a changed link/format reject the sample.
- Copies the adapted gain, lookup texture and 48 bytes of tone-mapping constants, respecting constant-buffer binding offsets. Four reusable staging slots, about 33 KiB of payload storage. No full-screen copies, per-sample resource allocation after warmup, or capture files.
- A GPU event query proves completion before a nonblocking Map. No forced flush, CPU/GPU wait, resource-state changes or altered shader bindings. Results older than 350 ms are ignored. Missing shader chains, signature changes or unsupported samplers leave rejection inactive.
- Disable/re-enable increments the generation so old readbacks cannot become new readings. Normal bridge teardown disables scanning and releases completed staging resources; unfinished GPU work is never waited on from teardown. Allocation/readback failure or a prolonged GPU timeout stops the scan for the session.
- UI reads a locked CPU snapshot; it never reads a game texture. The render path samples shader bindings only during an armed sample frame, until a linked DLSS input has been observed.

## History policy

The CPU evaluates four fixed neutral inputs through the captured ToneMapping equation, constants and lookup sampler. It compares tone response rather than scene brightness, so changing the object under the camera does not itself trigger rejection. This signal represents the verified tone-mapping stage; later game colour effects and local lighting changes are not fully covered.

Initial conservative threshold: a change of at least 0.30 stops (approximately 23%) in any probe between samples no more than 200 ms apart. There is no accumulated drift threshold. Rearming requires 300 ms of quiet readings (steps no larger than 0.04 stops) and at least 750 ms since the previous cut. Initial readings, invalid values, stale gaps and duplicate samples cannot trigger a cut.

Only fresh events affect NR. Each consumer acknowledges an event once: model history across all active passes, and PreSR's private residual reconstruction/held/alternate-frame history. Main game DLSS and DLSS-G histories are unchanged. The existing motion rejection thresholds remain unchanged. Frame hold suppresses lighting resets. This is a whole-NR-history cut on selected lighting events, not spatial/per-pixel rejection or a claim to eliminate movement ghosting.

## Validation and gameplay test

Release x64 compilation and local WARP tests with the D3D11 debug layer exercise the actual game AdaptLum, ToneMapLut and ToneMapping shaders. The CPU tone-response probes match GPU output with both point and linear lookup sampling. Tests cover CB subranges, unchanged bindings, exact resource links, HDR/mismatched-input rejection, nonblocking drain, disabling with work in flight, stale/invalid readings, slow drift, abrupt cuts, oscillation, cooldown and separate NR/PreSR event consumers. No debug-layer errors occurred. These do not establish visual quality or FPS cost on the user's NVIDIA GPU.

In game, confirm the status reads **Native DX11 lighting: active** and the reading count advances. Play normally, then move between substantially different lighting conditions. Toggle **Reject abrupt lighting history** for comparison, keeping Scanned exposure selected. Watch for reduced lighting trails, any flash/blotching at a detected cut, and FPS impact. A zero cut count during gradual or modest changes is expected. Logs contain `FFXIV native lighting` and `DLSS-NR ... native lighting event` entries; no manual capture is needed.

The separate NR-dependent exit crash was subsequently addressed by the [PreSR lifetime fix](FFXIV-NR-EXIT.md), with its own successful live test. Exposure validation alone did not establish that fix.

## Sampler compatibility follow-up

The first gameplay run (06:27–06:31, 2026-09-23) produced no accepted native readings. Most sampled gameplay frames were rejected by the original sampler allowlist; startup/loading/exit also produced missing-chain or colour-link rejections. No native lighting events or associated NR history resets occurred. The user's visually stable result therefore does not validate lighting-history rejection yet.

The follow-up accepts all nine ordinary D3D11 filter combinations, including anisotropic, and standard address modes. Probes represent uniform neutral fields (zero lookup-coordinate derivatives), so their lookup uses the magnification filter. The verified texture has one mip. Both interpolation taps must lie inside the LUT; under that constraint wrap/mirror/border/clamp produce the same values. Comparison and min/max reduction filters remain unsupported. The observed filter and address modes are now logged when they change.

Local WARP debug-layer validation covers all 45 filter/address combinations against actual GPU ToneMapping output for uniform probes, plus rejection of comparison/reduction filters and out-of-bounds probe coordinates. No history thresholds were relaxed. The subsequent live run below confirmed active scanning and an advancing reading count.

The follow-up live run (07:00–07:05, 2026-09-23) confirmed the sampler is point-filtered with mirrored U/V addressing. At least 1,100 readings were accepted. One abrupt 0.613-stop tone-response change produced a single event, acknowledged once by both NR and PreSR about 14.5 ms after publication. Subsequent gradual changes and disabling/re-enabling the scan produced no extra event. The drop count remained unchanged throughout the accepted-reading interval; no native readback failure or timeout was logged. This validates signal collection and history-event delivery, not the magnitude of any visual or performance improvement. Conservative thresholds are retained.

The portable regression test is `tests/nr_lighting_history.cpp`. From a Visual Studio developer command prompt, compile with `cl /nologo /std:c++17 /EHsc /UNDEBUG tests\nr_lighting_history.cpp /Fe:nr_lighting_history.exe` and run the executable. It requires no game assets or NVIDIA GPU. The separate local WARP tests use extracted game shaders, which are not redistributed in this repository.
