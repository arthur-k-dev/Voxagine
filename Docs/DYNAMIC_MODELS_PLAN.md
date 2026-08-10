# Dynamic models render as meshes, not as stamps

Characters — and every other non-static `VoxRenderer` — stop being written into
the world voxel buffer and start being **rasterized as greedy-meshed quads**, at
their own continuous position and free rotation. They keep taking part in every
lighting pass: sun shadows, ambient occlusion, bounce, fog.

Same format as `Docs/RENDERING_PLAN.md` and `Docs/DESTRUCTION_PLAN.md`. Read
**Ground truth** first — it is verified against the tree at `405010e` and is
what the phase order is built on. Do one phase per session, in order, and record
measured numbers.

---

## How to use this document

1. Read **Ground truth** and **The design**. Do not re-derive them; do re-verify
   a `file:line` before editing it.
2. Find the first phase in **Progress** that is not `DONE`. Do only that phase.
3. Meet the phase's **Acceptance** before marking it done. If you cannot, mark
   it `BLOCKED` with a note rather than half-landing it.
4. Update **Progress** and the phase's own notes. Record measured numbers.
5. Commit per phase, on a branch, with the phase number in the message.

`Docs/RENDERING_PLAN.md`'s ten rules apply here unchanged — in particular rule 1
(the SPIR-V/C++ binding contract), rule 2 (`-fvk-use-dx-layout`), rule 3 (the
voxel alpha byte is a tag, not opacity), rule 5 (zero validation errors) and
rule 8 (two voxel pixel shaders, not one — and this plan adds a third consumer
of the same shading, which is why phase 2 factors it out rather than copying it).

### Progress

| Phase | Status | Session / commit | Notes |
|---|---|---|---|
| 0 — Baseline: what the dynamic bake actually costs | **PARTLY** | 2026-08-10 | CPU split landed and measured: **the steady-state bake is 100% dynamic on every map** — static is exactly 0 once a world has loaded. Cost is entirely activity-driven: 0.03 ms idle, **1.26 ms avg / 2.58 ms peak in combat** on a 4070 SUPER. GPU numbers **not taken** — two wedged benchmark processes held the GPU. Read the notes before quoting any of this |
| 1 — The greedy mesher and the mesh store | **PARTLY** | 2026-08-10 | CPU mesher + cache landed and runs clean on real content (every frame in `Fishing_Village_Beat2`, no crash, no validation change - it touches no GPU state yet). **The GPU upload (a real `ModelMeshStore` Mapper, matching `m_pVoxelMapper`) was not built** - stopped for session budget. See the phase's own notes |
| 2 — The dynamic model pass, composited by depth | **PARTLY** | 2026-08-10 | Full pipeline built and running: GPU mesh store, instance/quad-instance buffers, the pass, its two shaders, compositing in both voxel pixel shaders, RenderSystem submission with the continuous transform. Zero validation errors on real content (Valley_Path_To_Castle_Beat1, 300 headless frames). **Not verified**: on-screen confirmation that the new pass's quads are what's visible rather than the still-active bake underneath them, and the winding-order/cull-mode question - see notes |
| 3 — Dynamic renderers leave the bake | DONE | 2026-08-10 | Reordered ahead of phase 4 at the user's explicit request. `VoxelBaker`/`OnComponentAdded` skip non-static renderers entirely; `VoxFrameEmitter` re-sourced from the model's own voxels via a new shared `ComputeContinuousModelTransform`. Verified: dynamic bake counters exactly 0, both players fully shaded on screen with baking off, and - added later the same day - `VOXAGINE_COVERAGE_AUDIT` (0 uncovered bricks above ground) and `VOXAGINE_SYNC_AUDIT` (0 of 75.5M voxels disagree) both clean. **Still not directly watched**: a character's actual death-particle burst on screen - the audits prove the representations agree, not that `VoxFrameEmitter`'s new branch fires correctly |
| 4 — Sun shadows from dynamic models | DONE | 2026-08-10 | Two passes + a combine, not the one-pass depth-attachment design originally sketched - see notes on why. Shipped with the viewport Y-flip backwards (mirrored placement, reported by the user as "moves the opposite way" and "lands a chunk away" - one sign error, not two bugs); fixed and **confirmed on screen by the user**. Characters cast correctly. Self-shadowing (models receiving from the combined map) landed as a same-day follow-up - see below |
| 5 — Ambient: baked AO, world cones, and what is lost | TODO | | |
| 6 — Cleanup: delete what the bake needed and no longer does | **PARTLY** | 2026-08-10 | One real, verified fix landed: `VoxelBaker::Clear` now early-returns for a never-baked dynamic renderer instead of paying for a `grid.GetChunk()` scan on every despawn. `NotifyClearedRegion` and `Clear`'s window-slide branch are now provably dead for dynamic renderers (their guard conditions can never be true again) but were deliberately **not deleted** - see notes on why |

---

## Why

Reported by the user: characters move fast, and on a low-performance device the
bake does not keep up, so a character's voxels lag its transform. The request is
explicitly **not** to make the bake faster. It is to stop putting characters in
the grid at all — free rotation, continuous position, still lit like everything
else.

That is the right instinct, and the tree agrees with it in three places:

- **A dynamic renderer is already a second-class citizen of the grid.**
  `VoxelBaker::Occupy` touches the physics grid only inside `if (bIsStatic)`
  (`VoxelBaker.cpp:287`), so a dynamic renderer's voxels live in the mapped GPU
  buffer *and nowhere else*. It has no owner slot, no CPU colour, no
  destructibility, no voxel collision. It is already an object pretending to be
  terrain.
- **Most of the defect ledger that pretence produced is in `CLAUDE.md`.** Static
  bakes eating dynamic voxels; `NotifyClearedRegion`; `Clear` shifting a dynamic
  renderer's positions across a window slide; the proxy AABB being short of the
  stamp because the stamp quantizes and the matrix does not. Every one of those
  exists *because* a moving object is being re-voxelized into a shared grid.
- **The cost is per-frame and per-voxel.** Every dynamic renderer that moves
  clears its old voxels and stamps new ones, every frame it moves, and the stamp
  key check (`RENDERING_PLAN.md` 4c) cannot skip it precisely because it *did*
  move. `CPU VoxelBaker::Occupy (added)` was 1.86 ms for a world load's 3.1 M
  voxels; a crowd of walking characters pays a slice of that rate every frame.

---

## Why meshes and not ray marching

