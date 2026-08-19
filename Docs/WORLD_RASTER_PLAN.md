# The static world renders as meshes, not as a per-pixel march

The resident window's static geometry stops being ray-marched per pixel and
starts being **rasterized as greedy-meshed quads**, exactly the way
`Docs/DYNAMIC_MODELS_PLAN.md` already did it for every non-static renderer.
Destruction still works — the mesh is rebuilt incrementally from the same
dirty-brick set `VoxelEditBatch` already maintains. The look is preserved by
construction: the geometry is the same axis-aligned cubes, and the shading is
the same `ShadeVoxelSurface` both existing voxel pixel shaders and the model
pass already share.

Same format as `Docs/RENDERING_PLAN.md`, `Docs/DESTRUCTION_PLAN.md` and
`Docs/DYNAMIC_MODELS_PLAN.md`. Read **Ground truth** first. One phase per
session, in order, measured numbers recorded per phase.

---

## How to use this document

1. Read **Ground truth**, **The target**, and **The design**. Re-verify any
   `file:line` before editing it.
2. Find the first phase in **Progress** that is not `DONE`. Do only that phase.
3. Meet the phase's **Acceptance** before marking it done; mark `BLOCKED` with
   a note rather than half-landing it.
4. Update **Progress** and the phase's notes with measured numbers.
5. Commit per phase, on a branch, with the phase number in the message.

`Docs/RENDERING_PLAN.md`'s ten rules apply unchanged — in particular rule 1
(the SPIR-V/C++ binding contract), rule 3 (the world voxel alpha byte is a
tag, not opacity), rule 5 (zero validation errors every phase) and the
`EncodeSceneColor` boundary (`CLAUDE.md`, "Lighting is linear"): every new
shading site encodes before writing to an 8-bit target.

### Progress

| Phase | Status | Session / commit | Notes |
|---|---|---|---|
| 0 — Measure the mesh the window would need | TODO | | Go/no-go gate for the whole plan |
| 1 — The window cell mesher, CPU only | TODO | | |
| 2 — The static mesh store and the world raster pass | TODO | | March still on underneath |
| 3 — Destruction: dirty cells re-mesh | TODO | | |
| 4 — Chunk streaming: mesh lifecycle | TODO | | |
| 5 — Flip: raster is primary visibility, the march is off | TODO | | The headline measurement |
| 6 — Sun shadow map from meshes | TODO | | Second-biggest iPad cost |
| 7 — Reclaim: memory, dead machinery, docs | TODO | | ~300 MB VRAM on the table |

---

## Why

`Docs/GPU_PERFORMANCE_GAUGE_2026-08-13.md`, on an iPad Pro 12.9" (4th gen,
A12Z), `Fishing_Village_Beat1`, Release, `--render-quality low`. **Mind the
resolution**: the preset deliberately does not touch ResolutionScale, and on
a mobile build `ApplyPlatformRenderDefaults` has already set it to **0.35**
(and the preset suppresses PlayerPrefs, so nothing overrides it) — so these
GPU passes ran at ~956×717, about 12% of the display's 2732×2048 pixels. The
fixed-resolution passes (Sun Shadow at its 512² map, Post/UI at output
resolution) are unaffected:

| Pass | GPU time |
|---|---:|
| Voxel (per-pixel march of the window) | **50.586 ms** |
| Sun Shadow (per-texel march of the window) | 5.347 ms |
| Post Processing (incl. far-field march) | 2.411 ms |
| UI Renderer | 0.436 ms |
| **Voxel Models (rasterized greedy quads)** | **0.156 ms** |

10.7 fps, decisively GPU-bound, and 86% of the GPU frame is the primary
march — **at 35% resolution scale**. The march is fragment-bound and close to
linear in pixel count (measured on the S23, per the comment in
`Settings::ApplyPlatformRenderDefaults`), so at the full 2732×2048 the same
pass extrapolates to roughly **50.6 × (1/0.35)² ≈ 400 ms**: the gap to a
16.7 ms frame at full resolution is about **25×**, not 3×. No amount of
tuning closes 25× on the same algorithm. The same engine, same device, same
frame rasterizes greedy-meshed voxel quads with the full shared lighting in
0.156 ms (also at 0.35 scale, and characters cover a small screen fraction —
which is why the per-pixel arithmetic below, not this number, carries the
full-resolution estimate). The S23 measurement says the same thing: 48 ms of
Voxel pass at ResolutionScale 0.5 with everything already turned off.

