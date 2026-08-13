# GPU performance gauge — 2026-08-13

## Scope and revision

This note covers the latest fetched `origin/master`, merged locally at
`1790b4abfba5f258fda2d04e73988f13ff253c2d` on 2026-08-13. The local branch
also retains the pre-existing one-commit `build-and-deploy.sh` update, so it is
two commits ahead of `origin/master`; nothing was force-updated or discarded.

The project is Vulkan 1.3. On macOS this is exercised through the installed
Vulkan stack/MoltenVK; it is not an RTX 4070 SUPER-equivalent environment.

## Verdict

**The current streaming failure is CPU frame pacing, not sustained GPU pass
time.** The latest recorded desktop GPU work is comfortably below a 16.7 ms
60 fps frame budget, but a Beat2 chunk-window transition is still recorded at
**26.9–28.0 ms**. That is about **1.6–1.7x over budget** and produces four
over-budget frames per roughly 3,400-frame run.

The remaining transition spikes are attributed to one indivisible
`VoxelBaker::Occupy`/stamp per frame. Phase 5 removed the prior 95 ms-class
stalls by making stamping resumable, but one renderer can still exceed the
frame budget. The next meaningful performance task is to split/otherwise bound
a single renderer stamp; lowering shader cost alone is unlikely to close this
specific hitch.

## On-device iPad result — Fishing Village Beat 1

The iPad measurement changes the device-facing conclusion: **Fishing Village
Beat 1 is decisively GPU-bound on the tested iPad.** The Voxel pass alone is
about three times the 60 fps frame budget and is the dominant cost by a wide
margin. CPU stage timings remain small, so reducing CPU stamping will not make
this scene reach 60 fps on this device.

| Metric | Steady-state result | Notes |
|---|---:|---|
| Frame rate | **10.72 fps** | 71 one-second samples; range 10–11 fps. |
| Voxel GPU pass | **50.586 ms** | 50.116–50.977 ms; primary bottleneck. |
| Sun Shadow GPU pass | 5.347 ms | 5.188–5.507 ms. |
| Post Processing GPU pass | 2.411 ms | 2.396–2.434 ms. |
| UI Renderer GPU pass | 0.436 ms | 0.392–0.471 ms. |
| Voxel Models GPU pass | 0.156 ms | 0.114–0.172 ms. |

### Test configuration

- Device: iPad Pro 12.9-inch (4th generation, iPad8,11), physical wired device
- OS: iOS 26.6
- Display: 2732 × 2048, landscape
- Build: Release, Vulkan through MoltenVK, profiling enabled
- Scene: `Content/Worlds/Fishing_Village/Fishing_Village_Beat1.wld`
- Render preset: `low` (the shipped mobile-quality preset)
- Run mode: uncapped; samples taken after load stabilised, from 14:06:00 to
  14:07:10 local device-log time on 2026-08-13

The run used `VOXAGINE_LAUNCH_ARGUMENTS` because UIKit/SDL does not expose the
device runner's command-line arguments to the app. This is now a mobile-only
equivalent of the existing launch options and permits repeatable iPad scene,
quality, frame-limit, and capture runs without editing shared settings files.

### Interpretation and next step

At the measured 50.586 ms, the Voxel pass consumes roughly **86%** of the
observed 58.9 ms GPU-pass subtotal (Voxel + Sun Shadow + Post Processing + UI
Renderer + Voxel Models). Reaching 60 fps requires the Voxel path to drop by
at least 3× before accounting for other passes. The first mobile optimisation
target should therefore be voxel render resolution, ray-march step count, or
visible voxel/AABB workload in Fishing Village; shadow and UI work are not the
first-order limiter.

## Evidence from the repository's measured runs

All measurements below were already recorded by the project on a quiet RTX
4070 SUPER desktop in Release, headless, uncapped mode. They remain the
comparable baseline for this codebase; they are not measurements of this Mac.

