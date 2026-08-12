# Chunk streaming plan

Eliminate hitching from chunk loading/unloading and world switches, in gameplay
**and** in the editor, by re-landing the `progressive-chunk-experiment` branch
as a sequence of small, verified phases on master. Written to be executed **one
phase per session, in order**, by a future agent (Opus 5) with no memory of the
session that produced it.

Companion to `CLAUDE.md` (general handover), `Docs/RENDERING_PLAN.md` (renderer)
and `Docs/DESTRUCTION_PLAN.md` (destruction/particles). This file owns chunk
streaming: the resident-window slide, entity load/unload across chunks, the
world switch, and the loading screen.

---

## How to use this document

1. Read **The verdict**, **The rules**, **Ground truth** and both ledgers.
   They were verified against master `0538de6` and branch `1b157f9`
   (2026-08-12); re-verify any `file:line` before editing — lines drift.
2. Find the first phase in **Progress** that is not `DONE`. Do **only that
   phase**. Each phase says *why this order*; do not skip ahead or bundle.
3. Meet the phase's **Acceptance** criteria before marking it done. If you
   cannot, mark it `BLOCKED` with a note rather than half-landing it.
4. Update **Progress**, the phase notes, and the ledgers (`OPEN` →
   `FIXED (phase N)`), with measured numbers — the next session plans against
   them.
5. One branch per phase (`chunk-streaming-phase-N`), re-synced with master
   first — Joey squash-merges PRs and deletes merged branches. No AI
   attribution in commits.

### Progress

| Phase | Status | Session / commit | Notes |
|---|---|---|---|
| 0 — Foundations, baselines, standalone fixes | DONE except T1 | `chunk-streaming-phase-0`, PR #55 | Symptom recorded (E12), standalone fixes landed, hitch gate in with a 150 ms baseline, all numbers below, ASan lane green, 60 Hz feel and un-pausing confirmed on screen. **T1 (the streaming harness) is BLOCKED on a render-context seam — see the phase 0 notes; phase 1 owns it.** |
| 1 — Atomic off-thread window commit | DONE | `chunk-streaming-phase-1` | T1's seam and harness landed with it. Hitch gate **150.2 → 102.4–104.7 ms** peak, 7 → 2 violations; `FlushDirty` 68.8 → 0.001 ms peak on a slide. ASan + TSan clean. |
| 2 — Bounded unload (serialize + encode) | OPEN | | **Also owns M7** — destroyed terrain respawning on chunk reload |
| 3 — Bounded load and the gameplay contract | OPEN | | |
| 4 — Streamed world switch behind the loading screen | OPEN | | |
| 5 — Bounded stamping of admitted renderers | OPEN | | |
| 6 — Occupancy-cell proxies + march budget (**gated**) | OPEN | | Only if phase 5 measures a need |
| 7 — Editor integration and guards | OPEN | | |

---

## The verdict on `progressive-chunk-experiment`