The user's stated requirements, which this plan takes as the acceptance bar:

- **iPad Pro 2020 renders the world at 60 fps at full resolution** (2732×2048).
- The **Intel HD 630** macOS machine reaches 60 fps at its display resolution.
- **The look and feel does not change.** No resolution-scale rescue, no
  temporal upscaler, no "mobile look".
- **Destruction keeps working**, at its current fidelity.

Why the march can never meet that bar on these GPUs, structurally rather than
by tuning: a per-pixel dependent-read DDA loop is the worst shape for a
tile-based GPU (divergent, unbounded, serial loads), it gives up early-Z (the
hit depth needs `SV_Depth`), and its cost scales with pixel count times march
length — which is exactly the "very resolution dependent" behaviour reported.
`Docs/DYNAMIC_MODELS_PLAN.md`'s "Why meshes and not ray marching" table
already made this argument and won it for dynamic models; big voxels spanning
many pixels make it stronger still, because the quad-per-face representation
amortizes better the bigger the voxels are relative to pixels.

**Why the look survives.** The marcher does not produce a "ray-traced look" —
it produces axis-aligned cubes shaded by `ShadeVoxelSurface` with a PCSS
lookup into a sun shadow map, corner AO, cone ambient/bounce against the
pyramid, fog and aerial perspective. Every one of those inputs (world
position, face normal, albedo, the four corner AO values, the shadow map, the
pyramid) is available to a rasterized fragment of the *same cube face*, and
the model pass already proves it: `VoxelMesher.h`'s AO packing reproduces
`AmbientOcclusion.hlsl`'s `GetSkyVisibility` **exactly**, not approximately,
because the merge is keyed so no quad spans differing corner values. Unlike
free-rotated characters, world quads stay world-axis-aligned, so even
`GetShineLine` keeps working. What changes per pixel is who decided coverage:
the rasterizer instead of a DDA. Same cubes, same shading, same image — and
phase 2/5 acceptance is image comparison, not this paragraph.

---

## Ground truth

Verified against the tree at `4548dcc` (chunk streaming phase 8 landed).

### What already exists and is directly reusable

- **`VoxelMesher::BuildFrameMesh`** (`Core/ECS/Systems/Rendering/VoxelMesher.h/.cpp`)
  — greedy mesher producing 3-word packed quads (origin, axis, sign, extents,
  four 2-bit corner AO levels, colour). Currently merges on colour only; the
  AO bake and its merge key are an explicitly recorded gap (DYNAMIC_MODELS
  phase 1 notes, item 2). It walks a `VoxFrame`'s dense voxel array; the
  window mesher in phase 1 needs the same algorithm over a different source.
- **`ModelMeshStore` + `RenderContext::m_pModelMeshMapper`** — the CPU quad
  store and its grow-only GPU mirror (`SyncModelMeshStore`), uploaded inside
  the existing "GPU is not still reading last frame" guard.
- **`VoxelModelPass`** + `VoxelModel.vs/.ps.hlsl` — the instanced no-vertex-
  buffer quad draw (6 vertices × N quad-instances, `SV_VertexID` expansion),
  two render views (colour + linear world-distance depth) plus a real depth
  attachment, composited into both voxel pixel shaders by nearest-depth
  branch. Registers: b0 camera, t0 instances, t1 quad instances, t2 mesh
  quads, t3 pyramid, t4 combined sun shadow map.
- **`ShadeVoxelSurface.hlsl`** — the shared shading tail all three current
  consumers call. A fourth consumer (the world quad shader) is the point.
- **`SunShadowModelPass` + `SunShadowCombinePass`** — quads rasterized into a
  light-space map with a depth attachment making `LESS` the min, then a
  full-screen `min()` combine. This is the exact pattern phase 6 extends to
  the world's own map.
- **The dirty-brick set.** `VoxelEditBatch` already accumulates which bricks
  every destruction/island/bake-on-impact write touched, and
  `VoxelGrid::GetWriteGeneration()` already lets a consumer notice writes
  without being told (`CLAUDE.md`, "Invalidation is polled, not pushed").