| Measurement | Result | Interpretation |
|---|---:|---|
| Beat2 streaming-transition peak, phase 4 | 94.5–95.6 ms | Before bounded per-renderer stamping; plainly CPU-hitch bound. |
| Beat2 streaming-transition peak, phase 5/latest landed streaming work | **26.9–28.0 ms** | Major improvement, but still misses the 16.7 ms gate. |
| Budget violations after phase 5 | 4 / ~3,400 frames | All reported at 16.9–28 ms. |
| Voxel GPU pass during an earlier transition baseline | 1.46 ms | GPU is not the source of the original 150 ms stall. |
| Sun Shadow GPU pass during that baseline | 0.41 ms | Also comfortably inside budget. |
| Pyramid Upload GPU pass during that baseline | 0.82 ms | GPU-side contribution remains small. |
| GPU total, Valley Path at 2880x1620, high quality | 3.21 ms | Earlier desktop rendering-quality measurement; about 31% of a 60 fps frame. |
| GPU total, same scene, low quality | 1.51 ms | 2.13x faster than high; useful mobile fallback, but not a fix for CPU stamp hitches. |

The high/low pass timing was taken on commit `405010e`, so it is retained as
renderer-cost context rather than claimed as a same-revision benchmark. The
streaming figures above are the relevant latest-master performance evidence.

## What the GPU gate actually tests

`gpu_chunk_streaming_frame_budget` runs the real Vulkan renderer and requires
a Beat2 window transition to complete without any application-loop frame over
**16.7 ms**. It also watches the Vulkan direct timeline so an apparently quick
run cannot hide a stalled GPU submission.

The sampled number is a **whole-frame wall-clock interval**, not a pure GPU
timestamp. That is intentional: it catches user-visible stalls regardless of
whether their source is CPU work, a fence wait, or the driver. Consequently it
is the right release gate, but it must be paired with the frame profiler when
assigning blame. The profiling notes identify the remaining cost as CPU voxel
stamping.

## Local machine and validation status

The present machine reports:

- macOS 26.6.1 (25G76)
- Intel HD Graphics 630, Metal supported, 1.5 GiB dynamic maximum VRAM
- Vulkan loader 1.4.357 available to CMake

I configured a fresh Release GPU-test build with
`VOXAGINE_BUILD_GPU_TESTS=ON`. It downloaded the pinned RTTR 0.9.6 dependency
and began compiling the full target; the long first build was stopped before
execution so it does not continue consuming the workstation. The build tree is
resumable. No local frame-time result is recorded here yet: an integrated
Intel/MoltenVK result must be kept separate from the RTX 4070 SUPER baseline,
and a result should not be invented from build success.

When the target is ready, run the focused gate three times on an otherwise
quiet machine:

```bash
ctest --test-dir Build/Darwin/Game/Release \
  -R gpu_chunk_streaming_frame_budget --output-on-failure
```

Also capture profiler pass times from the exact same run. For a device-facing
decision, repeat on the intended Android/iOS hardware after the desktop run;
the existing Android emulator numbers are SwiftShader (CPU rasterisation) and
are not GPU performance data.

## Recommended acceptance bar

1. Three Release runs pass the 16.7 ms streaming gate with no violations.
2. Record peak, median, and 99th-percentile whole-frame times along with the
   per-pass GPU timestamps and CPU `VoxelBaker::Occupy` time.
3. Keep the RTX desktop baseline and each target device in separate tables.
   Do not compare their raw fps as a regression signal.
4. Treat a pass that is GPU-bound differently from the present issue: reduce
   pass resolution/quality for the former; split or defer a single stamp for
   the latter.

## Sources in this tree

- `Docs/CHUNK_STREAMING_PLAN.md` — phase 0–5 transition measurements and the
  diagnosis of the remaining stamp hitch.
- `Docs/MOBILE_PORT_LOG.md` — GPU pass timings and high/low quality study.
- `Tests/Rendering/DestructionSyncStress.cpp` and `CMakeLists.txt` — focused
  GPU integration/frame-budget gate definition.
