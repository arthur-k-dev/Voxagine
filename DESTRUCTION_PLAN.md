# Destruction & particle simulation rewrite plan

Rewrite the voxel destruction pipeline and the particle simulation for
performance, correctness and verifiability. This is a **redesign**, not a bug
hunt: the current architecture is the cause of most of the defect ledger below,
and the phases replace it rather than patch it. Written to be executed **one
phase per session, in order**, by a future agent with no memory of the
conversation that produced it.

Companion to `CLAUDE.md` (general handover) and `RENDERING_PLAN.md` (renderer).
This file owns destruction, debris/effect particles, voxel integrity (island
detection) and the loose-voxel registry.

---

## How to use this document

1. Read **The rules**, **Ground truth** and **The defect ledger**. They were
   verified against the tree at `5471bdc` (2026-08-09); re-verify a `file:line`
   before editing it — line numbers drift.
2. Find the first phase in **Progress** that is not `DONE`. Do **only that
   phase**. Each phase says *why this order*; do not skip ahead or bundle.
3. Meet the phase's **Acceptance** criteria before marking it done. If you
   cannot, mark it `BLOCKED` with a note rather than half-landing it.
4. Update **Progress**, the phase's notes, and tick off ledger entries the
   phase fixed (change `OPEN` to `FIXED (phase N)`). Record measured numbers —
   the next session plans against them.
5. Commit per phase, on a branch, with the phase number in the message.
   Re-sync the branch with master first — Joey squash-merges PRs.

### Model guidance

| Phase | Nature | Recommended |
|---|---|---|
| 0 Harness, baseline, CI | mechanical | Sonnet |
| 1 Unified write path | design (cross-system invariants) | **Opus** |
| 2 Destruction rewrite | mostly mechanical once 1 exists | Sonnet/Opus |
| 3 Particle core rewrite | design (deletes the claim system) | **Opus** |
| 4 Incremental connectivity | design (new data structure) | **Opus** |
| 5 Connectivity off-thread | design (concurrency) — *measured gate* | **Opus** |
| 6 GPU debris sim | design — *measured gate* | **Opus** |

### Progress

| Phase | Status | Session / commit | Notes |
|---|---|---|---|
| 0 — Harness, baseline, CI | DONE | destruction-phase-0 | Gauntlet is a CPU harness, not the running game — see notes |
| 1 — Unified voxel write path | DONE | destruction-phase-1 | Immediate apply, not deferred — see notes |
| 2 — `ApplySphericalDestruction` rewrite | DONE | destruction-phase-2 | Hash unchanged; the seeding rule changed and found the same islands |
| 3 — Particle core rewrite (SoA, no claims) | TODO | | |
| 4 — Incremental connectivity | TODO | | |
| 5 — Connectivity off-thread | GATED | | Only if phase 4's budget measurably limits |
| 6 — GPU debris simulation | GATED | | Only if phase 3's sim cost measurably limits |

---

## The rules

Invariants that must survive the rewrite. Breaking one fails in ways that are
hard to attribute — most were paid for once already.

1. **The mapped voxel buffer is write-only from the CPU.** It is
   `DEVICE_LOCAL | HOST_VISIBLE` (ReBAR); a read is an uncached PCIe read of
   VRAM. Old occupancy comes from the brick grid's CPU-side bitmap, colour
   from the CPU voxel. Violating this cost 5.3 s of a world load once.
2. **The voxel alpha byte is a `rendererState` tag, not opacity.** Occupancy is
   `(Color >> 24) != 0` on both CPU (`Voxel::IsActive`, `VoxelGrid.h:24`) and
   GPU. `sizeof(Voxel) == 4` is a static_assert; keep it.
3. **Every representation moves together or not at all.** A voxel write must
   maintain: CPU voxel, mapped VRAM word, occupancy bitmap, brick counts,
   owner slot — and, for a voxel no renderer owns, the loose-voxel registry
   (or it renders only when an unrelated proxy covers the pixel). Phase 1
   makes this a property of one API instead of a per-call-site obligation.
3b. **The brick grid must be told about every occupancy transition** — that is
   what "brick counts" above means concretely. A voxel written behind its back
   is geometry silently missing from the image. **View → Validate Occupancy
   Bricks** in the editor is the camera-independent check.
4. **Owner slots are never recycled**; a slot is a stable identity for the
   world's life. `0` = no owner. (Slot `0xFFFF` = particle claim — until
   phase 3 deletes claims; after that, `0xFFFF` must be treated as reserved
   and unused, not reassigned, because encoded chunk data may be older than
   the code.)
5. **Zero validation errors is the bar**, and both Debug and Release must
   build — this tree has shipped two Release-only link differences.
6. **Windows must keep building.** Platform code behind `_WIN32`/`if(WIN32)`.
7. **Build the `bringup` preset before pushing anything under `Vulkan/`** — CI
   builds it and the other presets skip `VulkanBringup.cpp`.
8. **Any new shader resource** (phase 6) updates both the DXC register shifts
   in `cmake/Shaders.cmake` and `VKBindings` in `VKShaderBindings.h`, and a
   read-write structured buffer must go through `MakeUnorderedStorageBuffer`.
9. **Run the game from `Game/`** — assets are relative.
10. **Ask the user to look at the screen** for visual verification; they see
    flicker and behaviour over time that a still frame does not.
11. **Measure before building** (phases 5 and 6 are gated on this). The depth
    prepass was built on an unmeasured premise and deleted; a percentage that
    stays constant as you scale the input is the signature of optimizing the
    wrong thing.
12. **Do not reintroduce a global `WaitForGPU`** in the frame path.

---

## Ground truth

Verified against the tree at `5471bdc` on 2026-08-09. If any of this turns out
false, stop and re-plan.

### What exists today

Two separate particle mechanisms share one GPU buffer and one draw pass:

- **Debris ("pool") particles** — `ParticleLinkedList` (`Core/ECS/Systems/
  Physics/ParticleLinkedList.{h,cpp}`): a fixed `std::vector<Particle>`
  (default 150,000) with an intrusive free list and an intrusive alive
  doubly-linked list. `Particle` is a union of live state and the free-list
  link; no generation counters, no aliveness flag. Simulated by
  `PhysicsSystem::SimulateParticles` (`PhysicsSystem.cpp:1239-1435`), main
  thread, every fixed tick, oldest→newest by pointer chase.
- **Component particle systems** — `Core/ECS/Components/Particles/`:
  `ParticlePool` (SoA, swap-compaction), `ParticleSystem` + emitters
  (`BoxEmitter`, `SphereEmitter`, `VoxFrameEmitter`) + modules
  (`BasicTimerModule`, `CollisionModule`, `AttractorModule`). Never bake,
  never touch ownership. Ticked by `PhysicsSystem::TickParticleSystems`
  (`:210-243`) into the **same** GPU buffer and the **same** shared cap.