- **Chunk CPU voxels are authoritative for static colour.** A static
  renderer's final colour (override, damage, emissive tag) is in the chunk's
  `m_VoxelData`, and decoded chunk voxels carry their damage
  (`StreamingCounters::ChunkInstanceRestamps` must stay zero). The occupancy
  bitmap (`VoxelBrickGrid`, cached CPU memory) answers "occupied?" without a
  PCIe read. A mesher that reads only these obeys the "nothing on a write
  path reads the mapping" rule for free.
- **Dynamic renderers are already gone from the bake** (DYNAMIC_MODELS phase
  3): the static window contains static geometry plus settled debris and
  nothing else. There is no static-eats-dynamic interaction left to re-solve.

### What consumes the per-pixel march today, exhaustively

Grepped by `voxelWorldData` / `SDFMarcher.hlsl` include:

| Consumer | What it does | Fate under this plan |
|---|---|---|
| `VoxelRenderer.ps.hlsl` + `.ShadowLess` | primary visibility + shading of the window | replaced by the world raster pass (phase 5) |
| `SunShadow.ps.hlsl` | one march per shadow texel | replaced by rasterizing the same quads (phase 6) |
| `AmbientOcclusion.hlsl` `GetVoxelAO` | per-hit corner AO from the buffer | replaced by baked corner AO, same algorithm (phase 1) |
| `PostProcessing.ps.hlsl` | endless ground samples the window's y=0 layer; far-field composite | **kept** — see phase 7 for the ground read |
| `VoxelBaker.cs.hlsl` | abandoned GPU baker remains | dead, delete in phase 7 |
| `SDFDepth.vs/.ps.hlsl` | orphaned (no C++ references — the deleted phase 3 prepass) | delete in phase 7 |

The far field (`FarField.hlsl`, marched in post against its **own** coarse
volume) does not read the window buffer and is untouched by this plan: it is
2.4 ms of Post Processing on the iPad *including* everything else post does,
and it covers pixels no mesh will cover. The pyramid (`voxelPyramid` texture)
is fed from the CPU density mirror by `FlushDirty`, not from the mapped
buffer, so cone AO/bounce survive anything this plan does to the buffer.

### Scale

A resident window is 3×3 chunks of 256×128×256 = 75.5 M voxel addresses; a
settled `Fishing_Village_Beat2` holds **4,104,267 active voxels**
(`VOXAGINE_VOXEL_AUDIT`). The number that matters for rasterization is
**exposed faces after greedy merge, excluding the ground layer** (the endless
ground is composited analytically in post and must stay that way — it is the
window's y=0 tiled to the horizon, and meshing it would reintroduce the seam
`RENDERING_PLAN.md` phase 4 already fought). That number is not known and is
exactly what phase 0 exists to measure. For calibration only: the 346
character frames merge 1.89× over exposed faces, and structured architecture
merges far better than organic character shapes.

### The composite convention

The voxel pass is currently the compositor: it takes model colour/depth and
particle colour/depth as inputs and returns nearest-wins. When the march goes
away, so does the compositor — phase 5 has to make the world quads, model
quads and particles resolve against **one shared depth attachment** instead,
with post processing continuing to receive one scene image plus coverage in
alpha (sky / ground / far field fill uncovered pixels there, unchanged).
`RenderPass::Data::m_PassOutput` is an input-only binding
(`VKPassBindings.cpp`) and no pass can today draw into a target another pass
owns — the same limitation that shaped `SunShadowModelPass`. Phase 5 must
either extend `VKRenderPass` to share a depth attachment across the three
passes (small, contained: they are consecutive) or draw all three quad
species in one pass. This is the one place the plan touches engine machinery
rather than reusing it, and it is called out honestly.

### The hazards file

`CLAUDE.md` records the defect classes this plan must not reintroduce. The
ones that bite here: every new shading site must `EncodeSceneColor`; the
`E_STORAGE_BUFFER` register-class trap in `MakeBinding`; work counters, not
timings, gate CI (`Tests/Baselines/perf.txt`); geometry loss is invisible and
must be proven against occupancy counts, not frame times (the phase-9
resumable-Occupy revert is the cautionary tale — its oracle,
`VOXAGINE_VOXEL_AUDIT`'s 4,104,267, is this plan's oracle too, joined by a
new mesh-vs-occupancy audit).

---

## The target, arithmetically

iPad, mobile settings. "Today" is measured; note the resolution each number
was taken at, because they differ. "After" is the estimate **at the full
2732×2048** — the requirement this plan is accepted against:

