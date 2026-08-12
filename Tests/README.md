# Tests

One binary, `voxagine_tests`, with three modes. No GPU, no display, no window —
see [`Harness/VoxelWorldHarness.h`](Harness/VoxelWorldHarness.h) for why the
whole voxel write path is reachable without any of them.

```bash
cmake --preset editor-release -DVOXAGINE_BUILD_TESTS=ON && cmake --build --preset editor-release
ctest --test-dir Build/Linux/Editor/Release --output-on-failure
```

| mode | what it is | cost (Release / Debug) |
|---|---|---|
| `checks` | assertions about one unit at a time | 11 s / 132 s |
| `scenarios` | every scenario × every invariant, each run twice | 3.3 s / 36 s |
| `perf` | every benchmark, compared against a baseline | 1.1 s / 8.4 s |

All three run in CI on both configurations, and the whole suite also runs under
ASan+UBSan (`.github/workflows/build.yml`'s `sanitizers` job). Almost all of
`checks`' wall time is one case - `ModelMeshUpload` is 10.9 of the 11 seconds -
so a filter is cheap: `Streaming` is 0.18 s, `VoxelStorage` 13 ms.

Each takes an optional substring filter — `voxagine_tests scenarios diagonal`, `voxagine_tests checks VoxelGrid`
— which is what you want while chasing one failure.

## Opt-in GPU integration test

`Tests/Rendering/DestructionSyncStress.cpp` is a separate executable because it
needs the real game world, chunk jobs, Vulkan renderer and a desktop GPU. It is
not part of the CPU suite or CI, and its fixture lives entirely under `Tests/`.

```bash
cmake --preset game -DVOXAGINE_BUILD_GPU_TESTS=ON
cmake --build --preset game --target voxagine_gpu_destruction_stress
ctest --test-dir Build/Linux/Game/Debug -R gpu_destruction_sync_stress --output-on-failure
```

The fixture alternates the resident window between two positions, first in
Fishing Village Beat 1 and then in Beat 2 after switching levels through the
game's real asynchronous replace-world path. It waits for all nine chunk
mappings to commit, restricts each new phase to chunks that entered the window,
and accepts a burst only when a live destructible model voxel is removed
immediately. Whole application-loop intervals have separate hard hitch limits
for steady destruction, chunk-window changes, and level replacement. It also
fails if the Vulkan direct timeline stops advancing while submissions are
outstanding. The CTest timeout remains the backstop for a main thread blocked
inside a fence wait.

## Layout

Directories name the **system under test**, not the engine directory the header
happens to live in.

```
Framework/     the runner, the four registries, the assertion macros, the baseline format
Harness/       a whole voxel world with no GPU (VoxelWorldHarness), the destruction
               pipeline driven once (DestructionRun), and a whole chunk-streaming
               world with no render context (StreamingHarness)
Fixtures/      synthetic .wld worlds the streaming harness loads
Baselines/     the checked-in perf baseline

Streaming/     ChunkSystem - the window commit transaction and what survives a slide
VoxelStorage/  VoxelGrid, VoxelBrickGrid, owner slots
VoxelEditing/  VoxelEditBatch — the one voxel write path
Destruction/   SphericalDestruction, plus the scenarios and invariants
Integrity/     IntegrityChecker — which geometry is still holding itself up
Particles/     ParticleCore, ParticleLanding, ParticleSimulation
Foundation/    engine-wide primitives that are not about voxels at all
```

Within a system: `<Thing>Checks.cpp` and `<Thing>Perf.cpp`. Scenarios and
invariants get one file each, named for the situation they set up or the
property they assert.

## The four kinds, and when to write which

Everything self-registers from a static initialiser, so **adding a case is
adding a file** — no runner edit, no header to update, one line in
`CMake/TestSources.cmake`. That cheapness is the design goal: every defect the
first play session found was a case that did not exist, and the cost of adding
one is what decides whether the next one gets written.

**Check** — one function, one input, one assertion.
[`Framework/Check.h`](Framework/Check.h).

```cpp
VOXAGINE_CHECK(VoxelGrid, GetCellCrossesChunkBoundaries)
{
    CHECK_EQ(world.Grid().GetCell(32, 1, 0).GetColor(), 0xFF804020u) << "at the seam";
}
```

`CHECK_` records and carries on; `REQUIRE_` records and abandons the case. Both
take a trailing `<< message`, which is usually the loop index that failed.

**Scenario** — a world with a shape somebody chose and a script fired at it.
[`Framework/Scenario.h`](Framework/Scenario.h),
[`Destruction/Scenarios/`](Destruction/Scenarios).

**Invariant** — something that must hold after *every* scenario.
[`Framework/Invariant.h`](Framework/Invariant.h),
[`Destruction/Invariants/`](Destruction/Invariants). A new invariant applies to
every scenario that already exists without touching any of them, and a new
scenario is checked by every invariant without knowing they exist. That is why
they are separate registries and not methods on each other.

**Benchmark** — a measurable piece of work.
[`Framework/Benchmark.h`](Framework/Benchmark.h).

## The perf suite, and what "regressed" means

Two kinds of metric, because "is it slower?" is two questions.

**Work** is exact: voxels destroyed, cells the flood fill visited, seeds that
survived filtering, islands emitted. None of it depends on the machine, the
compiler or the build type — Debug and Release produce byte-identical work
numbers, which is checked on every CI run. Any increase over the baseline is a
regression and fails the run. This is the signal a stopwatch cannot give you: a
rewrite that walks twice as many cells is invisible in wall time on a fast
machine and fatal on a slow one.

**Time** is not exact. It moves with the hardware, the build type and whatever
else the machine is doing. Timings are measured, compared and printed, but they
never fail a run unless `--strict` is passed, and they only mean anything at all
against a recording from the same machine.

So the checked-in [`Baselines/perf.txt`](Baselines/perf.txt) holds **only work
metrics**, which is what makes `ctest` mean the same thing on a CI runner as on
a workstation. For a timing comparison, record your own on both sides of the
change:

```bash
voxagine_tests perf --record before.txt      # before, on a quiet machine
# ...make the change, rebuild...
voxagine_tests perf --baseline before.txt    # after, same machine
```

Re-record the shared baseline when a change legitimately moves the work — and
say why in the commit, because that file is the only record of what the numbers
are supposed to be:

```bash
voxagine_tests perf --record-work Tests/Baselines/perf.txt
```

## Why one binary and not three

This used to be a gtest suite, a scripted `voxagine_gauntlet` and a
`voxagine_selftest`, each with its own `main`, its own output format and its own
copy of the destruction tick loop. The copies had already drifted apart on the
destructibility predicate and the debris density, which means the benchmark was
measuring something the scenarios never proved correct.

There is now one driver — [`Harness/DestructionRun.h`](Harness/DestructionRun.h)
— and scenarios and benchmarks both run it. A test that reimplements the thing
it tests, tests the reimplementation. That is the same reason
`SphericalDestruction::Apply` was pulled out of `PhysicsSystem`: the game, the
scenarios and the benchmarks all execute the same code now.

GTest went with it. The assertion macros in `Framework/Check.h` are a few dozen
lines and cover everything the suite used; a registry that can hold checks,
scenarios, invariants *and* benchmarks is worth more here than a third-party one
that can only hold the first.

## What this does not cover

The harness has no entities, no camera, no chunk streaming and no job threads,
and that is deliberate — it is what makes a run reproducible, and a state hash
worthless without it. The integration half lives in the running game as
`VOXAGINE_SYNC_AUDIT`, `VOXAGINE_COVERAGE_AUDIT`, `VOXAGINE_VOXEL_AUDIT` and
`VOXAGINE_INTEGRITY_AUDIT`. Both halves are needed and neither replaces the
other.
