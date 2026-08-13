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
2. Find the first phase in **Progress** that is not `DONE` or `NOT TAKEN`. Do
   **only that phase**. Each phase says *why this order*; do not skip ahead or
   bundle. **The table is in execution order, which is no longer numeric
   order**: 11, 12 and 13 were opened after 10 and run before it, because 10
   needs Joey at the machine and deletes the reference branch. Phase numbers
   are identities and are not renumbered — the notes, the branch names and the
   merged PRs all refer to them.
3. Meet the phase's **Acceptance** criteria before marking it done. If you
   cannot, you have two honest options and "PARTLY" is neither of them: mark it
   `BLOCKED` with a note, or - if what you landed is a complete mechanism and
   what is left is a *different* one - **rescope this phase to what it did and
   open the remainder as a new numbered phase with its own acceptance**.
   Phases 4, 5 and 7 were rescoped that way and 8, 9 and 10 are the remainders;
   the phase headings say so. A status that is neither done nor blocked rots,
   because nobody can tell what picking it up involves.
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
| 2 — Bounded unload (serialize + encode) | DONE | `chunk-streaming-phase-2` | M7 diagnosed and fixed — it was a re-stamp on reload, not the codec. Unload is two budgeted states; `Chunk Unload` 2.70 → 0.37 ms peak, `Chunk Encode` 2.00 ms peak over 14 slices. Four further defects found by the phase's own fuzz and failure injection, three of them pre-existing on master. Hitch gate unchanged at 101.7–112.6 ms: what is left is phases 3 and 5. |
| 3 — Bounded load and the gameplay contract | DONE | `chunk-streaming-phase-3` | Loading a chunk's entities is staging + admission, both budgeted; the gameplay contract is option 1 (atomic non-static admission), measured. M3/K6 landed: links are keyed by identity and retried. R1's gate exists and is inert by construction. **The hitch gate does not flip — and phase 3 is why that is now a finding rather than a guess**: the transition frame is `World::Render` (54.7 ms) + `PreTick` (34.3 ms), both `VoxelBaker` stamping, both phase 5's. Peak 101.7–112.6 → **94.2–95.1 ms**, violations 2 → 1. |
| 4 — Streamed initial window and far field | DONE | `chunk-streaming-phase-4` | Both costs *inside* `World::Initialize` are gone: the initial window streams through the update group machine (which also makes R1 live and testable) and the far field builds in 4 ms slices. `[world-switch] initialize` **876 → 316.6 ms**; what is left of it is one thing, `RenderSystem::Start`'s buffer allocation at 258 ms. **Renamed from "Streamed world switch behind the loading screen": the world-manager half became phase 8**, because it is a session's work with its own acceptance rather than this one's remainder. |
| 5 — Bounded stamping, per renderer | DONE | `chunk-streaming-phase-5` | Both stamps are bounded per *renderer*: `OnComponentAdded` requests instead of stamping inline, and `VoxelBaker::Bake` runs under `StreamingBudgets::VoxelBaking`. Peak transition frame **95 → 27.9 ms**. **Splitting below one renderer is phase 9** — it was built here, made the gate pass at 9.5–10.1 ms, and was reverted for silently losing 580 k voxels, so it needs its own acceptance rather than a note. |
| 6 — Occupancy-cell proxies + march budget (**gated**) | **NOT TAKEN** | — | Closed on phase 5's measurements, which is what the gate asked for. Every remaining frame over budget is *CPU*, and named: one `VoxelBaker` stamp of 140,640 voxels. During a slide the GPU passes read Voxel 0.24 ms, Sun Shadow 0.34, Pyramid Upload 0.82 — three orders of magnitude below the frames that break the budget. Neither proxy submission nor voxel-pass overdraw is a remaining cost, so E5 and E6 stay open and unmeasured rather than being re-derived on speculation. |
| 7 — The save guard | DONE | `chunk-streaming-phase-7` | `JsonSerializer::SerializeWorld` refuses a world whose chunks are still streaming, with a harness check. A data-loss class rather than tidiness — see the phase 7 notes. **Renamed from "Editor integration and guards": everything needing the editor open is phase 10**, which cannot be done headless and should not sit inside a phase that can. |
| 8 — World switch behind the loading screen | DONE except three named items | `chunk-streaming-phase-8` | K5's world-manager half. A level is initialized hidden, streamed over frames the loading screen keeps drawing, and swapped in whole: the same **~300 ms** `initialize` is paid behind the loading screen instead of in the one frame the player waits through, plus **1666–1717 ms** of streaming over **2518–3716 drawn frames**, plus a **16.8–19.0 ms** activation. Readiness folds in the far field and that is free, which closes phase 4's horizon judgement call. `join` and the auto-joined player one landed first, so menu → level is scriptable headlessly at all. **Not done and not attempted: `RenderSystem::Start`'s 258 ms, the far-field harness scenarios, E9's sprite-only *flag* (the sprite discard itself landed).** Confirmed on screen by Joey after the black-frame fix. |
| 9 — Splitting one renderer's stamp | DONE | `chunk-streaming-phase-9` | **The hitch gate flips: peak 27.9 → 9.82 / 10.15 / 10.31 ms against 16.67, and the `WILL_FAIL` is off.** The walk turned out not to be what lost the 580 k voxels — it is resume-identical at every budget from 1 up, checked against two real models — so the fixes are the two *interactions*: `Bake` no longer clears a half-written renderer, and the bake bookkeeping is written at the start of a stamp rather than the end. Occupancy identical at four slice sizes over eight runs; coverage, sync and pyramid audits clean during streaming — **not** with combat, whatever this row said before: the `fire` token does not work and phase 11 owns it. Costs **728–779 → 910–1,071 held ticks** before gameplay starts. Left open: **Joey has not judged pop-in on screen**, and `gpu_destruction_sync_stress` is broken *on master* — see the notes. |
| 11 — `--ui-script fire`, and the first headless run that destroys a voxel | DONE | `chunk-streaming-phase-11` | **The token was never broken; the script's clock was.** It counted display frames from process start, so a level whose hold ran long spent every token before `Player::Start` had bound anything — 621 held ticks in one Release run and **2,930** in the next, same binary, same command line, which is the whole of why it read as intermittent. The clock now stops while `World::IsGameplayHeld()`. A scripted run destroys **25,062 voxels over 32 bursts** where it destroyed 0, ever; `[destruction]` is printed by every run. **Both prior diagnoses were wrong** — see the notes. The acceptance run reproduces phase 12 headlessly (228 CPU-only voxels), which is the point of doing 11 first. |
| 12 — The CPU/GPU voxel disagreement during play | OPEN — **has a headless repro now** | | 540 voxels present only on the CPU (invisible but solid) and 4,426 only on the GPU (visible and not there), seen by Joey in play after phase 9. Phase 11's acceptance run reproduces the CPU-only half in four minutes: **228 of 75.5 M**, against **0** for the identical run with the `fire` tokens replaced by `wait`. Caused by destruction, and *not* an audit race — see the phase 11 notes for both, and do not re-derive either. |
| 13 — A lifetime handle for streamed content | OPEN | | Four of the ten defects the phase 9 play session found are one shape, and so is ledger M8: a raw pointer to streamed content, held across a frame, with nothing to say the target died. More guards is not the answer — two of them crashed *inside* the guard. `PlayerSlot` generalised into a handle (id + generation, resolved on use). |
| 10 — Editor session and closing the plan | OPEN — **last** | | Needs Joey at the machine. Runs after 11–13: it deletes `progressive-chunk-experiment` and its acceptance is the whole suite green, so it cannot honestly precede work that is still landing. Also owns `gpu_destruction_sync_stress` (broken on master, see the phase 9 notes) and the pop-in judgement. |

**The order to fix, and why it is this one.** 11 first because it is small and
because 12 cannot be *measured* without it. 12 second because it is a live
correctness defect the player can see, and because the instrument that finds it
(`VOXAGINE_SYNC_AUDIT` over a run that destroys geometry) is exactly what 11
delivers. 13 third because it is the largest and the least urgent — the crashes
it prevents are known and individually fixed — and because it will touch code
11 and 12 are editing. 10 last, unchanged.

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
- **M3 — FIXED (phase 3).** The connection records `(source entity id, source
  component type)` and is retried until both ends are in the world rather than
  cleared unconditionally; see the phase 3 notes. The original entry:
  **`World::WorldConnectionInformation` holds a raw `rttr::instance`** of a possibly chunk-owned component; links are
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
- **M7 — FIXED (phase 2). It was a re-stamp on reload; the codec was innocent,
  and the entry below was wrong about where to look.** Kept whole because two
  of its three bullets were confidently wrong and that is the useful part: the
  guard it says suppresses the stamp is **anded with `!UpdateRequested()`**, and
  a reload always requests one — the chunk re-serializes its roots from the
  *live* reflection registration, so the JSON it carries gains an `"Emissive"`
  member the level on disk never had, and that setter requested a bake update
  unconditionally. The unload-order race it names as "the first thing to
  measure" is not it: the instrument it asks for was built and shows damage
  surviving encode, decode and republication exactly. Read the phase 2 notes.

  **The original entry, as written:** destroyed terrain comes
  back when its chunk reloads. Not covered anywhere else in this plan, which
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

- **E1 (memory safety) — FIXED (phase 2), by not reproducing it.** The
  experiment's `Chunk::PrepareUnloadBatch` snapshots raw `Entity*` into
  `m_PendingUnloadEntities` and serializes them across many fixed ticks *while
  gameplay runs and can destroy/delete them*; `IsDestroyed()` is only consulted
  after a root finishes, so the serializer itself walks freed memory. The
  re-derived version holds no `Entity*` across a frame boundary at all: the
  chunk's entities are re-discovered every slice (0.06 ms) and a root is
  serialized whole inside one slice. Defense (b) of the two the phase offered,
  chosen on the measurement it asked for — the largest root hierarchy in any
  shipped level is 68 nodes. `Streaming/AnEntityDestroyedMidUnloadIsNotSerialized`
  is the repro, and it runs under ASan.
- **E2 (memory safety / state leak) — OPEN.** Group cancellation
  (`RemoveUpdateGroup`) can erase a group mid `US_LOADING_*` /
  `US_START_UNLOADING` / `US_ENCODING`. The per-chunk resumable state
  (`m_PendingLoadedRoots`, `m_uiNextStagedAdmission`, `m_bUnloadPrepared`,
  `m_bEncodePrepared`, `m_uiEncodeCursor`, `m_bIsUnloading`) leaks into the
  next group: duplicate admissions, staged roots deleted only at chunk
  destruction, half-encoded chunks recycled into the pool. Rule R5; phases 2
  and 3 must each add the walk-back test.

  **Phase 2's half is done, and it found the trap in the obvious fix.**
  `Chunk::ResetStreamingState` exists, asserts its own completeness, and
  `RemoveUpdateGroup` calls it — but *only for chunk state the erased group
  actually created*. Resetting every chunk a cancelled group mentions reaches
  into a transaction the **front** group is half-way through, because a chunk is
  an item of several groups at once; it made the front group re-prepare an
  unload, clear the roots it had already written and re-serialize entities it
  had already destroyed, so the chunk came back empty. Intermittent, and only
  the seeded walk found it. Phase 3's staging cursors join the same reset and
  the same rule.