| | today (measured) | after (est., full res) |
|---|---:|---:|
| Voxel (march) | 50.59 ms **at 0.35 scale** (~400 ms extrapolated to full res) | — |
| World raster pass | — | 4–8 ms (see calibration below) |
| Sun Shadow | 5.35 ms (marched, fixed 512² map) | ~1 ms once rasterized (phase 6) |
| Post Processing | 2.41 ms (already full output res) | ~2.4 ms |
| Voxel Models | 0.156 ms at 0.35 scale | ~1 ms at full res |
| UI + Combine | ~0.5 ms | ~0.5 ms |
| **Total** | — | **est. 8–13 ms** |

The one full-resolution calibration point that exists today is Post
Processing: **2.41 ms at 2732×2048** for a pass that samples the scene,
composites sky/ground, and *marches the far field* for uncovered pixels. A
forward raster fragment doing a shadow lookup and `ShadeVoxelHit` is
comparable arithmetic per pixel without the march, and A-series TBDR
hidden-surface removal shades opaque geometry once per pixel regardless of
overdraw — hence 4–8 ms for the world pass, with real headroom left in the
16.7 budget if it lands at the top of that range. The estimate is written
down so phases 5/6 can be judged against it; nothing in the plan depends on
it being precise, and phase 0 adds a cheap on-device probe to replace it
with a measurement before anything is built on it. The HD 630 has no HSR
and much less bandwidth; it relies on early-Z with rough front-to-back cell
ordering, and it is the platform the vertex-count gate in phase 0 exists
for.

**If this is not feasible, phase 0 is where it says so** — in quad counts and
vertex-rate arithmetic, before anything is built. The two ways it could fail,
and their outs, are in the phase.

---

## The design

### 1. Mesh cells, not one window mesh

The unit of meshing, culling, and rebuild is a **cell**: a fixed-size cube of
the window (32³ to start; phase 0 measures 16³/32³/64³ and picks). Per cell:
a quad range in one grow-only store (the `ModelMeshStore` pattern), rebuilt
whole when any voxel in it changes. Cells are keyed **level-space** — same
lesson as the loose-voxel registry — so a window slide changes which cells
are resident, never what a resident cell means. The ground layer (y = 0) is
excluded from meshing entirely; post's analytic ground remains the one
authority on it.

Quads come out of the same 3-word packed format `VoxelMesher.h` defines, with
one difference: word2 carries the **world buffer's tag-byte colour word**
(rule 3 — alpha is `VoxelStateTag`, so emissive works), not the model
palette's real alpha. The world quad pixel shader branches on
`IsEmissiveVoxel` exactly as the marcher does today.

Corner AO is baked per quad corner at mesh time by the same neighbour test
`GetVoxelAO` runs per pixel today, read from the occupancy bitmap — including
across cell and chunk boundaries. **The merge key is colour AND the four
corner AO values**, closing the recorded DYNAMIC_MODELS gap for both meshers
in one place; a boundary voxel's AO changing (because a neighbour cell
changed) dirties the abutting cell too — one cell of border slop, tracked by
the dirty propagation in phase 3.

### 2. The world raster pass

`VoxelModelPass`'s idiom verbatim: no vertex buffers, 6 vertices × N
quad-instances, per-cell instance records (cell origin — the transform is a
translation, world quads never rotate), CPU frustum cull per cell into the
quad-instance list each frame. Two shaders: a trivial vertex expansion
(cheaper than the model pass — no rotation, no per-instance matrix multiply)
and a pixel shader that is `VoxelModel.ps.hlsl` minus the free-rotation
concessions: `GetShineLine` stays, the emissive branch reads the tag byte,
sun visibility comes from the combined map through the same
`SunShadowLookup.hlsl`.

Back-face culling: quads have defined facing; additionally each cell keeps
its quads bucketed by the 6 face directions, so the CPU cull can skip whole
buckets facing away — this halves vertex work and is the cheap concession to
the HD 630. Cells are submitted roughly front-to-back for early-Z on
non-HSR hardware.

### 3. Destruction rebuilds cells, immediately