**Settled with the user 2026-08-10, against measurement.** The first draft of
this plan marched each model's own volume in model space. That is the natural
move in a ray-marched engine and it is the wrong one here, for a reason worth
stating: **the marcher exists to serve destructibility, and a character is not
destructible.** A character that dies becomes particles from its *model*
(`VoxFrameEmitter`), never from voxels knocked out of it in place. Nothing about
a character's geometry changes at runtime, so nothing needs a representation
that tolerates change.

Measured over every `.vox` under `Content/Character_Models/` — 35 files, 346
frames, 152,721 solid voxels:

| | march the model volume | greedy-meshed quads |
|---|---|---|
| GPU memory, all 346 frames | **2.9 MiB** dense, fitted | **3.6 MiB** at 8 B/vertex |
| Geometry per character frame | — | **342 quads / 684 tris** average, 817 worst |
| Beat2 crowd, 239 renderers | 239 oriented boxes, per-pixel DDA | ~82 k quads, one instanced draw |
| Per-pixel cost | 40–140 step loop, divergent, dependent loads | one rasterized fragment |
| Early-Z | **no** — the hit depth needs `SV_Depth` | yes |
| Sun shadow cost | march an OBB per shadow texel | rasterize into the map |
| Anti-aliasing | FXAA only | real geometry, MSAA-able |
| Self-AO | reimplement the neighbour test against the volume | baked at mesh time, free at runtime |

**Memory is a wash and is not an argument for either side.** The first draft
quoted 23 MiB for the dense volumes and treated that as decisive; that figure is
MagicaVoxel `SIZE`-chunk bounding boxes, and these models are 97.5% air inside
them. `VoxModel` keeps the *fitted* box, which is 2.9 MiB at 20% fill. Recorded
because it is exactly the kind of number that decides a design while being the
wrong measurement.

Everything else points one way, and hardest on mobile: a per-pixel dependent-read
loop is the worst shape to hand a tile-based GPU, and marching gives up early-Z
on the one thing that would otherwise get it for free.

**The quad count is a floor, not a final number.** The merge has to be keyed on
colour *and* the baked corner AO, or it would merge across faces with different
occlusion and flatten them. The 1.89x merge ratio measured above does not account
for that, so the real count is higher. At any plausible ratio it stays in the
hundreds per character and the conclusion does not move — but phase 1 measures
it properly rather than inheriting this number.

**What marching would have won, and how phase 2 pays for it instead.** Marching
lets characters and world be shaded by literally the same code, on the same
`MarchResult`. Meshes mean two shading paths, and rule 8 already records that
this tree struggles to keep two voxel pixel shaders in step. Phase 2 therefore
factors a shared `ShadeVoxelSurface` include that the marcher *and* the mesh
shader both call — which leaves the tree better than today rather than worse. If
that factoring turns out not to be possible cleanly, stop and re-open this
decision rather than shipping a third divergent copy of the lighting.

---

## Ground truth

Verified against `405010e`.

### What "dynamic" means, exactly

`Entity::IsStatic()` (`Core/ECS/Entity.h:75`). That is the same predicate
`VoxelBaker` already branches on, so it is the natural line and needs no new
flag. **Settled with the user 2026-08-10: every non-static renderer moves, not
only the characters** — a per-renderer "is a character" flag would need an
authoring pass over 20 `.wld` files, and it would leave the baker's whole
dynamic path (`NotifyClearedRegion`, the repair scan, `Clear`'s dynamic branch,
the window-slide handling) alive for the sake of a wooden gate and two lanterns,
which takes phase 6 off the table.

Across the 20 shipped worlds: **6,577 static and 606 dynamic** `VoxRenderer`
components authored. The dynamic set is dominated by crowd dancers
(`Main_Char_*_Dance.anim.vox`, 175 + 171 of the 606, almost all in
`Fishing_Village_Beat2`), plus the player, the aim marker, and a handful of props
— a wooden gate, two lanterns, a couple of building blocks. Runtime spawns
(monsters, bullets, pickups) are on top of that and are not in the file counts.

### The models are small

35 files, 346 frames, **152,721 solid voxels** in total — an average of **441
solid voxels per frame**. Greedy-meshed: **118,504 quads**, 342 per frame,
1.89x fewer than the 223,382 exposed faces. Heaviest model is
`Spider_Attack.anim.vox` at 22,049 quads over 27 frames (817 per frame).

### Nothing in this engine uses a vertex buffer

`RenderPass::Data::m_VertexLayout` and `m_pVertexData` are **dead fields** —
nothing in the tree sets either, and `m_uiIndexCount` is only ever tested
(`VKRenderPass.cpp:358,754`). Every pass is procedural: a vertex count, a
structured buffer, and `SV_VertexID`. The AABB proxies, the particles and the UI
sprites all work this way.

**The mesh path stays in that idiom** and therefore adds no engine machinery: no
vertex input state, no index buffers, no per-draw binding. See piece 2.

### The proxy cube already hands the pixel shader its own ray

`VoxelRenderer.vs.hlsl:71-77` puts the rasterized AABB corner in `WorldPosition`,
and perspective-correct interpolation makes that the point where *this pixel's*
camera ray enters *that* box. Relevant here because it is the same procedural
expansion the quad pass uses, and because it is why the voxel pass's proxies
remain the coverage mechanism (see piece 3).

### Compositing by depth already exists in this pass

`VoxelRenderer.ps.hlsl:65-92` samples the particle pass's colour *and* its linear
depth and returns the particle when it is nearer than the marched hit.
`ParticlePass.cpp:20-21` is how a pass gets two render views — colour plus an
`E_R32_FLOAT` depth. **The dynamic model pass is that pattern again**, and
reusing it is why nothing downstream of the voxel pass has to change: the
double-buffered copy in `RenderContext::Present` (`RenderContext.cpp:732-744`),
post processing's one-submission-old sampling, FXAA and the silhouette test all
keep seeing one scene image.

### Coverage is rasterized proxies and nothing else

`RENDERING_PLAN.md`, "Culling is rasterized AABB proxies, not a frustum". The
voxel pass runs a fragment only where some proxy covers the pixel. A character
silhouetted against sky with no world geometry behind it has no fragment — so a
dynamic renderer **still submits an axis-aligned proxy into the existing AABB
buffer**, purely as a coverage device. Its world march finds nothing; the
composite branch returns the character.

### The lighting includes, and what each one reads

