# Depth-tested nameplate image midpoint test — 2026-09-27

Built and installed as game/winmm.dll; prior successful handoff build, config and log backed up beside this document. Companion plugin unchanged (0.1.2). No INI changes. This source build also includes pending XeFG OptiHDR support; the separately staged XeFG tester DLL is unchanged.

In OptiScaler's Companion settings, enable Lightweight 2x position interpolation, then Start depth-tested nameplate replacement (30s). Checkbox defaults OFF. Compare with checkbox off during the same test. Inspect Midpoints shown and Bypassed.

Implementation: capture the published Companion snapshot at the end of the exact native NamePlate draw; retain it with owned queue metadata, then the shared visibility slot. On the worker, use only consecutive images and matching consecutive Companion snapshots. Present one halfway integer-pixel translation of isolated current-image nameplate regions, then the latest endpoint. Uses small GPU clear/copy operations, no neural FG, image crossfade, CPU pixel readback, future-frame wait, mouse prediction or game-thread redraw.

Guards: identity/slot, text/icons/colors, size, bounds, overlaps and swept paths, 24px maximum movement, source cadence 4–50ms, layer cadence 1–8ms, sufficient 2x headroom, fresh image, previous endpoint shown. Native clicking unchanged. The copied image retains current depth-tested visibility; moved occlusion edges can shift briefly. Untracked glyph extents outside Companion bounds and alpha-only changes remain visual test risks. This is a partial lightweight test, not guaranteed 2x presentation delivery or zero added latency. An inserted midpoint costs one overlay refresh of visual delay. Slow composition bypasses interpolation.

Validation: full Companion suite passed, including native depth/transparency transport, context identity, exact snapshot ownership through queue/consumer, stale metadata invalidation, movement guards, WARP GPU translation, cleared original footprint, exact alpha, unchanged source and unrelated pixels. Release build successful with existing C4744/LNK4098 warnings only.

Test: slow and rapid camera pans, circling, overlapping nameplates, partially occluded plates/buildings, text/icons appearing/disappearing, clicking, FPS and responsiveness. Log counts actual displayed midpoint submissions versus bypasses; it does not prove display scanout timing.