- **E3 (design) — FIXED (phase 3), by contract rather than by patch.** All
  non-static roots of the incoming window admit in one frame (measured worst
  case 430, `Walking_Through_The_Maru_Beat3`), so gameplay->gameplay links are
  never half-present; gameplay->static links ride K6's retry, which is now the
  general mechanism rather than four hand-rolled `Refresh...` calls. The
  original entry, as written: Gameplay ticks against a half-admitted world and
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

  **The pool half is done (phase 2).** One block size per world (asserted), a
  hard cap of six — a slide turns over three chunks and one group can be queued
  behind another — no re-sorting, anything past the cap goes back to the
  allocator, and the whole pool dies with the `ChunkSystem`, so a second world
  with a different chunk size cannot see it. `ChunkStorageReused` against
  `ChunkStorageAllocated` is in `perf.txt`: a settled slide reuses three and
  allocates none. The stamp arena is still phase 5's, and still gated on
  measurement.
- **E11 (observability) — FIXED by not repeating it.** Unconditional stderr
  prints and per-node profiler reports in hot loops (R9).
- **E12 (reported by Joey, phase 0 step 1) — HALF FIXED (phase 3).** The first
  flow's answer, R1, is landed and inert (`ChunkSystem::IsInitialWindowReady`,
  `World::IsGameplayHeld`) and phase 4 is what makes it live. The second - "chunks
  load in too late during normal gameplay" - is now bounded rather than
  unbounded, and the *latency* it trades against is
  `ChunkUpdateGroup::MillisecondsSinceCreated`: **489 ms** end to end for a
  Beat2 transition, measured. Whether that reads as late on screen is Joey's
  call and phase 5's budget constants are the knob. **Still owed: the
  frames-between-resident-and-last-root-admitted counter this entry asks for -
  phase 9, where it is the number pop-in is judged against.**
  The original entry, as written - two flows, neither covered by any gate: Asked at phase 0 which flow he saw broken on the branch; the
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

#### Phase 2 notes — what landed, and the five defects it found

All Release, headless, quiet machine (RTX 4070 SUPER), compared against the
phase 1 table above.

| What | Phase 1 | Phase 2 |
|---|---|---|
| **Peak frame across a Beat2 window transition** (three runs) | 104.7 / 102.6 / 102.4 ms | **111.1 / 101.7 / 112.6 ms** |
| Violations of the 16.7 ms budget, per run | 2 of ~3,760 | 2 of ~3,700 |
| `CPU Chunk Unload` (find + serialize), peak | 2.70 ms, once | **0.37 ms**, 4 slices/s |
| `CPU Chunk FindEntitiesInChunk`, peak | 0.04 ms | 0.06 ms, 6/s (once per slice now) |
| `CPU Chunk Encode` | 9–12 ms per chunk, on a worker | **2.00 ms peak, 14 slices/s**, on the main thread |
| `CPU Chunk LoadEntities`, peak | 48.4 ms | 44.2 ms |
| `CPU Occupy added (static)`, peak | 17.4 ms | 17.6 ms |

**The hitch gate did not move, and that is the expected result.** Unload was
2.7 ms of a 102 ms transition frame. What is left is `LoadEntities` (44 ms,
phase 3) and the admitted renderers' stamps (17.6 ms, phase 5), exactly as
phase 1 predicted. Phase 2's subject was where the *rest* of the unload cost
lives and whether interrupting it is safe, not the peak.

**Encode moved onto the main thread, on purpose, and the trade is stated.**
The branch's reason - a worker freeing 48 MiB deschedules the game thread -
is not reproducible here in Release, so it is not the argument. The argument
is that a 32 MiB scan and a 48 MiB free competing for memory bandwidth during
the frames a transition is already expensive is worth converting into a
bounded 2 ms a frame, and that the storage pool removes the free entirely.
What it costs is transition *latency*: 14 slices is roughly 230 ms before the
group drains and the next one may start. Affordable because a chunk is 256
units across, so two crossings are seconds apart; `MillisecondsSinceCreated`
is the number to watch if that ever stops being true.

**The branch's shallow post-order serialization stack did not land, and
should not.** It exists to bound work below one root. Measured first, as the
plan asks: the largest root hierarchy in any shipped level is **68 nodes**
(over all 17 `.wld` files and 3,430 roots; the next largest is 34, the median
is 1), against a whole chunk's roots serializing in 1.10 ms. Bounding below a
root buys tens of microseconds and costs a resumable stack holding half-built
JSON and a raw `Entity*` across frames - which *is* ledger E1. Roots are
serialized whole instead, and the E1 defense is that plus re-discovering the
chunk's entities every slice (0.06 ms) rather than snapshotting pointers: **no
`Entity*` crosses a frame boundary at all.**

##### M7: it was never the codec

**Diagnosed before anything was touched, and the plan's own first theory was
wrong.** The ground-truth entry pointed at the unload ordering race -
`Clear` running after `EncodeVoxels` was enqueued - and named the two guards
that suppress a re-stamp. Both were checked and both are innocent:
`DamageSurvivesAnUnloadAndReload` shows a cleared voxel surviving encode,
unload, decode and republication exactly.

**What actually puts the model back is a third route through the guards.**
`VoxelBaker::Bake` consults `IsChunkInstanceLoaded()` only as
`(!Updated || bIsStaticChunkLoaded)`, **anded** with `!UpdateRequested()` - so
an explicit update request walks straight past it. And a reload always makes
one: `Chunk::SaveAndDeleteEntities` re-serializes departing entities from the
*live* reflection registration, so the JSON a chunk carries in memory gains
every property this build knows about even when the level on disk predates
them, and `VoxRenderer`'s "Emissive" setter called `RequestUpdate()`
unconditionally. First unload adds `"Emissive"`; every reload after it hands
the baker a renderer that says it was edited, and the pristine model is
stamped over voxels that were decoded with their damage in them.

Fixed in two places, deliberately: `SetEmissive` only requests an update on a
*change* (setting a value to itself should not be work), and
`SetChunkInstanceLoaded(true)` clears `m_bUpdateRequested`/`m_bIsFrameChanged`
- which closes the class rather than the one setter that happened to reach it,
at the one point that knows the decoded voxels are authoritative.

**Expressible now, in two halves.** There is no `RenderSystem` in the harness,
so `Tests/Streaming/ChunkReloadChecks.cpp` asserts the *decision* - no
chunk-restored static renderer may ask to be re-stamped - and
`StreamingCounters::ChunkInstanceRestamps` asserts the *consequence* in the
game and in `perf.txt`. Same division as destruction's: harness for the
algorithm, in-game audit for the pixels. The fixture gained a static
`VoxRenderer` root per chunk to make the first half possible at all, with no
`"Emissive"` member - which is how every shipped level is stored, and the
condition M7 needs.

##### Four more defects, three of them pre-existing on master

The fuzz (T6) and failure injection (T7) each earned their place on first run.

- **A cancelled group reset a chunk another group was mid-way through.**
  Introduced by this phase and caught by the seeded walk. A chunk is an item
  of several groups at once - the front one unloading it, a queued one about
  to load it back - so "reset every chunk this cancelled group mentions"
  reaches into a live transaction. It made the front group prepare a second
  time, clear the roots it had already written and re-serialize entities it
  had already destroyed, so the chunk came back **empty**. `RemoveUpdateGroup`
  now resets only chunk state a group actually created.
- **Unloading a chunk that was never loaded destroys it.** *Pre-existing.*
  Group creation reads `IsTargetLoaded()` - a promise a *queued* group made -
  and emits `T_MOVE` for a chunk a later cancellation then never loads.
  Serializing it writes an empty root list over the level's only copy of its
  entities and encoding it writes an empty RLE stream over its voxels; master
  loses the same data by the same route, since `SaveAndDeleteEntities` cleared
  and resized to however many entities it found. Skipped and counted
  (`UnloadsOfUnloadedChunks`) rather than fixed at the scheduling layer, which
  is a bigger change and belongs with phase 3's ordering work.
- **A chunk unload can delete the world's main camera, and everything then
  reads it freed.** *Pre-existing*, and known: the GPU stress fixture pins its
  camera persistent and says why in a comment. The default `Camera`
  `World::Initialize` creates is not persistent, so a window sliding over it
  serializes and destroys it, and the next `ChunkSystem::FixedTick` reads a
  freed `Camera` to decide where the window goes. Caught by the fuzz under
  ASan. `World::DeleteEntityFromLists` nulls the world's pointer and every
  `ChunkSystem` reader checks it - the same move `World::GetRenderContext`
  made in phase 1 - and the harness pins its camera the way the game's
  `CameraMultiplayer` is.
- **Two aborts in the deserializer on malformed chunk data.** *Pre-existing.*
  `GetHighestEntityID` and `ValueToEntity` index `"Children"` and
  `"Components"` without asking whether they exist; rapidjson's `operator[]`
  on a missing member asserts in Debug and returns a static null in Release.
  A root with no children is legal data - an older save, a hand-edited world.
  Both ask now, and T7's fixture keeps them honest.

Also fixed while rewriting the same function: `m_CopyDoc`'s pool allocator was
never reset, so every unload of a chunk added a fresh copy of its whole root
hierarchy to an arena that only grew, for the life of the level.

##### The instruments that stay

`VOXAGINE_CHUNK_IO_TIMINGS` now reports occupied-voxel counts either side of
the codec (`[chunk] (x,y) encode N ms, M occupied`) and the chunk the camera
has entered - the first thing to check when a headless run produces no window
transitions at all, which cost this session two runs. `--ui-script` gained
`backward-on`/`backward-off`: walking *back* is the only way a script reaches
the reload path, and M7 lives there.

##### Verification

Whole suite green in Debug and Release, and green under ASan+UBSan (checks
102, scenarios 31, perf 0 regressions, no reports). The fuzz was run 60 times
consecutively after the last fix with no failures - worth stating, because the
harness drives real worker threads, so the interleaving is not reproducible
and a defect that needs one is a *frequency*, not a pass/fail.

**In the real game**, Beat2, headless, with destruction and window transitions:
`VOXAGINE_VOXEL_AUDIT` reports **0 diverging voxels over 9 loaded chunks**, and
the encode/decode occupancy either side of the codec matches to the voxel on
every chunk. Destruction is visible in those numbers - chunk (1,1) encodes
235,819 occupied after three bursts against 236,234 pristine - so the codec is
demonstrably carrying damage rather than agreeing about nothing.