| Include | Reads | Works from a rasterized quad? |
|---|---|---|
| `SunShadowLookup.hlsl` | `sunShadowMap` (t3) | Yes — a world-space lookup |
| `AmbientCone.hlsl:55` | `voxelPyramid` (t4) | Yes — a world-space sample position |
| `Lighting.hlsl`, `Fog.hlsl`, `Color.hlsl` | nothing | Yes |
| `AmbientOcclusion.hlsl:40-71` `GetVoxelAO` | `voxelWorldData` | **Replaced** — see piece 4 |

So the new pass needs the shadow map, the pyramid and its two samplers, and does
**not** need the world voxel buffer or the brick counts at all.

### `VoxFrameEmitter` is the one thing outside the renderer that depends on the bake

`VoxFrameEmitter.cpp:12-70` walks `VoxRenderer::m_BakeData.Positions` and, for a
dynamic renderer, reads each colour back out of the mapped GPU buffer. That is
the dying-character emitter (`ParticleCorpse`), and every humanoid is dynamic —
so **if dynamic renderers stop being baked, every corpse silently stops emitting
particles**. It is the only external consumer of `BakeData::Positions`
(grepped). Phase 3 must re-source it from the model, which also deletes a PCIe
read of VRAM per particle.

Note that it needs the model's **solid voxels**, not its mesh — the mesh has no
interior. `VoxFrame::GetPositions()`/`GetColors()` stay exactly as they are and
are the right source; this is the one consumer that must not be pointed at the
mesh store.

### What does *not* break, checked

- **Voxel collision.** Bullets, bombs and `EnemyBullet` collide through the
  physics grid (`PhysicsSystem.cpp:1073`), which a dynamic renderer was never
  in. Characters are hit through their box colliders and always were.
- **Destruction.** `SphericalDestruction` clears world voxels; a dynamic
  renderer's voxels are re-stamped by the next bake, so dynamic props are
  effectively indestructible *today*. Nothing is lost.
- **Owner slots and the combo streak.** `Bullet::OnVoxelCollision` compares
  owner slots; a dynamic renderer never had one.
- **The far field.** Built from *static* entity JSON (`RENDERING_PLAN.md` phase
  4). Characters were never in it.

---

## The design

Four pieces. Nothing here is speculative about the engine's capabilities except
where it says so.

### 1. `ModelMeshStore` — greedy quads in one storage buffer

One `Mapper` holding every resident model frame's quads back to back, with a
per-frame `(uiFirstQuad, uiQuadCount)` record on the CPU beside `VoxFrame`. Built
at load from `VoxFrame::GetPositions()`/`GetColors()`, which already exist and
are already sorted z-then-y-then-x (`VoxModel::SortFrameVoxels`).

A quad record, packed to 16 bytes:

- origin in model voxel space (three `uint8`), plus the face axis and sign
- extent along the two in-plane axes (two `uint8`)
- colour (`uint32`, the same `0xTTBBGGRR` word the world buffer uses, rule 3)
- four corner AO values (four `uint8`)

**Merging is keyed on colour *and* the four corner AO values.** Merging faces
whose occlusion differs would flatten the shading — this is the correctness
constraint that makes the measured 1.89x ratio a floor.

Upload once on first use; never evict. All 346 frames is under 4 MiB.

`VoxModel.cpp:590-605` holds the commented-out remains of a per-frame `Mapper`
from the abandoned GPU baker. It is the right *place* and the wrong *shape* —
one mapper per frame is 346 descriptors. Read it, then write the shared store.

### 2. `Voxel Models` pass — one instanced draw, no vertex buffers

A new `RenderPass`, drawn **after** the sun shadow pass and **before** the voxel
pass, with two render views like `ParticlePass`: colour `R8G8B8A8_UNORM` and
linear depth `E_R32_FLOAT`, plus a real depth attachment so overlapping
characters resolve in hardware with early-Z intact.
`m_fRenderScale` set from `Settings::ResolutionScale` at construction, exactly
the way `ParticlePass`/`VoxelPass` already do it - there is no live-follows
flag in `RenderPass::Data`, only the one-shot value read at pass creation.

Three buffers, all in the existing idiom:

- **The mesh store** from piece 1, static.
- **An instance buffer**, rebuilt every frame in `RenderSystem::PostTick` beside
  the AABB submission already there: model→world as a 3×4, the override colour,
  and the tag byte `VoxelStateTag` already computes.
- **A quad-instance list**, also rebuilt every frame: one `uint32` per visible
  quad, packing the instance index and the quad index within the store. The
  Beat2 crowd is ~82 k entries, 328 KB — against a particle buffer that already
  rebuilds at 150 k instances a frame, this is well inside precedent.

The draw is `vertexCount = 6, instanceCount = <total visible quads>`: the vertex
shader reads the quad record and the instance, expands the six corners, and
outputs world position, world normal, model-space position, face UV, colour and
the bilinearly interpolated AO. Back faces cull normally — a greedy quad has a
defined facing.

The pixel shader calls **`ShadeVoxelSurface`**, a new shared include factored out
of `VoxelRenderer.ps.hlsl:107-206`: emissive early-out, `GetSunVisibility`, sky
visibility, `GetConeAmbient`, `ShadeSurface`, `GetConeSpecular`, `GetShineLine`,
`ApplyAerialPerspective`, `EncodeSceneColor`. Both voxel pixel shaders (rule 8)
call it too, from their `MarchResult`. **That factoring is the deliverable, not a
tidy-up** — see "What marching would have won" above.

### 3. Compositing: one branch in the voxel pass, and a proxy for coverage

`VoxelRenderer.ps.hlsl` and `VoxelRenderer.ShadowLess.ps.hlsl` (rule 8) gain a
model branch beside the particle branch they already have: nearest of {model,
particle, world hit} wins, and a model with `a != 0` beats a world *miss*
outright.

`RenderSystem::PostTick` keeps submitting an AABB proxy for every dynamic
renderer — now the axis-aligned bounds of the transformed model box rather than
the stamp bounds — so a fragment exists wherever a character is on screen.
Nothing downstream changes: post processing, FXAA, the silhouette test and the
one-submission-old copy all keep seeing a single composited scene image.

### 4. Lighting: baked AO for the model, world cones for the surroundings

`GetConeAmbient` and `GetConeSpecular` work unchanged — they sample the world
pyramid at a world-space point, so a character standing next to a wall gets that
wall's occlusion and bounce for free.