`VoxelEditBatch` already knows every dirty brick; end of frame, dirty bricks
map to dirty cells and dirty cells re-mesh **that frame**, before render —
a destroyed voxel must not survive into the image. Re-meshing a cell is a
bitmap-driven scan of ≤32³ voxels in cached memory; phase 1 measures it and
phase 3 sets the budget. If a pathological burst exceeds the budget, cells
re-mesh nearest-camera-first and a far cell may draw one frame stale —
counted (`StreamingCounters` idiom), never silent. The expectation from the
existing numbers (whole-chunk encode of 8.4 M voxels: 2.0 ms budgeted) is
that a typical burst's cells are well under a millisecond total; that is a
number to verify, not to trust.

### 4. Streaming meshes cells where it stamps chunks

The chunk worker already rebuilds the incoming window's voxels into the back
buffer and its occupancy pyramid (`FlushDirtyBackBuffer`); it meshes the
incoming cells there too, from the chunk voxels it is already holding, under
the same ownership rules (no `Entity*`, no main-thread state). `US_COMMIT`
publishes cell residency with the same transaction that moves everything
else. Admitted static renderers' budgeted stamps (`VoxelBaking` budget)
dirty cells exactly as destruction does — the stamp writes through the batch
path, the batch records bricks, the cells re-mesh. R1 readiness folds in
"initial window cells meshed" beside `HasPendingVoxelBakes`.

### 5. What is deliberately NOT built

- **No LOD, no impostors, no occlusion culling.** Frustum + face-direction
  culling only, until a measurement says otherwise. But the LOD path is
  designed so it never has to be re-derived: the user intends wider
  viewports later (smaller voxels on screen), and if visible quad counts
  ever exceed the vertex budget — or merged quads approach pixel size —
  distant cells mesh from a **coarser level of the coverage pyramid**,
  which already stores the same world as occupied fractions at 8³/16³/32³.
  Same store, same pass, same shading; a cell just carries which level it
  was meshed from. That is a future phase gated on a measurement, not part
  of this plan.
- **No second code path per platform.** The raster path replaces the march
  everywhere — desktop, editor, mobile — behind one transition (phase 5), with
  the march kept only as long as verification needs it. Graphics quality
  remains runtime settings; no `#ifdef` (project rule).
- **No change to particles, far field, post, UI, physics, integrity,
  the pyramid, or the audits.** Destruction's data path (`VoxelEditBatch`,
  CPU voxels, owner slots, brick counts) is untouched — the mesh is a seventh
  *derived* representation with its own audit, not a new authority.

---

## Phases

### Phase 0 — Measure the mesh the window would need

Nothing ships. Build a throwaway offline pass (a `--mesh-census` launch mode
or a `voxagine_tests` benchmark reusing the harness) that loads each level's
full window the way `FarFieldBaker` loads chunks, meshes every cell with the
existing greedy algorithm (colour-only key is fine for a census), and
reports: quads per level (total / per cell histogram / worst cell), bytes,
per-cell mesh time on this machine, all at 16³/32³/64³ cell sizes, ground
layer excluded. Sweep all shipped levels; `Fishing_Village_Beat1`/`Beat2`
and the castle valley are the decision cases.

Then the arithmetic, written into this file: worst-case visible quads, × 6
vertices, × 60 fps, against the HD 630 (the weak vertex machine) and the
A12Z. **The sizing case is the entire resident window in frustum (minus
face buckets), not today's camera pose** — the user intends wider viewports
in the future (smaller voxels on screen), and a wider view grows exactly
this number while leaving fragment cost untouched; the window is the hard
upper bound on it, because everything beyond the 3×3 chunks is far field,
which is per-pixel in post and does not care how much of it is visible.
Report today's pose too, as the typical case. And the memory: quad store
bytes against the 302 MB the window buffer costs today.

Also the **on-device fragment-cost probe** the estimate table leans on: one
iPad run at ResolutionScale 1.0 (add a `--resolution-scale` launch option
beside `--render-quality` if none exists — same rationale, it is a
measurement lever), recording the per-pass times at full 2732×2048. The
Voxel pass will be catastrophic and that is fine — the numbers that matter
are Post Processing, Voxel Models and UI at full resolution, which bound
what a full-screen rasterized fragment workload actually costs on this GPU
and replace the 4–8 ms guess with a measurement.