**And the whole of M7, observed rather than inferred**, in a Debug **ASan**
game run of Beat2 with destruction, a window slide out and a walk back: chunk
(1,1) took damage (236,077 occupied against 236,234 pristine), encoded 236,077
on the way out, decoded **236,077** on the way back, and the run reports **zero
`ChunkInstanceRestamps`** - no static renderer was re-stamped over its restored
voxels. No ASan reports.

**Two things about scripting this that cost more time than the work did.**
A `--ui-script` token whose scancode is missing from `applyScriptedScancode`
does nothing, silently, and the run still looks healthy - `backward-on` spent
ninety scripted seconds not moving the player before that was noticed. And a
headless run that produces *no* window transition at all is usually the camera,
not the streaming: `VOXAGINE_CHUNK_IO_TIMINGS` now prints the chunk the camera
enters, which answers that in one grep.

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

#### Phase 3 notes — what landed, and where the transition frame actually goes

All Release, headless, quiet machine (RTX 4070 SUPER), compared against the
phase 2 table.

| What | Phase 2 | Phase 3 |
|---|---|---|
| **Peak frame across a Beat2 window transition** (three runs) | 111.1 / 101.7 / 112.6 ms | **94.5 / 94.8 / 95.1 ms** |
| Violations of the 16.7 ms budget, per run | 2 of ~3,700 | **1 of ~3,550** |
| `CPU Chunk LoadEntities` (the whole entity pass), peak | 44.2 ms | **gone** — split into the two below |
| `CPU Chunk StageEntitiesPass` (budgeted, per frame), peak during a slide | did not exist | **7.4 ms** |
| `CPU Chunk StageRoot` (one root, the budget's atom), peak during a slide | did not exist | **7.4 ms** |
| `CPU Chunk AdmitGameplay`, peak | did not exist | below the profiler's resolution |
| `CPU Frame World PreTick`, peak during a slide | not measured | **34.3 ms** |
| `CPU Frame World Render`, peak during a slide | not measured | **54.7 ms** |

**The gate did not flip, and the reason is now measured rather than assumed.**
Phase 1 predicted the remaining 102 ms was `LoadEntities` (44) plus the
admitted renderers' stamps (17.6) plus unnamed work. The entity half is gone
and the frame only fell by 7 ms, so the prediction was wrong about the split.
New main-loop stage timers (`CPU Frame *`, `Application.cpp`) say where it
goes, and it is one frame made of two stamps:

- **`World::Render` — 54.7 ms.** `RenderSystem::Render` calls
  `VoxelBaker::Bake`, which stamps every renderer that asked for an update.
- **`World::PreTick` — 34.3 ms.** Entity `PreTick` registers the components of
  everything admitted this frame, and `RenderSystem::OnComponentAdded` stamps
  each new renderer (`CPU Occupy added (static)`, 17.2 ms of it).

Both are **phase 5's** subject, exactly as its heading says, and neither is
reachable from a streaming budget: admission can hand the frame sixteen roots
instead of a hundred, but a single renderer's stamp is one indivisible
`VoxelBaker::Occupy`. **The gate's `WILL_FAIL` therefore moves from this phase
to phase 5.** That is a schedule change, not a scope reduction: the number it
gates is the same number, and it is now 1 violation of ~3,550 frames rather
than 7 of ~4,270.

**Worth generalising: nothing in this tree said where a frame's time went.**
Every named cost was inside one system or one function, so a 94 ms frame with
no streaming timer above 8 ms in it was unattributable. Six `StageTimer` scopes
in `Application::Run` — PreTick, Tick, FixedTick, PostTick, Render, Present,
plus OnUpdate/OnDraw/JobCallbacks — answered it in one run, and they are kept.
They cost one branch per stage when the profiler is off.

##### The contract: option 1, and the number that decided it

**Every non-static root of the whole incoming window is admitted in one
frame.** Counted over all 17 shipped `.wld` files first, as the plan asks: the
worst three-chunk incoming slide is **430 non-static roots**
(`Walking_Through_The_Maru_Beat3`), the worst 3×3 initial window is 447, and
the median level is under 30. Admission is a pointer push per node —
`World::AddEntity` plus a recursive walk — so 430 of them is microseconds; what
costs is the stamp each renderer sets off on the *next* `PreTick`, and that is
bounded by phase 5, not by splitting this.

Splitting it would buy nothing measurable and would re-open the whole of ledger
E3: `GameManager::ResolvePlayers`, `SpawnerManager::RefreshSpawnerLinks` polled
from four places, and the `Weapon::Fire` guard exist on the experiment branch
because gameplay could see half of an incoming window's gameplay roots. None of
them land. `StreamingCounters::MaxGameplayRootsPerAdmission` is in `perf.txt`
and is the one baseline entry where a **drop** is the regression.

##### Loading a chunk is now two questions, and only the second is observable

`Chunk::StageEntityBatch` constructs roots **detached** — the entity and its
components exist, the world's lists are untouched, no system has seen it, no
`Awake` has run, nothing has been stamped. So it can run *before the commit*,
opportunistically, from `US_RENDERING` while the chunk worker owns the back
buffer; on a settled machine most of a slide's deserialization is paid for by
the time the window publishes. `Tests/Streaming/EntityStreamingChecks.cpp`
asserts that no incoming root reaches the world before the commit counter
moves.

Two decisions worth not undoing:

- **A root is the unit, and the experiment's shallow-stack deserializer is
  deliberately not re-derived** — the same call phase 2 made for the unload,
  for the same measured reason (68 nodes is the largest root hierarchy in any
  shipped level). A resumable per-node stack holds a half-built entity tree and
  a raw `Entity*` across frames, which is ledger E1 wearing a different hat.
- **`EntityAdmission` is a count, not a clock, and it is the only budget here
  that has to be.** Every other budgeted loop pays for its work as it does it.
  Admission pays *nothing* now and everything next `PreTick`, so a
  wall-clock budget would admit a thousand roots in twenty microseconds and
  hand the next frame the stall this phase exists to remove.

The staged roots are the one place this phase holds raw `Entity*`s across a
frame boundary, and R4's required defense is **ownership**: a staged root is
not in the world, so nothing else knows it exists and nothing else can destroy
it. The two exits are admission (which nulls the slot) and
`ResetStreamingState`/`~Chunk` (which delete them).

##### M3 and K6: a link is an identity, not a pointer

`World::WorldConnectionInformation` held a raw `rttr::instance` of the entity or
component being deserialized and dereferenced it a frame or more later, to ask
two questions it can answer without it: *which entity owns you* and *what type
are you*. It records `(source entity id, source component type)` now.

That is not only a lifetime fix. `ResolveWorldLinks` resolved once per
`PreTick` and cleared the list unconditionally, so a cross-chunk reference
whose target had not been admitted yet became a **permanent null** — the exact
bug class the experiment answered per manager. Links are retried until both
ends are in the world, bounded by `World::k_uiMaxWorldLinkRetries` (240
PreTicks, against a worst case of roughly sixty), then abandoned and counted.
And it silently fixed a case nobody had named: where a static root is
deserialized again and discarded because the world already holds it, the link
now lands on the **live** entity instead of on the copy about to be deleted.

##### R1 landed and is inert, on purpose

`ChunkSystem::IsInitialWindowReady()` and `World::IsGameplayHeld()`:
`World::Tick`/`FixedTick` advance the chunk system and the renderer and nothing
else until the first window is resident. `ChunkSystem::Start` still builds that
window synchronously, so it is true the moment `Start` returns — the gate
exists so that **phase 4** can make the initial window stream through the same
machine and put the loading screen over the wait, rather than discovering at
that point that gameplay has been running against a world that is not there.
That single rule is what replaces `Bootstrap` metadata,
`Player::SetPersistent` in the constructor and the `PhysicsBody` freeze
(E7, E8).

It is checked rather than asserted-by-comment: `StreamingHarness` can defer
`World::Initialize`, and `StreamingProbeEntity` (a reflected entity that exists
only in the test binary) counts its own ticks. Zero while held, non-zero after.
**Phase 4 owns making the hold non-trivial**, and its acceptance should assert
a held *duration*, not just the flag.

##### Two things the fixture had to grow before any of this was checkable

R7 again — "a property only a human can currently check is a property the phase
must first make checkable". Nothing in the engine has a reflected `Entity*`
property (every one is in game code, which the suite does not link) and nothing
counts its own ticks. `Tests/Harness/StreamingProbe.{h,cpp}` is both, registered
with RTTR from the test binary, and `Tests/Fixtures/StreamingLinks5x5.wld` puts
one in every chunk pointing at a marker in the next chunk of the same incoming
column — plus one row of orphans pointing at an id no level contains, which is
what the abandonment path needs.

##### And a 44 ms component, which was not the entity system at all

The first measurement after the split said a single **staging root took 44.65
ms**, which no root-granular budget can help. `VOXAGINE_CHUNK_IO_TIMINGS` now
names any component that takes over a millisecond to construct, and it was
`VoxRenderer` — specifically `SetFrame` calling
`ModelMeshStore::EnsureMeshed` on every model a chunk loads, static or not,
while `RenderSystem::Render` submits only non-static renderers to the model
pass. That is keep-list item **K10** and it is landed here rather than in phase
4, because a budgeted loop whose atom costs 44 ms is not a bounded loop. See
`Docs/DYNAMIC_MODELS_PLAN.md`'s phase 1 results for the other side of it. The
other half of K10 — `PrepareModelMeshes` during world load — is still unbuilt
and still phase 4's, and is only needed if a dynamic model's *first draw* ever
hitches.

### Phase 4 — Streamed initial window and far field

*Renamed. This phase was written as "Streamed world switch behind the loading
screen" and is scoped to what it landed: the two costs inside
`World::Initialize`. The world-manager flow it also described is **phase 8** -
it is a session's work with its own acceptance, and leaving it as this phase's
remainder is how a plan grows a permanent "PARTLY" nobody can pick up.*

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

#### Phase 4 notes — what landed, and what it measured

All Release, headless, quiet machine (RTX 4070 SUPER).

| What | Before | After |
|---|---|---|
| `[world-switch] initialize` (`--map` Beat2) | **876 ms** | **316.6 ms** |
| ↳ `ChunkSystem::Start` | 453.4 ms | **49.4 ms** (all of it model pinning) |
| ↳ ↳ `BuildFarField` | 447.2 ms | **0 ms here** — 1,440 ms of wall clock in 4 ms slices, at 60 fps |
| ↳ `RenderSystem::Start` | 256.3 ms | 258.2 ms — **untouched, and the whole of what is left** |
| First `World::PreTick` after a load | 557 ms | 251 ms |
| Peak frame across a Beat2 window transition (three runs) | 94.5 / 94.8 / 95.1 ms | 94.9 / 94.5 / 95.6 ms — unmoved, as expected: it is phase 5's two stamps |

**The initial window is not special any more, and that is the keystone.**
`ChunkSystem::Start` pushed nine chunks through a synchronous load inside
`World::Initialize` — decode, deserialize every root, stamp every static
renderer — off the frame loop, where a loading screen cannot animate and the
compositor gets no ping. It queues an ordinary `ChunkUpdateGroup` now, marked
`IsInitial()`, and gameplay is held (R1) until that group has *admitted its
roots* — not until it commits, because the window being published is not the
same as the world being in it.

Three things follow that are easy to undo:

- **The chunk the camera starts in is already resident**, because
  `JsonSerializer::DeserializeWorld` loads `CameraChunkIndex` while it is still
  building the world. So the group sees it as a `T_MOVE` and the player never
  stands on nothing. A change that stops doing that turns the hold into a
  visible black frame.
- **R1 stopped being inert**, which is what phase 3 said this phase owed.
  `Streaming/GameplayIsHeldUntilTheInitialWindowIsResident` now steps frame by
  frame through the hold and asserts a fixture entity's tick count stays at
  zero for every one of them. It also asserts that the world is *not* ready the
  instant `Initialize` returns — so a future change that quietly restores the
  synchronous load fails a check rather than silently re-inflating
  `initialize`.
- **The harness settles the initial window in its constructor and resets the
  counters**, so every check still gets "you are handed a world whose first
  window is resident" and every count still means the check's own slide. Four
  checks that encoded the synchronous behaviour were rewritten rather than
  patched; one of them, `TheInitialWindowIsResidentOnceItHasStreamed`, now sees
  **nine** chunks' ground in the window instead of one, because the initial
  window is published by the same render job as every other.

**The far field builds while the game runs.** `FarFieldBaker::Begin/Continue/
Cancel` walks the level's static roots under `StreamingBudgets::FarFieldBuild`
(4 ms), driven from `ChunkSystem::Tick`. What makes a partial volume safe is
not the budget but the ordering: **the volume reports itself unbuilt for the
whole build**, `RenderContext::GetFarFieldGridSize` returns zero while it does,
and every shader reads that as "no far field" — so a half-filled volume is
never sampled, and neither is the *previous level's*, which is the failure this
exists to prevent. The model pins have a stated owner: taken in `Begin`,
released on completion **and** on `Cancel`, and `World::Unload` cancels — a
build abandoned half-way must not leave the level's whole model set pinned for
the session.

The horizon therefore arrives about 1.4 s after the level does. That is a
judgement call and **Joey has not seen it**; if it reads badly the answer is a
larger `FarFieldBuild` budget, which is one constant.

##### What phase 4 deliberately did not do

The far-field `IncrementalBuild` harness scenarios the testability note asks
for (cancel mid-build releases pins, complete build transfers them, zero size
reported while building) are **not written**: the harness has no render context
and so no `FarFieldVolume`, and giving it one is a seam of its own. The
cancel-releases-pins path is exercised in the game by `World::Unload` on every
world switch, which is not the same thing and should not be counted as if it
were. Phase 8 inherits it.

**Joey has not seen this on screen, and there is one thing to look at.** Phase 4
turned an 876 ms freeze into a few hundred milliseconds of the world
*materialising* with gameplay held - and phase 8's loading screen, which was
meant to cover exactly that, is not landed. Responsive beats frozen and the
camera's own chunk is resident from the start, so this should read better than
what it replaces; but it is the first thing anybody sees on launching a level,
and the horizon arriving ~1.4 s later is a second judgement of the same kind.

### Phase 5 — Bounded stamping, per renderer

*Renamed and rescoped: bounding the stamp to whole renderers is this phase,
splitting one renderer's stamp is **phase 9**. The split was built here and
reverted for losing geometry, which is exactly why it needs an acceptance
criterion of its own rather than a bullet in someone else's.*

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
`VOXAGINE_PYRAMID_AUDIT` clean during heavy streaming + destruction;
the hitch gate's peak driven down and recorded - **the flip itself is phase 9's**,
because it turned out to need the split below one renderer and phase 5 measured
exactly that; visible pop-in of freshly admitted chunks judged by Joey
on screen (budget constants are the tuning knob and live in
`StreamingBudgets.h`); numbers recorded.

#### Phase 5 notes — bounded per renderer, and why one is not enough

| What | Phase 4 | Phase 5 |
|---|---|---|
| **Peak frame across a Beat2 window transition** (three runs) | 94.9 / 94.5 / 95.6 ms | **27.8 / 28.0 / 26.9 ms** |
| Violations of the 16.7 ms budget, per run | 1 of ~3,500 | 4 of ~3,400 — all between 16.9 and 28 ms |
| `CPU Frame World PreTick` peak during a slide | 34.3 ms | below the profiler's noise |
| `CPU Frame World Render` peak during a slide | 54.7 ms | ~2 ms + one renderer |

Two stamps became one budgeted loop. `RenderSystem::OnComponentAdded` used to
write a renderer's voxels *inline as its component registered* — inside
`World::PreTick`, once per admitted renderer, with no way to stop; it asks for
a stamp now, under the identical four-clause condition (`m_bStarted`,
`!IsChunkInstanceLoaded()` for M7, `IsEnabled()`, `IsStatic()`), and
`VoxelBaker::Bake` does it under `StreamingBudgets::VoxelBaking`.

**Resumption needs no cursor and holds no pointer, and that is the design.** A
renderer the budget did not reach still has its `Updated`/`UpdateRequested`
flags, so the next frame's scan finds it. The scan is a handful of comparisons
per renderer and was already happening every frame; the budget is consulted at
the one point where comparisons become voxel writes. Nothing survives a frame
boundary, so there is no ledger-E1 shape here to defend.

**One defect this shape introduces, fixed, and worth knowing.** A force means
"re-examine every renderer", and a budgeted pass stops part-way — so clearing
`m_bForcedUpdate` afterwards leaves every renderer past the stopping point
never examined *at all*. It has no flag of its own; the force was its trigger.
On Beat2 that left a third of the level's voxels unwritten, and the only symptom
is missing geometry, which reads as content. `m_bForcedUpdate` is now cleared
only when the pass reached the end.

**R1 grew a second half here, and the game told us so.** The initial group
admitting its roots puts the level's entities in the world; their models are
stamped over the frames after that. The first run of this phase had the player
start walking while the river bed was still being stamped — they fell through
the hole where it was going to be and the run ended on the game-over screen.
`ChunkSystem::IsInitialWindowReady` now also waits on
`RenderSystem::HasPendingVoxelBakes`, and `IsStreaming` folds in both that and
an outstanding far-field build.

##### The resumable Occupy: built, measured, reverted

The remaining violations are each **one renderer**: `RiverBedStraight10` and its
siblings stamp **140,640 voxels in 22 ms**, so a frame that starts one is a
22 ms frame whatever the per-renderer budget says
(`VOXAGINE_CHUNK_IO_TIMINGS` names any single stamp over 5 ms). Splitting below
a renderer is the plan's own answer, and it was built:
`ForEachStampedVoxelRange` with the cursor and the duplicate-suppression
position carried on `BakeData`, `OccupyInProgress`, the skip tests taught not to
skip a half-stamped renderer, `Clear` resetting the cursor.

**It made the gate pass — peak 9.5 / 9.5 / 10.0 ms over ~13,300 frames, zero
violations — and it was reverted anyway**, because the level it was drawing was
not the whole level. `VOXAGINE_VOXEL_AUDIT` reports **4,104,267 active voxels**
for a settled Beat2 with the unbounded stamp and with the per-renderer budget
alone; with the sliced stamp at the shipping 8,192 samples it reported
**3,520,944**, and at 2,048 it reported 2,836,669. Around 580 k voxels short,
scaling with how finely it was sliced.

That is the failure this tree keeps meeting from a new direction, and it is why
the number above is the acceptance rather than the frame time: **missing
geometry is invisible in any single frame and reads as content**. A green hitch
gate measured against a level that is 14% unwritten is worse than a red one.

**If you rebuild it, the oracle is that occupancy number**, and it has to be
part of the change rather than a check afterwards: run a settled Beat2 under
`VOXAGINE_VOXEL_AUDIT` with slicing on and off and require the counts to be
equal, at several slice sizes, because the deficit *scaled with slice size* —
which is the shape of state that is not being carried across a slice boundary.
The cause was not found; the suspects in order are the `lastPosition`
duplicate-suppression state, the interaction with `Clear`'s owner-arbitration
when a partial stamp is cleared and restarted, and the skip tests reading
`Generation`/`StampKey` that Occupy only writes on completion. A harness check
would settle it far faster than the game can — `ForEachStampedVoxelRange` needs
only a `VoxFrame`, and giving the suite one (`Tests/Harness/VoxModelFile`, which
CLAUDE.md records as written but uncommitted for the bamboo work) is the
cheapest way in.

##### What phase 5 did not do

- **`VOXAGINE_COVERAGE_AUDIT` during streaming.** Not run. The per-renderer
  bound cannot produce voxels-without-proxy - a renderer is cleared and
  re-stamped inside one `Bake` call, exactly as before - but the phase's own
  acceptance names the audit and it was not run. **Phase 9 must run it**, where
  the argument above stops holding.
- **E4's `WaitForVoxelReaders` measurement** in Release. Untouched; phase 9.
- **The stamp arena (E10's second half).** Still gated on measurement, still
  unmeasured; `AcquireStampBuffer`'s `new uint32_t[]` per re-stamp has not been
  profiled. Phase 9.
- **Joey on screen** for pop-in of freshly admitted chunks, which is what the
  budget constants are tuned against.

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

### Phase 7 — The save guard

*Renamed. Everything in the original phase 7 that needs the editor open and a
human watching is **phase 10**; what is left here is the one guard that is
checkable headless, and it is the one that prevents data loss.*

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

#### Phase 6 — closed as not taken, with the numbers

The gate on this phase was "only if phase 5's measurements show per-renderer
proxy submission or voxel-pass overdraw is a real remaining cost". They show
the opposite, and clearly:

- Every frame that still breaks the 16.7 ms budget is one `VoxelBaker::Occupy`
  of a single 140,640-voxel model, named by
  `VOXAGINE_CHUNK_IO_TIMINGS` (`[bake] 'RiverBedStraight10' stamped 140640
  voxels in 22.50 ms`). It is CPU, it is one renderer, and it is phase 5's.
- The GPU passes during a slide are Voxel **0.24 ms**, Sun Shadow **0.34 ms**,
  Pyramid Upload **0.82 ms** — three orders of magnitude below the frames that
  break the budget. There is no marcher cost to attack.

So E5 (`MARCH_STEP_BUDGET` 16384 → 1024) and E6 (occupied-32³-cell proxies)
stay **open and unmeasured**, which is the honest state: the experiment bundled
them into streaming without evidence, and re-deriving them on speculation would
repeat exactly that. If the voxel pass ever becomes the cost, this phase is
here with its method intact.

#### Phase 7 notes — the guard, and why it is a data-loss guard

**A save taken mid-stream is refused.** `JsonSerializer::SerializeWorld` now
returns false with a log line when the world's `ChunkSystem::IsStreaming()`.
This is a data-loss class rather than tidiness: `ChunkifyWorld` distributes the
**live** entities into the chunk grid, and mid-transition the live set is not
the world — an incoming chunk's roots may be constructed but not admitted, so
they are in no list it walks, and an outgoing chunk's may be half
serialized-and-destroyed. The result is written over the only copy of the
level. The editor's five-second autosave is the caller that matters, and it
simply comes back on the next interval.
`Streaming/SavingAWorldMidStreamIsRefusedRatherThanTruncated` drives a
single-stepped transition and asserts the refusal.

**What it does not prove.** The refusal makes the truncation case impossible;
it does not prove "a saved world is diff-identical to a save taken at rest".
That, the editor session and the `Validate(bBack)` split are phase 10.

### Phase 8 — World switch behind the loading screen

*Why now: phase 4 removed both costs inside `World::Initialize` and phase 5
bounded the stamp, so a world can now be brought up over frames rather than in
one - which is the precondition this always had. It is the last user-visible
stall class (M4) and the only phase left that a player would name.*

Re-derive K5: `WorldManager::LoadWorldAfterStreaming` + `UpdateStreamingWorld`
(target world initialized hidden; only its `ChunkSystem` + `RenderSystem`
advance - `PreTick` for admission, chunk `FixedTick`/`Tick`, budgeted `Render`
slice, `PrepareModelMeshes` (K10); activation as one deferred transaction;
deferred audio autoplay (`AudioSystem::SetAutoPlayDeferred`/
`ActivateDeferredAutoPlay`); fade preserved; `DiscardActiveWorldSprites` before
texture release - the freed-bindless-ID crash class),
`World::Unload(bool releaseRenderContext)` and
`RenderSystem::BeginWorldUnload` (skip the per-renderer clears and the mapped
clears the incoming build overwrites anyway), and `WorldSwitch` passing
`bWaitForInitialStreaming`.

Three things this phase owns that are *not* in K5:

- **`RenderSystem::Start`'s 258 ms**, which is `ResizeWorldBuffer` allocating
  the two host-visible window buffers. It is the entire remainder of
  `[world-switch] initialize` and phase 4 did not touch it. Reusing the
  allocation across worlds of the same window size is the obvious move and has
  not been costed.
- **The `join` ui-script token.** Phase 0 recorded that joining a player on the
  main menu is bound to `IK_GAMEPADOPTION`/`IK_MOUSEBUTTONLEFT` only and
  `--ui-script` is keyboard-only, so **menu → level is not scriptable
  headlessly** and this phase's headline acceptance cannot be met without it.
  Do it first, not last.
- **The far-field `IncrementalBuild` harness scenarios** phase 4 inherited and
  did not write: cancel mid-build releases the model pins (assert by refcount),
  a completed build transfers them, zero size is reported while building. The
  harness has no `FarFieldVolume`, so this needs a seam of its own - the same
  shape as `IVoxelWindow`, and worth the same care about it being a *seam* and
  not an abstraction layer.

**E9 discipline:** the sprite-only-world handling in `Present` must be
re-derived minimally on top of master's bindless-packing `Present`, as one
clearly-commented block, not scattered predicates - and it must distinguish
"world with no scene submissions this frame" from "sprite-only world" by an
explicit flag set by the world manager, not by `m_AABBList.empty()` inference.
Wait for VDirect only where a buffer is genuinely replaced.

**Editor note:** the editor swaps worlds through `LoadWorld`, not the loading
screen; it benefits from `BeginWorldUnload` automatically. Verify open-world in
the editor no longer multi-second-stalls.

**Testability note:** `WorldManager`'s streaming logic must be written so its
decisions are checkable without a render context - the pending-world state
machine (single pending world, second-request rejection, activation only after
`IsStreaming()` and mesh sync report done, `ClearWorlds` discarding a pending
world) goes through methods the checks suite can drive with a stub "readiness"
provider. What genuinely needs the real renderer (fade, sprite lifetime, VDirect
gating) is verified by the headless game runs below and stays out of the
harness.

**Acceptance:** the WorldManager and far-field checks above green (ASan);
menu → level via loading screen, headless ui-script: loading artwork animates
for the whole load (no 0-fps stretch - the `[fps]` lines prove it), activation
transaction < one frame, audio starts at activation, `[world-switch]` splits
recorded and `initialize` reported per system; switching between two levels
repeatedly leaks nothing (RSS plateau recorded, model-pin refcounts return to
rest); editor open-world timed before/after; Joey watches the flow once on
screen - fade, music timing, first-frame HUD, **and whether the materialising
world phase 4 left uncovered now reads as a load rather than as a glitch**.

#### Phase 8 notes — what landed, and what it measured

All Release, headless, quiet machine (RTX 4070 SUPER), through the *real* menu
flow: `--map Main_Menu` and a `--ui-script` of three `confirm`s, which reaches
Main_Menu → Level_Selection_Menu → Loadingscreen → `Fishing_Village_Beat1`.

| What | Before | After |
|---|---|---|
| `[world-switch] initialize` for the level | **293.8 – 307.9 ms, in one visible frame** | **301.6 / 322.9 / 342.2 ms**, behind the loading screen |
| Arriving | — | **1666 / 1684 / 1717 ms**, over **2518 – 3716 drawn frames** |
| Activation transaction | — | **16.8 / 18.0 / 19.0 ms** |

**`initialize` did not get cheaper and that is the point** - it moved. It is the
same ~300 ms of work (258 of which is `RenderSystem::Start`'s
`ResizeWorldBuffer`, still phase 8's unlanded item), paid on a frame where a
loading screen is animating rather than on the one frame the player was waiting
through.

**Count frames *and* wall clock, or the number lies in the direction that
matters.** The activation line reports both, and the first reading of the frame
count alone said this phase had made loading seven times worse: 1400 frames
divided by the 81 fps the `[fps]` line was reporting is seventeen seconds. It
is not - the loading screen is a sprite-only world with a hidden world behind
it, so its frames cost almost nothing and it runs at two to three thousand of
them a second. **The frame count is the evidence that the artwork animated; it
is not a clock.** Two runs disagreeing by 4x (441 against 1687 frames) were
this machine compiling in the background, which is the other half of the same
lesson.

**The defect this phase introduced, found by Joey on screen: the level was
black.** `RenderContext::ResizeWorldBuffer()` took no world - it asked the
world manager for the **top** world, which is right exactly while the world
being sized is the one on screen. It never was here: a level is initialized
*behind* the loading screen, so at `RenderSystem::Start` the top world was the
menu, and the resident voxel window was sized from a sprite-only menu world's
grid while the level streamed into it. It takes a `World*` now, and both
callers pass their own.

Three things about how it was found, because the first two nearly went the
wrong way:

- **`VOXAGINE_VOXEL_AUDIT` split the problem in one run.** It reported
  **2,706,535 active voxels** on the black frame - the level was entirely
  there on the CPU - which took "the world did not load" off the table and left
  only rendering.
- **The next probe said the rendering state was identical**: fader 1.000, 464
  proxies submitted, the same camera position and offset as a direct `--map`
  load of the same level. Everything a frame is drawn *from* matched; what did
  not match was what the buffer had been *sized* as, which no probe was
  looking at.
- **Headless capture plus two scalars is the whole test.** Mean luminance
  **1.73 / non-black 0.026** against a direct load's **96.94 / 0.995**, and
  **96.11 / 0.995** after the fix. No screenshot needed looking at.

**Readiness is `IsInitialWindowReady() && !IsStreaming()`, and the second half
turned out to be free.** The obvious worry is that waiting for the far field
doubles the load, so both were measured: **832 / 848 / 882 ms** with it against
**867 / 914 ms** without. Identical, because `FarFieldBaker` builds in 4 ms
slices from `ChunkSystem::Tick` *alongside* the window rather than after it. So
the strict test costs nothing and **closes phase 4's open judgement call** - the
horizon arriving 1.4 s into gameplay is now in front of the loading screen
instead. (Both of those were taken *before* the black-frame fix below, so they
are on the low side in absolute terms; the comparison between them is the
finding and it is unaffected - the same work in both.)

**The pending world is the one `World` that exists and is not in
`GetWorlds()`.** Same shape as phase 3's staged root, same defense: it leaves by
activation, by refusal, or with `ClearWorlds`, and nothing else knows it is
there. A second `LoadWorldAfterStreaming` is **refused**, not queued and not
allowed to replace the world already being prepared - this is not a cancellation
API, and cancelling would strand a half-streamed world's chunk jobs and staged
roots.

**`IWorldStreamingReadiness` (`Core/ECS/WorldStreamingReadiness.h`) is a test
seam, not an abstraction layer** - the same warning `IVoxelWindow` carries, and
for the same reason. `Streaming/WorldStreamingChecks.cpp` drives the whole state
machine on `PreLoad(false)` worlds with a stub that arrives when the check says
so: no renderer, no chunks, no clock. Four checks, and the one worth naming is
*a pending world is not advanced once its activation is queued* - there is a
real frame of gap between deciding and swapping, and a slice run across it
advances a world that is about to be ticked properly instead.

**A world's music belongs to the world you are looking at.**
`AudioSystem::SetAutoPlayDeferred` gates **both** autoplay paths, and the second
is the one that matters: chunk streaming admits roots for as long as the world
is hidden, so `OnComponentAdded` - not `Start` - is where a hidden level's
music would actually have come out.

**The loading screen leaving must not undo what the world behind it built.**
`World::Unload(bool bReleaseSharedRenderState)` is that distinction and it has
exactly one caller passing false. `RenderSystem::BeginWorldUnload` is the other
half and is a win on every path: every renderer's stamp is *forgotten* rather
than cleared (the same distinction a chunk unload makes) and the renderer list
is emptied once instead of erased from the middle per component.

##### What phase 8 did not do

Three of its four listed items are **not landed**, and none of them was
attempted:

- **`RenderSystem::Start`'s 258 ms** (`ResizeWorldBuffer` allocating the two
  host-visible window buffers) is untouched and still the whole remainder of
  `initialize`. It is now paid behind the loading screen rather than in a
  visible frame, which is why it stopped being the thing to fix first - but it
  is still 258 ms of the ~900.
- **The far-field `IncrementalBuild` harness scenarios** inherited from phase 4
  are still not written. Still needs a seam of its own.
- **E9's explicit sprite-only-world flag in `Present`** is not landed.
  `DiscardActiveWorldSprites` *is*, on every world change rather than only this
  one - that is the crash class (a frame with no fixed tick drawing freed
  bindless IDs), and it is the half that was actually reachable. The
  "distinguish no-submissions-this-frame from sprite-only-world by an explicit
  flag" half was not needed by anything observed here and is unlanded.

Also not done: the two-level repeat leak run (RSS plateau, model-pin refcounts)
and the editor open-world timing.

##### Confirmed on screen by Joey

The whole menu → level flow, after the black-frame fix: fade, music timing at
activation and the first frame of the level all read correctly. That was the
phase's last acceptance item and the one nothing headless can answer - the
black frame itself is the argument, since every automated gate in this plan was
green while the level was rendering nothing.

### Phase 9 — Splitting one renderer's stamp

*Why now: it is the only thing between the hitch gate and passing, and phase 5
established that with a name and a number. Deliberately last of the performance
phases, because it is the one that can lose geometry.*

**Read phase 5's notes before writing a line.** This was built - cursor and
duplicate-suppression position on `BakeData`, `ForEachStampedVoxelRange`,
`OccupyInProgress`, the skip tests taught not to skip a half-stamped renderer,
`Clear` resetting the cursor - and it took the worst transition frame from
27.9 ms to **9.5 ms with zero violations**. It was reverted because the level it
was drawing was **580 k voxels short**, and the deficit scaled with slice size.

Re-derive it with the oracle built in from the first commit, not bolted on:

- **The acceptance is an occupancy count, not a frame time.**
  `VOXAGINE_VOXEL_AUDIT` on a settled `Fishing_Village_Beat2` reports
  **4,104,267 active voxels** with the unbounded stamp and with phase 5's
  per-renderer bound. A sliced stamp must report exactly that, **at several
  slice sizes**, because the deficit scaling with slice size is the signature
  of state not carried across a slice boundary.
- **Do it in the harness first.** `ForEachStampedVoxelRange` needs only a
  `VoxFrame`, and the check the plan always wanted - resuming at *every* cursor
  value produces the identical voxel set as one unbounded walk, swept at
  budget = 1 - is a unit test, not a game run.
  `Tests/Harness/VoxModelFile.{h,cpp}` is recorded in CLAUDE.md as written but
  uncommitted for the bamboo work; committing it is the cheapest way in.
- **The suspects, in order**, none confirmed: the `lastPosition`
  duplicate-suppression state; the interaction with `Clear`'s owner arbitration
  when a partial stamp is cleared and restarted; and the skip tests reading
  `Generation`/`StampKey`, which `Occupy` only writes on completion.

Also this phase's, all inherited from phase 5 and all small: **E4's
`WaitForVoxelReaders` measurement** in Release (if per-frame stamping into the
front buffer really does stall against VDirect reads, prefer batching bake
writes after the frame's fence over a blocking wait); and **the stamp arena**
(E10's second half), still gated on `AcquireStampBuffer` actually costing frames
- per-renderer buffer reuse via a high-water capacity in `BakeData` is the
simple version and probably the whole of it.

**Acceptance:** the harness resume-equivalence sweep green at budget = 1 under
ASan; **the occupancy count identical to 4,104,267 at three slice sizes**;
`VOXAGINE_COVERAGE_AUDIT` run *during* streaming with above-ground uncovered
bricks at zero - phase 5 could argue voxels-without-proxy was impossible, and
that argument stops holding here; `VOXAGINE_SYNC_AUDIT` and
`VOXAGINE_PYRAMID_AUDIT` clean; **`gpu_chunk_streaming_frame_budget` passes in
Release and `WILL_FAIL` comes off**; Joey judges pop-in on screen.

#### Phase 9 notes — the walk was never the problem

**The split is back, and this time the equivalence is a check rather than a
claim.** `ForEachStampedVoxelRange` carries the loop counters *and* the
duplicate-suppression position on a `VoxelStampCursor`, and
`ForEachStampedVoxel` is now that function with no budget - one loop, not two
that have to be kept in step.

The first thing that fell out of building it that way: **the walk was not where
the 580 k voxels went.** `Tests/Rendering/VoxelStampChecks.cpp` sweeps the
sliced walk against the unbounded one at budgets 1, 2, 3, 7, 64, 97, 1000 and
8192, over six poses (quantized rotation, scale above one, negative scale,
off-lattice origin, with and without an override colour) and two real models -
the three-stalk bamboo and the 140,640-voxel `Riverbed` that phase 5 named. Every
one is identical, position for position and colour for colour, first time.
`Tests/Harness/VoxModelFile` is what made that possible: a `.vox` read without
the resource stack, sorted exactly the way `VoxModel::SortFrameVoxels` sorts -
which matters, because a sorted model is the input that makes the duplicate
suppression fire at all.

So the suspects that remained were the two *interactions*, and one of them is
almost certainly what happened:

- **`Bake` clears before it stamps, and a resumed stamp must not be cleared
  again.** `Clear` erases by recorded position, and failing that by owner slot
  over the renderer's whole box - either way it reaches the half of the model
  that is already written, and the resumed walk never goes back for it. A
  renderer interrupted *n* times would keep only its last slice. That is a
  deficit that grows as the slices get finer, which is exactly the reported
  shape. `bResuming` now gates the clear, the transform resets and every skip
  test in the pass.
- **The bookkeeping is written at the *start* of a stamp, not the end.**
  `Positions`, `Size`, `Generation`, the stamp key and the stamped box all
  describe what is in the buffer *right now*, including mid-stamp, so a partial
  stamp is an ordinary state: it can be cleared, abandoned or resumed by the
  same code that handles a whole one. `Clear` and `ForgetChunkStamp` reset the
  cursor, so no path can leave a cursor pointing into a stamp whose first half
  has been erased.
- **A partial that the ground moved under is restarted, not continued.** If the
  voxel buffer was rebuilt (`Generation`) or the window slid (`WorldOffset`) or
  the stamp key changed, the second half would land where the first half is not.
  `StreamingCounters::VoxelStampRestarts` counts it; it should stay small, and a
  number that grows with playing time means stamps are being invalidated faster
  than they can finish.

**The oracle, run for real.** A settled `Fishing_Village_Beat2` reports
**4,930,065 active voxels** unsliced and at 8,192, 2,048 and 512 samples - four
slice sizes, two runs each, **all eight the same number to the voxel**. (The
plan's 4,104,267 is a pre-phase-8 figure for the same level; what the acceptance
needs is that the runs agree, and they do.) `VOXAGINE_STAMP_SAMPLES` is what
makes that a sweep rather than four rebuilds - `StreamingBudgets.cpp`, and the
same argument as `--map` replacing an edit to `ProjectSettings.vgps`.

**The trap that number sets, and what was done about it.** Read *six seconds*
into the same run, the sliced counts are 3,431,976 at 8,192 and 2,194,720 at 512
against 4,930,065 unsliced - a deficit of over a million that scales with slice
size, which is precisely the signature the phase notes tell you to accept as
proof of lost geometry. It is not. Bounding a stamp moves work out of the worst
frame and into *more frames*, so a sliced run reaches any given instant with
less of the level written; wait for it to settle and every voxel is there. It is
not possible to say from here whether that is what the first attempt measured,
because that code is gone - but the measurement it was reverted on cannot
distinguish the two, and this one could not either until it was taken later.
`AuditVoxelRepresentation` now says **`- STILL STREAMING, this count is not
settled`** on the line itself when `ChunkSystem::IsStreaming()`, so the reading
cannot be misread again.

**What it costs, which is the honest half.** The level takes longer to become
playable, because each frame does less. `[world] gameplay held`, two runs each
(`ChunkSystem` prints it now - CLAUDE.md described that line as existing and it
did not):

| slice | held ticks | active voxels |
|---|---|---|
| unsliced | 728 / 779 | 4,930,065 |
| 8,192 (shipping) | 910 / 1,071 | 4,930,065 |
| 2,048 | 1,422 / 1,480 | 4,930,065 |
| 512 | 3,041 / 4,567 | 4,930,065 |

The knob for that is `StreamingBudgets::VoxelBaking`'s 2 ms, **not** the sample
count: unsliced, one renderer overshoots that budget by 20 ms, so the level is
written sooner precisely *because* the frames are bad. Whether 2 ms is still the
right allowance while the initial window is being built - where the only thing
competing for the frame is a loading screen animating - is a judgement for
phase 10 and for Joey on screen. **Do not lower the sample budget to make
streaming smoother**; below 8,192 the hold grows faster than the frame time
falls, and at 512 it is four times the unsliced wait.

##### The gate, and the gate that was already dead

`gpu_chunk_streaming_frame_budget` **passes**: peak **9.82 / 10.15 / 10.31 ms**
against the 16.67 ms ceiling over 13,159–13,421 frames, three runs. That is the
whole arc of this plan - 150.2 (phase 0) → 102.4 (1) → 94.2 (3) → 27.9 (5) →
**10.3 ms** - and the `WILL_FAIL` is off. The three streaming audits are clean
over a scripted boundary crossing: **0** uncovered bricks above the ground row
(18,313 in the deliberately-uncovered y = 0 row), **0** of 10.8 M pyramid cells
disagreeing, **0** of 75.5 M voxels disagreeing between the CPU chunk and the
mapping.

**That run has no combat in it, and this phase's acceptance asked for some.**
The `--ui-script` `fire` token presses P and P never reaches `Weapon::Fire` -
measured by instrumenting its first line, which printed nothing across 2,500
frames with `join` and four `fire`s. So the audits above cover streaming and
movement across a window slide, not destruction during one. Nothing headless in
this tree destroys a voxel today, and the verification reference has been
claiming otherwise for four phases; making `fire` work is small and is owed.

The CPU suite is green in Release and under **ASan + UBSan** in Debug - 118
checks, 31 scenarios x 5 invariants, the perf counters unmoved (879 s for the
three under the sanitizers). Nothing in the perf baseline shifted: the harness
does not run `VoxelBaker`, so the two new counters
(`VoxelStampSlices`/`VoxelStampRestarts`) are reported by the game and gated by
nothing yet.

`gpu_world_occupancy` needed a real fix rather than a new number, and it is the
same lesson twice: it audited on a **wall clock**, so it read 1,400,089 of
Beat1's 2,706,535 and failed a level that was entirely correct and still being
written. The clock is a floor now and the audit fires at the first frame after
it at which `ChunkSystem::IsStreaming()` is false. It passes, at the same
2,706,535 as before.

**`gpu_destruction_sync_stress` is broken and it is not this phase's doing.**
It fails 0/256 bursts, 0/6 chunk switches, "application exited before the
fixture completed" - **identically on master `c5595d3`**, built and run to check.
Its `--frames 6000` is a main-loop iteration count and the fixture is
`--uncapped`, so 6,000 frames go by in 1.7 s while R1 holds gameplay for ten
seconds or more; it has therefore measured nothing since phase 4 made the
initial window stream. Raising it to 400,000 (the streaming gate's own trick -
let `TIMEOUT` be the bound) gets the fixture *running* but it does not complete
within 300 s, so the frame count is not the whole of it. **Deliberately not
fixed here**: it is a destruction gate, this is a streaming phase, and a fix
that has not been shown to make it *pass* would only move the failure. Phase 10
should own it, and until then `-L gpu` is two green and one red rather than
three green.

#### What the play session after phase 9 found

Phase 9's own acceptance is met and its numbers are above. What follows was
found by Joey playing the branch, and is recorded here because most of it is
*streaming* work that no phase had named - not because it belongs to phase 9.
Nine of the ten defects predate this branch; the tenth is phase 9's own.

| what | where | mine? |
|---|---|---|
| Solo throw segfaults on the partner | `Weapon::Fire`, `Player::Catch` | no, 2023 |
| Players never paired - edge-triggered cross-link | `GameManager::ResolvePlayers` | no |
| Zero-extent collider spins the sweep forever | `PhysicsSystem` | no |
| Raycast loop cannot terminate (`dist == 0.f`) | `PhysicsSystem` | no |
| Grid lock counter is a plain `int` across threads | `PathfindingChunkGrid` | no |
| Job captures a vector element by reference | `PathfindingChunkGrid` | no |
| Concurrent `operator[]` on shared node maps | `ContinuumCrowdsGroup` | no |
| Spawner links abandoned before their chunk arrives | `JsonSerializer` | no |
| Container links never nulled on destroy | `JsonSerializer` | no |
| Uninitialised `glm::quat` in four stamp structs | `VoxelStamp.h`, `VoxRenderer.h` | half |

**The theme is one sentence: a raw pointer to streamed content, held across a
frame, with nothing to tell it the target died.** CLAUDE.md has documented that
as a class since M8 and each instance is still being found by crashing. The
answer is not more guards - two of mine crashed *inside* the guard - but a
handle type (id plus generation, resolved on use) that game code uses instead of
`Entity*`/`Component*` for anything it keeps. `PlayerSlot` is that, for one
case. It was bigger than a phase note, so it is **phase 13** now.

**Open, and not this branch's**: the CPU voxels and the mapping disagree during
play - 540 voxels present only on the CPU (invisible but solid) and 4,426 only
on the GPU (visible and not there), which is a destroyed pillar coming back
visually while collision stays correct. **Not phase 9**: master `c5595d3` and
this branch both report 0 disagreements on the same headless run, so it needs
input, destruction or a world switch to appear. The map-load reading of
`149800 of 8388608` is probably an artefact of auditing during a phase-8 world
switch, where two worlds are alive and the audit compares one world's voxels
against the other's mapping. Chasing it needs a headless run that actually
destroys voxels, which needs the `fire` token fixed first. That is **phase 12**,
after **phase 11**, and the ordering is the reason those two are numbered the
way round they are.

### Phase 11 — `--ui-script fire`, and the first headless run that destroys a voxel

*First, because it is the smallest of the four and because phase 12 cannot be
measured without it. The verification reference at the bottom of this file has
claimed "a scripted boundary crossing with combat" for four phases and it has
never been true: nothing headless in this tree destroys a voxel, so the
destruction audits' in-game half has only ever been run by hand.*

**What is already known.** Verified 2026-08-13; re-check the line numbers, they
drift.

- The token exists and is mapped the whole way to the keyboard snapshot:
  `ScancodeForToken` returns `SDL_SCANCODE_P` (`SDLKeyboard.cpp:45`) and
  `applyScriptedScancode` sets `state.P` (`SDLKeyboard.cpp:316`). So this is
  *not* the `backward-on` failure, where a token had no case in the overlay and
  spent ninety scripted seconds doing nothing in silence.
- **`KeyboardController::Update` is the only caller of `Keyboard::GetState()`
  in the tree** (`KeyboardController.cpp:50`), once per display frame from
  `Application::Run` → `InputContextNew::Update` (`Application.cpp:219`). That
  kills the most attractive theory before anyone builds on it — a second reader
  consuming the one frame the scripted key is true, which would explain why
  `forward-on` works (it uses `Held`, which persists across calls) and `fire`
  does not. Checked, not assumed.
- `UpdateKeyState(IK_P, keyboardState.P)` (`KeyboardController.cpp:104`) is what
  turns it into `IKS_PRESSED`, and `VoxApp.cpp:71` registers "Fire" on `IK_P`
  with exactly that status.
- What phase 9 measured: `Weapon::Fire`'s first line printed nothing across
  2,500 headless frames with `join` and four `fire`s.

**Two hypotheses to price before touching any input code.** Both are free —
they need one run each with an environment variable already in the tree — and
between them they split "the input never arrives" from "the input arrives and
does something else", which phase 9 never actually distinguished.

1. **The binding does not exist yet.** `Player::Start` binds "Fire"
   (`Player.cpp:265`), and gameplay does not tick while `World::IsGameplayHeld()`
   — a Debug `Fishing_Village_Beat1` holds **910–1,071 ticks** after phase 9.
   A script at `--ui-script-interval 60` has spent its first sixteen tokens
   inside that hold, and the presses land where no player has bound anything.
   Read the `[ui] step` lines against `[world] gameplay held N ticks`; both
   already print. This is the same mis-attribution shape as the link retry
   budget (see phase 3's notes) — a budget burned during the hold.
2. **The callback runs and takes another branch.** The Fire binding prints
   receiver / ammo / incoming / casted / partner / return on entry under
   `VOXAGINE_GAMEPLAY_DEBUG=1` (`Player.cpp:270`), which is *upstream* of
   `Weapon::Fire`. If that line prints, the input path is healthy and the branch
   chosen is the recall path, which is a gameplay question and not this phase's.

**Scope it small.** A token that works, and — if the fix is timing — a token
that *waits on gameplay* rather than a longer interval, which goes stale the
next time a level's hold changes. `--ui-script` is a test harness; it does not
need a scripting language.

**Aiming is part of the acceptance and is easy to forget.** A throw that hits
nothing destroys nothing, so "the weapon fired" is not the deliverable. Pick a
level and a start position where firing forward hits destructible geometry, and
say which in the phase notes so the run is reproducible.

**Acceptance:**

- A headless run destroys voxels, and the count is printed — not inferred from
  a screenshot. If nothing counts destroyed voxels today, add the counter to
  `StreamingCounters`, where the exact/machine-independent rule already applies.
- `VOXAGINE_SYNC_AUDIT`, `VOXAGINE_COVERAGE_AUDIT` and `VOXAGINE_PYRAMID_AUDIT`
  clean over a single run that **both** slides the window and destroys geometry.
  That is the case every audit in this plan exists for and has never had.
- The "**Not combat**" caveat comes out of the verification reference below and
  out of `CLAUDE.md`, and nothing replaces it with a claim the run does not
  support.
- CPU suites green in Debug and Release; `Tests/Baselines/perf.txt` unmoved.

#### Phase 11 notes — the clock was the bug, and both prior diagnoses were wrong

**The token was never broken. The script's *clock* was.** `StepUIScript` counted
display frames from process start, and a level spends hundreds of them with
gameplay held while its initial window streams in — so the tokens were spent
into a world where no entity had ticked and `Player::Start` had not run, which
means nothing had bound "Fire" to anything. The keys arrived; there was no
listener.

**It was intermittent for one reason and the reason is in `CLAUDE.md` already**:
how long the hold lasts is how long the bake takes. Two Release runs of the same
binary, the same command line and the same level measured **621** and **2,930**
held ticks. At `--ui-script-interval 60` that is the difference between a script
whose tokens land in gameplay and one that spends all twelve of them before the
level exists. The 621-tick run *did* enter the Fire callback — which is why the
first diagnostic run of this phase disproved phase 9's conclusion before any
code was written.

**Both recorded diagnoses were wrong, and both were wrong in the same way.**
Phase 9 instrumented `Weapon::Fire`'s first line, saw nothing, and concluded the
input stopped "somewhere between the synthetic key event and
`InputHandler::BindAction`". The `[player] Fire on ...` line under
`VOXAGINE_GAMEPLAY_DEBUG=1` is *upstream* of `Weapon::Fire` and was already in
the tree; one run with it on separates "no input" from "input, other branch",
and phase 9 never ran it. This phase's own notes then predicted the second
hypothesis (the callback runs and picks recall) and that was wrong too — the
callback did not run at all, for a third reason neither had named.
**When two hypotheses are cheap, run both before writing either down as the
answer.**

**The fix is one rule: the script's clock does not run while gameplay is held.**
`Keyboard::SetUIScriptPaused`, set each frame from `Application::Run` off
`World::IsGameplayHeld`. Deliberately *not* a longer interval — an interval is
tuned against a hold that varies 5x between runs of the same binary — and
deliberately not a new `wait-for-gameplay` token, which would leave every
existing script silently broken. **Both worlds are asked**: during a phase 8
switch the top world is the sprite-only loading screen, which is never held,
while the level behind it is, and pressing keys at a loading screen is the same
defect one indirection along. It prints
`[ui] script paused: gameplay held` / `[ui] script resumed after N held frames`
on the transition, which is the line that tells a later session whether a script
that "did nothing" was ever running at all.

**Measured, `Fishing_Village_Beat1`, Release, headless:**

| | |
|---|---|
| `fire` tokens reaching the Fire callback | 0 of 4 (when the hold is long) → **4 of 4** |
| voxels destroyed by a scripted run | 0, ever → **25,062 over 32 bursts** |
| chunk transition in the same run | yes — chunks at Y = 3 arrive mid-run |

Four `fire` tokens produce more than four bursts because a thrown bullet is
recalled, ricochets and explodes; the count is of `ApplySphericalDestruction`
calls, not of trigger pulls.

**`[destruction] N voxels destroyed, M protected, over B bursts` is printed by
every run, unconditionally**, at the end of `Application::Run`, from three new
`StreamingCounters`. Destroyed and protected are separate because a shot that
hits only indestructible geometry destroys nothing and is *not* the same event
as a shot that hit air — and because the cheapest guard against this plan
claiming combat coverage again for four more phases is a run that reports zero.
The counters do not move in `voxagine_tests`: the harness drives
`SphericalDestruction::Apply` directly and never constructs a `PhysicsSystem`,
so `Tests/Baselines/perf.txt` is unchanged.

**Aiming turned out to matter and is worth knowing before writing a script.**
`Weapon::Fire` throws along `Player::GetDirection()`, and with two players
present the partner catches it: the first acceptance attempt on
`Fishing_Village_Beat2` produced a perfect throw/catch/recall loop and
**0 bursts**. Beat1, walking forward into the village first, destroys 20–25 k
voxels. *The weapon firing and the weapon hitting something are different
acceptances, and only the second one is what the audits need.*

##### What the acceptance run found, which is phase 12's

The run this phase exists to make possible — window slide **and** destruction,
Beat1, all three audits — is clean on two of the three:

- **Coverage: 0 uncovered bricks above the ground row** (25,766 in the
  deliberately-uncovered y = 0 row), 456 proxies.
- **Pyramid: 0 of 10,785,024 texels disagree** over 5 mips; brick/bitmap
  validation 0 of 75,497,472.
- **Sync: 228 of 75,497,472 voxels occupied only on the CPU** — invisible but
  solid. 0 only on the GPU.

**That is phase 12's defect, reproduced headlessly for the first time**, and the
run is four minutes. Two things are already established about it and neither
needed a hypothesis:

- **It is caused by destruction.** The identical script with every `fire`
  replaced by `wait` reports **0** disagreements and 0 bursts. Same level, same
  walk, same audit, same window slide.
- **It is not an audit race, and that theory should not be retried.** The audit
  stages the whole mapping on the main thread with the world not ticking, so
  there is no window in which the CPU grid can move under it. The 10 s in
  `staged ... in 10073.5 ms` is PCIe read time inside one frame, not a period
  during which the game ran.

The direction is the opposite of the play session's larger number (540 CPU-only
*and* 4,426 GPU-only there, 228 CPU-only and 0 GPU-only here), so this may be
one half of it rather than all of it — phase 12 should say which rather than
assume.

### Phase 12 — The CPU/GPU voxel disagreement during play

*Second, because it is a live correctness defect a player can see, and because
the instrument that finds it is the run phase 11 delivers.*

**The symptom, from Joey's play session after phase 9**: `VOXAGINE_SYNC_AUDIT`
reports **540 voxels present only on the CPU** — invisible but solid — and
**4,426 only on the GPU** — visible and not there. On screen that is a destroyed
pillar coming back visually while collision stays correct.

**Not phase 9's**: master `c5595d3` and the phase 9 branch both report **0**
disagreements on the same headless run. It needs input, destruction or a world
switch to appear, which is the whole reason phase 11 comes first.

**Start from phase 11's repro, it is four minutes and it is exact.** Beat1,
`--ui-script "wait,forward-on,fire,wait,fire,wait,fire,wait,wait,fire,wait,fire,wait,forward-off,fire,wait,fire,wait"`
at interval 60 with `VOXAGINE_SYNC_AUDIT=10`: **228 of 75,497,472 voxels
occupied only on the CPU**, 0 only on the GPU. The same script with every `fire`
replaced by `wait` reports 0, so destruction is the cause and the window slide
is not. The audit is not racing the world (it stages on the main thread with
nothing ticking) — phase 11's notes have both, and neither is worth re-deriving.

**Two things to rule in or out before theorising**, both cheap and both exact:

- **`StreamingCounters::ChunkInstanceRestamps` must be zero.** A re-stamp over
  decoded voxels is the *known* way this tree produces "destroyed terrain came
  back" (M7), and the counter exists precisely so the answer is a number rather
  than an argument. GPU-visible-only voxels are that defect's exact shape.
- **The `149800 of 8388608` reading at map load is probably an audit bug.**
  Phase 8 keeps two worlds alive during a switch; if the audit takes the voxels
  of one and the mapping of the other it will report garbage while nothing is
  wrong. Check its world selection against
  `WorldManager::LoadWorldAfterStreaming`/`UpdateStreamingWorld` — and note that
  `RenderContext::ResizeWorldBuffer` had the *same* bug in phase 8, taking the
  visible world where it needed the world being sized. If that is what this is,
  fixing the audit is this phase's work and the play-session numbers still need
  explaining separately.

**Do not re-derive the encode-vs-`Clear` ordering race** (M7's notes): it was
measured and the damage survives the codec exactly.

**Acceptance:** a headless repro; the mechanism *named and demonstrated*, not
inferred; `VOXAGINE_SYNC_AUDIT` reporting 0 of 75.5 M over a run that destroys
geometry, slides the window **and** switches world; and whichever exact counter
would have caught it earlier added to `StreamingCounters` and gated in
`Tests/Baselines/perf.txt`. A fix with no counter behind it leaves the next
instance to be found by crashing, which is how this one was found.

### Phase 13 — A lifetime handle for streamed content

*Third: the largest of the four, the least urgent — every crash it prevents has
already been fixed individually — and the one most likely to touch code phases
11 and 12 are editing, which is why it is not first.*

**The class.** Four of the ten defects the phase 9 play session found, and
ledger M8, are one sentence: *a raw pointer to streamed content, held across a
frame, with nothing to tell it the target died.* Chunk streaming destroys
entities routinely, so this tree manufactures the hazard as a matter of course.
`CLAUDE.md` has documented it as a class since M8 and every instance since has
still been found by crashing.

**More guards is not the fix, and that is measured rather than aesthetic**: two
of the crashes were *inside* the guard —
`pSpawner->GetOwner()->IsDestroyed()` where the owner was not null but freed.
**A pointer you must dereference to validate cannot be validated.**

**What the answer looks like**, and both halves already exist as one-offs worth
generalising rather than copying a third time:

- `PlayerSlot` (`Game/Source/General/PlayerSlot.h`) — an empty slot re-resolves
  by index every tick, forever, so there is no deadline to lose and a player
  destroyed by a chunk unload re-attaches when it comes back.
- `SpawnerManager` — reduces each link to an entity id on the frame it is
  written and resolves ids from then on.

One handle type (id + generation, resolved on use) that game code holds instead
of `Entity*`/`Component*` for anything it keeps across a frame boundary.

**What it is not.** Not an ECS rewrite, and not a replacement for
`World::EntityLinkRecord`/`JsonSerializer::ClearEntityLinks` — those repair
*reflected* `Entity*` properties, which is a different mechanism and the reason
this was invisible for the life of the tree (no engine type has one; every one
is game code, which the test suite does not link).

**The first deliverable is the list, not the conversion.** Audit what holds a
pointer to an entity or component across a frame — managers, components, the
weapon/bullet lists, the camera's players — and write it into the phase notes
before converting anything. A conversion list assembled from memory will miss
the holder that crashes.

**Acceptance:** the handle type exists with checks in `Tests/`; every holder on
the audited list converted or explicitly excused in the notes;
`Tests/Harness/StreamingProbe.h` grown a case that destroys a target and then
resolves a stale handle, green under **ASan** (it is what found M8's
use-after-free on its first run); the streaming fuzz run with targets being
destroyed under load, clean; CPU suites green in Debug and Release.

### Phase 10 — Editor session and closing the plan

*Last of all four remaining phases — see the order note under Progress — and the
only one that cannot be done headless. Everything it asks for needs the editor
open in front of somebody.*

- Guard the remaining editor operations that assume a settled world - play-mode
  enter and map switch wait on (or refuse during) `ChunkSystem::IsStreaming()`.
  Save is already guarded (phase 7).
- Editor View menu: the validation items (`Validate Occupancy Bricks`,
  `Validate Coverage Pyramid`, `Validate Voxel Representations`) must be correct
  against the double-buffered flush semantics phase 1 introduced - take the
  branch's `Validate(bBack)` split.
- **Fix `gpu_destruction_sync_stress`, or retire it and say so.** It has
  measured nothing since phase 4 and fails identically on master - see the
  phase 9 notes for what was tried and why raising `--frames` is not the whole
  of it. Until then `-L gpu` is two green and one red, and a red gate nobody
  expects to pass is worse than no gate.
- **Joey judges pop-in on screen.** The streaming budget constants are tuned
  against a judgement nobody has made yet, and phase 9's sliced stamping moved
  work into more frames precisely where that shows. Whether
  `StreamingBudgets::VoxelBaking`'s 2 ms is right while a loading screen is the
  only thing competing for the frame is the same question and is asked in
  phase 9's notes.
- Sweep the diff for anything in the keep list not yet landed, then **delete
  `progressive-chunk-experiment` (local and origin)**. Not before: E5, E6 and
  E9's sprite-only flag are unlanded and the branch is still the reference for
  them, and phases 11-13 may want to read it.
- Update `CLAUDE.md`'s streaming sections and the ledgers here, and move this
  plan's summary paragraph into `CLAUDE.md` the way the other plans do.

**Acceptance:** editor session - open large world, slide window, edit, save,
play, stop, switch world - with validation layers on and zero errors; a saved
world diff-identical to a save taken at rest (phase 7's refusal makes
truncation impossible, which is not the same claim); the complete suite -
checks, scenarios (ASan, single-step sweep, fuzz), perf counters, both GPU
gates - green in Debug and Release; pop-in judged on screen; branch deleted.

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

# A scripted boundary crossing with combat, headless, audits on. Real combat as
# of phase 11: every run ends with a `[destruction] N voxels destroyed` line, and
# a run that reports 0 has not tested destruction whatever else it did.
#
# Beat1 rather than Beat2, and walk before firing: the throw goes along the
# player's facing direction, and on Beat2 from spawn the partner simply catches
# it - a perfect throw/catch loop and 0 bursts. Expect ~20-25 k voxels over
# ~23-32 bursts here, and chunks arriving at Y = 3 as the window slides.
#
# --frames is a main-loop iteration count and the initial-window hold spends
# hundreds of them, so budget several thousand more than the script needs. The
# script's own clock no longer runs during that hold (phase 11).
cd Game && VOXAGINE_AUDIO_NULL_DEVICE=1 VOXAGINE_SYNC_AUDIT=10 VOXAGINE_COVERAGE_AUDIT=10 \
  VOXAGINE_PYRAMID_AUDIT=10 \
  ../Build/Linux/Game/Release/bin/BitBuster --hidden --size 1440x810 --frames 12000 \
  --map Content/Worlds/Fishing_Village/Fishing_Village_Beat1.wld \
  --ui-script "wait,forward-on,fire,wait,fire,wait,fire,wait,wait,fire,wait,fire,wait,forward-off,fire,wait,fire,wait" \
  --ui-script-interval 60
# Add VOXAGINE_GAMEPLAY_DEBUG=1 to see the Fire callback's branch per press.
# The sync audit reports 228 CPU-only voxels on this run: that is phase 12's
# open defect, not a regression in whatever you are checking.
```

Headless always (`--hidden`, never a window on Joey's display), quiet machine
for numbers, `journalctl -k | grep Xid` before blaming a new change for a
freeze, and ask Joey to look at the screen for every judgement call — pop-in
visibility, loading-screen feel, penumbra-class rendering changes.