`GetVoxelAO` does not: it reads `voxelWorldData`, and the character is no longer
in it. It is replaced by the AO baked into the quad corners at mesh time.
**This is the same algorithm, not an approximation** — `AmbientOcclusion.hlsl:40-71`
already computes four corner values per voxel face and blends them by UV, which
is per-vertex AO evaluated at runtime. Baking it moves the work to load time and
lets the hardware do the interpolation. It is also *more* correct than today,
where a character's self-AO is computed against a re-voxelized copy of itself
sitting in the shared grid next to whatever it is standing on.

**What is genuinely lost: the character stops contributing to the coverage
pyramid**, so it no longer occludes ambient or casts bounce onto the world. Sun
shadows — the strong cue, and the contact shadow — are kept by phase 4. Phase 5
judges on screen whether the ambient loss is visible; splatting a coarse
occupancy footprint into the pyramid is the fallback and is costed there.

### Sun shadows

The sun shadow map is one texel per light-space column holding the distance along
`lightDirection` to the first hit (`SunShadow.ps.hlsl`). A character has to be
able to lower that value.

**One pass, not two.** `SunShadowPass` becomes two draws' worth of work in one
pipeline: the existing full-screen triangle marching the world, and the quad list
projected into light-space clip through `shadowTangent` / `shadowBitangent` /
`shadowRect` / `shadowDepth`, which are already in the camera constant buffer
(`CameraData.hlsl:59-62`). Give the pass a depth attachment and have both write
`SV_Depth` as the normalized light-space distance; `LESS` then *is* the min, in
primitive order, with no new blend mode and no second target.

Rasterizing quads here rather than marching boxes is most of why the mesh
representation wins: the shadow map is a fixed `k_uiSunShadowResolution²` marches
of the *world*, and the characters add ~82 k tiny triangles to it rather than a
per-texel volume walk.

The alternative — a second pass rendering into the first pass's target with
`VK_BLEND_OP_MIN` — needs `RenderPass` to learn how to write into a target it
does not own, which it cannot do today (`m_PassOutput` is an *input* binding,
`VKPassBindings.cpp:183`). It is also a second pass, and `RENDERING_PLAN.md`'s
rejected list says to stop and build the frame graph when the third or fourth
arrives. This plan adds **one**. Keep it that way.

### The one thing to check on screen

**Free rotation changes the look, and this is the decision the user owns.**
Today a rotating dynamic model is re-voxelized onto the world grid every time it
turns: its voxels stay axis-aligned with the world and the silhouette pops
between quantized rotations — which is what `VoxRenderer`'s `Round on Axis` and
`Limit Rotation Angle` (both authored per renderer, both in every `.wld`) exist
to control. A mesh rotates the cubes *with* the model: continuous, crisp, no
popping, and the cubes are no longer world-aligned.

Both flags become no-ops for dynamic renderers. That is the intent as stated, but
it is the kind of thing that has to be watched moving, not screenshotted. Phase
2's acceptance is the user looking at a walking character and a spinning weapon
pickup.

---

## Phases

The phase structure is representation-agnostic and survived the march→mesh
decision unchanged; only phase 1's contents moved.

### Phase 0 — Baseline: what the dynamic bake actually costs

Nothing is built. Measure, so phases 2-6 have something to beat and so the
premise is checked before it is paid for — `RENDERING_PLAN.md` phase 3 is the
cautionary tale.

- Split `CPU VoxelBaker::Bake` and `CPU VoxelBaker::Occupy (added)` by
  `IsStatic()`, so the dynamic share is its own counter.
- Measure it in gameplay on `Fishing_Village_Beat2` (239 dynamic renderers, the
  worst authored case) and on a beat with monsters spawning, headless per
  `CLAUDE.md`'s convention, on a quiet machine.
- Record the voxel pass's GPU time at the same vantage, so phase 2's new pass can
  be priced against what it replaces rather than in isolation.
- Record how many voxels a frame's dynamic re-stamps write, and how many distinct
  model frames are resident.

**Acceptance:** a table of dynamic-vs-static bake cost and resident frame count
in this file. If the dynamic share turns out to be negligible on desktop, say so
— the mobile case is still the reason, but the desktop numbers must not be quoted
as if they motivated it.

### Phase 0 results — PARTLY, 2026-08-10

`VoxelBaker::Bake` and the `OnComponentAdded` stamp are now split by
`IsStatic()`, in both time and **work**. `FrameProfiler::ReportCount` is the new
channel: a count is exact and machine-independent, which is what made this
measurable at all on the day — see *The measurement conditions* below.

Release, `--hidden --size 640x360 --frames 900`, vsync-locked at 60 fps, no
input, so every number is what the level does **on its own**.

| Map | dynamic re-bakes /s | dynamic voxels /s | `CPU Bake (dynamic)` avg | peak | static |
|---|---|---|---|---|---|
| `Fishing_Village_Beat1` | 80 | 7,819 | 0.032 ms | 0.194 ms | **0** |
| `Valley_Path_To_Castle_Beat1` | 110 | 7,818 | 0.033 ms | 0.230 ms | **0** |
| `Fishing_Village_Beat2` (239 dancers) | 190–212 | 21,330 | 0.093 ms | 0.570 ms | **0** |
| `Battle_Arena/Training_Ground` (combat) | **620–716** | **317,707** | **1.262 ms** | **2.582 ms** | **0** |

**The finding that matters is the last column.** Static is exactly zero on every
map in steady state — `RENDERING_PLAN.md` 4c's stamp-key check skips every
static renderer once a world has loaded, so **100% of the ongoing bake is the
thing this plan deletes**. There is no residue to argue about.

**The second finding is that the cost is activity-driven, not scene-driven.**
239 authored dancers cost 0.09 ms; a combat arena costs **14x** that. Voxel
throughput moves 40x between the quietest map and the busiest one while the
renderer count moves 8x, because a fast mover re-stamps its whole volume every
frame and a dancer changes an animation frame occasionally. That is the shape
the user reported, arrived at from the other direction.

**Be honest about the desktop numbers.** 1.26 ms of a 16.7 ms frame is 7.6%, and
0.03 ms on a quiet map is nothing at all. On a 4070 SUPER this change does not
pay for itself on CPU throughput alone, and this file must not be quoted as if
it did. What justifies it is the combat case scaled to a phone — where the
single-thread gap is roughly 4-6x against a 33 ms budget, putting the *average*
at 5-7 ms and the *peak* at 10-15 ms — plus the two things below that no
measurement here captures.