**The architecture is right. The delivery is wrong.** The branch's bones —
build the next voxel window in the back buffer on the worker, publish window +
physics + camera offset as one short main-thread transaction, make every other
piece of streaming work resumable under a wall-clock budget — are the correct
design and this plan keeps them. But it landed as one ~4,700-line WIP commit
that mixes that redesign with an unrelated rendering change, five standalone
bug fixes, per-manager gameplay band-aids, allocator experiments, test-harness
work and committed local settings, with no design doc and at least two
memory-safety defects (ledger E1, E2). Two follow-up commits ("bootstrap
players", "freeze physics bodies") are band-aids for a self-inflicted problem —
gameplay ticking against a world whose initial window does not exist yet — that
a one-line ordering rule removes (see **The design**).

**Treat the branch as a reference implementation, not a base.** Do not rebase
it, do not merge it, and do not cherry-pick from it mechanically. Each phase
below names exactly which parts of the branch it re-derives; re-type them
against master with the ledger in hand. The branch stays alive as
`progressive-chunk-experiment` until phase 7 is done, then gets deleted (local
and origin).

For the record: the branch builds clean on `editor-release`+tests, all three
CPU suites pass, both of its GPU ctest gates pass in Release on the dev
machine, and a direct `--map` run of Valley Beat1 holds 60 fps with no
`[stall]`. Whatever Joey saw broken is therefore **not** covered by any
existing gate — phase 0 starts by pinning it down with him.

---

## The rules

These are invariants for every phase. Violating one is how the experiment got
into trouble.

- **R1 — Gameplay never ticks against a missing initial window.** A world's
  gameplay (`World::Tick`/`FixedTick` of entities and game systems) must not
  advance until the initial 3×3 resident window is fully committed and its
  roots admitted. With a loading screen, the screen covers the wait; without
  one (direct `--map`, editor play), the first gameplay tick is simply held.
  This single rule replaces the branch's `Bootstrap` entity metadata,
  `Player::SetPersistent(true)`-in-constructor, and the `PhysicsBody`
  non-resident-ground freeze — none of those land.
- **R2 — The voxel window is published atomically or not at all.** Physics
  pointers, render mapper swap, world offset and camera offset move together
  in one main-thread transaction (the experiment's `US_COMMIT`). Nothing
  observes a half-window. The committed back buffer already contains the full
  static voxel volume of every incoming chunk, so collision and rendering are
  complete at commit even while entity admission lags.
- **R3 — No unbounded main-thread work, ever.** Every streaming step
  (serialize, encode, deserialize, admit, stamp, far-field) runs under a named
  wall-clock budget with a resumable cursor. All budget constants live in one
  header (`Voxagine/Source/Core/ECS/Systems/Chunk/StreamingBudgets.h`, create
  it in phase 1) — the experiment scattered `2.0` five times and `0.5`, `4.0`,
  `12.0`, `8192`, `32768` as magic numbers.
- **R4 — A raw `Entity*`/`Component*`/`rttr::instance` held across a frame
  boundary must have a stated lifetime defense** (event subscription, liveness
  check against the world's entity list, or ownership). This is the class
  behind ledger E1 and the pre-existing master bug M3, and it is the same
  class CLAUDE.md's "Raw pointers to resources outlive the resources" already
  names.
- **R5 — Cancellation is part of every state.** The player walking back across
  a boundary erases the in-flight `ChunkUpdateGroup`
  (`ChunkSystem::RemoveUpdateGroup`). Every resumable state a phase adds must
  define what a cancelled group leaves behind in each `Chunk` (cursors, staged
  roots, encode state, pool storage) and reset it. Add the walk-back scenario
  to the tests of every phase that adds a state.
- **R6 — Rendering-visible changes get their own phase, captures, and Joey.**
  The experiment silently bundled `MARCH_STEP_BUDGET` 16384 → 1024 with a
  proxy redesign. Anything that changes what the marcher does goes through
  phase 6, with headless captures at the hard crops (CLAUDE.md: "Verify at the
  case that is hard") and Joey looking at the screen for judgement calls.
- **R7 — Measure on this plan's gates, not on vibes.** The acceptance
  instruments are the **Testability & robustness** requirements T1–T8 (the
  CPU streaming harness, single-step sweeps, sanitizer lanes, work-counter
  gates), `gpu_chunk_streaming_frame_budget` (16.7 ms per-frame ceiling
  across a real window transition), and the in-game audits
  (`VOXAGINE_SYNC_AUDIT`, `VOXAGINE_VOXEL_AUDIT`, `VOXAGINE_COVERAGE_AUDIT`,
  `VOXAGINE_PYRAMID_AUDIT`, `VOXAGINE_INTEGRITY_AUDIT`) run **during**
  streaming. Build and test Debug *and* Release (two Release-only/Debug-only
  breakages already shipped in this tree). A property that only a human can
  currently check is a property the phase must first make checkable —
  "not expressible" was the reason two of the destruction play-session
  defects went uncaught until `SetDynamic` existed.
- **R8 — Keep the tree portable.** Windows stays `_WIN32`-clean, mobile still
  links (the Android CI lane), and nothing new reads the voxel mapping back
  (ReBAR: every CPU read of it is a PCIe read of VRAM).
- **R9 — Never land**: `Game/PlayerPrefs.vgprefs` changes, `.idea` attribute
  churn, stderr prints in per-renderer/per-entity paths (the experiment
  measured its own `[bake]` warnings turning a 2 ms bake into 20+ ms — keep
  hot-path reporting behind env vars), or profiler `Report` calls inside
  per-component inner loops without a profiler-enabled guard.

---

## Ground truth — what hitches today, on master

Verified by reading master and by the experiment's measurements (its numbers
are quoted from branch comments; most were taken in Debug — re-measure in
Release at phase 0).

- **The whole entity half of a window slide runs in the job's *completion
  callback*, which is the main thread.** `ChunkSystem::UpdateGroup`
  (`US_RENDERING`'s callback, master `ChunkSystem.cpp`) does, in one frame:
  `SetChunkVolumeAt` for every chunk, `LoadEntities()` for every incoming
  chunk (full RTTR deserialization of every root — "hundreds of milliseconds
  in Debug" per chunk), `UpdateEntities` for moves, the offset/camera update,
  then `UnloadAsync` for outgoing chunks — whose "async" is only
  `EncodeVoxels`; `FindEntitiesInChunk` + `SaveAndDeleteEntities` (full RTTR
  serialization) run on the main thread first. CLAUDE.md's "Chunk loading
  stalls the frame" ledger entry is this.
- **M1 — FIXED (phase 1).** `VoxelBrickGrid::FlushDirty` walked *both*
  buffers' dirty bits on the main thread every frame while
  `ChunkSystem::RenderChunk` wrote back-buffer bits from the worker: a data
  race, and 68.8 ms of main-thread stall in Release (0.6–0.8 s in the
  experiment's Debug measurement). It walks the front buffer only now;
  `FlushDirtyBackBuffer` runs at the end of the chunk render job, on the
  thread that owns that buffer. Measured on a slide afterwards: **0.001 ms
  peak.** `BeginBackBufferBuild`/`EndBackBufferBuild` plus
  `StreamingCounters::BackBufferFlushRaces` make a repeat of it an assertion
  in Debug and a baseline entry in Release (T5).
- **M2 — FIXED (phase 1).** `RenderChunk` wrote the mapped window one
  `uint32_t` at a time into write-combined memory and scanned all 128 rows of
  first-load chunks that contain only the ground row. It publishes a row per
  `memcpy` now and skips the occupancy scan above y = 0 for a chunk that has
  never been admitted. Not separately attributable in the slide measurement —
  it is inside the render job, which is off the main thread as of the same
  phase — which is the honest statement of what a fix on a worker buys.
- **M3 (pre-existing lifetime bug): `World::WorldConnectionInformation` holds
  a raw `rttr::instance`** of a possibly chunk-owned component; links are
  resolved once and dropped. The experiment's rework (source entity + component
  type, liveness check, retry) fixes a real master bug and is kept (phase 3).
- **M4: world switch stalls.** `Application::Run` does a full `WaitForGPU`,
  then `WorldManager::SwapWorlds` runs old-world `Unload` + new-world
  `Initialize` synchronously: measured here at 270 ms for Valley Beat1 on the
  branch (which already skips some clears); master additionally pays
  `RenderSystem::~RenderSystem`'s per-renderer voxel clears, a full
  `ClearVoxels` of two 288 MiB mapped buffers ("multi-second" per the
  experiment's comment), the far-field build (213–243 ms, CLAUDE.md), and
  `OnComponentAdded` stamping of every static renderer (~400 ms,
  RENDERING_PLAN 4c).
- **M5: `Chunk::EncodeVoxels`** reserves 10 MB unconditionally, builds the
  stream with `insert`-at-offset, frees 48 MiB per chunk on the worker
  (allocator/TLB interruptions of the main thread — experiment measured
  18–31 ms of main-thread descheduling), and prints timing to stderr
  unconditionally.
- **M7 (reported by Joey after phase 0, unreproduced): destroyed terrain comes
  back when its chunk reloads.** Not covered anywhere else in this plan, which
  is about *when* streaming work happens rather than what survives it — but the
  whole mechanism is in the unload/reload path, so **phase 2 owns it** and must
  not close without it. Three things established by reading, before anyone
  spends a session on the obvious theory:
  - **Damage has no carrier but the chunk volume.** A destroyed voxel is a
    cleared voxel; a `VoxRenderer` serializes a model path and a transform, so
    the `RootEntities` JSON a chunk writes on unload cannot hold damage even in
    principle. The encoded voxel blob is the only candidate.
  - **The reload does *not* naively re-stamp the pristine model, and that
    theory should not be re-derived.** `Chunk::LoadEntities` sets
    `SetChunkInstanceLoaded(true)` on every renderer of a non-first load, and
    that flag suppresses the stamp in both places it could happen —
    `RenderSystem::OnComponentAdded` (the `!IsChunkInstanceLoaded()` term) and
    `VoxelBaker::Bake` (`bIsStaticChunkLoaded`). The remaining way through is
    `m_bForcedUpdate`, which no chunk-load path sets today.
  - **The unload order is a race, and it is the first thing to measure.**
    `SaveAndDeleteEntities` calls `Entity::Destroy()`, which is deferred
    (`World::RemoveEntity`), so `VoxelBaker::Clear` — which erases each static
    renderer's voxels *from the chunk's own `m_VoxelData`* — runs on a later
    main-thread pass, while `EncodeVoxels` has already been enqueued on a
    worker. Whether the blob captures the model geometry, the cleared geometry
    or a torn mix is undefined, on the same `m_VoxelData` as P7.

  **Diagnose before fixing** (CLAUDE.md: voxel theories die on contact). The
  decisive instrument is occupied-voxel counts either side of the codec for a
  chunk that took damage — report from `EncodeVoxels` and `DecodeVoxels` under
  `VOXAGINE_CHUNK_IO_TIMINGS`, destroy a wall, walk out and back. If encode
  already shows the model missing, Clear won the race and the fix is ordering.
  If encode preserves the damage and the reloaded world does not, something
  re-stamps and the two guards above are the place to look. **Phase 2's
  harness scenarios must include "destroy, unload, reload, assert the same
  voxels"** — this is exactly the class of defect that is invisible until it is
  expressible.
- **M6 (unrelated but real, found on the branch): `GameTimer` runs the fixed
  callback once per display frame** no matter how many steps accumulated —
  at 30 display fps the game simulates at half speed. Also: `TextureReadData`
  frees stbi memory with `delete[]` (heap corruption on world transitions);
  `m_uiFramesThisSecond` counts frames wrong; `AudioSystem::OnComponentAdded`
  autoplays before `Start`. All land in phase 0.

**How the experiment's state machine works** (reference for phases 1–4; branch
`ChunkSystem.cpp`/`Chunk.cpp`): `US_INIT → US_WAIT → US_RENDERING` (worker
builds the whole back buffer via `RenderChunk` and finishes it with
`FlushDirtyBackBuffer` *on the worker*; the main thread meanwhile constructs
first-load entity trees detached from the world) `→ US_LOADING_ENTITIES`
(reload chunks admit in slices; moves update) `→ US_COMMIT` (the atomic
publish: volumes, offset, one `WaitForVoxelReaders`, mapper swap,
`PublishVoxelWindow`, camera) `→ US_LOADING_FIRST_GAMEPLAY_ENTITIES` (all
non-static roots window-wide, before any static art)
`→ US_LOADING_FIRST_ENTITIES → US_START_UNLOADING` (serialize outgoing roots,
shallow post-order, one root per tick) `→ US_ENCODING` (RLE encode in 2 ms
main-thread slices) `→ US_UNLOADING`. Group advance happens once per display
frame (`Tick`), not per fixed tick. Chunk storage recycles through a pool
instead of being freed.

---

## The experiment's defect ledger

What must **not** be reproduced when re-deriving. E-numbers are referenced
from the phases.

- **E1 (memory safety) — OPEN.** `Chunk::PrepareUnloadBatch` snapshots raw
  `Entity*` into `m_PendingUnloadEntities` and serializes them across many
  fixed ticks *while gameplay runs and can destroy/delete them*. `IsDestroyed()`
  is only consulted after a root finishes serializing; the serializer itself
  walks freed memory. Phase 2 defines the defense.
- **E2 (memory safety / state leak) — OPEN.** Group cancellation
  (`RemoveUpdateGroup`) can erase a group mid `US_LOADING_*` /
  `US_START_UNLOADING` / `US_ENCODING`. The per-chunk resumable state
  (`m_PendingLoadedRoots`, `m_uiNextStagedAdmission`, `m_bUnloadPrepared`,
  `m_bEncodePrepared`, `m_uiEncodeCursor`, `m_bIsUnloading`) leaks into the
  next group: duplicate admissions, staged roots deleted only at chunk
  destruction, half-encoded chunks recycled into the pool. Rule R5; phases 2
  and 3 must each add the walk-back test.
- **E3 (design) — OPEN.** Gameplay ticks against a half-admitted world and
  every cross-chunk link becomes a transient null, patched per manager
  (`GameManager::ResolvePlayers`, `SpawnerManager::RefreshSpawnerLinks` polled
  from four places, `Weapon::Fire` guard). Unbounded bug class. Phase 3
  replaces it with a contract; the per-manager polling does not land.
- **E4 (perf shape) — OPEN.** `RenderSystem::Render` calls
  `WaitForVoxelReaders()` (a VDirect fence wait) **every frame** while any
  static bake is pending — a per-frame CPU/GPU serialization during exactly
  the frames that are already busy. The wait belongs at the commit point and
  at genuinely-required buffer replacements only (phase 5 defines where).
- **E5 (rendering, unmeasured) — OPEN.** `MARCH_STEP_BUDGET` 16384 → 1024
  bundled invisibly with the proxy redesign. Master's own comment documents
  why a too-low budget produces bright speckle in dense geometry (budget
  exhaustion reports *lit*). Phase 6 re-derives it with evidence or leaves it.
- **E6 (rendering, unmeasured) — OPEN.** Occupied-32³-cell proxies start rays
  up to a cell diagonal before geometry and overlap the loose-voxel registry's
  proxies; the overdraw-vs-march-length trade was never measured in isolation.
  Phase 6.
- **E7 (semantics) — OPEN.** `Player` gets `SetPersistent(true)` in its
  constructor and a `Bootstrap` RTTR metadata path in `DeserializeWorld`,
  changing serialization order and chunk-save behavior for every world.
  Dropped by R1.
- **E8 (fragile inference) — OPEN.** `PhysicsBody::Tick` infers "chunk not
  resident" from `GetVoxel(x,0,z) == nullptr` and silently freezes bodies.
  Dropped by R1/R2 (ground of the committed window always exists).
- **E9 (correct but entangled) — OPEN.** The `Present()` rework special-cases
  sprite-only worlds via `bHasSceneWork = !m_AABBList.empty() || …` scattered
  through the render loop; a legitimate zero-AABB frame of a real world would
  silently skip scene submission and reuse the stale draw texture. It also
  conflicts textually with master's iPad-port bindless-packing changes in the
  same functions. Phase 4 re-derives the minimal version on top of master's
  `Present`.
- **E10 (unbounded/unowned pooling) — OPEN.** The chunk-storage pool and the
  16 M-word stamp arena have no ownership story across worlds with different
  chunk sizes, re-sort the free list on every recycle, and never return heap
  fallback blocks. Phases 2 (pool) and 5 (arena, if measurement still wants
  it) land simplified versions with stated bounds.
- **E11 (observability) — FIXED by not repeating it.** Unconditional stderr
  prints and per-node profiler reports in hot loops (R9).
- **E12 (reported by Joey, phase 0 step 1) — OPEN. Two flows, neither covered
  by any gate.** Asked at phase 0 which flow he saw broken on the branch; the
  answer was **menu → level load**, and **"chunks load in too late during
  normal gameplay"**. Read together they are the two halves of the same
  omission: the branch made every streaming step resumable but never made
  anything *wait* for the result. Menu → level hands gameplay a world whose
  window and roots are still arriving (which is exactly what E3/E7/E8's
  band-aids were papering over), and a window slide admits static art so far
  behind the camera that the geometry arrives visibly late. **R1 is the
  answer to the first** (phase 3 holds the first gameplay tick; phase 4 puts
  the loading screen over it) and **the admission budget is the answer to the
  second** (phase 3's ordering, phase 5's `StreamingBudgets.h` constants,
  judged on screen by Joey — phase 5's acceptance already names pop-in as his
  call). Neither is expressible as a check today; **phase 3 and phase 5 must
  each add one before claiming their acceptance** — the plan's own R7 rule.
  A candidate for the second: a harness counter for *frames between a chunk
  becoming resident and its last static root being admitted*, gated in
  `perf.txt`; and in-game, the existing `VOXAGINE_COVERAGE_AUDIT` extended to
  report bricks that are resident-but-unstamped rather than only uncovered.

## The keep list

Verified-good pieces of the branch, referenced from the phases. K1 the atomic
commit + worker back-buffer build (incl. `FlushDirtyBackBuffer`); K2 the
row-`memcpy` + ground-only-first-load `RenderChunk`; K3 budgeted resumable
loops with per-item cursors (`ChunkUpdateGroup` item cursor, shallow
(de)serialization stack walkers in `Chunk`, `ForEachStampedVoxelRange`);
K4 gameplay-first admission ordering + camera-distance chunk sort; K5
`LoadWorldAfterStreaming`/`UpdateStreamingWorld` (loading screen covers
streaming; single activation transaction; deferred audio autoplay; fade
preservation; sprite-list discard before texture release); K6 resumable
`ResolveWorldLinks` keyed by source entity + component type with liveness
check; K7 the standalone fixes (GameTimer, stbi, fps count, audio guard,
JT_IO decode routing, encode reserve/append fix, decode timing behind env
var, `ResizeWorldBuffer` guards, `OnWorldResumed` far-field rebuild); K8 the
`VOXAGINE_GPU_STREAMING_ONLY` 16.7 ms hitch gate and the bounded candidate
search in the stress fixture, plus the `fire`/`forward-on`/`forward-off`
ui-script tokens; K9 model-mesh mapper geometric growth + append-only upload
+ drain-before-replace, and the model instance NaN/index validation in
`Present`; K10 `PrepareModelMeshes` during load + meshing moved off
`VoxRenderer::SetFrame` (a 202 ms hitch meshing static models that never use
the mesh).

---

## The design

One paragraph, so every phase can check itself against it:

**The voxel volume streams invisibly; entities stream contractually; gameplay
never sees a partial world it wasn't promised.** The worker builds the next
window's complete static voxel volume in the mapper's back buffer and its
brick/occupancy hierarchy, then one main-thread transaction publishes
volume + physics pointers + world offset + camera (R2). Entity work
(serialize out, deserialize in, admit, stamp) is resumable in budgeted slices
(R3) — construction happens *detached* from the world, so gameplay only ever
observes admission. Admission is ordered gameplay-roots-first and, if
measurement allows, **all non-static roots of the whole incoming window admit
in a single frame** (they number in the dozens; static art numbers in the
hundreds) — which makes cross-links between gameplay entities atomic again
and shrinks the link-retry mechanism (K6) to the gameplay→static edge only.
The initial window is not special: it uses the same machine, but gameplay's
first tick waits for it (R1) — behind the loading screen when there is one,
as a held first frame when there isn't. World switches initialize the target
world hidden and stream it to readiness before one activation transaction
swaps it in (K5).

---

## Testability & robustness

The destruction rewrite earned its stability from one decision: the whole
write path runs at full fidelity in a unit test, so every invariant is checked
by machine on every commit, in CI, with no GPU. Streaming gets the same
treatment. **A phase is not done because the game feels smooth; it is done
when the suite proves the property the phase claims.** These are requirements,
not suggestions — each phase's Acceptance names which of them it extends.

- **T1 — A streaming harness in `voxagine_tests`, no GPU, no display.**
  `Tests/Harness/StreamingHarness.{h,cpp}`: a real `ChunkSystem`, real
  `Chunk`s, the real `JsonSerializer`, a real `VoxelGrid` and
  `VoxelBrickGrid`, with the voxel mapping backed by a plain
  `std::vector<uint32_t>` (exactly `VoxelWorldHarness`'s trick — the engine
  only ever *writes* the mapping, so a plain array is full fidelity). It
  drives the state machine directly: scripted camera positions, explicit
  `FixedTick`/`Tick` calls, a synchronous inline job queue so "worker"
  work runs deterministically at a chosen point between ticks. World content
  comes from small synthetic `.wld` JSON fixtures checked into `Tests/`
  (a 4×4-chunk level with a handful of entities per chunk, including one
  deep hierarchy, one gameplay root with cross-chunk links, one oversized
  scaled renderer) — not from shipping levels, so CI needs no content pack
  and a failure names a five-entity world, not a thousand-entity one.
  The skeleton landed in phase 1 (phase 0 was blocked on the seam); every
  later phase adds its scenarios to it. Read phase 1's "T1 — landed" note for
  the two deviations from this description.
- **T2 — Budgets are injectable, and tests budget by count, not by clock.**
  Wall-clock budgets are the right runtime behavior and the wrong test
  behavior (time is machine-dependent; the house perf gate exists because of
  exactly this). Every resumable loop takes its budget through
  `StreamingBudgets.h` as a policy the harness can replace with a *count*
  (nodes, roots, samples, runs). Two consequences the suite exploits:
  every scenario also runs in a **single-step sweep** (budget = 1 unit) so
  *every* resumption point is exercised, not just the ones a fast machine
  happens to hit; and interleavings become reproducible — "cancel after
  exactly N admissions" is a test line, not a race.
- **T3 — Sanitizer lanes.** The scenario suite (including the streaming
  scenarios) runs under ASan+UBSan locally before every phase lands, and a
  CI job is added in phase 0 (`-fsanitize=address,undefined` on the existing
  Linux lane — the suite is 3–36 s, it can afford it). Phase 1 additionally
  runs its worker/main-thread scenarios under TSan once, and records the
  result; the harness's inline job queue means TSan findings implicate real
  ordering bugs, not test scaffolding.
- **T4 — Work counters gate, timings report** (the destruction plan's split,
  applied to streaming). The harness records exact, machine-independent
  counters per scenario: entity nodes serialized per slice (max), roots
  admitted per frame (max), encode runs per slice (max), commits per
  transition (must be exactly 1), voxels written per commit, staged-root
  high-water mark. Any regression against `Tests/Baselines/perf.txt` fails
  the run. "Bounded work per tick" stops being a comment and becomes a
  number CI enforces.
- **T5 — Invariants are assertions, not conventions.** Debug builds assert
  the load-bearing rules where they can be checked cheaply: the atomic-commit
  invariant (no `SetChunkVolumeAt`/offset/camera write outside the commit
  transaction — guard with a `ChunkSystem` "in commit" flag); no main-thread
  back-buffer flush while a render job is in flight (thread-id + job-active
  check in `FlushDirtyBackBuffer`'s front-side twin); cancellation reset
  completeness (`Chunk::ResetStreamingState` leaves every cursor/flag at its
  post-construction value — assert by comparison in the reset itself);
  admission never touches an entity whose chunk state says unloading. An
  invariant that fires in the harness's single-step sweep is a phase blocker.
- **T6 — Cancellation is fuzzed, not spot-checked.** One harness scenario
  per phase 2+ drives a seeded random walk: camera crosses a boundary,
  reverses after a random number of budget units, repeats a few hundred
  times, with destruction edits interleaved — then asserts the terminal
  state (all groups drained, every chunk's streaming state reset, voxel
  audit/codec round-trip clean, no staged roots outstanding). Seeded
  `DeterministicRandom`, same as the particle sweep, so a failure replays.
- **T7 — Failure injection.** Streamed content is data, and data is hostile:
  scenarios cover a chunk root with an unknown `EntityType`, a root whose
  JSON lacks `Children`, an entity destroyed by "gameplay" between staging
  and admission, an entity destroyed mid-unload-serialization (the E1
  repro), a link whose target id never arrives, and a root that exceeds the
  stamp-capacity `UINT32_MAX` guard. Defined behavior in every case is: skip
  or defer the item, log once, keep streaming — never crash, never wedge the
  state machine (the group must still drain).
- **T8 — The GPU gates are the integration half, not the proof.**
  `gpu_chunk_streaming_frame_budget` (16.7 ms ceiling) and
  `gpu_destruction_sync_stress` stay local/RUN_SERIAL and verify what the
  harness cannot: real frame pacing, real driver behavior, the audits
  running against the real mapper. The division of labor is the same as
  destruction's: harness = algorithmic half, in-game audits + GPU gates =
  integration half. Neither substitutes for the other.

## Phases

### Phase 0 — Foundations, baselines, standalone fixes

*Why first: everything later is measured against this phase's gates and
baselines, and the standalone fixes are tangled through the experiment diff —
landing them separately shrinks every later re-derivation.*

1. **Sit with Joey and pin down "broken".** The branch passes every automated
   gate on this machine, so the broken state he saw is not captured by any
   test. Get the symptom (which flow — menu→level? window slide? editor?),
   add it to the experiment ledger, and if it is expressible as a check, add
   it to the gate in step 3. Do this before writing code.
2. **Land the standalone fixes (K7, M6) on master**, each as its own commit:
   `GameTimer::RunFixedUpdates` running the callback per accumulated step +
   the `ManualGameTimer` unit test (branch `Tests/Foundation/GameTimerChecks.cpp`);
   `TextureReadData` → `stbi_image_free`; `m_uiFramesThisSecond` accounting;
   `AudioSystem` `m_bStarted` guard; chunk decode on `JT_IO`
   (`JobQueue::EnqueueWithType` public); `EncodeVoxels` reserve-once +
   append-with-`resize` (keep the *synchronous* shape — slicing is phase 2);
   decode/encode stderr timing behind `VOXAGINE_CHUNK_IO_TIMINGS`;
   `ResizeWorldBuffer` null-guards and skip-identical-resize;
   `OnWorldResumed` far-field rebuild. Take K9's mapper-growth +
   validation changes too if they apply cleanly to master's `Present` —
   they fix real lifetime bugs independent of streaming.
   **Note:** the GameTimer fix changes simulation under low display fps —
   run `voxagine_tests` and have Joey confirm feel at 60 Hz.
3. **Port the streaming hitch gate (K8) to master**: the
   `VOXAGINE_GPU_STREAMING_ONLY` mode of `DestructionSyncStress`, the
   `gpu_chunk_streaming_frame_budget` ctest entry, and the ui-script tokens.
   On master it will fail — set its ctest `WILL_FAIL` property (or a
   `DISABLED_` prefix with a comment) and record the measured numbers here as
   the baseline instead. This is the plan's primary instrument; phases 1–5
   drive its peak frame time down and phase 3 flips it to a hard gate.
4. **Baseline measurements, Release, quiet machine, headless** (record here):
   worst frame during a Beat2 window transition; `[world-switch]` split for a
   menu→level load (add the branch's timing instrumentation around
   `SwapWorlds`/`LoadWorld` — it is cheap and permanent); encode/decode ms per
   chunk; `CPU Chunk LoadEntities` per chunk.
5. **The streaming harness skeleton (T1) and the sanitizer lane (T3).**
   `StreamingHarness` with the inline job queue, the synthetic `.wld`
   fixtures, and two first scenarios against *master's* behavior: a plain
   slide completes and the codec round-trips; the walk-back drains. These
   pass trivially today — they exist so phase 1 has a harness to extend and
   so the ASan+UBSan CI job is proven green before any streaming code moves.
   Add the first work counters (T4) to `Tests/Baselines/perf.txt`:
   commits-per-transition, nodes-per-slice (unbounded on master — record the
   observed value; later phases ratchet it down).

   **The harness half did not land — it is blocked on a production seam and
   moves to phase 1. Read the T1 note under "Phase 0 notes" first.** The
   sanitizer half (T3) did land and is green.

**Acceptance:** all standalone fixes green in Debug + Release, ctest suites
pass (now including the harness scenarios, under ASan in CI), the GPU gate
exists with a recorded failing baseline, work-counter baselines recorded,
numbers filled in above, Joey's symptom recorded. No streaming semantics
changed yet.

#### Phase 0 notes — measured baselines

All Release, headless, quiet machine (RTX 4070 SUPER), `640x360 --uncapped`
unless stated. Re-measure against these, not against the branch's Debug
numbers.

| What | Baseline |
|---|---|
| **Peak frame across a Beat2 window transition** (`gpu_chunk_streaming_frame_budget`) | **150.2 / 150.2 / 153.6 ms** over three runs, 7 violations of ~4,270 measured frames, against the 16.7 ms budget — **~9x over** |
| `[world-switch]` a level (`--map` Beat2) | `initialize` **977 ms**, everything else ~0 |
| `[world-switch]` a menu world | `initialize` 28 ms, previous world `unload` 10.5 ms |
| `CPU Chunk LoadEntities` | **41.6 ms** per chunk |
| `CPU Chunk SaveAndDeleteEntities` / `FindEntitiesInChunk` / `Unload` | 1.10 / 0.04 / 2.63 ms |
| `Chunk encode` | **9–12 ms** per chunk |
| `Chunk decode` | <1 ms (the chunks that came back were near ground-only; re-measure on a dense one) |
| `CPU VoxelBrickGrid::FlushDirty` | **68.8 ms peak** — this is **M1**, in Release. The branch's 0.6–0.8 s was Debug. |
| `CPU VoxelBaker::Occupy (added)` | 50.1 ms peak at world load, 15.0 ms avg / 18.7 peak per slide |
| `CPU VoxelBaker::Bake` | 55.1 ms peak (static), 0.024 ms repair scan |
| GPU passes during a slide | Voxel 1.46 ms, Sun Shadow 0.41, Pyramid Upload 0.82 — the hitch is entirely CPU |

**Three findings that change what later phases have to do:**

1. **`gpu_destruction_sync_stress` is already red on master**, on this machine,
   in Debug *and* Release, and was red before this phase touched anything
   (verified by rebuilding the fixture, `Player.cpp` and `CMakeLists.txt` at
   master and re-running). It completes phase 0 and then exhausts `--frames`;
   given more frames it sticks in phase 1 with the window oscillating and
   emits chunk-transition hitches forever. **The cause is its steering**:
   `RequestCurrentPhase` recomputes `SetCameraLoadOffset` from the camera in
   `Tick`, and `ChunkSystem::FixedTick` consumes it against the camera as of
   `FixedTick` — so any camera that moves between the two drags the effective
   load position with it. It was left exactly as master has it: repairing it
   is a fixture rewrite, not a phase-0 item, and it is now recorded rather
   than quietly failing. **Phase 7 should either fix its steering the way the
   streaming mode does — observe, do not steer — or retire it in favour of
   scenario coverage.**
2. **The menu → level flow is not scriptable headlessly.** Joining a player on
   the main menu is bound to `IK_GAMEPADOPTION` and `IK_MOUSEBUTTONLEFT` only
   (`StartToJoinPlayerComponent`), and `--ui-script` is keyboard-only, so a
   script gets as far as the main menu and stops. **Phase 4's acceptance
   ("menu → level via loading screen, headless ui-script") cannot be met until
   a `join` token exists** — either a keyboard binding for the join action or
   a mouse-click token. Budget it into phase 4.
3. **`--frames` is a main-loop iteration count and behaves opposite to
   intuition**: Release runs it out ~10x faster than Debug, so the existing
   6000-frame GPU gate covers *less* wall time in the config it is measured
   in. The streaming gate is set to 200000 and is bounded by the fixture
   exiting itself and by the ctest `TIMEOUT`. Do the same for anything new.

**Deviation from the plan, recorded:** the `Player` direct-launch
single-player default (`HasMap()`) was pulled forward from phase 3, because
without it a `--map` launch has *no* controllable player — nobody has joined,
both player entities stay live and neither takes input — so no headless run
could script gameplay and phase 0's own instrument could not produce a
number. Only that part; `Bootstrap` metadata and `SetPersistent` in the
constructor stay dropped (E7).

#### Confirmed on screen by Joey

The two judgement calls phase 0 could not make headless, both confirmed:

- **The GameTimer fix reads correctly at 60 Hz.** It is the one change here
  that alters simulation timing, so it was the one that needed a human.
- **Un-pausing is acceptable as it stands.** The far-field rebuild on resume
  fixes the horizon vanishing after the pause menu is opened once, at the cost
  of the full ~215 ms rebuild. Explicitly accepted for now; **phase 4 still
  owes the incremental build** (`FarFieldBaker::IncrementalBuild`, K5) and
  should re-ask once it lands.

#### A defect phase 0 introduced and fixed, and two wrong theories about it

Reported from a play session as **a character mesh disappearing** part-way
through, permanently. Worth reading whole, because the two theories that were
wrong are both the kind that look right.

**The cause.** K9's append-only mesh upload assumed `Mapper::Resize` preserves
contents. It does not — `VKMapper.cpp` drops both `VkBuffer`s and creates new
ones. So the first growth *after anything had been uploaded* left every
previously meshed quad as uninitialised memory, while
`m_uiModelMeshUploadedQuads` still claimed those quads were present, so nothing
ever put them back. Every model meshed before that moment stopped being drawn.
Master's version re-uploaded the whole store every time, which is why it never
showed. The fix re-uploads from zero whenever the allocation was replaced.

**Wrong theory 1: the NaN dropped at `Present`.** K9 also began dropping model
quad references whose instance transform is non-finite, and CLAUDE.md's
"Something produces NaN transforms" is open — so "a NaN character is now
deleted instead of drawn as garbage" was compelling. It is a real improvement
and it was made (repair the rotation and scale rather than reject the model,
and report by name), but it was **not this**: the running binary already
contained it.

**Wrong theory 2: the NaN dropped earlier, at the proxy check.**
`RenderSystem::PostTick` `continue`s on a non-finite AABB proxy *before* the
dynamic-model submission block, so a NaN would drop the renderer somewhere the
repair never runs. Also true, also not this — neither that warning nor the
repair's ever fired.

**What settled it was an audit, not an argument.**
`VOXAGINE_MODEL_MESH_AUDIT=1` compares the whole uploaded range against the
store after each sync. Temporarily making the growth policy exact so every sync
reallocates: 14 reallocations, an audit failure on every one before the fix
(word 0 uploaded as zero against a non-zero store), zero after. **The general
lesson is the one this tree keeps relearning** — an incremental upload needs an
invariant check against its source, because the failure is invisible in any one
frame: a model that stopped being drawn reads as content, not as corruption.

#### T3 — the sanitizer lane is in and green

`.github/workflows/build.yml` gains a `sanitizers` job: the whole suite in
Debug under `-fsanitize=address,undefined`. Green on master with no reports —
checks 3.2 s, scenarios 219 s, perf 32 s. Debug only (a second configuration
says nothing new for 4 more minutes), and `detect_leaks=0`, because the
harnesses deliberately leave their worlds to process exit and leak-clean
teardown is separate work rather than what this lane watches for.

#### T1 — BLOCKED, and exactly why

**The streaming harness did not land, and it should not be forced.** T1 asks
for "a real `ChunkSystem`, real `Chunk`s, the real `JsonSerializer`, a real
`VoxelGrid` and `VoxelBrickGrid`" driven with no GPU. `VoxelWorldHarness`
gets away with a `std::vector<uint32_t>` for the mapping because the write
path only ever *writes* it. `ChunkSystem` is not like that — it reaches
through the world for the render context itself:

- `GetVoxelData()` / `GetVoxelBackData()` / `GetVoxelDataSize()` — the front
  and back windows `RenderChunk` writes (`ChunkSystem.cpp` ~89, ~317, ~541)
- `GetVoxelMapper()->SwapBuffer()` — the publish at the end of a slide (~398)
- `BuildFarField(World*)` in `Start` and `OnWorldResumed`

and it needs a `World` with a `Camera` and a `PhysicsSystem`. **A `World` is
the harder half**: both `World::Initialize` and `World::PreLoad` construct a
`RenderSystem`, and `RenderSystem::Start` calls `ResizeWorldBuffer` on the
render context. So there is no path to a headless `World` today, let alone a
headless `ChunkSystem`.

Constructing a stub `RenderContext` is not the way out either: only two of
its methods are pure virtual, but `GetVoxelData` returns a private pointer
that `ResizeWorldBuffer` sets, and `GetVoxelMapper()->SwapBuffer()` needs a
real `Mapper`, which needs a device.

**What T1 actually needs is a seam, and it is a production change** — which
is precisely what phase 0's own acceptance forbids ("No streaming semantics
changed yet"). The seam is small and worth stating so the next session can
cost it:

> A `IVoxelWindow` interface with `GetFront()`, `GetBack()`, `GetSize()` and
> `Swap()`, which `RenderContext` implements over its voxel mapper and the
> harness implements over two `std::vector<uint32_t>`s; plus a way for a
> `World` to be built without a `RenderSystem` (or for `RenderSystem::Start`
> to tolerate a null render context).

**Do it as the first item of phase 1**, not as a phase of its own: phase 1
introduces `US_COMMIT` and the atomic publish, which is exactly the code that
would consume the seam, and phase 1's own acceptance already demands harness
scenarios asserting the commit invariant. Landing the seam and the commit
together means the seam is designed against a real caller instead of against
a guess. Phase 1 therefore owns T1's skeleton, the synthetic `.wld` fixtures,
the inline job queue and the first work counters in `Tests/Baselines/perf.txt`,
and its acceptance should not be signed off without them.

**Done — see "T1 — landed" under the phase 1 notes.** The seam turned out to
be `IVoxelWindow` plus `World::PreLoad(false)` and `World::GetRenderContext()`,
and a stub `RenderContext` was indeed not needed. The prediction that a `World`
would be the harder half was wrong: what actually cost time was four unrelated
places that dereferenced a platform subsystem without checking it existed.

Nothing else in phase 0 depended on the harness: the standalone fixes are
covered by the existing suites, and the baselines above are all in-game
measurements.

### Phase 1 — Atomic off-thread window commit

*Why now: it removes the two biggest hitches (M1's 0.6–0.8 s flush race and
the commit-callback pile-up) without touching entity semantics at all, and
every later phase assumes the commit transaction exists.*

Re-derive K1 + K2 from the branch (`ChunkSystem.cpp`, `VoxelBrickGrid.{h,cpp}`,
`RenderContext.{h,cpp}` — note master's `RenderContext` has moved: iPad-port
bindless packing landed in `Present`; re-derive around it, don't paste):

- `VoxelBrickGrid::FlushDirty` walks the **front buffer only**;
  `FlushDirtyBackBuffer` runs on the chunk worker at the end of the render
  job. Fixes M1 (both the race and the hitch).
- `RenderChunk`: row `memcpy` into the write-combined mapping; ground-only
  occupancy scan for first-load chunks (K2).
- `US_COMMIT` as one short main-thread transaction: `SetChunkVolumeAt` +
  `SetGridTarget` for every non-unload item, world offset, **one**
  `WaitForVoxelReaders()` (add it: a VDirect-only `WaitForGPU`), mapper
  `SwapBuffer`, camera offset + recalculate. Entity loading stays where it is
  on master for now (it moves in phases 2–3), so the state machine grows only
  `US_COMMIT`, and the callback's entity work runs after it.
- Group advance from `ChunkSystem::Tick` (once per display frame) in addition
  to `FixedTick`'s group creation — take the branch's shape.
- The camera-distance + loads-before-moves-before-unloads sort of group items
  (K4's sort half — it is inert while processing is still monolithic, but it
  is where the sort belongs and it is easier to verify now).
- Create `StreamingBudgets.h` (R3) with the first constants, injectable per
  T2 from day one — retrofitting injectability later is the expensive order.

**Cancellation (R5):** with only `US_COMMIT` added, cancellation windows are
no larger than master's; verify `RemoveUpdateGroup` still only ever runs
between states.

**Harness (T1/T4/T5):** scenarios asserting the commit invariant — after a
scripted slide, physics volumes, world offset and window contents changed
*together* (probe mid-machine with the inline queue: before the commit tick,
the old offset and old volumes are still live; after it, all new); the
commits-per-transition counter pins at exactly 1; the T5 assertions for
"no main-thread back flush while job active" and "no publish outside commit"
land here and the single-step sweep runs against them.

**Acceptance:** harness scenarios above green in the single-step sweep and
under ASan; one TSan pass over the phase-1 scenarios recorded (T3);
`VOXAGINE_SYNC_AUDIT` and `VOXAGINE_PYRAMID_AUDIT` clean while sliding the
window back and forth in the real game (drive with
`--ui-script forward-on/off`); hitch-gate peak frame time drops and the new
number is recorded; scenarios/perf suites green. The editor slides windows
through the same code — open the editor, move the camera across a boundary,
confirm no validation errors (ask Joey to watch).

#### Phase 1 notes — what landed, what it bought, what it did not

All Release, headless, quiet machine (RTX 4070 SUPER). Compare against the
phase 0 baseline table above, taken the same way.

| What | Phase 0 | Phase 1 |
|---|---|---|
| **Peak frame across a Beat2 window transition** (`gpu_chunk_streaming_frame_budget`, three runs) | 150.2 / 150.2 / 153.6 ms | **104.7 / 102.6 / 102.4 ms** |
| Violations of the 16.7 ms budget, per run | 7 of ~4,270 frames | **2 of ~3,760** |
| `CPU VoxelBrickGrid::FlushDirty`, peak during a slide | **68.8 ms** (M1) | **0.001 ms** |
| `CPU Chunk Commit` (the whole atomic publish) | did not exist | **0.002 ms** |
| `CPU Chunk LoadEntities`, peak | 41.6 ms per chunk | 48.4 ms for the pass |
| `CPU Occupy added (static)`, peak during a slide | 18.7 ms | 17.4 ms |
| `CPU Chunk Unload`, peak | 2.63 ms | 2.70 ms |

**The gate still fails, and the remaining 102 ms is entirely entity work.**
Commit is 0.002 ms; the two frames that break the budget are
`LoadEntities` (48.4) plus the admitted renderers' stamps (17.4) plus unload
serialization (2.7) plus what the profiler does not name individually. That is
exactly the work phases 2, 3 and 5 own, and it is now the *only* thing left in
the transition frame — which is what this phase was for. Phase 3 flips the gate
to a hard pass.

**Two frames, not one, and that is deliberate.** The group advance moved from
the end of `FixedTick` to `Tick`, so one state runs per display frame. Leaving
it in both would let the commit and the entity pass land together again, which
is the pile-up being broken up. The editor drives `World::Tick` unconditionally
in edit mode, so nothing loses its advance.

**Ordering, and why it is not cosmetic.** Entity work runs *after* the commit
now. Master's callback repointed the grid's chunk slots, then loaded entities,
then moved the world offset, then swapped the buffer. A first-load chunk's
static renderers therefore reached the baker while the slots were new and the
offset was old. Nothing stamps synchronously inside `LoadEntities` — components
register on the next `PreTick` — so this was latent rather than live, but it
was one deferred registration away from stamping into a window that was about
to be retired.

**Audits, in the real game.** `VOXAGINE_SYNC_AUDIT=4` and
`VOXAGINE_PYRAMID_AUDIT=4` across a long Beat2 traversal
(`--ui-script forward-on ... forward-off`): four of each, **0 of 75,497,472
voxels disagree, 0 brick/bitmap disagreements, 0 of 10,785,024 pyramid texels
disagree.** The `[stall]` line that run emits is the audit's own 20–32 s
main-thread readback of the window over PCIe, which is documented expected
behaviour; `journalctl -k | grep Xid` shows no GPU timeout, and a run without
audits holds 60 fps with no stall.

**Sanitizers.** The whole suite under ASan+UBSan: checks 530 s, scenarios 222 s,
perf 33 s, **no reports** (T3). One TSan pass over the streaming checks and the
streaming benchmark — the ones that actually run the chunk worker against the
main thread — **no reports** (T3's phase-1 clause). The harness's job queue is
the real one with real worker threads, so a TSan finding there would implicate
real ordering rather than test scaffolding.

#### T1 — landed, and where it deviates from the description

`Tests/Harness/StreamingHarness.{h,cpp}` drives a real `ChunkSystem`, real
`Chunk`s, the real `JsonSerializer`, a real `VoxelGrid` and `VoxelBrickGrid`
with no GPU and no display. What unblocked it is `IVoxelWindow`
(`Core/Voxels/VoxelWindow.h`): front, back, word count, brick grid, wait, swap.
`RenderContext` implements it over its voxel mapper; the harness implements it
over two `std::vector<uint32_t>`s. Nothing else should implement it — it is a
seam, not an abstraction layer, and having exactly two implementations is what
keeps it honest.

The rest of the blockage was smaller than phase 0 feared and needed no new
interface: `World::PreLoad(false)` builds a world with no `RenderSystem`, and
`World::GetRenderContext()` is the one place that answers "or null". Four
places crashed rather than degraded when there was no render context, input
context or clock — `Camera`'s constructor, destructor, `Awake` and
`SetOrthographic`; `Canvas`'s destructor and enable/disable (reached because
`World`'s constructor builds a throwaway `Canvas` as a link anchor); and
`LoggingSystem::Log`, which dereferenced a game timer that does not exist until
`Platform::Initialize`. All four are guarded now, in the same shape `Camera::
Awake` already used.

**Two deviations, both recorded rather than papered over:**

- **The job queue is the real one, not an inline queue.** T1 asked for a
  synchronous inline queue; `JobQueue` is a concrete class whose enqueue is a
  template, so inlining it means an interface and a virtual on the engine's
  hottest dispatch path — more production change than the seam it would test.
  What the assertions need is that *the state machine* advances only when the
  test says so, and that holds: every state is entered from `FixedTick`/`Tick`
  and every completion callback from `ProcessFinishedJobs`, all three called by
  `StreamingHarness::Frame`. A worker taking longer changes how many `Frame()`
  calls a transition needs and nothing else. Where a check must be *at* a
  state it watches a `StreamingCounters` value rather than counting frames.
  `JobManager::Initialize`/`Deinitialize`/`ProcessFinishedJobs` are public for
  this.
- **The single-step sweep (T2) has nothing to sweep yet.** `StreamingBudgets.h`
  exists, is injectable as a unit count from day one, and names its budgets —
  but phase 1 added no resumable loop, so every budget in it reads
  `Unbounded()`. That is the honest description of the state of the machine,
  and it makes the remaining unbounded work greppable. Phase 2 is the first
  phase whose sweep means something.

**The fixture is 5x5 chunks of 32 voxels** (`Tests/Fixtures/StreamingGrid5x5.wld`),
so the resident window is 96x32x96 and there are three distinct window positions
along x — the fewest that can express a walk-back, since cancelling a group
requires overtaking one. Two roots per chunk, one of them three deep. No
renderers: a `VoxRenderer` needs the resource stack, and phase 5 is where a
stamping test belongs. The whole streaming suite is **0.18 s in Release, 0.37 s
in Debug**.

**What the seven checks assert** (`Tests/Streaming/WindowCommitChecks.cpp`):
the initial window is resident before anything ticks; a slide publishes exactly
once (commits, committed groups and window swaps all 1); *the window moves in
one frame or not at all* — stepped frame by frame, with the offset, the camera
offset and the swap count each required not to move until the frame the commit
counter increments, and all three required to have moved on it; the occupancy
pyramid agrees with the published window after a slide; a walk-back that
overtakes a queued group cancels it and still reaches the terminal state the
camera asks for; a slide and back leaves two commits and two swaps and every
entity present once; and a chunk that leaves and returns keeps its voxels
through `EncodeVoxels`/`DecodeVoxels` and back into the window.

One finding worth carrying, surfaced by the first check rather than by
reasoning: **`ChunkSystem::Start` pushes a chunk into the window only where it
is a *move*.** The eight it loads outright never reach the mapping there. In
the game the gap is invisible because `RenderSystem::Start` writes the whole
window's y = 0 row itself (`RenderSystem::SetGroundPlane`); with no
`RenderSystem` the harness sees one chunk's ground instead of nine. Not a
defect today, but it means the initial window is assembled by two subsystems
that do not know about each other, which phase 4 will have to reconcile when it
streams a world switch.

### Phase 2 — Bounded unload (serialize + encode)

*Why now: unload is self-contained (its inputs are entities that already
exist), it is where E1/E2 live, and doing it before load means the load phase
never has to reason about a mid-unload chunk being reloaded.*

Re-derive the branch's `PrepareUnloadBatch` (shallow post-order serialization
with an explicit stack, one root per tick / node budget) and
`BeginVoxelEncoding`/`EncodeVoxelBatch` (2 ms main-thread slices — the branch
moved encode off the worker because freeing/scanning 48 MiB there descheduled
the main thread; keep its shape), plus `US_START_UNLOADING`/`US_ENCODING`
states, `MarkChunkUnloading` + `VoxelBaker::ForgetChunkStamp` (the departing
renderer must not clear its stamp out of the *new* window — the branch's
comment chain on this is correct and matches CLAUDE.md's window-slide
clear rules), and the chunk-storage pool (`AcquireChunkStorage`/
`RecycleChunkStorage`), simplified per E10: clear the pool on world unload,
assert per-world chunk dimensions, cap its size at the six-slot slide
turnover.

**E1 defense (required, choose and document):** either (a) subscribe each
snapshotted root to `Entity::Destroyed` and null its
`m_PendingUnloadEntities` entry, or (b) serialize each root *in the same
fixed tick* it is discovered, bounding work by node count rather than by
snapshot age. (b) is simpler if the per-root node counts allow it — measure
the largest root in the shipped levels first.

**E2 defense (required):** a `Chunk::ResetStreamingState()` called for every
item of a group `RemoveUpdateGroup` erases — clears unload snapshot, encode
cursor, staged roots (deleting detached ones), and the
`m_bIsUnloading`/`m_bUnloadPrepared`/`m_bEncodePrepared` flags coherently,
with the T5 completeness assertion inside it.

**Harness (T1/T2/T4/T6/T7):** the E1 repro as a scenario — destroy an entity
of an unloading chunk between two serialization slices (trivial to express
with count budgets) and assert the unload completes and the survivor set
round-trips; the seeded cancellation fuzz (T6) lands here and runs the
unload states at budget = 1; failure injection (T7) for the oversized root
and the mid-unload destroy; work counters: nodes-serialized-per-slice max,
encode-runs-per-slice max — ratcheted in `perf.txt` to the budget values.

**M7 (required): destroyed terrain must survive its chunk unloading.**
Read M7 in the ground-truth list first — the mechanism, the theory that is
already ruled out, and the instrument. Diagnose before touching anything;
then fix, and add the "destroy, unload, reload, assert the same voxels"
scenario to the harness. This phase rewrites the exact sequence the defect
lives in, so it is both the right place to fix it and the easiest place to
make it silently worse.

**Acceptance:** all of the above green under ASan, including the single-step
sweep; **M7 fixed and its scenario green**; `VOXAGINE_VOXEL_AUDIT` (codec
round-trip) clean during slides with
destruction happening in the real game (debris + owner slots exercise the
RLE); the walk-back ui-script survives an ASan Debug game run; hitch gate
number recorded; suites green.

### Phase 3 — Bounded load and the gameplay contract

*Why now: it is the hardest phase, it needs phases 1–2's states to exist, and
it is the phase that turns the hitch gate into a hard pass.*

Re-derive: staged detached construction during `US_RENDERING` and
`US_LOADING_ENTITIES` (`StageEntityBatch` — `ValueToEntityShallow` /
`EntityToValueShallow` in `JsonSerializer`, the explicit
deserialization stack in `Chunk`), admission after commit
(`AdmitStagedGameplayBatch` window-wide first, then static batches — K4), the
resumable `ResolveWorldLinks` (K6, fixes M3), and
`SetChunkInstanceLoaded` suppressing the redundant re-stamp request.

**The contract (E3's replacement), in order of preference — measure, then
pick the first that fits the budget, and write the decision here:**

1. **Atomic non-static admission:** all non-static roots of the whole
   incoming window admit in one frame (count them per level first — if the
   worst window is a few dozen roots, this fits a frame easily). Gameplay↔
   gameplay links are then never half-present, and `GameManager`/
   `SpawnerManager`/`Weapon` need no changes at all. Static art admits in
   budgeted batches afterwards; gameplay→static links ride K6's retry.
2. If some window's non-static roots are too many: gameplay-first budgeted
   admission (the branch's shape) **plus** K6's retry as the *general*
   mechanism — and no per-manager polling; if a specific manager still
   misbehaves, fix it by making its links go through the serializer's retry,
   not by hand-rolled `Refresh…` calls (E3).

**R1 lands here:** hold the first gameplay tick until the initial window's
commit + non-static admission completes. For `--map`/editor-play this is a
held frame (log how long); the loading-screen path arrives in phase 4. Do
**not** port `Bootstrap` metadata, `Player::SetPersistent` in the
constructor, or the `PhysicsBody` freeze (E7, E8). Do port the Player
direct-launch single-player default (`HasMap()`) — it is a test-enablement
fix, and the ui-script combat run below needs it.

**E2 again:** staged-admission cursors join `ResetStreamingState()`; the T6
fuzz now covers the load states too (cancel between staging and admission,
after partial gameplay admission, mid static batches).

**Harness (T1/T2/T4/T5/T7):** scenarios for the contract itself — a gameplay
root whose serialized link targets an entity in another incoming chunk
resolves by admission-frame end (option 1) or within N retry ticks
(option 2), asserted, at budget = 1; a link whose target never arrives
resolves to null without error spam and without keeping the source alive
(T7); staged roots destroyed pre-admission are dropped cleanly; the R1 hold
is asserted (no entity `Tick` observable before initial residency — count
ticks in a fixture entity); work counters: roots-admitted-per-frame,
staged-root high-water, link-retries-outstanding max.

**Acceptance:** all harness scenarios green under ASan including the
single-step sweep and the extended fuzz;
`gpu_chunk_streaming_frame_budget` **passes in Release and becomes a
required ctest gate** (remove `WILL_FAIL`); a scripted crossing with combat
(`forward-on`, `fire`) produces no null-link errors/warnings over repeated
transitions; scenarios/perf suites green; `[world-switch]` and
held-first-tick numbers recorded; Joey plays a level crossing several
boundaries and reports nothing popping visibly late (renderers admitted in a
batch become visible on their bake — confirm the pop is imperceptible or
schedule it for phase 5's budget tuning).

### Phase 4 — Streamed world switch behind the loading screen

*Why now: it composes phases 1–3 (the target world's initial window streams
through the same machine) and owns the last user-visible stall class M4.*

Re-derive K5: `WorldManager::LoadWorldAfterStreaming` +
`UpdateStreamingWorld` (target world initialized hidden; only its
`ChunkSystem` + `RenderSystem` advance — `PreTick` for admission, chunk
FixedTick/Tick, budgeted `Render` slice, `PrepareModelMeshes` (K10);
activation as one deferred transaction; deferred audio autoplay
(`AudioSystem::SetAutoPlayDeferred`/`ActivateDeferredAutoPlay`); fade
preserved; `DiscardActiveWorldSprites` before texture release — the freed-
bindless-ID crash class), `World::Unload(bool releaseRenderContext)` and
`RenderSystem::BeginWorldUnload` (skip per-renderer clears and the mapped
clears the incoming build overwrites anyway), `BeginVoxelWindowBuild`/
`PublishVoxelWindow` window-ready gating, `WorldSwitch` passing
`bWaitForInitialStreaming`, and the **incremental far-field build**
(`FarFieldBaker::IncrementalBuild` + `BeginFarFieldBuild`/
`ContinueFarFieldBuild`/`CancelFarFieldBuild`, model pins with clear
ownership — released on cancel, transferred on completion; far field reports
zero-size while building so the old level's volume is never sampled).
Meshing moves off `VoxRenderer::SetFrame` (K10).

**E9 discipline:** the sprite-only-world handling in `Present` must be
re-derived minimally on top of master's bindless-packing `Present`, as one
clearly-commented block, not scattered predicates — and it must distinguish
"world with no scene submissions this frame" from "sprite-only world" by an
explicit flag set by the world manager, not by `m_AABBList.empty()`
inference. Wait for VDirect only where a buffer is genuinely replaced.

**Editor note:** the editor swaps worlds through `LoadWorld`, not the
loading screen; it benefits from `BeginWorldUnload` and the far-field
incremental build automatically. Verify open-world in the editor no longer
multi-second-stalls (this is an editor-workflow hitch Joey cares about).

**Testability note:** `WorldManager`'s streaming logic must be written so its
decisions are checkable without a render context — the pending-world state
machine (single pending world, second-request rejection, activation only
after `IsStreaming()` and mesh sync report done, `ClearWorlds` discarding a
pending world) goes through methods the checks suite can drive with a stub
"readiness" provider; the far-field `IncrementalBuild` gets harness scenarios
of its own (cancel mid-build releases pins — assert via refcounts; complete
build transfers them; zero-size reported while building). What genuinely
needs the real renderer (fade, sprite lifetime, VDirect gating) is verified
by the headless game runs below and stays out of the harness.

**Acceptance:** the WorldManager and far-field checks above green (ASan);
menu → level via loading screen, headless ui-script: loading artwork
animates for the whole load (no 0-fps stretch — the branch's own `[fps]`
lines prove it), activation transaction < one frame, audio starts at
activation, `[world-switch]` splits recorded; switching between two levels
repeatedly leaks nothing (RSS plateau recorded, model-pin refcounts return
to rest); editor open-world timed before/after; Joey watches the flow once
on screen (fade, music timing, first-frame HUD).

### Phase 5 — Bounded stamping of admitted renderers

*Why now: after phase 3 the remaining per-slide main-thread cost is
`VoxelBaker` stamping the admitted static renderers (~400 ms worth per fresh
window, RENDERING_PLAN 4c), and it needs the admission machinery to exist.*

Re-derive: `OnComponentAdded` requesting an update instead of stamping
inline; `VoxelBaker::Bake(samples, budget)` with the global per-tick sample +
wall-clock budget; resumable `Occupy` via `ForEachStampedVoxelRange`
(cursor + last-position in `BakeData`, `OccupyInProgress`, the
0.5 ms-per-renderer inner slice); `Clear` skipping the never-stamped
first-load fallback scan; `HasPendingVoxelBakes`.

**Coverage while stamping (do not take E6's cell proxies for this):** a
renderer whose stamp is mid-flight must either already submit its proxy
(an AABB over unstamped space marches empty bricks — cheap and correct) or
be invisible with its voxels absent; never voxels-without-proxy (CLAUDE.md
"Four ways a voxel goes missing"). Run `VOXAGINE_COVERAGE_AUDIT` during
streaming: above-ground uncovered bricks must stay zero.

**E4 discipline:** `WaitForVoxelReaders` before host writes to the live
mapping happens **once per commit** (phase 1) — if per-frame incremental
stamping into the *front* buffer measurably stalls against VDirect reads
(the branch claims 20–22 ms implicit driver serialization in Debug), measure
in Release first; if real, prefer batching bake writes right after the
frame's VDirect fence signals rather than a blocking wait each frame.
Record what was measured either way.

**Stamp-buffer allocation (E10):** only if `AcquireStampBuffer` profiling
shows the `new uint32_t[]` per re-stamp actually costs frames in Release,
land the arena — simplified: free-list without per-recycle re-sort, heap
fallbacks returned at world unload, or simply per-renderer buffer reuse via
high-water capacity kept in `BakeData`. The full best-fit/coalesce allocator
from the branch is more machinery than the measurement justified.

**Harness (T1/T2/T4):** `ForEachStampedVoxelRange` gets direct checks —
resuming at every cursor value produces the identical voxel set as one
unbounded walk (sweep a small model at budget = 1), including the
duplicate-rounded-position path; a `VoxelWorldHarness` scenario stamps a
renderer across N slices and asserts buffer, occupancy bitmap, brick counts
and owner slots agree with a one-shot stamp (the six-representations rule);
Clear-mid-Occupy and cancel-mid-Occupy leave no partial ownership; work
counter: samples-per-tick max.

**Acceptance:** harness green including single-step sweep; slide +
world-load with `VOXAGINE_SYNC_AUDIT`, `VOXAGINE_COVERAGE_AUDIT`,
`VOXAGINE_PYRAMID_AUDIT` clean during heavy streaming + destruction; hitch
gate still green; visible pop-in of freshly admitted chunks judged by Joey
on screen (budget constants are the tuning knob and live in
`StreamingBudgets.h`); numbers recorded.

### Phase 6 — Occupancy-cell proxies + march budget (**gated**)

*Gated: do this only if phase 5's measurements show per-renderer proxy
submission or voxel-pass overdraw is a real remaining cost. The experiment
bundled this into streaming; it is a renderer change and must stand alone.*

If opened: re-derive the occupied-32³-cell proxy submission
(`RenderSystem::PostTick` static path, dirty-tracked cell cache, flat-ground
cell skip), the `MARCH_STEP_BUDGET` re-derivation, and the coverage-audit
extension for low bricks — as **separate commits with separate evidence**:
GPU pass timings at 4K before/after (per RENDERING_PLAN's method), captures
at the hard crops (valley walls, enclosed geometry), the speckle check for
budget exhaustion in dense scenes (E5 — budget exhaustion reports *lit*), a
`MARCH_STEP_DEBUG` sweep, and Joey on screen. Loose-voxel proxy overlap (E6)
resolved explicitly (cells already cover baked debris — decide whether
`SubmitLooseVoxelProxies` still runs). If the evidence is ambiguous, close
this phase as "not taken" with the numbers.

### Phase 7 — Editor integration and guards

*Last: it is polish over a machine that now exists.*

- Guard editor operations that assume a settled world: save-world, play-mode
  enter, map switch wait on (or refuse during) `ChunkSystem::IsStreaming()` —
  a save taken mid-stream must never write a half-admitted chunk's
  `RootEntities` (this is a data-loss class; add an assertion in the save
  path regardless).
- Editor View menu: the validation items (`Validate Occupancy Bricks`,
  `Validate Coverage Pyramid`, `Validate Voxel Representations`) must be
  correct against double-buffered flush semantics from phase 1 (front vs
  back flush — take the branch's `Validate(bBack)` split).
- Sweep the diff for anything in the keep list not yet landed; delete
  `progressive-chunk-experiment` (local + origin) once confirmed empty of
  unlanded value; update `CLAUDE.md`'s streaming sections and the ledgers
  here; move this plan's summary paragraph into CLAUDE.md the way the other
  plans do.

**Acceptance:** editor session — open large world, slide window, edit,
save, play, stop, switch world — with validation layers on and zero errors;
saved world diff-identical to a save taken at rest (add this as a harness
scenario: save mid-stream must equal save at rest, or be refused); the
complete suite — checks, scenarios (ASan, single-step sweep, fuzz), perf
counters, both GPU gates — green in Debug and Release; branch deleted.

---

## Verification reference (all phases)

```bash
# Build both configs (Release measures, Debug catches misuse):
cmake --preset game-release -DVOXAGINE_BUILD_GPU_TESTS=ON && cmake --build --preset game-release
cmake --preset editor -DVOXAGINE_BUILD_TESTS=ON && cmake --build --preset editor

# CPU suites:
ctest --test-dir Build/Linux/Editor/Release --output-on-failure

# The streaming hitch gate and the destruction stress (GPU, local only):
ctest --test-dir Build/Linux/Game/Release -L gpu --output-on-failure

# A scripted boundary crossing with combat, headless, audits on:
cd Game && VOXAGINE_AUDIO_NULL_DEVICE=1 VOXAGINE_SYNC_AUDIT=10 VOXAGINE_COVERAGE_AUDIT=10 \
  ../Build/Linux/Game/Release/bin/BitBuster --hidden --size 1440x810 --frames 3000 \
  --map Content/Worlds/Fishing_Village/Fishing_Village_Beat2.wld \
  --ui-script "wait,wait,forward-on,wait,fire,wait,wait,forward-off" --ui-script-interval 60
```

Headless always (`--hidden`, never a window on Joey's display), quiet machine
for numbers, `journalctl -k | grep Xid` before blaming a new change for a
freeze, and ask Joey to look at the screen for every judgement call — pop-in
visibility, loading-screen feel, penumbra-class rendering changes.