Both write 16 B `GPUParticle` records into a **single-buffered**, persistently
mapped particle mapper (`RenderContext.cpp:910-919`), drawn instanced by
`ParticlePass` (24 of 36 cube indices — bottom/back faces never drawn).

**Destruction** is `PhysicsSystem::ApplySphericalDestruction`
(`PhysicsSystem.cpp:460-578`), main thread, called by `Bullet`, `EnemyBullet`,
`Bomb`, `ExplosionTrigger` via `World`. It heap-allocates two `diameter³`
arrays per call, fetches the box via `VoxelGrid::GetChunk`, and per destroyed
voxel: resolves the owner (entity lookup with a one-element cache), reads the
colour back **out of the mapped VRAM buffer** (`:519`), spawns one particle
per four voxels, claims the source cell, zeroes the voxel through **two**
calls (`VoxelGrid::ModifyVoxel` + `RenderSystem::ModifyVoxel`), and pushes
**nine** integrity seeds.

**Integrity** is `IntegrityChecker` (`Physics/IntegrityChecker.{h,cpp}`) — a
main-thread, resumable, budgeted (20,000 visits/tick) iterative DFS with
26-connectivity and an `unordered_set` visited set; ground contact
(`y - 1 == 0`) discards the island, exhaustion emits it. **`IntegrityJob` no
longer exists** — deleted in #19; `CLAUDE.md`'s "IntegrityJob data race" known
defect, `SOURCE_MAP.md`, `ARCHITECTURE.md` and `FINDINGS.md` VX-PHY-002/003
all describe the deleted design. The checker has **no memoisation between
seeds** (`m_CheckedVoxels` cleared per seed) and dedups seeds only within one
`EnqueueBulk` batch. Islands convert to debris at 16,384 voxels/tick (#27),
each voxel costing a spawn + two `ModifyVoxel` (one a VRAM write).

**Ownership** is `VoxelOwnerVolume` (`VoxelGrid.h:44-109`): one `uint16_t`
slot per voxel; `0xFFFF` marks a particle claim whose `Particle*` lives in a
sparse `unordered_map`. The chunk RLE codec deliberately drops claims on
encode. A particle claims the empty cell it currently occupies and releases it
when it moves; "may I clean up / bake" is `owner == me || no owner`.

**Bake on impact** (`PhysicsSystem.cpp:1331-1388`) writes the landed colour
into the voxel buffer and registers it with `RenderSystem::AddLooseVoxel` —
baked debris has **no owner** (the owner write at `:1387` passes a
never-assigned `UserPointer`, i.e. always clears), which is exactly why the
loose-voxel registry exists: it is the only mechanism giving a non-renderer
voxel a rasterized proxy. The registry is level-space, 32³ cells, validated
32 cells/frame round-robin against brick counts (#25).

**Threading**: everything above is main-thread. The only concurrency touching
this data is chunk load/unload: `Chunk::DecodeVoxels`/`EncodeVoxels` run on
job threads and write/free the same vectors the grid points at (see P7 in the
ledger — this is a live race, not a historical one).

### The four representations (plus two side tables)

| Representation | Where | Maintained by |
|---|---|---|
| CPU voxel colour | `Chunk::m_VoxelData` via `VoxelGrid` | `VoxelGrid::ModifyVoxel` |
| Mapped GPU voxel word | voxel mapper (ReBAR) | `RenderContext::ModifyVoxel[Fast]`, `BeginRegion`/`AddVoxel`/`EndRegion` |
| Occupancy bitmap (1 bit/voxel, cached CPU) | `VoxelBrickGrid` | same `RenderContext` calls |
| Brick counts (`uint16_t` per 8³) | `VoxelBrickGrid` + brick mapper | same |
| Owner slot (`uint16_t`/voxel) | `VoxelOwnerVolume` | callers, ad hoc |
| Loose-voxel registry | `RenderSystem` | callers, ad hoc |

Nothing enforces that a call site updates all of them. Three sites currently
make the paired colour calls (`PhysicsSystem.cpp:307/313`, `:554/555`,
`:1360/1366`); owner and registry updates are best-effort and the ledger shows
where they are missed.

### Where the time goes (shape, to be quantified in phase 0)

- **Explosion burst**: `2 × new[] (diameter³)` + zero-init, one `sqrt` per
  candidate voxel, one **PCIe readback per destroyed voxel**, one entity-map
  lookup per accepted voxel, 9 seed pushes per destroyed voxel.
- **Integrity**: repeated full flood fills from thousands of overlapping,
  undeduplicated seeds — the quadratic-shaped "gets worse the more you
  destroy" arc (#27's message: 127 frames >30 ms, several at 100 ms,
  immediately preceding a GPU timeout).
- **Island conversion**: per voxel, spawn + owner-map write + CPU write +
  VRAM write + brick RMW. Budgeted (16,384/tick) but not cheap.
- **Particle sim**: pointer-chase over up to 150,000 nodes, 1–3 full
  chunk-index `GetCell` computations per particle that changed cell, one 16 B
  write-combined store per active particle per fixed tick (~2.4 MB/tick at
  cap), all main-thread inside `FixedTick`.

---

## The defect ledger

Every entry verified against `5471bdc`. A phase that fixes one changes `OPEN`
to `FIXED (phase N)`. **Do not fix these piecemeal outside their phase** —
half the point of the rewrite is that whole classes disappear structurally.

### Destruction path

| # | Status | Defect |
|---|---|---|
| D1 | FIXED (phase 2) | Leak on early return: both `new[]` arrays allocated before `if (!isValid) return;` — `PhysicsSystem.cpp:472-478` vs `:576-577` (VX-PHY-001) |
| D2 | FIXED (phase 2) | Radius unvalidated: NaN/negative → UB on the unsigned cast; `diameter³` overflows `uint32_t` past 1625 — `:467-471` |
| D3 | FIXED (phase 2) | Off-by-one: loop pre-increments `volumePos` then uses `volumePos.x - 1`; on row wrap the cleared voxel, the sphere test and `voxels[i]` disagree — `:490-504`, `:551` |
| D4 | FIXED (phase 2) | PCIe readback per destroyed voxel: `m_pRenderSystem->GetVoxel` at `:519` reads the mapping; the CPU colour is already in hand as `voxels[i]->Color`. Same pattern in `VoxFrameEmitter.cpp:34` |
| D5 | FIXED (phase 1) | Stale owner slots: `ModifyVoxel` clears colour only; 3 of 4 destroyed voxels keep their static owner's slot; islands leave stale slots on pool exhaustion — `:525-555`, `:296-303` |
| D6 | FIXED (phase 2, producer) | Integrity seeds: 9 per destroyed voxel, deduped only within one batch, never against `m_Pending`; no memoisation between flood fills — `:557-569`, `IntegrityChecker.cpp:34-35`, `:82-83` |
| D7 | FIXED (phase 1) | Null-cell deref: `ProcessIntegrityChecks` calls `cell.IsActive()` without testing `cell`; `VoxelCell::IsActive` unconditionally derefs `pVoxel`, and islands are held across ticks so the window can slide between discovery and conversion — `:287-293`, `VoxelGrid.h:124` |
| D8 | FIXED (phase 1) | `VoxelGrid::ModifyVoxel` has no bounds check and indexes a possibly-null chunk volume — `VoxelGrid.h:291-299`; called with unclamped coordinates at `:307`, `:554`, `:1360` |
| D9 | FIXED (phase 2) | `PhysicsSystem::m_pRenderSystem` never initialised by `PhysicsSystem`; set only by `RenderSystem`'s own constructor — `PhysicsSystem.h:114` |
| D10 | FIXED (phase 1) | `VoxelGrid::GetChunk` is two near-duplicate 3-deep loops with different owner-slot handling; the fast path computes a clamp it does not apply to the write index — `VoxelGrid.cpp:150-242` |
| D11 | FIXED (phase 2) | Two open-coded copies of the position hash; both truncate `float → uint16_t`, so negative coordinates wrap — `IntegrityChecker.cpp:8-15`, `PhysicsSystem.cpp:563-566` |

### Particle path

| # | Status | Defect |
|---|---|---|
| P1 | OPEN | GPU record written **after** `DestroyParticle` — a retired particle (free-list link aliasing its state) is rendered one extra frame — `PhysicsSystem.cpp:1391/1418` vs `:1422-1430` |
| P2 | OPEN | `DestroyParticle` has no double-free guard and no aliveness flag; a second call cycles the free list — `ParticleLinkedList.cpp:51-79` |
| P3 | OPEN | Head/tail repair in `DestroyParticle` is asymmetric and fragile — `:56-75` |
| P4 | OPEN | `Live.UserPointer` never assigned; the bake's "set owner" is a permanent clear, so baked debris is unowned by fiat, not by design — `:1387` |
| P5 | OPEN | `bakeVoxelPos`/`bakeCellPos` mismatch: colour, registry and owner writes can land on different voxels; known and deliberately unfixed — `:1335-1388` |
| P6 | OPEN | A particle whose claim was taken over destroys itself **without baking** (bake is inside the ownership branch, destroy is outside) — silently vanishing debris — `:1323-1391` |
| P7 | FIXED (phase 1) | **Live race**: `Chunk::EncodeVoxels` on a job thread does `m_VoxelData.resize(0); shrink_to_fit(); m_OwnerVolume.Release()` while `VoxelGrid::m_ChunkVolumes`/`m_ChunkOwners` still point at them (re-pointed only for non-unload items) — main-thread `GetCell` vs worker free — `Chunk.cpp:200-202`, `ChunkSystem.cpp:357-360`, `VoxelGrid.h:269` |
| P8 | FIXED (phase 1) | `VoxelOwnerVolume::Release` drops live particles' claims wholesale; they then treat foreign voxels as their own — `VoxelGrid.h:59-64` |
| P9 | OPEN | Window slide invalidates `prevGridPos`; slides under 100 units are undetected by the teleport clamp and the particle releases/claims wrong cells — `PhysicsSystem.cpp:1271-1275`, `ChunkSystem.cpp:397` |
| P10 | OPEN | World pause/teardown clears integrity state but not the particle pool — `:164-171`, `VoxelGrid.cpp:63-74` |
| P11 | OPEN | `ParticleLinkedList(0)` indexes an empty vector and underflows loop bounds (VX-GRID-003) — `ParticleLinkedList.cpp:16-23` |
| P12 | OPEN | `SimulateParticles` derefs `m_pGPUParticles` which stays null when constructed without a world — exactly the unit-test path — `:1244`, `:48-55` |
| P13 | OPEN | Cap `break` starves the **newest** particles (unsimulated, unrendered, still holding claims) — `:1249-1250` |
| P14 | OPEN | The cap is shared and component systems consume it first — a busy emitter starves debris simulation entirely — `:227-229` |
| P15 | OPEN | Destroyed particles still increment the count → instance count exceeds live count — `:1252` |
| P16 | OPEN | Particle mapper is single-buffered and written every fixed tick with no fence against the in-flight frame (voxel/brick mappers are double-buffered; this one is not) — `RenderContext.cpp:910-915` |
| P17 | OPEN | Islands spawn **one particle per voxel** (explosions: one per four) — cost and visual-density inconsistency — `:296` vs `:525` |

### Loose-voxel registry

| # | Status | Defect |
|---|---|---|
| L1 | OPEN | Retirement judges brick occupancy of **any** writer; a cell overlapping static geometry never retires and its box re-tightens to that geometry — `RenderSystem.cpp:1026-1029` |
| L2 | OPEN | Cells outside the window are never judged and nothing evicts on chunk unload — unbounded level-space growth — `:947-949` |
| L3 | OPEN | Round-robin cursor indexes `unordered_map` iteration order; a rehash reorders and the cursor skips cells — `:924-937` |
| L4 | OPEN | Negative or >1023 cell coordinates silently dropped — voxels in the buffer with no proxy at all — `:855-866` |

### Miscellaneous

| # | Status | Defect |
|---|---|---|
| M1 | FIXED (phase 1) | `RenderContext::ForceUpdate` sets `m_bWorldUpdated`, which nothing reads — the per-particle-tick call is a no-op — `RenderContext.h:365` |
| M2 | FIXED (phase 0) | Stale docs: `CLAUDE.md` "IntegrityJob data race" known defect, `SOURCE_MAP.md:34-35`, `ARCHITECTURE.md:47,206`, `FINDINGS.md` VX-PHY-002/003 describe the deleted worker-thread design. VX-PHY-002/003 are now `overtaken-by-events` and VX-PHY-001 `partially-overtaken`, under a new `resolution` field documented in the index README |
| M3 | OPEN | `Live.Timer` is only ever set to `NO_PARTICLE_TIMER`; the pool-particle timer path is dead code — `ParticleLinkedList.cpp:89`, `PhysicsSystem.cpp:1437-1458` |

---

## The design

### What replaces what

| Today | After |
|---|---|
| Per-call-site pairs of `ModifyVoxel` + ad-hoc owner/registry updates | **One transactional write API** (`VoxelEditBatch`) that maintains every representation, bounds-checks once, and rejects non-finite input |
| Intrusive linked-list debris pool + separate component SoA pools, shared cap | **One SoA particle core** with swap-compaction and generation-tagged handles; debris and emitters are spawners with per-source budgets |
| Per-voxel particle claims (slot `0xFFFF` + sparse `Particle*` map) | **Deleted.** Particles are point/velocity/colour; collision reads the occupancy bitmap (cached CPU memory); bake resolves conflicts at impact time |
| Repeated flood fills from undeduplicated seeds | **Brick-level incremental connectivity**: per-brick local components from the 512-bit occupancy mask, a cross-brick component graph, grounded reachability from the y=0 layer; dirty bricks recompute locally |
| Trust that it works | **Oracles and audits**: brute-force flood fill kept as the connectivity oracle; representation-sync audit; deterministic scripted gauntlet with state hashing; unit tests in CI |

### Why delete the claims (the radical part, argued)

The claim system exists to answer "is this cell still mine" during flight and
to reserve a landing cell. The ledger shows it failing at both: claims are
dropped wholesale on chunk unload (P8), silently lost to takeover with the
bake skipped (P6), corrupted by window slides (P9), never persisted (by
design), and the one write that was supposed to transfer ownership to baked
debris has never worked (P4) — baked debris is unowned *today* and the loose
registry already carries it. Meanwhile the claim costs a sparse-map presence,
special cases in the chunk codec, `VoxelBaker`'s stamp and clear paths, and
`ResolveOwnerSlot`. Every consumer either has a cheaper answer (occupancy
bitmap for "is it empty", impact-time resolution for landing conflicts) or is
satisfied by "particles own nothing". What is lost: a `VoxelBaker` stamp can
place static colour in a cell a particle currently occupies (one overlapping
cube for a frame or two — cosmetic), and two particles can transiently occupy
one cell (already possible today whenever a claim is lost). Both are
strictly milder than the defects removed. Phase 3 carries a checklist to
prove no other consumer depends on claims before deleting.

### Alternatives considered and deferred

- **GPU compute debris sim** (phase 6, gated): the occupancy data is already
  GPU-resident and per-particle CPU cost would drop to ~zero, but bake-on-
  impact and gameplay reads would cross the PCIe boundary via a feedback
  ring. Do not build it until phase 3's measured numbers say the CPU sim is
  a limit at target counts — the depth prepass rule (rule 11).
- **Rigid-body island debris** (Teardown-style falling chunks instead of
  dissolving islands into particles): a gameplay/visual change, not a
  rewrite goal. Out of scope; noted here so it is a deliberate decision.
- **Sparse owner map instead of the slot array**: already evaluated and
  rejected in phase 4d for write-burst reasons; the rewrite keeps slots.

---

## Phases

### Phase 0 — Harness, baseline, CI

**Goal**: make every later phase measurable and falsifiable before changing
behaviour. No behavioural changes in this phase.

Work items:

1. **Wire `UnitTesting/` (gtest) into CMake** behind `VOXAGINE_BUILD_TESTS`
   (default off, on in CI). The existing suite needs C++17, which the build
   already requests. New tests land here from phase 1 on.
2. **Turn `VOXAGINE_BUILD_ENGINE=ON` in CI** and add the test target to the
   workflow — CLAUDE.md already calls this the single highest-value CI
   change. Keep the bringup job as-is.
3. **Deterministic destruction gauntlet**: a scripted mode (env var, e.g.
   `VOXAGINE_GAUNTLET=<script>`) that loads a level, fires a fixed sequence
   of `ApplySphericalDestruction` calls at fixed ticks with a **seeded RNG**
   for particle velocities, runs N fixed ticks, then reports. Two variants:
   *static camera* (no window slides — state is deterministic) and *sliding
   camera* (drives chunk streaming — invariants only, no state hash).
4. **State hash**: at gauntlet end, hash CPU voxel colours + owner slots +
   brick counts + occupancy bitmap over the window. Same build, same script →
   same hash, twice. This is the refactor-safety net for phases 1–2.
5. **Audit consolidation**: promote the editor's *Validate Occupancy Bricks*
   recompute to an env-triggered periodic audit (`VOXAGINE_SYNC_AUDIT=<sec>`)
   alongside the existing `VOXAGINE_VOXEL_AUDIT` / `VOXAGINE_COVERAGE_AUDIT`;
   add a particle-pool audit (free/alive lists disjoint and complete; today's
   list version, then the SoA version from phase 3).
6. **Profiler markers** around: explosion burst, integrity `Process`, island
   conversion, `SimulateParticles`, `TickParticleSystems`, loose-cell
   submission/validation. Record the baseline table below with
   `VOXAGINE_PROFILE=1`, Release, quiet machine, static-camera gauntlet.
7. **Doc sync (M2)**: fix the stale `IntegrityJob` references in `CLAUDE.md`,
   `SOURCE_MAP.md`, `ARCHITECTURE.md`; annotate VX-PHY-002/003 as
   overtaken-by-events in the findings per the index's resolution protocol.

**Why this order**: every later phase claims a perf win or behavioural
identity; without this phase those claims are vibes. The hash in particular
is what lets phases 1–2 refactor aggressively.

**Acceptance**: CI green with engine + tests on both configs; gauntlet
reproduces its own hash across two runs; baseline table filled in below.

#### Baseline

`voxagine_gauntlet` with no arguments, Release, quiet machine. 192×96×192
window, 64×96×64 chunks, 15 towers of 12×56×12, 60 explosions of radius 10 in
three waves, 528 ticks.

| Metric | Value |
|---|---|
| Level build (198,144 voxels) | 3.58 ms |
| Explosion burst, radius 10 | **0.157 ms avg, 0.297 peak**, over 60 |
| Integrity flood fill per tick | **0.089 ms avg, 0.756 peak**, over 528 (47.1 ms total) |
| Island conversion per tick | 0.009 ms avg, 0.032 peak, over 100 ticks that converted |
| Integrity cost by quarter of the run | **22.9 / 18.0 / 6.3 / 0.0 ms** |
| Destroyed by explosions / converted from islands | 130,500 / 30,580 from 100 islands |
| Representation disagreements | 0 |
| Gauntlet state hash | `c869b806aa820b57` |

The quarters are the #27 curve and they need reading carefully: they *fall*,
which is not the same as being flat. Destruction is front-loaded — by the third
wave most of what could be destroyed already has been, so there is less left to
flood. Phase 4's acceptance is that the *first* quarter comes down, not that
the shape changes.

Not measured here, and deliberately: `SimulateParticles` and the fixed-tick
total. Both need the particle core, which is phase 3, and neither is reachable
from the harness until then — the harness draws from the same seeded RNG the
spawner does so the stream stays aligned, but it does not simulate. Phase 3
fills those two rows in.

#### Notes

**The gauntlet is a CPU harness, not a scripted run of the game, and that is a
deliberate departure from work item 3.** The plan asked for `VOXAGINE_GAUNTLET=
<script>` inside the running engine with static-camera and sliding-camera
variants. That cannot be made deterministic and the state hash is worthless
without determinism: a real level is full of entities with their own
randomness, chunk streaming runs on job threads, the camera slides the window,
and the number of fixed ticks in a second depends on the frame rate.

What made the alternative possible is rule 1. Everything destruction touches is
ordinary memory *except* the mapped voxel buffer, and the CPU only ever writes
that — so handing the write path a plain `std::vector<uint32_t>` reproduces it
at full fidelity with no GPU, no window and no `World`. That is
`VoxelWorldHarness`, and it is why the gauntlet and the unit tests run in CI.

Two consequences to carry forward:

- **The sliding-camera variant is not built.** The harness models no chunk
  streaming, so there is nothing to slide. Phase 1 fixes the streaming race
  (P7) and phase 3 the window-slide particle defect (P9); both should add a
  harness-level `slide` that re-bases the grid offset and rewrites the affected
  region the way `ChunkSystem::RenderChunk` does, and the invariants to check
  are the audits, not a hash.
- **The in-game audits are the integration half and stay.**
  `VOXAGINE_SYNC_AUDIT=<seconds>` is new and is the important one: it compares
  the CPU voxel against the mapped word — the one link `ValidateBrickGrid`
  cannot see — and re-derives the counts and occupancy bits. It lives in
  `RenderSystem::PostTick` off the wall-clock delta, **not** in `Render` off
  the fixed timer where the other two audits sit, because `Render` is only
  reached on a fixed step and an audit that never fires looks exactly like one
  that found nothing. It announces once that it is armed for the same reason.

##### What the sync audit costs, and what it found

**It stages the mapping into ordinary memory with one bulk copy before
comparing, and that is not a detail.** Two passes want the words, and read
straight from the mapping each one is a PCIe read of VRAM per voxel. The first
version did that and never returned: 100% CPU, minutes per pass, the compositor
putting up "application is not responding". Staged, the PCIe traffic is one
sequential read and both passes walk cache.

The sequential read is still **19.4 s for 75,497,472 words (302 MB, ~15 MB/s)**
on this machine, which is what uncached VRAM costs and matches the ~500 ns per
voxel read RENDERING_PLAN.md phase 4b measured. `memcpy` does not help; the
memory type does. Run it on an interval of a minute or more.

First run on `Fishing_Village_Beat1.wld`, at rest:

```
[bricks] validated 147456 bricks over 75497472 voxels: 0 disagree, 0 would lose geometry; 0 occupancy bits disagree
[sync-audit] 794 of 75497472 voxels disagree between the CPU chunk and the mapping
             (0 occupied only on the CPU, 794 only on the GPU)
[pool-audit] 0 alive + 150000 free of 150000
```

**Zero brick and bitmap disagreements, and zero occupied-only-on-the-CPU** —
which is the direction that is always a defect. The 794 the other way are
expected and are not a finding: a *dynamic* `VoxRenderer` stamps the mapping
and nothing else, because `VoxelBaker::Occupy` only touches the chunk and the
physics grid for static renderers (CLAUDE.md, "Dynamic renderers are invisible
to the physics grid"). The number to watch is its size and trend against the
dynamic renderers on screen, not whether it is zero, and the audit's output
says so rather than leaving the next reader to work it out.

**The reference writer is the point of the harness, not a detail.**
`VoxelWorldHarness::Set`/`Clear` is the dumbest possible implementation of rule
3: touch every representation, in the open, no batching. Phase 1's
`VoxelEditBatch` should be checked *against* it — same edit script through
both, compare hashes — which is a much stronger statement than "the hash did
not change", because it says the new path agrees with an independent
implementation rather than with its own previous self.

**Particle velocities now come from `DeterministicRandom`** (xorshift64\*,
`Core/Utils/DeterministicRandom.h`) rather than `glm::linearRand`. The latter
draws on a process-global engine that every other caller perturbs, so a replay
diverged on the first unrelated call anywhere in the process. This is the one
behavioural change in phase 0 and it is invisible in play.

**CI now builds the engine, the game, the bring-up target and the tests on both
configurations, and runs `ctest`.** `VOXAGINE_BUILD_ENGINE` had been off there
for the whole port. Turning it on needed one fix: `add_dependencies(BitBuster
voxagine_shaders_game)` is a generate-time error when `dxc` is absent, because
`voxagine_add_shaders` returns without creating the target — and the runner has
no Vulkan SDK. It is guarded now. CI proves compile, link and CPU tests; it
still does not render.

**Five integer class constants became `static constexpr`.** `k_uiBrickShift`,
`k_uiBrickSize`, `k_uiBrickVolume`, `k_uiWordShift`/`k_uiWordMask`,
`k_uiLooseCellShift`, `k_uiLooseValidatePerFrame` and
`k_uiIntegrityConvertPerTick` were `static const` with in-class initializers,
so anything taking one by reference odr-uses it and needs a definition. A gtest
`EXPECT_EQ` does exactly that and failed to link. This is the third time this
tree has hit that, and CLAUDE.md documents the previous two.

**The editor takes about two minutes to load `Fishing_Village_Beat1.wld`**,
which matters when verifying anything in-game: budget for it, and watch for
Hyprland putting up "application is not responding" over the load. That is the
known chunk-loading stall in CLAUDE.md, not something phase 0 introduced.

---

### Phase 1 — The unified voxel write path

**Goal**: one API through which every destruction/bake/clear write flows,
maintaining all representations atomically; plus the two ordering fixes that
make the grid safe to read while chunks stream.

Work items:

1. **`VoxelEditBatch`** (name flexible): open against the grid, accept
   `Clear(pos)` / `Set(pos, colour, ownerSlot)` / bulk region clears; apply
   in brick-sorted order using `BeginRegion`/`AddVoxel`/`EndRegion` for bulk
   and `ModifyVoxelFast` for points. Per edit it maintains: CPU voxel, VRAM
   word, occupancy bitmap + brick counts, **owner slot (cleared on clear —
   fixes D5)**, and registers ownerless sets with the loose registry. It
   bounds-checks and clamps once per run (fixes D8's call-site exposure),
   rejects non-finite positions centrally, and never reads the mapping
   (rule 1).
2. **Migrate all three existing dual-write sites** (`PhysicsSystem.cpp:307`,
   `:554`, `:1360`) to the batch. Behaviour must be identical except for the
   owner-clear — verify with the gauntlet hash *excluding* owner slots first,
   then bless the new hash including them.
3. **Null-safe grid reads**: `VoxelCell` gets a validity test that D7's call
   site uses; `VoxelGrid::ModifyVoxel` (kept for the batch's internal use
   only) checks chunk residency (fixes D7, D8 proper).
4. **Fix the streaming race (P7/P8 root)**: on the main thread, detach the
   grid's chunk-volume and owner pointers (null them) **before** the unload
   job that frees the storage is enqueued, not after. Readers then see
   "not resident" instead of a freed vector. This also gives P8 a defined
   meaning until phase 3 deletes claims.
5. **`GetChunk` consolidation (D10)**: collapse the fast/general paths into
   one correct implementation; keep the perf with the batch's brick-sorted
   iteration instead of the duplicated loops.
6. Delete `ForceUpdate`'s dead flag or make it mean something (M1).

**Why this order**: phases 2–4 all express their writes through this API;
building them first would mean migrating them twice. The streaming-race fix
must precede any phase that adds grid readers.

**Acceptance**: gauntlet hash identical to phase 0's (modulo the blessed
owner-slot change); sync audit zero disagreements across the sliding-camera
gauntlet; no measured regression in the baseline table; unit tests for the
batch (bounds, owner clear, occupancy transitions, non-finite rejection)
green in CI. Ledger: D5, D7, D8, D10, M1, P7, P8 closed.

#### Notes

**The batch applies immediately; it does not buffer and flush.** The work item
sketched a brick-sorted deferred apply and that is the one part not built.
Deferring changes semantics — the integrity checker reads the grid straight
after a destruction burst and would see the pre-burst world — and brick-sorting
is an optimization with no measurement behind it (rule 11). What the batch keeps
instead is the **dirty-brick set**, which is the thing phase 2's seeding and
phase 4's incremental connectivity actually want. Sorting the writes can be
added later behind a measurement; nothing in the API assumes it is absent.

**`VoxelEditBatch` is checked against the phase 0 reference writer, not against
a recorded hash.** `VoxelEditBatchTest.AgreesWithTheReferenceWriter` runs one
edit script (ground, a block, a sphere out of the middle, then unowned debris
landing back in) through the batch and through `VoxelWorldHarness::Set`/`Clear`
and compares every representation. That is a statement about agreement with an
independent implementation rather than with the batch's own previous self.

**The gauntlet now writes through the batch and its hash is unchanged**:
`c869b806aa820b57`, byte for byte with phase 0 over 130,500 destroyed and
30,580 converted voxels. The one measured cost is the explosion burst, **0.157
→ 0.170 ms** (+8%) from per-write validation and the dirty-brick vector.
Integrity and conversion are unchanged within noise.

**Ordering the claim after the clear is load-bearing**, and it is the one place
where "clear the owner" was not a free win. Both spawn sites wrote a particle
claim onto the cell they were about to destroy, then cleared the colour — and
that worked precisely because the old clear did not touch the owner. With D5
fixed the claim has to go on *after*, or the clear wipes it and the particle
flies with no claim on the cell it came from. Reordered at both sites.

**`VoxelGrid::ModifyVoxel` is deleted rather than kept for internal use.** The
work item said to keep it for the batch; the batch resolves a `VoxelCell` and
writes through that, so nothing needed it. It wrote a colour and nothing else,
with no bounds check and no residency check, and was called with unclamped
coordinates from three places — leaving it in the header is leaving the defect
reachable.

**`GetChunk` is now one implementation walked in runs.** The two near-duplicate
loops disagreed rather than merely duplicating: the "fast" one clamped the read
start to x = 0 for a negative origin but kept writing from output index 0, so a
box straddling the left edge filled the wrong cells. Runs keep the fast path's
property (resolve the chunk once, then walk pointers) for boxes of any shape,
because residency and the chunk-local base only change when x crosses a chunk
boundary. `VoxelGridTest.GetChunkAgreesWithGetVoxel` — written in phase 0
against the *old* code, over origins and sizes that select both branches — is
what made this safe to do.

**P7's fix is `VoxelGrid::DetachChunkStorage`, by storage rather than by
location.** An unloading chunk's grid slot may already have been re-pointed at
a chunk that moved into it, and nulling by location would evict a live chunk.
`ChunkSystem` calls it on the main thread before enqueuing the unload job, so a
reader sees "not resident" instead of a freed vector. That makes non-residency
an ordinary state, which is why `ResolveIndex`, `GetCell`, `GetVoxel` and
`VoxelCell::IsActive` all became null-safe in the same change — they had to.

**P8 now has a defined meaning rather than a fix.** A particle whose claimed
cell was in an unloaded chunk used to read a freed `VoxelOwnerVolume`; it now
reads an invalid cell, `ownsOldCell()` answers false, and the particle is
destroyed without baking. That is P6's shape, and P6 is phase 3's.

**`m_bWorldUpdated` is deleted** (M1). It was written from seven places —
`ModifyVoxel`, `ModifyVoxelFast`, `UpdateWorld`, `ForceUpdate`, two
missed-frame branches in `Application::Run`, and per particle tick in
`PhysicsSystem::FixedTick` — and read from none; only `VKRenderContext::Present`
cleared it. `RenderSystem::ForceUpdate` keeps its *own* flag, which is read.

**Not migrated, deliberately: `VoxelBaker`.** It writes 3.1 M voxels in one
burst at a world load through `ModifyVoxelFast`, and it maintains the physics
grid and owner slots itself along a path phase 4c tuned carefully. Moving it
onto the batch is a real change with a real risk of regressing that, and no
phase needs it. `RenderSystem::ModifyVoxel` stays for it.

---

### Phase 2 — `ApplySphericalDestruction` rewrite

**Goal**: the explosion burst becomes allocation-free, readback-free and
index-correct.

Work items:

1. Validate and cap the radius (finite, positive, ≤ a named constant) — D2.
2. Replace the two `new[]` arrays with reusable scratch storage or direct
   chunk-sliced iteration — D1 disappears with the allocation itself.
3. Iterate y/z/x with explicitly computed per-cell coordinates (no running
   `volumePos` with wrap corrections) — D3. Compare squared distances (no
   `sqrt`).
4. Colour for spawned particles comes from the CPU voxel, never the mapping —
   D4. Fix `VoxFrameEmitter.cpp:34` the same way while at it.
5. All writes through the phase 1 batch.
6. **Seed at brick granularity**: push the set of *bricks* whose boundary the
   destruction touched (deduped against pending) instead of 9 voxel seeds per
   voxel — D6's producer half. (The consumer half is phase 4.)
7. Initialise `m_pRenderSystem` explicitly and assert the wiring — D9.
   Consolidate the position hash into one shared, negative-safe function —
   D11.

**Why this order**: needs phase 1's batch; wants the gauntlet to prove the
new loop clears exactly the intended sphere. Before phase 3 because the old
particle-spawn call sites are few and mechanical to re-target later, while
the burst cost is a measured, user-visible symptom today.

**Acceptance**: burst cost vs baseline at radius 8 and 16 (expect the
readback and allocation to dominate the delta — record the split); a
one-session A/B confirming destroyed shapes are identical except the D3
off-by-one column, which must be visually verified with Joey; gauntlet hash
re-blessed with a note. Ledger: D1–D4, D6 (producer), D9, D11 closed.

#### Notes

**The loop is now `SphericalDestruction::Apply` in `Core/Voxels/`, not a method
on `PhysicsSystem`.** Everything gameplay-shaped — "is this entity
destructible", "spawn debris here with this colour" — is a callback, so the
algorithm needs no `World`, no entities and no particle pool. Templated on the
callbacks rather than behind an interface, because a burst clears tens of
thousands of voxels and an interface would be two virtual calls per voxel.

That is what let **the gauntlet stop carrying a copy of the algorithm**. Phase 0
had to reimplement the sphere loop to have anything to measure; it now drives
the shipping code, and so do the unit tests.

**The gauntlet hash did not change: `c869b806aa820b57`.** That is a stronger
result than it looks. Phase 0's gauntlet loop was written *correctly* — plain
z/y/x, no running position — so this says the rewritten engine loop destroys
exactly the same set of voxels as an independently written correct one, over
130,500 destroyed voxels. And the *seeding rule changed completely* yet still
finds the same 100 islands and converts the same 30,580 voxels.

| | phase 0 | phase 1 | phase 2 |
|---|---|---|---|
| explosion burst (r = 10) | 0.157 ms | 0.170 ms | **0.165 ms**, now including seed collection |
| integrity flood fill, total | 47.1 ms | 46.5 ms | **36.1 ms** |
| integrity by quarter | 22.9 / 18.0 / 6.3 / 0 | 22.5 / 17.8 / 6.2 / 0 | **25.6 / 10.5 / 0.01 / 0** |
| islands / converted | 100 / 30,580 | 100 / 30,580 | 100 / 30,580 |

The curve moving *earlier* is the seeding change: better seeds find islands on
the tick the damage happens instead of several ticks later, so the same work
front-loads. Total integrity cost is down 22%.

**D6's producer half is solved by asking the question after the clear, not
during it.** The old rule pushed nine hashes for every destroyed voxel — the
3×3 in x/z one layer up — which is tens of thousands per explosion,
deduplicated only within the one batch, and almost all of them naming voxels
the same loop destroyed moments later. `CollectSeeds` runs afterwards and seeds
**occupied voxels with an empty face neighbour** in the sphere's box grown by
two. That is the surface of what is left, so it scales with area rather than
volume, and it is a strict superset of the old set: an old seed was an occupied
voxel sitting directly on a destroyed one, i.e. one with an empty neighbour
below. It is also more correct — the old rule only looked *up*, so a wall cut
sideways was never seeded.

**The particle spawn counter advances per destroyed voxel, not per successful
spawn.** The old code incremented only when a particle was actually taken from
the pool, so once the pool was full the "one in four" rule resumed from wherever
it left off and debris clumped wherever the pool happened to refill.

**A voxel exactly at the explosion centre used to produce a NaN velocity.**
`glm::normalize` of a zero vector is NaN, the old code called it unconditionally,
and a NaN velocity becomes a NaN position, a non-finite proxy AABB and a frame
the marcher never finishes. It falls back to straight up now. This is a
plausible contributor to the Xid 109 timeouts in `CLAUDE.md` — not a proven one,
and the note there about a build with the other two NaN sources fixed still
hanging still stands.

**Radius cap is 64** (`SphericalDestruction::k_fMaxRadius`), logged once when
it fires. The loop is over the bounding box, so cost is (2r+1)³: 64 is 2.1 M
voxel tests and already far larger than anything the game fires. The old code
had no bound at all and `diameter³` silently overflowed `uint32_t` above 1625.

**Not done: the visual A/B with Joey.** The acceptance criteria ask for a
session confirming destroyed shapes are identical except the D3 off-by-one
column. This ran overnight with nobody watching, so it is **outstanding** —
what stands in for it is `SphericalDestruction.ClearsExactlyTheSphere`, which
asserts the cleared set against an independently computed sphere over every
voxel of the window, and the unchanged gauntlet hash. The thing neither of
those can see is whether the one-column shift is noticeable in play; it should
not be, since it makes the sphere symmetric where it was lopsided.

**`m_pRenderSystem` (D9) is null-initialised and both users check it**, with a
one-shot warning rather than a crash. The plan asked for an assert on the
wiring; a warning is better here because the wiring is legitimately absent in a
world with no `RenderSystem` — which is the unit-test path, and phase 3 needs
that path to keep working.

---

### Phase 3 — Particle core rewrite (SoA, unified, no claims)

**Goal**: one particle system for debris and emitters; the claim machinery
deleted; the GPU handoff correct.

Work items:

1. **SoA core**: positions (level space — P9), velocities, colours, flags,
   in parallel arrays with swap-compaction; generation-tagged handles for
   anything that must refer to a particle across frames. Zero-capacity safe
   (P11), null-mapper safe (P12), pool audited (phase 0's audit updated).
   Delete `ParticleLinkedList` (P1–P3, M3 die with it).
2. **Unify spawners**: destruction debris, island debris and the component
   emitters (`Box`/`Sphere`/`VoxFrame`) all spawn into the core with
   **per-source budgets** replacing the shared cap (P13, P14); rejection
   happens at spawn time, never mid-simulation; the instance count is the
   live count (P15).
3. **Collision via the occupancy bitmap** — a bit test in cached memory per
   moved-cell particle instead of 1–3 chunk-index `GetCell` computations.
   Keep the modules (`Collision`/`Attractor`/`Timer`) as functions over the
   core.
4. **Delete the claim system**: slot `0xFFFF` semantics, the sparse map,
   `SetParticleOwner`/`GetParticle`, the codec's claim special cases,
   `VoxelBaker`'s particle-slot branches, `ResolveOwnerSlot`'s mapping.
   **Checklist before deleting** — grep-verify the only consumers are the
   ones named in Ground truth; anything new found gets a decision recorded
   here. Slot `0xFFFF` stays reserved (rule 4).
5. **Bake at impact, resolved at impact**: one landing-cell resolution
   (occupancy test + neighbour search) producing a single position used for
   colour, registry and any future owner — P4, P5, P6 replaced by
   construction. Bake writes go through the phase 1 batch (which registers
   the loose voxel — L4's silent drop becomes a logged clamp).
6. **Loose registry hardening**: record debris-written bricks so retirement
   judges debris, not any writer (L1); evict/park cells on chunk unload and
   judge parked cells on re-entry (L2); iterate via a stable key vector, not
   `unordered_map` order (L3).
7. **Fix the GPU handoff**: per-frame-slot ranges or a back buffer for the
   particle mapper so the CPU never writes records the GPU is reading (P16);
   draw all 6 cube faces or document why 4 is correct; clear pool on world
   pause/teardown (P10).

**Why this order**: needs phases 1–2 (bake through the batch; destruction
spawns re-targeted once, not twice). Before phase 4 because island
conversion's consumer (spawn + clear) should land on the new core so phase 4
measures against the real thing.

**Acceptance**: unit tests for the core (spawn/retire/compact/handles/
budgets) and the landing resolver green in CI; `SimulateParticles` cost vs
baseline at 5 k/50 k/150 k (expect the bitmap collision and SoA walk to win
— record the split); sliding gauntlet clean under the sync + coverage +
pool audits; a destruction-heavy session with Joey watching for debris
behaviour changes (bounce feel, density, vanishing debris — P6's fix should
be *visible* as more debris landing). Ledger: P1–P6, P9–P17, L1–L4, M3
closed.

**Notes**: *(fill in when done)*

---

### Phase 4 — Incremental connectivity (integrity rewrite)

**Goal**: island detection stops re-flooding the world per seed; cost scales
with what changed, not with how much has ever been destroyed.

Work items:

1. **Per-brick local components**: for each dirty brick, label connected
   components of its 512-bit occupancy mask (bitwise flood, 26- or
   6-connectivity — **decide and document**; the current checker uses 26).
   Store a small per-brick table: component id per voxel (4 bits typically),
   plus which components touch each face.
2. **Cross-brick graph**: nodes are (brick, local component); edges from
   face-adjacency of the masks. Grounded = reachable from any component in
   the y=0/y=1 ground layer (match the current checker's `y - 1 == 0` rule).
3. **Incremental update**: destruction (via the phase 1 batch, which already
   knows dirty bricks) recomputes only dirty bricks' tables and their edges,
   then re-runs grounded reachability — budgeted and resumable like today's
   checker, but over ~10⁴–10⁵ graph nodes instead of 10⁶–10⁷ voxels.
   Ungrounded components emit islands as brick+mask runs (no per-voxel
   hash vectors).
4. **Conversion on the new machinery**: islands clear via bulk batch region
   ops (brick counts move per brick, not per voxel) and spawn debris at the
   explosion density, one per N voxels (P17), budgeted as today.
5. **Keep the flood fill as the oracle**: `VOXAGINE_INTEGRITY_AUDIT=<sec>`
   runs the old exhaustive check against the graph's answer and reports any
   voxel classified differently. This is the phase's acceptance instrument
   and it stays in the tree.
6. Window slide / chunk load-unload rebuild the affected bricks' tables —
   the sliding gauntlet must cover an island discovered before a slide and
   converted after it (D7's scenario, now structurally safe).

**Why this order**: needs the batch (dirty-brick feed, bulk clears) and
wants the new particle core (conversion cost). It is the largest design
risk, so it goes last of the core phases, when the harness is mature.
**Why after phase 3**: conversion spawns through the new core; landing it
on the old pool would re-do the work.

**Acceptance**: oracle agreement (zero misclassified voxels) across both
gauntlets and a long interactive session; the #27 symptom curve is flat —
destruction-heavy gauntlet shows no upward trend in integrity cost as
cumulative destruction grows, and zero frames >30 ms attributable to
integrity/conversion; D6 (consumer), D7, P17 closed. Record the graph's
memory footprint.

**Notes**: *(fill in when done)*

---

### Phase 5 — Connectivity off-thread (GATED)

**Gate**: build this only if phase 4's measured budget is a real limit —
i.e. the budgeted main-thread graph update visibly delays island fall or
eats a measurable share of the fixed tick at the target destruction rate.
Record the measurement that opens or closes the gate either way.

Sketch, if opened: the graph update consumes **copies** of dirty brick masks
(64 B each) snapshotted on the main thread into a job; results (island
brick+mask lists) return via the main-thread callback and are validated
against current brick generations before conversion — stale results are
re-queued, not applied. Safe by construction: the worker touches no shared
engine state, which is what the deleted `IntegrityJob` never had. The job
system's known shutdown semantics (VX-JOB-003/004) must be reviewed before
relying on cancellation.

**Notes**: *(fill in when done)*

---

### Phase 6 — GPU debris simulation (GATED)

**Gate**: build this only if phase 3's measured sim cost at the shipped
particle counts is a real limit on the fixed tick. Sweep the axis first
(rule 11): measure sim cost at 5 k/50 k/150 k and project; if the curve says
CPU sim at target counts fits the budget, close the gate and record why.

Sketch, if opened: compute pass integrates positions/velocities against the
brick-grid occupancy already resident on the GPU; impacts append to a
feedback ring the CPU drains next frame to perform bakes (through the
phase 1 batch) and retire handles. Spawns upload as compact bursts. Rule 8
applies to every new binding; the particle mapper's frame-slot correctness
from phase 3 is a prerequisite. Latency of one frame between impact and bake
is the accepted cost — note it and have Joey look at fast debris.

**Notes**: *(fill in when done)*

---

## Verification reference (all phases)

- `voxagine_gauntlet [script]` — deterministic destruction run over
  `VoxelWorldHarness`, printing per-phase timings and a state hash and failing
  on any representation disagreement. Built by `VOXAGINE_BUILD_TESTS=ON`, run
  by `ctest`, no GPU. **Not** an env var on the running game; phase 0's notes
  say why, and why the sliding variant is not built yet. (Phase 0.)
- `ctest --test-dir Build/<preset>` — the gtest suite plus the gauntlet.
  `VOXAGINE_BUILD_TESTS=ON` needs `VOXAGINE_BUILD_ENGINE=ON`. (Phase 0.)
- `VOXAGINE_SYNC_AUDIT=<sec>` — recompute occupancy bitmap + brick counts
  from CPU voxels, compare all representations. (Phase 0, from the editor's
  Validate Occupancy Bricks.)
- `VOXAGINE_INTEGRITY_AUDIT=<sec>` — exhaustive flood fill vs the
  connectivity graph. (Phase 4.)
- `VOXAGINE_VOXEL_AUDIT=<sec>` — existing: occupancy/slots/codec round-trip.
  Run during destruction, not at rest.
- `VOXAGINE_COVERAGE_AUDIT=<sec>` — existing: occupied bricks with no proxy;
  must be zero above the ground row.
- `VOXAGINE_PROFILE=1` — force the frame profiler on in Release.
- Unit tests (`VOXAGINE_BUILD_TESTS`) — batch semantics, particle core,
  landing resolver, brick component labeling, graph reachability. Pure CPU,
  no GPU needed, run in CI on both configs.
- **Joey's eyes** — debris feel, density, vanishing debris, landing shapes.
  Ask him to play a destruction-heavy minute and watch; do not screenshot.