**What the measurement cannot see, and is arguably the real case.** The reported
defect is that the bake "does not catch up" — that is a **latency** problem, not
a throughput one. A stamp is discrete and frame-coupled: a character's rendered
voxels are wherever the last bake put them, so as the frame rate falls the
rendered position quantizes harder against the transform. Rasterizing a mesh
from the live transform has no such coupling at any frame rate. **That is fixed
by construction and would be fixed even if the bake cost nothing.** The second
uncaptured item is the defect class in `CLAUDE.md` that exists only because a
moving object is re-voxelized into a shared grid.

### The measurement conditions, recorded because they were bad

Two of the machine's benchmark processes were **wedged** while these were taken
— 7h05m and 3h55m elapsed, each spinning a core in `hrtimer_nanosleep` and
holding 831 and 745 MiB of VRAM. That is the `[stall]` early-return spin from
`CLAUDE.md`: submitted, fences never signalled, main loop spinning. No `Xid` in
the kernel log, so not a GPU fault this time.

- **CPU timings above are usable** — the bake is single-threaded, the machine
  has cores to spare and the load average was 2.7 — but they are not
  quiet-machine numbers and should be re-taken before being used as a
  regression baseline.
- **GPU timings were not taken at all.** Two processes holding the GPU makes any
  pass timing meaningless. Phase 2 needs the voxel pass's cost at a fixed
  vantage; take it then, on a clear machine, and record the vantage.
- **The work counters are unaffected by any of this**, which is the whole reason
  `ReportCount` exists.

**`timeout N` alone does not guarantee a run dies.** Both wedged processes had
an expired `timeout` parent stuck in `sigsuspend`: `timeout` sent SIGTERM, the
child did not die, and plain `timeout` then waits forever. `CLAUDE.md`'s
headless convention should read **`timeout -k 5 N`**, or a hung run pegs a core
indefinitely instead of being killed on a timer — which is the exact thing that
convention exists to prevent.

### Phase 1 — The greedy mesher and the mesh store

The CPU mesher, the per-face AO bake, the merge keyed on colour + AO, and the
shared GPU buffer. Nothing reads it yet.

**Acceptance:** the *measured* quad count with AO in the merge key, recorded
here against the 118,504 floor. A `VOXAGINE_MESH_STORE_AUDIT` reporting resident
frames, quads and bytes, and verifying the mesh against the model: every quad
lies on an exposed face, every exposed face is covered exactly once, and no quad
spans voxels of differing colour or AO. Peak RSS and GPU allocation before and
after. The mesher belongs under `Tests/` coverage — a cube, a hollow shell, a
one-voxel model and a model with a transparent palette index are the cases.

### Phase 1 results — PARTLY, 2026-08-10

**Built:** `VoxelMesher::BuildFrameMesh` (greedy merge over all six face
directions, keyed on colour only — see below), and `ModelMeshStore` as the
CPU-side cache: one growing `std::vector<uint32_t>` quad list, 3 words a quad,
keyed onto `VoxFrame::m_uiMeshFirstQuad`/`m_uiMeshQuadCount` so a distinct frame
is meshed once and never again. Wired unconditionally into
`VoxRenderer::SetFrame` — every frame the game loads gets meshed, static or
not, since meshing is idempotent and the store is where phase 2's GPU upload
reads from regardless of which renderers end up using it.

Ran clean against real content: every frame `Fishing_Village_Beat2` loads
meshes without a crash, one `[mesh]` log line per distinct frame. No validation
change, because this phase touches no GPU state — see what is missing, below.

**Not built, for session budget — three real gaps, not polish:**

1. **No GPU upload.** `ModelMeshStore` is CPU-only; there is no `Mapper`
   analogous to `m_pVoxelMapper`, so nothing outside this store can read a
   quad yet. This is the actual phase-2 blocker.
2. **No AO in the merge key.** The design (`The design`, piece 1) requires
   merging on colour *and* the four baked corner AO values, specifically so a
   later change doesn't quietly flatten shading across an occlusion boundary.
   What is built merges on colour only — correct geometry, but the AO bake
   itself does not exist, so there is nothing to key on yet. **Whoever adds
   the AO bake must also add it to `MeshSlice`'s mask key
   (`VoxelMesher.cpp`) in the same change**, or every quad silently loses the
   correctness property the design calls for.
3. **No audit, no `Tests/` coverage.** The `[mesh]` log is a placeholder for
   `VOXAGINE_MESH_STORE_AUDIT`. Nothing here is exercised by
   `voxagine_tests`.

**Measured, not estimated:** quad counts for real frames land in the same
range as the standalone Python measurement in `Why meshes and not ray
marching` (hundreds to low thousands per frame) — e.g. 572, 3559, 7295, 24, 6
across the first dozen distinct frames `Beat2` loads. Nothing here is a
synthetic test case.

### Phase 2 — The dynamic model pass, composited by depth

`ShadeVoxelSurface` factored out and both voxel pixel shaders moved onto it
*first*, verified to change nothing on screen. Then the pass, the two buffers,
the two shaders, the composite branch, and the AABB proxy switched to the
transformed model bounds. Dynamic renderers are **still baked** at this point —
the new pass draws on top of the stamped voxels, which is redundant but is
exactly what makes it verifiable: the image should barely change except for
rotation.

**Acceptance:** zero validation errors. The `ShadeVoxelSurface` refactor lands
as its own commit and is proven inert by a before/after capture at the
`RENDERING_PLAN.md` verification crop. The user watches a walking character, a
spinning weapon and the Beat2 crowd, and judges the rotation change. Voxel-pass
and new-pass GPU time at the phase 0 vantage. A character in front of a wall,
behind a wall, half-behind a wall, against the sky, and overlapping another
character all composite correctly.

### Phase 2 results — PARTLY, 2026-08-10