**Acceptance:** the table, and an explicit go/no-go. **No-go looks like:**
(a) visible quad counts putting the HD 630 vertex-bound past budget even
with face buckets — the out is coarser merge, 64³ cells, or accepting
resolution scale on that one machine and saying so to the user; or (b)
per-cell mesh times that make same-frame destruction rebuild impossible —
the out is the naive-faces-first/greedy-later split, costed here. If both
outs fail, this plan stops and says the iPad target needs a different
approach; nothing has been built.

### Phase 1 — The window cell mesher, CPU only

`WindowCellMesher` (name TBD) over chunk CPU voxels + occupancy bitmap:
greedy merge with **colour + corner AO** as the key, tag-byte colour word
carried through, cross-cell/cross-chunk neighbour reads for AO, ground layer
excluded, non-resident neighbours treated as empty (matches what the marcher
shows at the window edge today). Shares the sweep/merge core with
`VoxelMesher` rather than copying it — one greedy algorithm in the tree.
While here, add the AO bake to the *model* mesher's merge key too (the
recorded gap), or record explicitly why not.

Tests in `voxagine_tests` (`checks` + a `Rendering/` or `VoxelMeshing/`
scenario dir): a cube, a hollow shell, colour boundaries, AO corner cases
including the diagonal-flip seam, a cell-boundary-spanning surface, an
emissive voxel, a chunk-boundary surface. Work counters into `perf.txt`:
cells meshed, quads emitted, voxels scanned. `VOXAGINE_MESH_AUDIT=<seconds>`:
every exposed non-ground face covered exactly once by a quad of the right
colour/AO, every quad backed by solid voxels — the mesh-vs-occupancy oracle,
headless, no camera dependence (the same design point as Validate Occupancy
Bricks).

**Acceptance:** tests green, audit clean on a real level, measured per-cell
mesh cost recorded, quad counts consistent with phase 0's census.

### Phase 2 — The static mesh store and the world raster pass

The grow-only cell store + GPU mapper (`SyncModelMeshStore` pattern), the
pass, the two shaders, per-cell frustum + face-bucket culling, submitted
through the existing model-colour/model-depth composite inputs of the voxel
pixel shaders — **the march still runs underneath**, exactly the
DYNAMIC_MODELS phase 2 trick: the image should not change (same cubes, same
shading, nearest-wins picks whichever drew), which is what makes the pass
verifiable at all. Zero validation errors; rule 1's register contract
documented in the pass header.

**Acceptance:** capture parity — mean-luminance and neighbour-difference
metrics at the `RENDERING_PLAN.md` verification crops, marched-only vs
raster-on-top, within noise, on the valley level and both Fishing Village
beats; zero validation errors; pass GPU time at the standard vantage
recorded on desktop. Joey looks at one live scene (this is a look-preserving
plan; his eyes are the standard the metrics approximate).

### Phase 3 — Destruction: dirty cells re-mesh

Dirty-brick → dirty-cell mapping (including the AO border-slop propagation),
same-frame budgeted rebuild, nearest-first ordering, stale-cell counter.
Scenario suite gains a sixth invariant: after every scenario, the mesh audit
agrees with occupancy (this is what makes a silently-dropped face a CI
failure, not a play-session discovery). `perf.txt` gains cells-remeshed /
quads-emitted counters for the destruction gauntlet.

**Acceptance:** all scenarios green with the new invariant; measured re-mesh
cost of the worst gauntlet burst; stale-cell counter zero across the
gauntlet at the default budget; audits (`VOXAGINE_VOXEL_AUDIT`, `SYNC`,
`MESH`) clean during live destruction.

### Phase 4 — Chunk streaming: mesh lifecycle

Worker-side meshing of the incoming window's cells beside the back-buffer
build; commit publishes residency; unload drops cell ranges (grow-only store
gets a free-list or compaction — decide by measuring fragmentation over the
17-level slide test); admitted-stamp dirtying; R1 folds "cells meshed".
`StreamingCounters`: cells meshed per group, re-used vs allocated.

**Acceptance:** the full-level slide test (the 17-level sweep) with the mesh
audit clean after every settled window; transition frame times not regressed
(`gpu_chunk_streaming_frame_budget` still passes); loading-screen flow
(phase 8) still shows the world at activation — readiness now includes
meshes, measured cost of that recorded.

### Phase 5 — Flip: raster is primary visibility, the march is off