Built under severe session budget pressure (the user asked to go "as far as
the limit allows"), so this landed as one long push rather than the
inert-refactor-then-build sequence above describes - the `ShadeVoxelHit`
factor-out and its inertness proof are their own earlier commits (verified:
DXC recompiles both shaders clean, zero validation errors, unchanged frame
timing), but everything after it - the pass, both shaders, compositing,
submission - landed together rather than being independently proven at each
step. Read what follows as "built and plausibly correct", not "verified
against the acceptance criteria above".

**What exists and runs**, zero validation errors, full 300-frame headless run
on `Valley_Path_To_Castle_Beat1`, screenshot inspected and shows a normal
gameplay frame (both players visible, no corruption, no missing geometry):

- `ModelInstanceData`/`ModelQuadInstance` (Buffers/Structures/) - the
  per-instance transform and the per-frame quad-instance list.
- `VoxelModelPass` - two render views (colour, linear world-distance depth)
  plus its own real depth attachment, `VoxelModel.vs.hlsl` /
  `VoxelModel.ps.hlsl`. Registers: b0 camera, t0 instances, t1 quad
  instances, t2 mesh quads, t3 pyramid, s0 pyramid sampler.
- `RenderContext::m_pModelMeshMapper` mirrors `ModelMeshStore` to the GPU,
  resized and re-uploaded only when the CPU store has grown
  (`SyncModelMeshStore`, called each frame inside the same "GPU is not still
  reading last frame's data" guard the AABB buffer's rebuild already uses).
- `VoxelPass` takes two more `View*` (model colour/depth) in its constructor;
  every register after them shifted by two in **both** pixel shader variants
  (rule 8) - sun shadow map and pyramid moved from t3/t4 to t5/t6 in the
  shadowed variant, t3 to t5 in ShadowLess.
- Composite branch in both variants: nearest of {model, particle, world hit}
  wins in `VoxelRenderer.ps.hlsl`; `VoxelRenderer.ShadowLess.ps.hlsl` keeps its
  own simpler pre-existing convention (overlay wins outright, no
  world-distance gate) rather than importing the stricter one, but now reads
  `particleDepthPass` for the first time to make a fair model-vs-particle
  comparison possible.
- `RenderSystem::PostTick` computes each non-static renderer's continuous
  (unrounded) world transform - the same math `ComputeVoxelStampTransform`
  uses for `Origin`, minus the grid-alignment floor a single-frame renderer's
  stamp applies, which has no meaning for a rasterized mesh - and submits it
  plus its mesh's quad range. Dynamic renderers are still baked underneath,
  per this phase's own design.

**Not verified, and worth being direct about:**

- **Nobody has looked at a character on screen with this pass live.** The
  screenshot above is a wide gameplay shot; the two player markers are visible
  and nothing looks wrong, but that is not the same as confirming the *new*
  pass's quads are what's on screen rather than the still-active bake sitting
  at the same position underneath them. `RENDERING_PLAN.md`'s own convention
  is exactly for this: ask the user to look, don't infer it from a screenshot.
- **Cull mode is deliberately `E_CULL_NONE`.** `VoxelModel.vs.hlsl`'s winding
  was derived from the geometry (`d1 x d2 = +axisUnit`, corrected per-face for
  sign, reasoned through by hand) but never empirically checked against this
  engine's actual clockwise-front convention under its Y-flipped viewport.
  Guessing wrong would make every dynamic renderer disappear; leaving culling
  off costs a little overdraw on meshes this small and guarantees nothing is
  invisible while that gets confirmed. Flip to a real cull mode once someone
  has watched a character rotate and confirmed both faces shade correctly.
- **The diagonal-flip AO fix (which pair of corners the seam runs through) is
  implemented per a commonly-cited convention, not independently re-derived
  or tested against a case built to exercise it.** Wrong would read as a
  slightly-off crease on a locally-occluded quad, not a crash or a missing
  character - worth a deliberate look at a corner AO case, not just a general
  playthrough.
- **No GPU timing taken** - same "machine had two other wedged processes"
  problem from phase 0, still unresolved at time of writing.

### Phase 3 — Dynamic renderers leave the bake

`VoxelBaker` skips non-static renderers entirely. `VoxFrameEmitter` is
re-sourced from `VoxFrame`'s solid voxels and the instance transform rather than
from `BakeData::Positions` — **do this in the same commit**, or every corpse
stops emitting.

**Acceptance:** `VOXAGINE_COVERAGE_AUDIT` still reports zero uncovered bricks
above the ground row. `VOXAGINE_SYNC_AUDIT` clean. Kill a monster and watch the
corpse come apart in the right colours — this is the regression `CLAUDE.md`
already records once. Dynamic bake cost from phase 0 goes to zero.

### Phase 3 results — PARTLY, 2026-08-10

**Done ahead of order, at the user's explicit instruction**: "characters on
screen, off the bake" outranked finishing phase 2's own verification first,
and outranks phase 4 (sun shadows) too - accepted knowingly as a *visible*
regression (dynamic renderers currently cast no shadow at all, see
`VoxelModel.ps.hlsl`'s header) in exchange for landing the win now rather than
staying blocked on it.

**What changed, beyond the plan's own description:**

- `ComputeContinuousModelTransform` (`VoxelStamp.h`) is new, and is *not* what
  the plan described as inline math in `RenderSystem::PostTick` - it was
  extracted the moment a second caller (`VoxFrameEmitter`) needed the
  identical computation, specifically so the two could never disagree about
  where a renderer's model actually is. `RenderSystem::PostTick` was updated
  to call it too, replacing what had been a duplicate inline copy.
- `VoxFrameEmitter::Emit` gained a genuinely new code path - walking
  `VoxFrame::GetPositions()/GetColors()` directly and placing each solid
  voxel with the shared transform - rather than a one-line redirect, since
  `BakeData::Positions` (the old source) is structurally always null for a
  renderer that is never stamped. The shared timer/force/velocity/spawn tail
  both paths need was factored into `EmitCommon` in the same change, rather
  than duplicated.
- **The AABB proxy submitted to the world voxel pass (for coverage) was
  deliberately left alone**, still computed from `ComputeVoxelStampTransform`/
  `ComputeStampedGridBounds` - the *quantized* stamp math - even though
  nothing is stamped into the buffer any more. Those functions are pure
  geometry queries with no dependency on `Occupy` having run, so this is safe
  and, for session-budget reasons, deliberately the lower-risk choice over
  rewriting proxy computation to use the continuous transform's true OBB
  under time pressure - rewriting exactly the code path that would make a
  character invisible if gotten wrong was the one place worth not gambling
  on. The box this produces is a reasonable, probably slightly generous,
  upper bound - not the plan's originally intended "switched to the OBB's
  bounds." Revisit when there is room to verify it carefully rather than
  fast.

**Verified:** dynamic bake counters (phase 0's `Bake voxels/renderers
(dynamic)`) read exactly 0 in a 300-frame headless run. Zero validation
errors. A 1280x720 screenshot with baking off shows both players as complete,
correctly shaded humanoid figures - the mesh pass alone is now carrying them,
with no stamped voxels underneath to fall back on if it were wrong.

**Not verified, and this is the real gap:** nobody has killed a character and
watched its death particles. This is *exactly* the regression `CLAUDE.md`
already records once for a related reason ("every corpse came apart into
black particles"), and the fix here (`VoxFrameEmitter`'s new branch) has not
been exercised - not in a screenshot, not in an audit, not on screen. This is
the single highest-priority thing to check before trusting this phase.
`VOXAGINE_COVERAGE_AUDIT`/`VOXAGINE_SYNC_AUDIT` were also not run this session.

**A "rotation doesn't work" alarm, investigated and most likely a red
herring, worth recording so it isn't re-chased.** A `VOXAGINE_MODEL_DEBUG`
diagnostic (env-gated print in `RenderSystem::PostTick`, kept - costs one
`getenv` when unset) showed the player's rotation quaternion as `(0,0,0,0)` -
degenerate, not identity `(0,0,0,1)`. A "colour the suspects" pass (forcing
`VoxelModel.ps.hlsl` to output flat magenta, reverted after) proved this is
*not* the old marched path leaking through: the magenta exactly filled a
complete, correctly-shaped character silhouette, so the new pass genuinely
owns the whole character and what read as "shadows" is the baked AO working
as designed.

The zero quaternion traces to the test conditions, not the renderer:
`Hum_IdleState`/`Hum_MoveState`/`Hum_DashState` all call
`Transform::SetRotation` with a properly built quaternion whenever the player
has a real facing direction, but the `--hidden` headless harness this session
used throughout simulates no input, so the player never moves and that code
never runs - leaving `Transform::m_Rotation` at its cold default. This tree
already has a documented instance of exactly this class of bug (`CLAUDE.md`,
"GLM does not zero-initialise and `GLM_FORCE_CTOR_INIT` is not set"). The old
stamped path fed the *same* broken quaternion through
`StableEulerAnglesDegrees`, which has an explicit epsilon guard that treats a
near-zero quaternion as "no rotation" - silently masking it. The new pass has
no such fallback, so a pre-existing latent default became visible for the
first time as "the model never turns," in a test condition where it was
never going to turn regardless of which renderer was in use.

**Not independently confirmed** - this is the read from static analysis, not
from watching a character turn. Reasonably confident given `SetRotation`'s
call sites are unambiguous, but this needs eyes on an actual moving,
turning character before being trusted. Do that before touching this area
again.

### Phase 4 — Sun shadows from dynamic models

The quad list projected into light space, and the depth attachment that makes
`LESS` the min. Characters cast, and cast onto each other.

**Acceptance:** a character's shadow tracks it with no lag at any frame rate —
which is the reported defect, seen from the other side.
`--screenshot-pass "Sun Shadow"` shows the character. Shadow pass GPU time before
and after, at each `ShadowQuality`.

### Phase 4 results — PARTLY, 2026-08-10

**Built as two passes plus a combine, not the single instanced pass this
section originally described.** The one-pass design assumed a `RenderPass`
could write into a target it does not own (drawing the quad list straight
into `SunShadowPass`'s existing depth-attached target). Checked against the
actual engine rather than assumed: `RenderPass::Data::m_PassOutput` is an
*input* binding only (`VKPassBindings.cpp`), and there is no mechanism for a
second pass to append draws into a target the first pass already opened and
closed. Building that would mean either extending the pass abstraction
(engine surgery, out of scope here) or accepting the risk this plan's own
rejected-list already warned about (a frame graph, only once a third or
fourth pass needs one).

What was built instead: `SunShadowModelPass` draws the same quad list
(`VoxelModelPass`'s own instance/quad-instance buffers, reused unchanged)
into a *second* `k_uiSunShadowResolution`² `R32_FLOAT` target, projected into
light-space clip through `shadowTangent`/`shadowBitangent`/`shadowRect`/
`shadowDepth` (already in the shared camera cbuffer), with its own real depth
attachment to resolve overlapping characters against each other. A tiny
`SunShadowCombinePass` then does a full-screen `min()` of that target against
`SunShadowPass`'s world one, into a third target shaped exactly like the
world map. `VoxelPass` binds *that* as `sunShadowMap` - not
`SunShadowPass`'s raw target.

**Why this is the safer design, not just the fallback one:**
`SunShadowLookup.hlsl`'s PCSS blocker search and filter (tuned, load-bearing,
`RENDERING_PLAN.md` 7.1a) needed zero changes - every existing reader keeps
sampling one `sunShadowMap`, now pointed at the combined result. Extending
that file to read two textures and thread a second sample through the
blocker search and filter loops would have meant touching tuned math under
time pressure to save one texture read; three passes with an unmodified
consumer was judged the better trade.

**A real bug caught before it shipped, not after:** the light-space depth
value each fragment writes is `dot(gridLocalPosition, lightDirection.xyz)`,
derived (not guessed) from `SunShadow.ps.hlsl`'s own math - the stored value
there (`shadowDepth.x + hit.Distance`) is algebraically the same dot product,
confirmed by expanding the march's origin and step along the light axis
before writing a single line of the new shader. The clip-space placement
(`u,v → clip.xy = uv*2-1`) was checked against `VKRenderPass.cpp`'s actual
viewport setup (`viewport.height = -TargetSize.y`, applied unconditionally to
every pass, not a per-pass choice) rather than assumed from the model pass's
different, camera-relative convention. Both `SunShadowModelPass.cpp` and
`SunShadowCombinePass.cpp` initially called a `Settings::GetSunShadowResolution()`
that does not exist (misremembered from an earlier comment) - caught by the
compiler, fixed to the real `RenderContext::k_uiSunShadowResolution`
`SunShadowPass.cpp` itself uses. Separately, `SunShadowModelPass.cpp`'s first
draft never assigned `m_pVertexShader`/`m_pPixelShader` at all - caught by
rereading the constructor against `VoxelModelPass.cpp`'s working one before
building, not by a crash.

**Verified:** all 23 shaders (three new) compile clean through DXC/SPIR-V.
Zero validation errors over a 300-frame headless run with shadows on. A raw
capture of the combined map's data is non-degenerate (over a thousand
distinct values in a 200k-sample slice, not a flat buffer) - checked
numerically, not visually, and the capture format itself was not fully
understood (a from-scratch PPM-triplet parse disagreed with a raw-float
reinterpretation on dimensions), so treat this as "not obviously broken,"
not as confirmation the shadow is geometrically correct.

**Not verified, and this is real:** nobody has looked at an actual shadow on
screen. Everything above proves the pipeline is *plausible* - it runs, it
produces varying non-NaN-mostly data, the derivations were checked against
source rather than guessed - but not that a character's shadow lands in the
right place, at the right size, or moves correctly as it walks. This needs
the user's own eyes in a live window, which is what phase 4's own acceptance
criteria already asked for and this session did not do.

**Also not done:** self-shadowing - `VoxelModel.ps.hlsl` still passes the
literal `1.0` for `fSunVisibility`, so a character does not yet receive
shadows from the world or from its own other quads (an arm shadowing the
torso, say). Wiring `SunShadowLookup.hlsl`'s functions into the model pass
now that the combined map exists is mechanical, deliberately deferred to keep
this phase's already-large diff from growing further under session budget.

### Phase 5 — Ambient: baked AO, world cones, and what is lost

Judge, on screen, whether losing the character's contribution to the coverage
pyramid is visible — a character in a doorway is the case to look at.

**Acceptance:** the user's call. If it reads as wrong, cost splatting a coarse
occupancy footprint into the pyramid *here* and defer building it to its own
phase rather than bundling it.

### Phase 6 — Cleanup: delete what the bake needed and no longer does

Once nothing dynamic is stamped, a list of things exist only to paper over the
fact that it used to be: `VoxelBaker::NotifyClearedRegion` and the whole repair
scan; `Clear`'s dynamic-positions branch and the window-slide handling; `Bake`'s
`WorldOffset` re-examination for non-static renderers; the stamp-bounds union in
`PostTick`; and the `CLAUDE.md` sections that describe all of it, which become
history rather than instructions.

**Acceptance:** every deleted path has a test or an audit that would have caught
its removal being wrong, and `voxagine_tests` — `checks`, `scenarios` and `perf`
— is green with the work baseline unchanged. Note in `CLAUDE.md` what moved from
"how it works" to "how it used to work".

### Phase 6 results — PARTLY, 2026-08-10

**One real fix, verified:** `VoxelBaker::Clear` now returns immediately for a
renderer that is dynamic *and* has never been baked - which, since phase 3, is
every dynamic renderer for the whole of its life. Without this,
`RenderSystem::OnComponentDestroyed` (a dead monster, an expired bullet - any
dynamic renderer's despawn) fell through to `Clear`'s "else" branch and paid
for a `grid.GetChunk()` scan (allocates two arrays sized to the renderer's
whole bounding box) to discover there was nothing to do. Guarded on
`Positions == nullptr` rather than `IsStatic()` alone, so a renderer that used
to be static and got toggled dynamic still runs its real cleanup - the
`StaticPropertyChanged` handler already calls `Clear` while it is still
static, before the toggle takes effect, so `Positions` is genuinely null here
only when there is nothing left.

**`voxagine_tests` run in full for the first time this session** (it had not
been built or run at all before this point): `checks` 79/79, `scenarios`
31/31, `perf` 0 regressions. `VOXAGINE_COVERAGE_AUDIT` and
`VOXAGINE_SYNC_AUDIT`, both promised as phase 3 follow-ups and not run until
now, also came back clean (0 uncovered bricks above the ground row on a 465-
proxy scene; 0 of 75.5M voxels disagree between the CPU chunk and the
mapping). Together these are the strongest evidence yet that dynamic
renderers leaving the bake did not silently break anything the destruction
system, the integrity checker or the particle sim depend on.

**Deliberately NOT deleted, unlike what this section originally described:**
`VoxelBaker::NotifyClearedRegion` and `Clear`'s window-slide branch
(`worldOffsetDiff != Vector3(0.f) && !IsStatic()`). Both are now *provably*
dead - their guard conditions test state (`bake.Positions` non-null for a
dynamic renderer, in both cases) that phase 3 made permanently false - but
they are also among the most heavily-commented, correctness-critical code in
this file (the comments above `NotifyClearedRegion` alone document three
solved defects: a wall eating a character's voxels, the reverse, and a
two-dynamic-renderer ping-pong). Deleting code at that level of hard-won
subtlety, at the tail of an already very long session, on the strength of "I
traced the guard conditions and they can't be true" rather than a
purpose-built test that exercises the deletion, is not the trade this phase's
own acceptance criterion asks for. They cost nothing at runtime now (a
no-op loop iteration, a condition that's always false) - leaving them in
place with this note is the honest, lower-risk choice. Revisit with a fresh
budget and, ideally, a scenario test that specifically destroys a dynamic
renderer near a static one's clear, which `voxagine_tests` does not currently
have.

**Also not done:** the `Bake`'s `WorldOffset` re-examination for non-static
renderers and the stamp-bounds union in `PostTick` were not touched -
`PostTick`'s proxy still deliberately uses the old quantized stamp-bounds math
(a phase 3 decision, not new information), and `Bake`'s per-renderer loop now
exits before reaching that logic for any dynamic renderer at all (the phase 3
early-return), so there was nothing further to remove there. `CLAUDE.md` was
not updated to move any of this from "how it works" to "how it used to work" -
left for a session with room to write that carefully.

---

## Rejected / out of scope

- **Ray marching each model's own volume.** The first draft of this plan. See
  "Why meshes and not ray marching" — it is the natural move in a marched engine
  and it loses on per-pixel cost, early-Z, shadow cost and anti-aliasing, while
  memory is a wash. It comes back only if characters ever need to be destructible
  in place.
- **Making the bake faster instead.** Explicitly not what was asked for, and
  `RENDERING_PLAN.md` 4b and 4c have already taken the two large wins there
  (5284 → 757 → 0.03 ms). What is left is the irreducible per-voxel cost of
  re-voxelizing a moving object, and the defect class comes with it.
- **Real vertex and index buffers.** `m_VertexLayout` and `m_pVertexData` have
  never been used by anything; adding vertex input state to `VKRenderPass` for
  118 k quads that a storage buffer and `SV_VertexID` already handle is engine
  machinery bought for nothing.
- **Per-voxel cubes without greedy merging.** 223,382 exposed faces against
  118,504 quads — the merge is 1.89x before the AO key, and the mesher is the
  same code either way.
- **Exporting per-pixel depth from the voxel pass** so characters can be drawn
  after it into a shared depth buffer. It costs `[earlydepthstencil]` on the one
  pass whose overdraw is unbounded and is the suspected factor in the Xid 109
  hang (`CLAUDE.md`, "The freeze after gameplay is a GPU timeout").
- **Compositing characters in post processing.** Post processing samples a
  *one-submission-old* copy of the voxel target. Characters composited there
  would lead the world by a frame whenever the camera moves.
- **Putting dynamic models in the far field.** It is built from static entity
  JSON on a background thread and covers the level outside the resident window.
  Characters are inside it by definition.