The composite restructure (shared depth across world/model/particle quads,
or one combined pass — see "The composite convention"), post processing
keeps receiving one scene image + coverage alpha; the voxel pass's world
march, the AABB proxy submission for static renderers, and the loose-voxel
proxy registry stop running (loose voxels are just voxels in cells now).
A `VOXAGINE_MARCH_PRIMARY=1` escape hatch keeps the old path buildable for
one release cycle of A/B and bisection, then dies in phase 7.

**Acceptance:** capture parity as phase 2, now raster-only vs the old
marched reference, plus the hard crops (valley walls, enclosed geometry —
"verify at the case that is hard"); `VOXAGINE_COVERAGE_AUDIT`'s successor is
the mesh audit (record the handover); **the iPad and HD 630 measurements**,
full resolution, same scene and vantage as the gauge doc — this is the
number the whole plan exists for; desktop Release not regressed. Joey plays
it: destruction, a window slide, a level transition, on screen.

### Phase 6 — Sun shadow map from meshes

`SunShadow.ps.hlsl`'s per-texel march replaced by rasterizing the static
cell quads into the light-space map — `SunShadowModelPass`'s exact pattern,
ideally unified with it so world + model quads render into one map and the
combine pass retires. `SunShadowLookup.hlsl` (PCSS, tuned, load-bearing)
does not change; only the map's producer does.

**Acceptance:** `--screenshot-pass "Sun Shadow"` parity between marched and
rasterized maps on the standard scenes; shadow pass GPU time on iPad and S23
before/after (the S23 numbers say 21.5 ms at 0.5 scale / 8.3 ms at 512 —
this phase should make the 512 map cost trivial and let mobile keep sharp
shadows); Joey judges a moving character's shadow and a destruction burst's
shadow change on screen.

### Phase 7 — Reclaim: memory, dead machinery, docs

- Decide the mapped window buffer's fate. After phases 5/6 its remaining
  readers are post's endless-ground y=0 sampling and the editor audits. Give
  post a dedicated y=0 layer strip (768×768 words = 2.25 MB) or a small
  dedicated buffer, keep the full buffer only where an audit runs, and
  **measure the ~302 MB** back on mobile — against the HD 630's 1.5 GB
  budget this is not a nicety. `VoxelBaker`'s GPU-word writes become
  editor/audit-only or die; the CPU-side authorities (chunk voxels, bitmap,
  bricks, owners, pyramid mirror) are untouched.
- Delete: `SDFDepth.*`, `VoxelBaker.cs.hlsl`, the march branches of the two
  voxel pixel shaders (or the shaders whole if the composite moved), the
  escape hatch, the static-proxy machinery, `MARCH_STEP_BUDGET`'s
  primary-ray role (the far field keeps its own).
- Rewrite the affected `CLAUDE.md` sections from "how it works" to "how it
  used to work", the way DYNAMIC_MODELS phase 6 promised and deferred.

**Acceptance:** full suite green, work baselines re-recorded with the commit
saying why, audits clean, peak RSS / VRAM before-after table, docs updated.

---

## Rejected / out of scope

- **Optimizing the march instead.** The constant-percentage lesson
  (`RENDERING_PLAN.md` phase 3, the deleted depth prepass) plus the S23/iPad
  numbers: the march is 3× over budget with every quality lever already off,
  at reduced resolution on the S23. Tuning does not close 3×; the shape is
  wrong for the hardware.
- **Resolution scale / temporal upscaling as the answer.** Explicitly against
  the stated requirement (full resolution, look preserved). ResolutionScale
  stays a *setting* for weaker devices than the targets, not the plan.
- **A per-platform renderer fork.** Two primary-visibility paths maintained
  forever, and the project rule is runtime settings, not build flags. The
  raster path must win on desktop too (it will — 2.4–3.5 ms of voxel pass at
  4K is the number to beat there) or phase 5 does not land.
- **Meshing the ground layer.** Re-introduces the window-boundary seam the
  far-field work solved; the analytic ground stays.
- **GPU-driven culling / meshlets / bindless.** This engine's idiom is
  procedural draws from structured buffers; the cell granularity gives the
  CPU cull everything it needs at 4-figure cell counts. Revisit only if
  phase 0's vertex arithmetic fails on the HD 630.
- **A signed-distance or sparse-octree re-architecture.** A rewrite of the
  world representation to accelerate a marcher this plan is removing.
- **Compute-shader marching / tile classification.** Same dependent-read
  loops, same divergence, now without early-Z at all.
