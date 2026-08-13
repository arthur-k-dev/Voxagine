# Rendering restoration & advancement plan

Restore the graphics fidelity that was cut when levels were scaled up, pay for
it with structural optimizations, and then go past the original. Written to be
executed **one phase per session, in order**, by a future agent with no memory
of the conversation that produced it.

Companion to `CLAUDE.md`, which stays the general handover doc. This file owns
the renderer work only.

---

## How to use this document

1. Read **The rules** and **Ground truth** below. They are verified against the
   tree; do not re-derive them, but do re-verify a `file:line` before editing it
   (line numbers drift).
2. Find the first phase in **Progress** that is not `DONE`. Do **only that
   phase**. Phases are ordered for a reason — see *Why this order* in each.
3. Meet the phase's **Acceptance** criteria before marking it done. If you
   cannot, mark it `BLOCKED` with a note rather than half-landing it.
4. Update the **Progress** table and the phase's own notes section. Record
   measured numbers — the next session plans against them.
5. Commit per phase, on a branch, with the phase number in the message.

**Do not skip ahead, and do not bundle phases.** The order is load-bearing, not
a preference — each phase's *Why before/after* section says what breaks if you
reorder it.

### Model guidance

If quality is the only axis, use **Opus for everything**. If cost matters,
Sonnet is adequate for the phases marked *mechanical*; the phases marked
*design* involve new GPU data structures, new descriptor bindings and
cross-thread invariants, where Opus is worth it.

| Phase | Nature | Recommended |
|---|---|---|
| 0 Instrumentation | mechanical | Sonnet |
| 1 Restore fidelity | mechanical (shader-local) | Sonnet |
| 2 Occupancy bricks | design | **Opus** |
| 3 Depth prepass | design | **Opus** |
| 4 Far-field LOD | design | **Opus** |
| 5 Point lights + specular | design (new engine feature) | **Opus** |
| 6 Beyond original | mixed — see sub-items | **Opus** |

### Progress

Phases are numbered in execution order. Do them in this order.

| Phase | Status | Session / commit | Notes |
|---|---|---|---|
| 0 — Instrumentation & baseline | DONE | 2026-08-08 | GPU timestamps + CPU breakdown, gated behind `Settings::IsGPUProfilingEnabled` |
| 0b — Headless benchmark harness | DONE | 2026-08-09 | `--hidden --size WxH --frames N --map <path> --screenshot <path>`. An unmapped SDL window renders normally, displays nothing, and is not the compositor's to resize — so a run is reproducible *and* invisible. `--screenshot` dumps any pass's target as a PPM, which is how 7.1a was debugged without a display |
| 1 — Restore fidelity (AO, shadow fade, sky) | DONE | 2026-08-08 | AO + shadow fade landed as planned; sky/ground moved to a new PostProcessing branch instead of the planned AABB-proxy approach — see notes |
| 2 — Occupancy brick grid | DONE | 2026-08-08 | Two-level DDA; 700-step cap and 2x shadow stride both gone; voxel pass 0.67 ms vs 1.07-1.10 ms at the Phase 0 baseline vantage. ReBAR rode along |
| 3 — Low-res depth prepass | **REVERTED** | 2026-08-08 | Built, correct, cheap — and redundant with the AABB proxy cubes, which already start every primary ray at the box entry. Deleted once phase 4 turned out not to use it. Re-measured first, uncapped and up to 4K: the saving is a flat ~0.6% of the voxel pass at *every* resolution, so it never grows relative to the work it optimizes. Read the notes anyway — they relocate where the pass's time actually goes (the shadow ray) |
| 4 — Far-field LOD volume | DONE | 2026-08-08 | Whole level at 4x (384x32x384, 18 MiB), marched in post processing where the window has no answer. Built from **entity JSON, not chunk voxel data** — there is none; read the notes before planning against them. Costs ~0.10 ms of post processing. The planned `m_VoxelData` ride-along does not exist |
| 4b — CPU occupancy bitmap (bake path) | DONE | 2026-08-08 | Phase 4's first unfixed finding. `ModifyVoxel`/`ModifyVoxelFast` learn old occupancy from a 9.4 MiB CPU bitmap instead of reading the ReBAR mapping back. World-load bake **5284.7 → 757.6 ms peak**, same level, same vantage. Validation cross-check: 0 bricks and 0 bits disagree over 75.5 M voxels |
| 4c — The bake stall (redundant re-stamping) | DONE | 2026-08-08 | The bake was writing back what was already in the buffer: 218 of 218 re-stamps byte-identical. A stamp key plus a voxel-buffer generation counter answer a re-bake request without performing it. `CPU VoxelBaker::Bake` **756.7 → 0.03 ms**, but the *world load* only improves **~1157 → ~400 ms (2.9x)** — the remaining stamp is in `OnComponentAdded`, which that timer never covered. Locality was measured and ruled out. Chunk streaming is **not** verified — read the notes |
| 4d — Shrink the CPU `Voxel` 16 B → 4 B (+2 B owner) | DONE | 2026-08-08 | `sizeof(Voxel) == 4`; ownership moved to a parallel `uint16_t` slot array, so **128 → 48 MiB per resident chunk**. Measured peak RSS **1.696 → 0.946 GB (−44%)** over three interleaved A/B runs. Not a regression anywhere measured: world-load stamp 0.624 → 0.592 ms avg, chunk encode 16 → 7 ms, decode 54 → 14 ms. The full 4 B was **not** taken — read why |
| 5 — Point lights + specular | TODO | | |
| 6 — Beyond the original | partly — 6.0, 6.2 done | | |
| 6.0 — A frame-time ceiling for the marcher | DONE | 2026-08-09 | Taken out of order, as the fix for the Xid 109 hang. `MARCH_STEP_BUDGET` (1024) caps the DDA crossings one pixel may make across *all* its rays, in both the window marcher and the far field. Swept against the voxel pass's own cost: it stops responding to the budget between 64 and 128, so 1024 clips nothing and costs nothing (2.58 ms either side). **It does not explain the hang** — the bound it removes was only ever worth ~100 ms, so read the notes before treating the trigger as found |
| 6.2 — Soft shadows | DONE, then superseded | 2026-08-09 | Shadow ray rewritten as `MarchShadow`, a transmittance rather than a hit; distance fade deleted. Brick size 8³ → **4³** rode along and is worth 8% of the *whole* pass. The softness scheme went quad-averaged cone → per-pixel cone → **replaced entirely by 7.1a's shadow map**, because no sample count is both smooth enough and affordable at 4K. Read the notes for the two rejected approaches and the premise check that split phase 7 |
| 7 — Voxel cone tracing (shadows, AO, GI) | 7.1a DONE, 7.1b DONE, 7.2 DONE | 2026-08-09 | Supersedes 6.5 and absorbs what is left of 6.2. One mip pyramid answers sun shadows, AO and bounce; a light-space shadow map answers only the first and would be a second structure |
| 7.1b — The coverage pyramid, and cone AO against it | DONE | 2026-08-09 | Route A built it as counts in the brick buffer and priced the hand-written filter at **4.6x the rest of the cone**; route B moved it into a 3D texture's mip chain, where a step is one `SampleLevel`. Cone AO is **on**: voxel pass 2.44 → **3.47 ms at 4K**, against route A's 7.67. The mip chain is *blitted* from mip 0 rather than uploaded per level — read why. Its validator found a missing call on its first run |
| 7.4 — Emissive voxels + specular cone | **PARTLY** | 2026-08-09 | The rule-3 audit found the tag byte **was never written**: it was ORed onto a palette alpha of 255, so every voxel carried 255 and claiming a bit without auditing would have made the whole world emissive. Byte reclaimed (invisible on screen: 82.68 → 82.68), emissive voxels authorable and self-lit, specular cone on. **Emissive light spilling onto neighbours is NOT BUILT** — it needs a channel an RGBA8 texel does not have; the second-texture design is costed in the notes. Lighting is at **2.96 of the 3 ms budget** |
| 6.1 — Fog / aerial perspective | DONE | 2026-08-09 | `Fog.hlsl`, on linear radiance, shared by both voxel shaders and the far field. **Two of its three stated purposes do not exist at this game's camera** - there is no horizon and no far field on screen - and the measurement that established that (turning the fog term itself into a distance visualization) is the reusable part. Retuned to the real 120-450 unit range: the top of the frame recedes 16% in luminance, the play area moves by 0.01. Density judged on screen (0.002 read too heavy). Costs nothing |
| 7.3 — Radiance in the pyramid, diffuse GI | DONE | 2026-08-09 | The texel is RGBA now — coverage in alpha, premultiplied linear albedo in RGB — so the **existing** AO cones gather bounce out of the same `SampleLevel`. The plan's quarter-res GI pass and bilateral upsample were **not built and are not needed**: they were budgeted against bounce cones being extra to the AO cones, and they are the same five cones. **+1.29 ms at 4K**, lighting total 2.74 against the 3 ms budget. Also: `--uncapped`, because every GPU number before this one was taken vsync-locked and therefore at a resolution-dependent GPU clock — read that section before comparing anything |
| 7.2 — Linear light (was 6.4) | DONE | 2026-08-09 | Lighting is linear; the encode is at the end of scene shading rather than at present, and the notes say why that is not the sRGB-swapchain version 6.4 described. Constants converted rather than retuned, except `SKY_AO_CURVE` — **a multiplier tuned against an encoded image needs its exponent scaled, not its value**, and getting that wrong deletes AO silently. Sunlit ground −14% and enclosed geometry +1.9%, which is the correction, not a regression. Costs nothing: Voxel 2.43 → 2.44 ms at 4K |

---

## The rules

Break any of these and the build fails in ways that are hard to attribute.

1. **The SPIR-V/C++ binding contract.** Any new shader resource means updating
   *both* the DXC register shifts in `CMake/Shaders.cmake:16-19` and
   `VKBindings` in
   `Voxagine/Source/Core/Platform/Rendering/Vulkan/VKShaderBindings.h:19-22`.
   HLSL's `b`/`t`/`u`/`s` are four namespaces, Vulkan has one per set; without
   the shifts a shader with `b0` and `u0` produces two descriptors at set 0
   binding 0, which is invalid **and compiles clean**. Class stride is 100, so
   a class may use 100 registers before colliding.
2. **`-fvk-use-dx-layout` stays** (`CMake/Shaders.cmake:24`). The engine memcpys
   packed C++ structs into structured buffers; std430 padding of `float3`
   silently changes strides. Any new structured buffer inherits this and needs
   `scalarBlockLayout` on the device.
3. **The voxel alpha byte is not just opacity.** `VoxelBaker.cpp:255-258` packs
   `rendererState + 1` into bits 24-31. Occupancy is `a > 0`. Any feature that
   reads or writes alpha must preserve that predicate and must not assume
   `a == 1`.
4. **Voxel linearization is `x + y*W.x + z*W.x*W.y`** — C++
   `VoxelBaker.cpp:248-252` and `RenderContext.cpp` must agree with HLSL
   `PosToVoxelID` (`Game/Engine/Assets/Shaders/SDFMarcher.hlsl:25-28`). Any new
   volume (bricks, LOD) must state its own convention explicitly and match on
   both sides.
5. **Zero validation errors is the bar.** The port reached it; keep it. Run with
   validation on and check the log every phase.
6. **Windows must keep building.** Platform-specific code goes behind `_WIN32`
   or `if(WIN32)`. `_WINDOWS` is not defined by this build.
7. **Do not reintroduce a global `WaitForGPU`** in the frame path. See
   `CLAUDE.md` — `VKSwapchain` waits per frame-slot fence and double buffering
   depends on it.
8. **Two shaders, not one.** `VoxelRenderer.ps.hlsl` and
   `VoxelRenderer.ShadowLess.ps.hlsl` are selected by the `ShadowsEnabled`
   setting at `RenderContext.cpp:733`. Non-shadow changes must land in both.
9. **Run the game from `Game/`** — assets are relative:
   `cd Game && ../build-game/bin/BitBuster`.
10. **Ask the user to look at the screen** for visual verification rather than
    screenshotting. They see flicker, input latency and behaviour over time that
    a still frame does not show.

---

## Ground truth

Verified against the tree on 2026-08-08. This is the model of the renderer the
plan is built on; if any of it turns out to be false, **stop and re-plan** —
several phases depend on these specifics.

### The resident volume is a 3×3 chunk window, not the level

`Voxagine/Source/Core/JsonSerializer.cpp:127-135` computes the voxel grid from
the chunk grid, using `GridSize` only for Y:

```
voxelGridSize = (1 * chunkSize.x, GridSize[1], 1 * chunkSize.y)
if chunkGrid.x > 1 -> voxelGridSize.x = 3 * chunkSize.x
if chunkGrid.y > 1 -> voxelGridSize.z = 3 * chunkSize.y
```

With `ChunkSize [256,256]` and `ChunkGrid [6,6]` (e.g.
`Game/Content/Worlds/Castle/Valley_Path_To_Castle_Beat1.wld`) that is
**768 × 128 × 768** — a sliding 3×3 window over a level that is really
1536 × 128 × 1536. Menu worlds are `ChunkGrid [1,1]` → 256 × 128 × 256.

**This is the "chunk system is restricting" problem.** The renderer cannot draw
more than the window because more than the window does not exist on the GPU.

### GPU voxel buffer

- One `Mapper` named `"Voxel Data Mapper"`, `E_R8G8B8A8_UNORM`, `E_READ_WRITE`,
  `m_bHasBackBuffer = true` — `RenderContext.cpp:673-688`.
- Sized in `RenderContext::ResizeWorldBuffer()` `RenderContext.cpp:230-249` to
  `x*y*z` elements of `sizeof(uint32_t)`.
- `VKMapper.cpp:74-96`: usage `STORAGE_BUFFER | UNIFORM_TEXEL | STORAGE_TEXEL |
  TRANSFER_SRC | TRANSFER_DST | SHADER_DEVICE_ADDRESS`, memory
  **`HOST_VISIBLE | HOST_COHERENT`**, **two** buffers when back-buffered,
  permanently mapped (`VKMapper.cpp:125-140`).
- **768×128×768 = 75,497,472 voxels × 4 B = 288 MiB, ×2 buffers = 576 MiB of
  host-visible memory.** On a discrete GPU every marcher fetch is a PCIe read.
  `VKMapper.cpp:11-20` says as much.
- **There is no per-frame upload and no GPU baking.** The CPU writes straight
  into the mapped pointer: `RenderContext.h:211-239` (`ModifyVoxel`,
  `ModifyVoxelFast`), `VoxelBaker::Occupy/Clear`
  (`VoxelBaker.cpp:284,290,348,389`), `ChunkSystem::RenderChunk`
  (`ChunkSystem.cpp:403-425`).
- Double buffering exists only for the chunk-window shift: `Mapper::SwapBuffer`
  (`VKMapper.cpp:115-123`) flips which buffer binds, and the `BufferSwapped`
  event repoints `m_pVoxelData` (`RenderContext.cpp:684-687`).

### `VoxelBaker.cs.hlsl` is dead — leave it dead

The compute baker compiles to `.spv` every build, `VoxelBakePass`
(`Passes/VoxelBakePass.cpp:6-17`) is **never constructed**,
`RenderContext.cpp:327` fetches it and gets null, the `"Bake Command Data"`
buffer is created and never filled, and the model upload that would populate
`voxelModelData[]` is commented out at
`Core/Resources/Formats/VoxModel.cpp:531-546`. Reviving GPU baking is a
separate project. **It is not part of this plan** — do not get pulled in.

### Culling is rasterized AABB proxies, not a frustum

- `RenderSystem::PostTick:138-143` submits one `StructuredVoxelBuffer` per
  enabled `VoxRenderer`, plus a **world-sized ground box** `768 × 10 × 768` at
  `:242-253`.
- `RenderContext::SortAABBs:207-218` sorts near→far; uploaded every frame.
- `VoxelPass.cpp:96-125` draws a 14-vertex tri-strip cube, `E_CULL_FRONT`,
  instanced per AABB. `VoxelRenderer.vs.hlsl:65-71` builds the ray.
- The pixel shader then marches the **whole world buffer** — `IsInChunk`
  (`SDFMarcher.hlsl:47-52`) tests `worldSize`, not the AABB. Combined with the
  world-sized ground box, most of the screen marches up to the step cap.

### Current fidelity, and what it replaced

`SplodyMcSplodeFace/` is the pre-scale-up copy of the same engine and is the
archaeological record — git history is useless (all commits post-date the
change; everything is already present at the initial commit).

| Feature | Now | Before |
|---|---|---|
| Shadow strength | flat `AMBIENT_VALUE` — `VoxelRenderer.ps.hlsl:70` | distance-faded, commented out at `SplodyMcSplodeFace/…/VoxelRendererForward.ps.hlsl:190` |
| Shadow ray | `MarchLight`, 64 steps, **2× stride** — `SDFMarcher.hlsl:106-107`, called at `VoxelRenderer.ps.hlsl:66` | `March`, 128 steps, full stride |
| Screen-space shadow prepass | `SDFDepth.*.hlsl` compiled, **nothing loads it** | fed `shadowPass : register(t5)`, sampled as `distShad` → `GetDirectLighting(distShad)` |
| AO | `AmbientOcclusion.hlsl` present, **included by nothing** | included and applied — Splody `:84, :291, :321-322` |
| Ambient constant | `0.7` — `Defines.hlsl:2` | `0.5` (raised to compensate for the losses) |
| March cap | hardcoded `700` — `VoxelRenderer.ps.hlsl:38` | world diagonal `maxDim` — Splody `:265,:279` (~1088 for this level, so 700 truncates) |
| Point lights / specular | **absent from engine and shader** | shader-side only, and already commented out — Splody `:203-228, :313-319` |
| Sky / endless ground | transparent black — `VoxelRenderer.ps.hlsl:47-50` | ground plane + sky — Splody `:339-366` |
| Resolution scale | wired, set to `1.0` — `Settings.h:58`, `VoxelPass.cpp:28`, `ParticlePass.cpp:26` | knob did not exist |

Leftovers that confirm the story: `struct ConstantDepthBuffer`
(`ConstantVoxelBuffer.h:57-78`) unreferenced; `Camera.hlsl` unreferenced in
`Game/`; `SplodyMcSplodeFace/Settings.vgs` has neither `ShadowsEnabled` nor
`ResolutionScale` — both keys were *added* for the scale-up.

### Why the engine is resolution-dependent (and why that is the wrong shape)

Cost is roughly `pixels × steps-per-ray × rays-per-pixel × fetch-cost`. Voxels
are far larger than pixels, so neighbouring pixels re-traverse the same empty
space — the marching cost scales with **screen** resolution while the scene's
information content is at **voxel** resolution. The scale-up attacked
steps-per-ray (700 cap, 64 double-stride shadow steps) and rays-per-pixel (AO
deleted), because those were the one-line levers. Phases 2 and 3 attack the
redundancy itself, which is what makes the restored fidelity affordable.

---

## Phase 0 — Instrumentation & baseline

*Mechanical. Sonnet. ~half a day.*

**Goal.** Every later phase can state what it cost or saved. Without this we
tune blind, and "it feels fine" is how the 700-step cap got there.

**Steps.**

1. Add a GPU timestamp helper to the Vulkan backend: a `VkQueryPool` of
   timestamps, `vkCmdWriteTimestamp` at begin/end of each render and compute
   pass in `VKRenderPass` / `VKComputePass`, results read back one frame later
   (never stall the frame to read them).
2. Aggregate per pass, log a rolling average every N seconds in the style of the
   existing one-shot `VKSwapchain` rescale warning. Gate behind a setting or
   `_DEBUG` so the release game does not pay for it.
3. Add a CPU-side frame breakdown for `VoxelBaker::Bake` and
   `ChunkSystem::UpdateChunks`, which are the known main-thread costs.
4. Capture the baseline on the **largest** level
   (`Valley_Path_To_Castle_Beat1`, 6×6 chunks) at native resolution, standing
   somewhere with long sightlines. Record in the table below.
5. If a discrete GPU is available, capture there too — the host-visible fetch
   cost differs enormously between iGPU and dGPU, and several phases are
   justified by that cost.

**Acceptance.** Per-pass GPU ms visible in the log; baseline numbers recorded
below; zero validation errors; no measurable cost when disabled.

**Implementation notes.**

- `FrameProfiler` (`Voxagine/Source/Core/Platform/Rendering/FrameProfiler.h/.cpp`)
  is the aggregator: a name → rolling-average-ms map, ticked once per frame
  from `RenderContext::Present`, logging `[timing] <name> <ms> (x<samples>/s)`
  every ~1s, in the same style as the existing `[fps]` log. Engine-agnostic on
  purpose — both the GPU side and the two CPU call sites report into it.
- GPU timing lives in `VKCommandEngine`, not the passes: one `VkQueryPool` of
  64 timestamps **per frame slot** (`VKCommandEngine::FrameData`), mirroring
  the existing per-slot `VkCommandPool`. `WriteTimestampBegin()` /
  `WriteTimestampEnd(name, beginIndex)` are called from `VKRenderPass::Begin`/
  `End` (right around `vkCmdBeginRendering`/`vkCmdEndRendering`) and from
  `VKComputePass::Compute` (around `vkCmdDispatch`), keyed by the pass's own
  `Data::m_Name` so the log lines line up with the names already used to look
  passes up (`"Voxel"`, `"Particles"`, `"UI Renderer"`, etc).
- Readback happens in `VKCommandEngine::Reset()`, immediately after the
  existing per-slot semaphore wait — that wait already guarantees the GPU
  retired this slot's previous submission, so `vkGetQueryPoolResults` never
  stalls; this is what "read back one frame later" means in practice with two
  frame slots. The pool is reset for the new recording in `Start()`.
- Gate is `Settings::IsGPUProfilingEnabled()` (default `_DEBUG`, off in
  Release, not RTTR-registered so it never round-trips through `.vgs`/
  `.vguser`). Read once into `FrameProfiler` and cached per-engine as
  `VKCommandEngine::m_bTimingEnabled` in `Initialize()` — a disabled Release
  build never creates the query pools at all, not just skips writing to them.
- `VoxelBaker::Bake()` and `ChunkSystem::UpdateChunks()` wrap themselves in a
  `std::chrono` span reported as `"CPU VoxelBaker::Bake"` / `"CPU
  ChunkSystem::UpdateChunks"`, guarded by the same `IsEnabled()` check so a
  disabled build skips the `chrono::now()` calls too, not just the report.
- Verified zero validation errors on both `editor`/`editor-release` (empty
  default world) and `game`/`game-release` with `Valley_Path_To_Castle_Beat1`
  actually loaded (via a temporary `DefaultMap` edit in
  `Game/ProjectSettings.vgps`, reverted after capture — the game's
  `VoxApp::OnCreate` under `!EDITOR` loads whatever `ProjectSettings::
  GetDefaultMap()` points at, which is the cheapest way to insta-launch a
  specific level without adding CLI argument parsing, which does not exist
  anywhere in the engine today).

**Baseline** — `game-release`, `Valley_Path_To_Castle_Beat1` (6×6 chunks,
768×128×768 resident window), steady state at the default player/camera spawn
(not a deliberately chosen long-sightline vantage — no interactive camera
control was available for this capture; re-measure from a longer sightline
before trusting these as a ceiling). Only a discrete GPU was available on the
capture machine, so the iGPU column is empty.

| Metric | iGPU | dGPU (NVIDIA GeForce RTX 4070 SUPER) |
|---|---|---|
| Voxel pass (ms) | — | 1.07–1.10 |
| Particle pass (ms) | — | 0.005 |
| Post/UI (ms) | — | Post Processing 0.022–0.023, UI Renderer 0.011–0.012 (fire only ~1–7×/s in Release, not every frame — UI/post content is static most frames) |
| `VoxelBaker::Bake` (ms, CPU) | — | 0.027–0.036 (×60/s, matches the fixed tick rate) |
| `ChunkSystem::UpdateChunks` (ms, CPU) | — | ~0.001, ×1/s (camera stationary at spawn, so no chunk-boundary crossings after the initial load) |
| Total frame (ms) / fps | — | 200 fps, hard-capped by `Settings::m_dFrameLimit` (5 ms budget) — the ~1.1 ms of measured GPU work is well under that cap, so the frame limiter is the binding constraint here, not render cost |
| Resolution tested | — | 1365×2003 (window default on the capture machine; not deliberately chosen) |

Read together: at this vantage the renderer is nowhere near its 5 ms budget,
so the scale-up costs described in "Ground truth" above (700-step cap, 2×
shadow stride) are not visible yet from a stationary default spawn — they
bite at long sightlines and after Phase 1 restores the fidelity that was cut.
This baseline is the "cheap seats" number; Phase 1's acceptance check (compare
against this table) should be re-measured from a longer sightline, ideally
with the user driving the camera live rather than a scripted spawn point.

---

## Phase 0b — Headless benchmark harness

*Not built. Scoped here because phase 6.2 measured ~20 GPU configurations by
launching the game into a window, which takes over the user's display and is
not reproducible.*

**The problem, concretely.** A measurement run today is: edit `ProjectSettings.
vgps` to point `DefaultMap` at a level, edit `Settings.vgs` for vsync and
`ResolutionScale`, launch the game detached, sleep, kill it, grep stderr. It
opens a window on whatever monitor is there. Hyprland picks the size, so two
runs of the same binary can differ in pixel count with nothing in the log
saying so — that produced a Voxel pass reading of 0.752 ms, *below* the same
build's shadow-less floor, and it was only caught because the full-screen Post
Processing pass moved with it.

**Do not select the map by editing `ProjectSettings.vgps`.** Every phase from 0
onward has done it and every one of them carries a "reverted after capture"
note, which is the tell. It also churns a shared file that another agent may be
holding. The map is an argument.

**There is no launcher trick that avoids this, which is worth recording so it
is not retried.**

- `SDL_VIDEODRIVER=offscreen` initialises, then dies:
  `SDL_Vulkan_CreateSurface failed: VK_EXT_headless_surface extension is not
  enabled`. That extension is **not present on this driver** — `vulkaninfo`
  lists `VK_KHR_{wayland,xcb,xlib}_surface` and no headless one. SDL cannot
  give Vulkan a surface without a display server.
- No `Xvfb`, `weston`, `cage` or `sway` is installed, so a nested or virtual
  compositor is not available either without adding a system dependency.
- Worth fixing on the way past: a failed backend init logs
  `[vulkan] backend initialization failed; renderer is inert` and then
  **segfaults**. Failure to initialise should exit, not crash.

**So it has to be an engine mode**, and it belongs on the **command line**, not
in env vars. Earlier phases reached for `VOXAGINE_*` env vars because
`Game/Source/main.cpp` discarded `argc`/`argv` and there was nowhere else to
put a switch; once the game parses arguments, a benchmark run should be one
command with no edited files behind it:

- `--map <path>` — the level. This is the one that removes the
  `ProjectSettings.vgps` edit.
- `--headless <W>x<H>` — no SDL window, no `VkSurfaceKHR`, no swapchain, and
  `Present` skips the blit and the queue present while still doing the fence
  work the frame loop and the GPU timestamps depend on. Every pass already
  renders into its own images; only `Present` touches the swapchain.
- The size is **authored**, not derived, which is forced by there being no
  window to derive from — and that is the half of this that fixes
  reproducibility even in windowed runs. `RenderContext::OnResize` would take
  it in place of the constrained window size.
- `--frames <n>` — run n frames and exit, so a run terminates itself instead of
  being killed on a timer. `voxagine_bringup --frames 120` is the precedent and
  the flag should keep that spelling.
- Report the `FrameProfiler` table once at exit rather than every second, so a
  run produces one parseable block.

`VOXAGINE_PROFILE` and the audit env vars stay as they are — they are diagnostic
toggles rather than run parameters, and they predate this.

**Why before phase 7.** That phase's acceptance is explicitly "cost measured at
1x *and* at 4K", because per-pixel scaling is the whole argument for cone
tracing over a per-pixel DDA. Taking those numbers by hand, twice per
experiment, on a display the user is trying to work on, is what this session
did and it is not repeatable.

**Note on what to log, learned here.** `RenderContext::OnResize` is the wrong
place to report the render size: it receives what the engine *asked* for —
`SetFullscreen` passes the screen resolution — and a tiling compositor may
ignore it. It reported 2732x1536 for a run whose passes were all rasterizing at
1365x767. `VKRenderPass::CreateAttachments` logs the size each pass actually
creates attachments at, which is the number a GPU timing is comparable against,
and it also exposes that `Settings::ResolutionScale` reaches only the Voxel and
Particle passes while UI, Debug and PostProcessing hardcode 1.0.

---

## Phase 1 — Restore the fidelity that is cheap

*Mechanical, shader-local. Sonnet. Independent of everything else — land it
first for a visible win.*

**Goal.** AO, faded shadows, sky and ground back, with no engine changes.

**Why this order.** It touches only HLSL and one constant, it is trivially
revertible, and it establishes the visual target the optimization phases must
preserve.

**Steps.**

1. **Shadow fading.** In `Game/Engine/Assets/Shaders/VoxelRenderer.ps.hlsl:69-71`
   replace the flat assignment with a distance-attenuated one. `MarchLight`
   already returns `result.Distance` (`SDFMarcher.hlsl:97`). Shape it after the
   original (Splody `:190`):
   `shadowMultiplier = min(marchLighting.Distance * k * difference + AMBIENT_VALUE, 1.0)`.
   **Retune `k` from scratch** — the original `0.0125` assumed 128 full-stride
   steps and this marcher takes 64 double-stride ones, so its `Distance` range
   is different. Expose `k` as a `#define` in `Defines.hlsl` while tuning.
2. **AO.** `GetAmbientOcclusion` needs `Mask`, `SRDirection` and `UV`, and
   `MarchDiffuse` (`SDFMarcher.hlsl:117-150`) does not write them. Copy the
   hit-time block from `March`'s `bDetailedResult` branch
   (`SDFMarcher.hlsl:179-201`) into `MarchDiffuse`'s hit branch. This is
   **hit-time only — zero added per-step cost**. Then
   `#include "AmbientOcclusion.hlsl"` and apply `Color.xyz *= ambient` as Splody
   did at `:291, :321-322`.
   - Note `AmbientOcclusion.hlsl` calls `IsVoxel`, which reads `voxelWorldData`
     — 12 extra fetches per *hit* pixel, bounded.
   - It also hand-applies `pow(…, 2.2)` at `:44` because lighting is in gamma
     space. Leave that alone here; Phase 6.4 addresses it properly.
3. **Retune `AMBIENT_VALUE`** (`Defines.hlsl:2`) down from `0.7` toward the
   original `0.5`. With AO and faded shadows back, `0.7` will look flat and
   washed out. Tune by eye with the user.
4. **Sky and endless ground.** Port Splody `:339-366` into the
   `marchDiffuse.Color.a == 0.0` branch (`VoxelRenderer.ps.hlsl:47-50`). Keep
   the sky colour as a constant for now; it becomes fog input in Phase 6.1.
   - Check this does not break the editor's expectations of a transparent
     background before committing.
5. **Mirror into `VoxelRenderer.ShadowLess.ps.hlsl`**: AO, ambient constant, sky
   and ground — but *not* the shadow work (rule 8).

**Acceptance.** User confirms on screen: contact darkening in corners, shadows
that soften with distance from the occluder, a sky. Zero validation errors.
Frame time compared against the Phase 0 baseline and recorded — if the cost is
larger than expected, note it and continue; Phases 2-3 are what pay for it.

**Do not** remove the 700-step cap or the 2× shadow stride here. Both are
Phase 2, after the brick grid makes them affordable.

**Implementation notes.**

- Shadow fading, AO and the `AMBIENT_VALUE` retune (0.7 → 0.5) landed as
  written: `SHADOW_FADE_K` and `SKY_COLOR` are `#define`s in
  `Defines.hlsl:9,13`; the shadow fade is
  `VoxelRenderer.ps.hlsl:75`; AO hit-time data was added to `MarchDiffuse`
  itself (`SDFMarcher.hlsl`, hit branch) rather than only to `March`, and
  applied at `VoxelRenderer.ps.hlsl:80`. Both landed in
  `VoxelRenderer.ShadowLess.ps.hlsl` too, minus the shadow ray (rule 8).
- **Sky/ground did not land where the plan said.** The `marchDiffuse.Color.a
  == 0.0` branch in `VoxelRenderer.ps.hlsl` only runs for pixels some AABB
  proxy cube rasterized to — this renderer draws instanced proxy cubes, not a
  full-screen quad like Splody's `VoxelRendererMRT.vs.hlsl` did, so "no AABB
  covers this pixel" was never transparent, it was simply never shaded at
  all. Growing the world-sized ground box to cover the sky (tried first) hit
  two dead ends: `VoxelRenderer.vs.hlsl:66` clamps every AABB vertex to
  `[0, worldSize]`, silently capping any taller box at `worldSize.y` (128 for
  the big level) regardless of what height C++ requests; and even at that
  capped height, a camera positioned inside a instanced-cube proxy this large
  hit some rendering-pipeline edge case (per-instance culling/depth,
  root cause not fully isolated) that dropped a large sky/ground to black
  depending on camera position. **Fixed instead in
  `PostProcessing.ps.hlsl`/`PostProcessing.Debug.ps.hlsl`**, which already run
  full-screen for every pixel (used for FXAA/UI compositing): on
  `targetTexture`'s raw alpha == 0 (sampled before FXAA, which repurposes the
  alpha channel as luma and can't be used as a hit/miss signal), it
  reconstructs the camera ray with `Camera.hlsl`'s `GetRay` (existing,
  previously-unreferenced code — see `CLAUDE.md`, "Camera.hlsl unreferenced in
  Game/") and ports Splody `:339-366` there instead. This needed a real
  binding change, not just shader-local: `PostProcessingPass` now also takes
  the voxel `Mapper` (`PostProcessingPass.h/.cpp`, wired from
  `RenderContext.cpp`'s existing `m_pVoxelMapper`), and both PostProcessing
  shaders declare `voxelWorldData : register(u0)` for the ground colour
  sample. No new SPIR-V/C++ contract entries were needed beyond the existing
  per-class offset scheme (`CMake/Shaders.cmake` / `VKShaderBindings.h`) — `u0`
  in this pass doesn't collide with anything already declared there.
- **Ride-along fix, independent of the above:** `MarchDiffuse`
  (`SDFMarcher.hlsl`) had no bounds check before its first `GetVoxel` sample —
  unlike `March`, which pre-tests `IsInChunk`/`GetDistanceToWorld`. Every
  caller happened to always pass an in-bounds origin, so this was latent, not
  triggered. Added the same guard `March` has. Found while chasing the sky
  proxy's out-of-bounds risk above; kept even though that approach was
  abandoned, since it closes a real gap in the voxel-buffer read path.
- **Known regression, not fixed here (by design):** shadow banding. The
  distance-based fade makes `MarchLight`'s 64-step, 2×-stride quantization
  visibly banded — before this phase a shadow hit just snapped to flat
  `AMBIENT_VALUE`, so the coarse sampling was invisible. This is the
  documented Ground Truth scale-up cut; Phase 2's brick grid is what makes
  removing the 2× stride affordable, per this phase's own "do not remove"
  note above. Confirmed with the user and deliberately left alone.
- **Shine-line fake specular pulled forward from Phase 5 step 6 and landed
  here instead.** User asked about it mid-phase; since it only needs
  `res.UV`/`res.Normal` (this phase's AO work already populates both) and
  costs nothing extra per step, there was no reason to wait for point lights.
  Ported from Splody `VoxelRendererForward.ps.hlsl:300-303` into both
  `VoxelRenderer.ps.hlsl` and `.ShadowLess.ps.hlsl`, applied as a multiplier on
  `shadowMultiplier` before the AO multiply (matches Splody's order: shine
  line scales `fDirectLighting`, which is then combined with ambient).
  User-confirmed on screen. **Phase 5 step 6 is done — skip it there.**
- **Shine line rewritten to actually depend on `lightDirection`, not ported
  as-is.** The Splody source it came from hardcoded a `-Z`-facing wall and a
  floor's `-Z` edge — fine for whatever fixed light Splody happened to be
  tuned against, but wrong in general. `lightDirection` here is also a fixed
  engine constant (`RenderContext.cpp:399`,
  `normalize(-0.4, -0.8, 0.6)`), not something that varies at runtime, so
  "follows the light" means computing the right fixed answer for *this*
  vector rather than literally animating. Landed as `GetShineLine()` in
  `AmbientOcclusion.hlsl`, called from both pixel shaders:
  - Walls (`abs(normal.y) < 0.5`): still always the top edge — `UV.y` is
    world-up for every wall axis (the `abs(Normal.b) > 0.5` swap in
    `SDFMarcher.hlsl`'s UV block makes this true for both X- and Z-facing
    walls, verified by hand from the `v3EndRayPos.zxy`/`.yzx` dot products,
    not just assumed) — but the boost is now scaled by `difference`, saturated
    over `(0.1, 1.0]`. Without that scaling, any wall just past the
    `difference > 0.1` lit gate got the same full boost as one facing the
    light head-on, which read as a rim baked onto every silhouette edge
    regardless of which way it faced — since `difference` for a wall is
    `dot(normal, -lightDirection)` with a purely horizontal normal, it's
    already exactly "how much this wall faces the light," so scaling by it is
    what makes faces facing away actually go dark.
  - Floors (`normal.y > 0.5`, `UV` = world `(X, Z)`, no swap): originally
    picked whichever single edge had the larger `towardLight` component.
    **User's own eyes caught the actual issue**: `lightDirection`'s horizontal
    split is roughly 34°/56° (`x=-0.4, z=0.6`), not axis-aligned, so a corner
    voxel should show a rim on *two* adjacent edges, weighted by how much
    each axis points toward the light — not snap entirely to the stronger
    one. Now evaluates the X and Z edges independently (each weighted by
    `abs(towardLight.x)` / `abs(towardLight.y)`) and combines with `max`, so a
    near-axis-aligned light still degrades gracefully to one dominant edge
    while this one shows both. User-confirmed on screen after three
    iterations — this took real back-and-forth to get right, worth reading
    the git history on `AmbientOcclusion.hlsl` if revisiting it.
- **Known issue, confirmed on screen but not chased down:** the shadow fade
  can push `shadowMultiplier` brighter than the AO-darkened areas around it —
  user described it as AO getting "excluded" near shadow edges. Likely
  `SHADOW_FADE_K` (still at Splody's original `0.0125`, per the "retune from
  scratch" note above — never actually retuned) saturates `shadowMultiplier`
  to its `1.0` cap too early: at `AMBIENT_VALUE = 0.5`, `difference ≈ 1`, that
  cap is reached at `Distance ≈ 40`, well inside `MarchLight`'s ~128-unit
  reach, so anything beyond a fairly short distance from the occluder renders
  at full brightness regardless of AO. **User explicitly deferred fixing this
  to Phase 2** (alongside the shadow banding above) rather than re-tuning `k`
  blind — both are downstream of the same 64-step/2×-stride shadow ray.
- Frame time versus the Phase 0 baseline, `game-release`,
  `Valley_Path_To_Castle_Beat1`, editor build with GPU profiling on: Voxel
  pass ranged **1.48–6.0 ms** depending on sightline (baseline was
  1.07–1.10 ms at the stationary default spawn only), Post Processing
  **0.06–0.18 ms** (baseline 0.022–0.023 ms) — the sky/ground reconstruction
  and its `PostFxGetVoxel` ground sample account for most of that increase.
  Zero validation errors throughout. Not re-measured from a deliberately
  chosen long sightline (same caveat as the Phase 0 baseline) — Phases 2-3
  are what pay this down, per the phase's acceptance note above.
- **Fixed, pre-existing CI failure carried over from Phase 0's merge:**
  `FrameProfiler.cpp` was only added to `EngineSources.cmake`, so it compiled
  into `voxagine` (the full-engine target) but not `voxagine_vulkan`.
  `VKCommandEngine.cpp` - part of `voxagine_vulkan` "by design, so it can be
  built and run before the rest of Core compiles" per `CMakeLists.txt` -
  calls `FrameProfiler::Get()`/`Report()` for its GPU timestamps, so
  `voxagine_bringup` (which links only `voxagine_vulkan`) failed at link time
  with an undefined reference. This was already broken on `master` after PR
  #3 merged - `gh run list` shows that CI run failed too, just not caught
  before merging. Fixed by moving `FrameProfiler.cpp` to `voxagine_vulkan`'s
  source list instead (and removing it from `EngineSources.cmake`, since
  `voxagine` already links `voxagine_vulkan` publicly - compiling it into both
  would have linked editor/game with a duplicate symbol). This only worked
  because `FrameProfiler.cpp` didn't actually need its `#include "pch.h"` -
  removed that too, since pulling in the full engine pch would have broken
  `voxagine_vulkan`'s "no engine dependencies" contract for real. Verified
  with a from-scratch configure into a clean directory (Debug and Release,
  `VOXAGINE_CHECK_RTTR=ON`) matching `.github/workflows/build.yml`, not just
  the local presets, since that's what CI actually runs.

---

## Phase 2 — Occupancy brick grid

*Design. **Opus**.*

**Goal.** Make traversal skip empty space structurally, so the 700-step cap and
the 2× shadow stride can be removed rather than tuned.

**Why before Phase 3.** Phase 1 restores fidelity but leaves the two ugliest
scale-up scars in place (the 700-step cap and the double-stride shadow ray).
This is the phase that pays for removing them, and it makes the Phase 3 prepass
rays cheap as a side effect.

**Design.**

- Brick = 8³ voxels. For a 768×128×768 window: 96 × 16 × 96 = **147,456 bricks**.
- Store a **count** per brick, not a bit — destruction decrements, and a bit
  cannot be un-set correctly without rescanning. 8³ = 512 fits in `uint16`.
  **288 KiB** on the GPU, negligible beside 288 MiB.
- Linearization must be stated explicitly and match on both sides; mirror the
  voxel convention `x + y*W.x + z*W.x*W.y` (rule 4).
- New `Mapper`, `E_READ_WRITE`. **It must be back-buffered exactly like the
  voxel mapper** — when `SwapBuffer` flips the voxel window, a brick grid built
  against the old buffer is stale garbage. Hook the same `BufferSwapped` event
  (`RenderContext.cpp:684-687`).

**CPU maintenance.** Every existing write site, all of which are already
choke points:

| Site | Action |
|---|---|
| `ChunkSystem::RenderChunk` (`ChunkSystem.cpp:403-425`) | bulk recount for the chunk, on the existing job thread |
| `RenderContext::ModifyVoxel` / `ModifyVoxelFast` (`RenderContext.h:211-239`) | increment/decrement on transition between empty and occupied |
| `VoxelBaker::Occupy` / `Clear` (`VoxelBaker.cpp:284,290,348,389`) | same, per stamped/erased voxel |

Occupancy is `alpha > 0` (rule 3). Only transitions change the count —
overwriting an occupied voxel with a different colour must **not** increment.

**Shader.** Two-level DDA in `SDFMarcher.hlsl`: march bricks, and descend into
the fine DDA only for bricks with a nonzero count. Then:

- Replace the hardcoded `700` (`VoxelRenderer.ps.hlsl:38`) with the true world
  diagonal, as Splody computed it (`maxDim`, `:265`). Distant geometry stops
  being truncated.
- Remove the 2× stride from `MarchLight` (`SDFMarcher.hlsl:106-107`) and raise
  its step budget. This restores shadows from single-voxel-thick occluders and
  shadow reach across the level — a large part of the "better shadows" memory.

**Memory-type fix, rides along.** Prefer `DEVICE_LOCAL | HOST_VISIBLE` (ReBAR)
in `VKMapper.cpp:87-89` when the memory type exists, falling back to the current
flags. Structurally free, large dGPU win. **Do not** attempt a staging-buffer /
dirty-region upload rewrite here — that is a much bigger change and is not on
this plan.

**Acceptance.** Cap and stride removed, and frame time still at or better than
the Phase 1 measurement (record both). Shadows visibly reach further and catch
thin occluders — user confirms on screen. Destruction
(`ApplySphericalDestruction`) leaves no stale bricks: blow holes in a wall and
confirm light passes through, then rebuild and confirm it does not. Chunk
crossing at a window boundary shows no corruption. Zero validation errors.

**Risks.**

- **Thread safety.** `CLAUDE.md` records an open `VoxelGrid` data race —
  `IntegrityJob` traverses on a worker while the main thread calls
  `ModifyVoxel`. Brick counts written from both threads make it worse. At
  minimum use atomics for the counts; note in `CLAUDE.md` if you do not fix the
  underlying race.
- A missed decrement is invisible (a brick stays "occupied", costing perf only);
  a missed increment **loses geometry**. When debugging, suspect increments.
- Add a debug validation path that recomputes the whole brick grid and compares,
  runnable on demand. It will pay for itself.

**Implementation notes.**

- `VoxelBrickGrid`
  (`Voxagine/Source/Core/Platform/Rendering/VoxelBrickGrid.h/.cpp`) owns the
  counts. It keeps **two representations on purpose**: a plain-CPU-memory
  count array that is authoritative and takes every read-modify-write, and a
  write-only mirror in the mapped GPU buffer. That split is the whole reason
  the CPU cost stayed small - the mapping is uncached, so `++count[brick]`
  performed directly on it would be an uncached *read* per voxel write. The
  earlier design sketch that stored counts only on the GPU would have been
  much slower than the marcher saving it was.
- **Bricks are 8³ and counts are `uint16`**, as planned. 147,456 bricks for a
  768×128×768 window: 288 KiB of CPU counts per buffer and 576 KiB of GPU
  mirror per buffer (the mirror is `uint32` - HLSL 16-bit types need
  `-enable-16bit-types` and buy nothing at this size).
- **The mapper is `E_UNKNOWN` + `E_READ_WRITE`**, so it binds as a plain
  storage buffer at `u1` (`RW_STRUCTURED_BUFFER(uint) voxelBrickData`).
  Read-write is not because the GPU writes it - nothing does - but because
  read-only would have put it in the `t` range, renumbering the AABB buffer
  and both particle textures and every shader that names them.
- **This surfaced a latent bug in `VKPassBindings.cpp` and it is worth
  knowing about.** `E_STORAGE_BUFFER` is the one binding kind that does not
  determine its own register class: HLSL spells the read-only form
  `StructuredBuffer` (`t`) and the read-write form `RWStructuredBuffer`
  (`u`), and Vulkan calls both a storage buffer. `MakeBinding` guesses `t`.
  `AddBuffers` already patched the binding number back to `u` afterwards;
  `AddMappers` did not, because until now no mapper had ever been
  read-write *without* a colour format. The brick mapper is the first, so it
  landed on binding 101 - on top of `particlePass` at `t1`. Both are now
  routed through one `MakeUnorderedStorageBuffer` helper so it cannot be
  forgotten a third time. Validation caught it immediately, which is the
  argument for rule 5.
- **Marcher.** `MarchBricks` in `SDFMarcher.hlsl` replaces the bodies of both
  `MarchDiffuse` and `MarchLight`. The trick that makes restarting the fine
  DDA in each occupied brick cheap: its state is a **closed-form function of
  (origin, direction, voxel position)**, not an accumulation, so it can be
  re-derived at any point along the ray from the *original* origin - which is
  also what keeps `Distance`, `SmoothPosition` and `UV` origin-relative at
  hit time with no fixups.
  - The brick a ray enters is entered through the same face as that brick's
    first voxel, so the outer DDA's crossing mask is the correct normal for a
    hit on the first sample. Missing this would put wrong normals on every
    surface first touched at a brick boundary.
  - Reconstructing the entry voxel from the crossing parameter lands exactly
    on a brick face, where `floor()` names the voxel on the far side for a
    negative direction component - and float error can push it either way.
    The `clamp` into the brick fixes both; the voxel it would otherwise pick
    belongs to a brick the walk has already cleared as empty.
  - `March` (single-level) is deliberately left alone. Its only caller is
    `SDFDepth.ps.hlsl`, which nothing loads, so there was no behaviour to
    preserve by porting it and no way to verify the port.
- **The 700-step cap is gone**, replaced by the window diagonal in bricks
  (`length(worldSize) * BRICK_INV_SIZE + 2`, ~139 for the big level). **The
  shadow ray's 2× stride is gone** and it now gets the same budget as the
  primary ray.
- **`SHADOW_FADE_K` retuned 0.0125 → 0.0025**, which is phase 1's deferred
  "AO looks excluded near shadow edges" item. The fade saturates at
  `(1 - AMBIENT_VALUE) / (K * difference)`: 40 units at the old value, so
  anything more than a short distance from its occluder was at full
  brightness regardless of AO. 200 units now. Phase 1's banding is gone too -
  it was the 2× stride quantizing `Distance`.
- **CPU maintenance splits by write shape**, and the split matters:
  - `ModifyVoxel` / `ModifyVoxelFast` (`RenderContext.h`) do the transition
    update. `ModifyVoxelFast` gained a read of the old voxel it did not do
    before - there is nowhere else to learn the old occupancy from.
    `ModifyVoxel` already read the same word, so this is a cost that path
    always paid.
  - `ChunkSystem::RenderChunk` **rebuilds** its chunk's bricks rather than
    diffing: `BeginRegion` / per-occupied-voxel `AddVoxel` / `EndRegion`,
    accumulating from the chunk's own CPU-side voxels. Diffing would mean
    reading 8.4 M voxels back out of uncached memory, which costs more than
    the write itself. Only occupied voxels call `AddVoxel`, and most of a
    chunk is air.
  - `RenderChunk` took a **`bool bBackBuffer`** parameter. The counts are
    per-buffer, and a pointer alone does not say which buffer it points at -
    writing voxels into one while updating the other's counts loses geometry
    a swap later, silently.
  - `ChunkSystem::ClearChunk` needs no brick work: it is **dead code**,
    declared and defined and called by nothing.
- **Front/back.** The brick mapper and the grid flip inside the voxel
  mapper's `BufferSwapped` subscriber in `RenderContext.cpp`, not at the
  `ChunkSystem` call site, so the three cannot drift apart. Each buffer's
  counts describe only that buffer's voxels, so the two never have to agree -
  and the pre-existing behaviour where front-buffer stamps are lost on a swap
  is mirrored exactly rather than papered over.
- **Thread safety**: counts are `std::atomic<uint16_t>` with relaxed
  ordering, per the phase's risk note. In practice the two buffers are
  disjoint and each is written by one thread at a time (point updates are
  main-thread, the chunk job owns the back buffer), so the atomics are
  insurance rather than a fix. **The underlying `VoxelGrid` data race in
  `CLAUDE.md` is still open and this phase did not address it.**
- **Underflow is reported, not wrapped.** A decrement on a zero count means a
  voxel was cleared that the grid never counted, and wrapping to 65535 would
  hide the accounting bug behind a merely-slower traversal. It puts the value
  back and logs once.
- **Debug validation**: `RenderContext::ValidateBrickGrid()`, wired to
  **View → Validate Occupancy Bricks** in the editor. Recomputes every count
  from the voxel buffer and logs the disagreements, separating "counted high"
  (costs traversal) from "counted zero while occupied" (deletes geometry).
  Run against `Valley_Path_To_Castle_Beat1` after ~1200 frames of play:
  **0 of 147,456 bricks disagreed.**
- **ReBAR rode along** in `VKMapper.cpp` as planned - `DEVICE_LOCAL |
  HOST_VISIBLE` preferred, falling back to plain host-visible. The memory
  type is tested for existence first so the fallback is silent on hardware
  that has no such heap, rather than logging an allocator error per resize.
- **Measurements**, `game` build, `Valley_Path_To_Castle_Beat1`, the same
  default-spawn vantage as the Phase 0 baseline (still not a long sightline -
  see that caveat). The two ride-alongs were measured separately because they
  pull in opposite directions:

  | | Voxel pass | `CPU VoxelBaker::Bake` |
  |---|---|---|
  | Phase 0 baseline | 1.07–1.10 ms | 0.027–0.036 ms |
  | Phase 1 | ~1.48 ms | — |
  | Phase 2, bricks only | 0.96 ms | 0.090 ms |
  | Phase 2, bricks + ReBAR | **0.67–0.68 ms** | 0.218–0.224 ms |

  The GPU numbers are for *more* work than any earlier row: the step cap and
  the shadow stride are both gone. ReBAR is a clear net win despite the CPU
  cost - it saves 0.29 ms of GPU at 200 Hz (58 ms/s) and costs 0.13 ms of CPU
  at the 60 Hz fixed tick (7.8 ms/s) - but the CPU side of it is real and
  worth remembering, because it makes every *read* of the voxel buffer a PCIe
  read of VRAM. Anything added to the CPU read paths from here is more
  expensive than it used to be.
- Zero validation errors in `game`, `game-release`, `editor` and
  `editor-release`.

---

## Phase 3 — Low-resolution depth prepass

*Design. **Opus**. The main answer to "we are too resolution dependent".*

**Goal.** Decouple traversal cost from screen resolution without softening the
image.

**Why it works here.** Voxels are much bigger than pixels, so hit distance is
extremely coherent across neighbouring pixels. A coarse ray can prove a long
stretch of space empty on behalf of ~64 fine pixels. **This is not upscaling** —
the final image is still shaded at full resolution, per pixel; the prepass only
supplies a safe starting distance. The original engine did exactly this: Splody
started its march at `dist - 0.5` from a depth prepass
(`VoxelRendererForward.ps.hlsl:276`).

**Why after Phase 2.** The prepass rays are themselves full traversals; with the
brick grid already in they are cheap, and the two optimizations compose instead
of competing. Building the prepass first would mean tuning its resolution
against a cost that Phase 2 then changes underneath you.

**Steps.**

1. Add a real prepass. `SDFDepth.vs.hlsl` / `SDFDepth.ps.hlsl` already exist and
   compile, but they are the *old* MRT depth+shadow pass — treat them as
   reference, and write a new one that outputs a single `R32_FLOAT` hit
   distance. Register the pass in `RenderContext.cpp:690-810` alongside the
   existing Particle/Voxel/Debug/UI/PostProcessing passes.
2. Size the target at ⅛ (start there) of the render resolution, using the same
   AABB proxy geometry as `VoxelPass` so coverage matches.
3. **Make it conservative.** A low-res texel covers ~64 pixels whose true hit
   distances differ. Take the **minimum** over the texel's footprint, then
   dilate by one texel (a 3×3 min filter, either a tiny compute pass or a
   gather in the consumer). Then subtract a safety margin of a voxel or two.
   *If this is wrong, geometry disappears* — this is the step to get right.
4. In `VoxelRenderer.ps.hlsl`, sample the prepass, advance the ray origin to
   `max(0, prepassDepth - margin)` and reduce the step cap for the refine march
   to a small number (start ~32, tune down). Keep a fallback: if the refine
   march exits without a hit, either accept the miss or fall back to a full
   march for that pixel — measure both, prefer the cheaper if artefacts allow.
5. Apply the same start-distance trick to the **shadow ray**, which is a second
   full traversal per lit pixel.
6. Bindings: new texture → `VKShaderBindings.h` and `CMake/Shaders.cmake` stay
   in lockstep (rule 1).

**Optional second step — half-res lighting.** Shadow and AO terms are
low-frequency on voxel art. Compute them at prepass resolution and upsample with
a depth-aware (bilateral) filter, keeping colour and edges at full res. Do this
only after the prepass itself is stable, and measure — it may not be needed.

**Acceptance.** Voxel pass GPU ms substantially down versus Phase 2 (record it).
No missing or flickering geometry at silhouettes, on thin structures, or when
the camera moves fast — have the user watch motion specifically, a still frame
hides conservativeness bugs. Zero validation errors. `ResolutionScale` still
works and is still `1.0`.

**Risk.** Under-conservative dilation produces holes that only appear at certain
angles or during motion. If artefacts appear, increase the margin before
increasing the prepass resolution.

**Implementation notes.**

- **The headline: the prepass works, and it saves nothing, because the AABB
  proxy cubes were already doing its job.** This phase's premise is that a
  coarse ray can prove a long stretch of space empty on behalf of ~64 fine
  pixels. That stretch does not exist. `VoxelRenderer.vs.hlsl` hands the pixel
  shader `WorldPosition` on the rasterized proxy cube, and perspective-correct
  interpolation makes that the point where *this pixel's* camera ray enters
  *that* box - so every primary ray already starts at a per-pixel, exact,
  free box entry rather than at the camera. The prepass can only re-derive a
  coarser version of the same number. "Ground truth" above says the pixel
  shader "marches the whole world buffer", which is true of `IsInChunk`'s
  bounds test but not of the origin, and that is the sentence this phase was
  planned on.
- **Measured, `game` build, `Valley_Path_To_Castle_Beat1`, the same
  default-spawn vantage as phases 0 and 2** (still not a long sightline). The
  middle two rows are the experiment that proves the prepass is functional
  rather than inert - they move the march origin back to the camera, which is
  the world this phase was designed for:

  | | Voxel pass |
  |---|---|
  | Phase 2 (proxy origin, no prepass) | 0.72–0.78 ms |
  | Phase 3 (proxy origin + prepass) | 0.745–0.773 ms |
  | Camera origin, no prepass | 0.85–0.94 ms |
  | Camera origin + prepass | 0.79–0.81 ms |
  | Proxy origin, shadow ray cut out of the shader | 0.34–0.39 ms |

  The prepass recovers about 70% of what marching from the camera costs. The
  proxy cubes recover all of it. Prepass pass itself: 0.017–0.019 ms.
- **Where the time actually goes, which is the useful finding.** The shadow ray
  is **53% of the voxel pass** - 0.75 ms with it, 0.35 ms without. It starts at
  the surface, has no proxy geometry bounding it, and nothing in this phase can
  reach it. Capping its reach at 200 units (25 brick steps, the distance beyond
  which `SHADOW_FADE_K` saturates the fade) changed the number by nothing, so
  that cost is **near-field per-lit-pixel work** - ray setup and the first steps
  out of the surface's own brick - not long traversal. The only lever left on it
  is casting *fewer* rays: the "Optional second step - half-res lighting" above,
  which is now the highest-value item in this document that is not already
  scheduled. **Step 5 of this phase is not implementable as written** - a
  "start distance" for the shadow ray needs a light-space prepass, which is a
  shadow map, which is 6.2.
- **Deleted, after being re-measured under the two conditions its numbers were
  open to.** The objections were fair: everything above was measured at a capped
  200 fps, because `Settings.vgs` never loaded and `FrameLimit` sat at the
  hardcoded 1/200, and only ever at one resolution — which matters, because
  decoupling traversal from resolution is this phase's whole premise. Re-run
  uncapped with vsync off so the GPU is saturated, sweeping a 16x range in pixel
  count, alternating the prepass on a 4 s timer *within* one run so the camera
  never moves between states:

  | Voxel target | Saves | Prepass costs | Net | fps ON/OFF |
  |---|---|---|---|---|
  | 682x384 | 0.58% | 1.99% | **+1.41%** | 843 / 861 |
  | 1365x767 | 0.53% | 0.90% | **+0.37%** | 321 / 322 |
  | 2730x1534 | 0.70% | 0.44% | -0.26% | 105 / 105 |
  | 3840x2158 (4K) | 0.72% | 0.43% | **-0.29%** | 58 / 58 |

  **The saving is a flat ~0.6% of the voxel pass at every resolution.** It never
  grows relative to the work it optimizes — the exact opposite of "a coarse ray
  proves a long stretch empty on behalf of ~64 fine pixels". What improves is
  only the prepass's own overhead amortizing, and that converged: 2x to 4K moved
  the net by 0.03 percentage points. After deletion, same vantage: 2.198 ms and
  326 fps against 2.227 ms and 322 fps for the toggle set to off — removal beats
  the toggle, which still cleared the target and bound the texture.

  **The generalisable part:** a percentage that stays constant as you scale the
  input is the signature of an optimization that is not addressing the thing
  that costs. Sweeping the axis an optimization claims to attack is a cheap and
  decisive test, and it is worth doing *before* building it.

- *(Historical, superseded by the bullet above.)* It was landed rather than
  reverted, behind **View → Depth Prepass** in the
  editor, on by default. Two reasons: phase 4's far-field volume marches where
  no proxy cube covers, which is exactly the case the prepass was built for; and
  the toggle is how the next session measures a long sightline without having to
  rebuild the thing first. If phase 4 does not use it, delete it.
- **Design choices worth keeping if it is ever revived.**
  - **It measures brick entry, not the voxel hit.** `MarchBrickEntry` in
    `SDFMarcher.hlsl` is the outer level of `MarchBricks` with the fine descent
    removed. Cheaper, and much safer: geometry thinner than a texel footprint
    can slip between the nine sampled rays and be skipped past, and a brick is
    `BRICK_SIZE` voxels wide, so a one-voxel feature stays catchable eight times
    further out than a voxel-exact prepass would manage. It gives up almost
    nothing, because the fine march has to walk the occupied bricks either way -
    what gets skipped is exactly the run of empty ones in front of them.
  - **Rays are reconstructed from a new `invMvp`** in the camera constant buffer
    (appended last; the four trailing scalars pack into one 16-byte register, so
    it lands 16-byte aligned at offset 224 - verified against the SPIR-V), not
    from `Camera.hlsl`'s `GetRay`. Inverting the matrix the voxel VS projects
    with makes the prepass ray provably the pixel's ray. `GetRay` rebuilds it
    from FOV and aspect via `mv`, which is `lookAt(0,0,0 → +Z) * transform` -
    that is only the inverse view matrix for yaw-only cameras, and the phase 1
    sky/ground could not have caught the difference because it only reads
    `rayDirection.y`, the one component the discrepancy leaves alone. **Not
    chased down; `GetRay` is still used by the PostProcessing sky.**
  - The consumer uses `Load` with clamped texel coordinates, not `Sample` with
    offsets: `R_DEF_WRAP_MODE` is `E_WRAP`, and a neighbour offset that wrapped
    to the far edge of the screen would report a distance from somewhere else -
    which, being fed into a minimum, could only ever be too small in the
    direction that skips too far.
  - Orthographic cameras get a miss written unconditionally. The voxel VS builds
    its ray as `worldPosition - camPosition`, which is the camera ray only under
    perspective, so "distance along the ray from the camera" has no meaning
    there. `Camera::SetOrthographic` cannot currently be called anyway - its
    guard reads `if (bIsOrthographic == bIsOrthographic) return;`.
  - The reduced step cap of step 4 is `worldDiagonal - skip` rather than a tuned
    constant, so a ray that skipped nothing keeps the full budget and cannot be
    truncated. The shadow ray keeps its own.
- **Ride-along fix.** `MarchDiffuse` used to return a miss outright when handed
  an origin outside the window. It now advances to the window entry and marches
  from there. The prepass made this reachable - a ray grazing the window's edge
  can land just outside after the skip - and returning a miss there punches
  holes along the window silhouette.
- **Debug lines now default off** (`RenderContext::m_bDebugEnabled`,
  `Editor::m_bRenderDebugLines`), unrelated to the phase and asked for
  separately. `m_bDebugCleared` had to start `false` alongside it: post
  processing samples the debug target unconditionally, so a pass that never
  drew *and* never cleared would leave it undefined.
- Zero validation errors in `game`, `game-release`, `editor` and
  `editor-release`, with the prepass both enabled and disabled. User confirmed
  on screen: no popping, flickering or dropped geometry while flying, beyond the
  pre-existing chunk-load hitch.

---

## Phase 4 — Far-field LOD volume

*Design. **Opus**. This is the "viewports should render more" phase.*

**Goal.** See the whole level, without growing the 3×3 window.

**Approach: a static, downsampled colour volume of the entire level, for display
only.**

- Keep the 3×3 detail window exactly as it is — physics, entities, destruction
  and gameplay all continue to live there, untouched.
- Full level for a 6×6 chunk world is 1536 × 128 × 1536. At **4× downsample**:
  384 × 32 × 384 = 4,718,592 voxels × 4 B = **18 MiB**. Build it per chunk at
  load (or bake at save time) from the RLE data in `Chunk::m_pEncodedVoxelData`.
- Shader: when a primary ray leaves the detail window — or starts outside it —
  continue marching in the far-field volume. Two volumes, two coordinate
  systems; keep the transform in one place and document it.
- The detail/far-field seam hides well behind Phase 6.1 fog, and voxel art
  tolerates a 4× LOD at distance. Shade the far-field more cheaply (no AO, no
  shadow ray, ambient + fog only).

**Rejected: widening the window to 4×4 or 5×5.** Memory scales quadratically —
4×4 is 1024×128×1024 = 512 MiB, ×2 buffers = **1 GiB** of host-visible memory —
and it drags entity streaming, physics and `Chunk` residency along with it for a
linear increase in view distance. The far-field buys "render the whole level"
for 18 MiB. Do not take the wider-window path without a written reason.

**Ride-along fix.** `Chunk::m_VoxelData` is resized once on first load
(`Chunk.cpp:49-51`, `:73-77`) and **never shrunk** — 8,388,608 `Voxel`s × 16 B =
**128 MiB of CPU memory per chunk that has ever loaded**, plus a ≥10 MB encoded
reserve (`Chunk.cpp:125-127`). On a 6×6 level that is tens of gigabytes if every
chunk is visited. Free or shrink `m_VoxelData` after `EncodeVoxels()`. Do this
first, in its own commit — the far-field build touches the same code and you
want the memory win isolated and attributable.

**Acceptance.** Standing at one end of `Valley_Path_To_Castle`, the far end is
visible. Cost measured and small (it should be, at 1/64 the voxels and cheap
shading). Chunk streaming still works and the seam does not pop distractingly
when the window shifts — user watches a full traversal of the level. Peak RSS
recorded before and after the `m_VoxelData` fix. Zero validation errors.

**Implementation notes.**

- **Two of this phase's stated premises were false, and the second one changed
  the design.**
  - *The ride-along fix does not exist.* `Chunk::EncodeVoxels` already ends with
    `m_VoxelData.resize(0); shrink_to_fit()` and `m_pEncodedVoxelData.
    shrink_to_fit()` — `Chunk.cpp:163-165`, present since the initial commit, so
    not something the port introduced. A chunk holds its 128 MiB only while it
    is resident, and the 3×3 window bounds that at nine. There was nothing to
    fix; the "tens of gigabytes" figure assumed every visited chunk kept its
    array forever. Nothing was changed here.
  - *There is no chunk voxel data to build from.* The plan says to build the
    volume "from the RLE data in `Chunk::m_pEncodedVoxelData`". A `.wld` stores,
    per chunk, only a `RootEntities` array — no voxels at all (verified against
    `Valley_Path_To_Castle_Beat1.wld`: 36 chunks, each with just that key). A
    chunk's `m_VoxelData` is a *product* of loading — `UpdateGroundPlane` plus
    whatever `VoxelBaker` stamps once its entities are in the world — and
    `EncodeVoxels` runs on *unload*, so a chunk that has never been resident has
    neither decoded nor encoded voxels. Building from chunk data would have
    produced a far field showing only where the player had already been.
- **So the source is the entities, which is what the window's source is too.**
  `FarFieldBaker::Build` walks every chunk's `RootEntities`, deserializes the
  static ones into throwaway entities that are never added to the world — the
  pattern `Chunk::LoadEntities` already uses for one it decides not to keep —
  and stamps their `VoxRenderer`s. Two details that cost time to find:
  `Entity::GetComponent` searches only `m_Components`, and a freshly
  deserialized component sits in `m_AddedComponents`, so it must be
  `GetComponentAll`; and `~Entity` deletes components but not children, so the
  cleanup recurses.
- **Placement is shared code, not a second copy of the maths.** `VoxelStamp.h`
  holds what was the middle of `VoxelBaker::Occupy`: the rotation quantization,
  the fitted-size origin, and the per-voxel walk. `Occupy` passes the window's
  world offset, the far field passes zero, and that parameter is the entire
  difference between the two grids. Getting this wrong is not subtle — the far
  field and the window disagree about where the level is, and the seam moves as
  the window slides. `forward`/`right`/`up` were computed in `Occupy` and never
  read; they did not survive the extraction.
- **It is marched in post processing, not in the voxel pass.** The voxel pass
  only rasterizes AABB proxy cubes, and by definition no proxy covers the part
  of the level that is not resident — a pixel showing only far-field geometry is
  never rasterized there at all. `PostProcessing.ps.hlsl` already ran
  full-screen and already owned the "the scene left this pixel empty" branch for
  sky and ground, so the far field slots in as one more layer of that.
  `GetBackground` in `FarField.hlsl` is now shared by the plain and Debug post
  processing shaders, which were carrying identical copies of the sky/ground
  half.
- **The march starts past the detail window, not at the camera.** The far field
  covers the whole level, window included, and its copy of the near geometry is
  conservatively fatter than the real thing — so marching from the camera paints
  blobs over exactly the space the window just proved empty. Skipping to the
  window's far face means it is only ever consulted where the window has no
  answer. It must also not start before the volume: a camera above the level
  looking at a distant part produces a ray that never touches the window *and*
  starts outside the volume, and a march beginning there finds its first brick
  out of bounds and gives up — which is precisely the "see the far end of the
  level" case. Both crossings come from `GetBoxCrossings`.
- **The ground plane was baked in, and then taken back out.** The endless ground
  `GetBackground` draws is not an approximation of the chunk ground plane — it
  samples the resident window's own y=0 layer, which *is*
  `Chunk::UpdateGroundPlane`'s output, tiled with `fmod`. It therefore already
  reproduces the level's ground exactly, everywhere, and past the level's edge
  where a baked one would stop. Baking a second copy put the same surface in two
  places with two different lightings — the far field's cells win on distance
  inside the level, the analytic plane takes over outside it — and produced a
  hard brightness edge along the window's boundary, which is what the user saw.
  It also marked the whole bottom brick layer occupied, so every downward ray
  descended to the fine walk.
- **Far-field shading is `AMBIENT_VALUE`, deliberately not a tuned constant.**
  It is exactly what `VoxelRenderer.ps.hlsl` computes for a surface whose shadow
  ray found nothing. The first attempt used its own `0.65` and the same hillside
  changed brightness as it crossed the window's edge.
- **Two ride-along fixes to the endless ground, both found by putting far-field
  geometry next to it for the first time.** Phase 1 introduced it as sky's
  companion, where neither was checkable against anything.
  - *It was a flat `× 0.6`* while the window's copy of the very same ground layer
    goes through the voxel pass's real lighting, so the level's floor visibly
    darkened at the window's edge. It goes through `ShadeFarField` now, with an
    up normal — the same ambient and N·L the voxel pass gives an unshadowed
    surface. It still cannot match exactly, since the window's copy also gets AO
    and the shine line, but those are now the whole of the difference.
  - *It solved for y = 0*, and the chunk ground plane is a layer of voxels at
    integer y = 0, which a marcher reports a hit on at its **top** face, y = 1.
    The plane was therefore one unit below the window's own ground, and the lip
    showed edge-on along the horizon as a row of teeth. `GROUND_PLANE_HEIGHT`.
- **Removing the chunk ground plane entirely and letting the endless one draw it
  was considered and is not possible.** `Chunk::UpdateGroundPlane` sets
  `Active = true` on `m_VoxelData`, which *is* the physics `VoxelGrid` — it is
  the floor entities stand on, not decoration.
- **`View → Validate Far Field`** cross-checks placement against the resident
  chunks' full-resolution copy of the same geometry — a *phantom* (far field
  occupied, level empty) is placement being wrong; a *missing* is mostly the
  ground plane and the dynamic entities, neither of which is baked. **Measured:
  89 phantom out of 1,179,648 cells against 9 chunks, in 433 ms** — 0.008%, and
  the phantoms cluster at one y/z, so placement is right.
  It reads `Chunk::GetVoxelData`, **not** the voxel mapper, and that is the
  whole difference between working and appearing to hang. Two versions were too
  slow before this: one gathered 64 voxels per cell, one swept the mapping
  linearly. Both read the mapping, which is uncached host-visible memory and
  likely VRAM over PCIe since it started preferring ReBAR; 75 M reads out of it
  take tens of seconds either way. A chunk's `std::vector<Voxel>` is ordinary
  cached memory and holds the same voxels.
- **Four more pre-existing defects, all of them invisible until the far field
  put lit ground on the far side of the window's boundary.** Each was found by
  *instrumenting*, not by reasoning: a diagnostic build colouring pixels by
  which code path drew them, then another colouring hits by surface normal.
  Three earlier hypotheses — the far field itself, `IsVoxel`, the ground proxy
  box — were all wrong, and each cost a build-and-look cycle. **Colour the
  suspects and take one screenshot; do not reason about a one-pixel artefact.**
  - *`IsVoxel` had a lower bound and no upper bound* (`AmbientOcclusion.hlsl`).
    `PosToVoxelID(x == worldSize.x)` is not out of range, it is `(x = 0, y + 1)`
    — so a voxel on the window's far face asked about its neighbour and was
    answered about one from the opposite edge, a row up, which is why the
    artefact *repeated* along the boundary. It also now reports the ground layer
    as solid outside the window, because the endless ground plane continues
    there; without that the outermost ground voxels read as the lip of a cliff.
  - *The ground proxy box spanned grid y `0..10`*, starting inside the ground
    voxel layer (`RenderSystem::PostTick`). It starts at `R_GROUND_PLANE_HEIGHT`
    now, so there is no way into it beneath the ground's top face.
  - *`MarchBricks` gave a march that begins inside an occupied voxel a normal
    from the DDA's **next** crossing* — and for an origin sitting on a voxel
    face, the nearest crossing is that same face. `VoxelRenderer.vs.hlsl` clamps
    the proxy cube to `worldSize`, so every edge fragment starts exactly there
    and immediately inside the ground: a strip of ±X/±Z normals where every
    neighbour was +Y, dark, with `GetShineLine`'s vertical-wall branch adding a
    lit rim. It reports a miss now and lets the background continue the world.
    The test belongs in `MarchBricks`, where both entry paths meet — putting it
    only in `MarchDiffuse`'s out-of-window branch fixed half the pixels and
    turned the strip into a dashed version of itself.
  - *FXAA blended every silhouette toward transparent black.* The voxel pass
    writes `float4(0,0,0,0)` on a miss, so a neighbourhood straddling a
    silhouette darkened by one pixel. Post processing now skips FXAA where the
    3×3 neighbourhood is not fully opaque; nothing is lost, since a silhouette
    pixel is composited against the background rather than another scene sample.
- **The far field lagged behind the scene when the camera moved, and the fix was
  three cbuffer fields rather than the pass restructure it looked like.**
  `Present` copies the voxel target at the top of the frame and post processing
  composites *that copy*, so the image it draws over was rendered a submission
  ago while `camPosition`/`invMvp` have already moved on to the render being
  recorded now. `sceneInvMvp`/`sceneCamPosition`/`sceneCamOffset` carry the
  previous upload's camera and `GetBackground` uses those. Sky and ground had
  the same one-frame error all along and hid it, because they read only the
  ray's Y.
- **Measured**, `Valley_Path_To_Castle_Beat1`, 6×6 chunks → 384×32×384 cells,
  18 MiB, 103,909 occupied:

  | | Debug | Release |
  |---|---|---|
  | Far-field build, at world load | 940 ms | **108 ms** |
  | Post processing, far field off | 0.03 ms | — |
  | Post processing, far field on | 0.12–0.21 ms | — |

  The build is one-shot at world load and dominated by RTTR deserialization of
  245 static entities; it is not on any per-frame path.
- **The depth prepass is not used by this phase.** Phase 3 landed it with "if
  phase 4 does not use it, delete it" on the grounds that the far field would
  march where no proxy cube covers. It does — but in post processing, against
  its own volume and its own bricks, so the prepass target (which measures brick
  entry in the *window*) has nothing to say about it. That conditional has
  fired. Deleting it was left out of this phase rather than bundled into it, so
  it is still there behind **View → Depth Prepass**; it is now dead weight in
  the frame and should go.

**Two things this phase measured but did not fix.** Both were raised by the user
while verifying the far field, both are pre-existing, and both now have numbers
rather than suspicions.

**1. The chunk-load stall is `VoxelBaker::Bake`, not the chunk system.** Peak
main-thread milliseconds across a window shift, instrumented per phase (the
instrumentation is kept — guarded, off in Release, following phase 0):

| phase | Debug | Release |
|---|---|---|
| `CPU Chunk SetChunkVolumeAt` | 0.001 | — |
| `CPU Chunk Offsets` | 0.012 | 0.002 |
| `CPU Chunk FindEntitiesInChunk` | 0.42 | 0.05 |
| `CPU Chunk SaveAndDeleteEntities` | 14.4 | 1.26 |
| `CPU Chunk LoadEntities` | 14.0 | 1.49 |
| `CPU Chunk Unload` (3 chunks) | 44.6 | 3.95 |
| **`CPU VoxelBaker::Bake`** | 84.3 | **74.0** |

Everything in the chunk path collapses roughly tenfold in Release. **`Bake` does
not**, which is what makes it the answer: it is real work, not Debug overhead,
and not the `VoxelStamp.h` extraction this phase made (a template, so a call per
voxel in Debug and inlined in Release — ruling that out is why the Release
column exists).

The cause is already written down elsewhere in `CLAUDE.md`: the voxel mapper
prefers `DEVICE_LOCAL | HOST_VISIBLE` (ReBAR), so every CPU *read* of it is a
PCIe read of VRAM. `RenderContext::ModifyVoxel` and `ModifyVoxelFast` both read
the old voxel before writing, purely to tell `VoxelBrickGrid` whether occupancy
changed — so every baked voxel pays an uncached read, and a chunk's worth of
entities is millions of them.

**The fix is the trick `VoxelBrickGrid` already uses one level up**: keep the
answer in ordinary cached memory. A CPU-side occupancy bitmap of the window —
768 × 128 × 768 bits is 9.4 MiB — answers "was this occupied" without touching
the mapping, making the bake path write-only. Note `ModifyVoxel`'s `bOverwrite`
false branch tests `uiOldColor == 0`, which the bitmap answers, and its
`uiOldColor != uiColor` redundant-write guard, which it does not; writing a
value twice is a streaming store and harmless.

**Done, as phase 4b.** The bitmap lives in `VoxelBrickGrid` next to the counts
rather than in `RenderContext`, because the counts are the only reason the old
occupancy was ever needed — putting both behind one `SetVoxel` is what makes it
impossible for them to drift apart. `SetVoxel` reads the old occupancy itself
now instead of taking it as a parameter, for the same reason. What was measured:

| | Peak `CPU VoxelBaker::Bake` |
|---|---|
| Before, world load, `Valley_Path_To_Castle_Beat1` | **5284.7 ms** |
| After, same level, same vantage | **757.6 ms** |

Three things worth carrying forward:

- **The world-load bake is a far better test than the window shift**, and it is
  the one to use next time: it needs no camera input, so it is reproducible, and
  at 5.3 seconds it dwarfs the 74 ms the window shift showed. Point `Startup
  World` in `Game/UserSettings.vguser` at the level and read the peak.
- **`FrameProfiler` now reports a peak alongside the average**, which is what
  made that measurable at all — a 5.3 second bake averaged over a second of
  200 fps frames reads as 107 ms and hides the shape entirely.
- **The correctness test is `Validate`, not the screen.** It recomputes counts
  *and* bits from the voxel buffer, and it is independent of what streamed —
  which matters, because resident occupancy legitimately swings by a million
  voxels when the camera moves the window, so comparing occupied-voxel totals
  between two runs proves nothing unless nobody touches the camera. After a
  load plus streaming: 0 bricks and 0 bits disagree over 75,497,472 voxels.

**2. Presentation feels laggy in Release, and nothing measurable explains it.**
Reported as the whole present, UI included, not just the world. Everything that
can be measured from inside the engine is clean:

- Frame delivery is *perfect*: `[fps]` now logs the p99 and worst frame of each
  second, and every frame is 5.00 ms with no outliers, at 200 world updates a
  second. There is no hitch to find.
- Present mode is `MAILBOX` where offered, FIFO otherwise, `minImageCount + 1`
  images (`VKSwapchain.cpp:103-111`) — the low-latency choice, never blocking.
- Engine-side latency is about two frames: the scene post processing composites
  is one submission old by construction (the copy at the top of `Present`).
- Setting `FrameLimit` to 0 changed nothing.

**`EnableVSync` is a dead setting** — serialized, stored on `Settings`, read by
nothing in the Vulkan backend. Do not offer it as a toggle; it does not do
anything. (Found while chasing this.)

**Solved, and it was none of the above.** The cause was outside the frame loop
entirely, which is why nothing inside it showed anything: **`Settings.vgs` was
never being read**. `Core/Settings.cpp` is a reflection-only translation unit,
so the linker dropped it from the static archive, so the `Settings` type had no
registered properties, so `JsonSerializer` applied none of them - silently, with
`FromJsonFile` returning true the whole time. Every setting ran at its
compiled-in default for the entire port.

The file asks for `FrameLimit: 0.0` and `EnableVSync: true`; the engine ran the
default 1/200 with `MAILBOX`. **200 fps into a 60 Hz display is the judder**:
200/60 is not an integer, so consecutive displayed frames advance the world
15 ms, then 20, then 15. Motion reads as skipping while every frame is on time -
which is precisely the symptom the user reported ("it just skips a LOT of
frames") and precisely what a frame-time log cannot show. Fixed by linking the
engine archive whole, honouring `EnableVSync` in `VKSwapchain`, and making the
serializer report both failure modes. Confirmed on screen: FIFO, 60 fps, smooth.
See CLAUDE.md for the full chain - it is the more general lesson.

The `game`-fullscreen-versus-windowed-editor test was run and showed no
difference, so the compositor was not implicated after all.

---

## Phase 4c — The bake stall

*Done. The user's report: chunk streaming hitches while moving the camera
through the level in the editor, and hitches in gameplay; Debug worse than
Release, but Release bad too.*

**Measured**, `Valley_Path_To_Castle_Beat1`, peak `CPU VoxelBaker::Bake`:

| | Debug | Release |
|---|---|---|
| Before (after phase 4b) | ~2 s | **756.7 ms** |
| After | **0.102 ms** | **0.03 ms** |

Steady-state per-frame cost is unchanged, 0.006 ms. Zero validation errors.

**Do not quote that ratio as the speed-up.** It is a property of the counter.
A world load stamps every renderer once more in `RenderSystem::OnComponentAdded`
— 587 renderers, 3,115,337 voxels, **399.5 ms** — and that call has never been
inside `Bake`. The honest accounting:

| | stamping at a world load | |
|---|---|---|
| Before | `OnComponentAdded` 399.5 ms + `Bake` 756.7 ms | **~1157 ms** |
| After | `OnComponentAdded` 399.5 ms + `Bake` 0.03 ms | **~400 ms** |

Three stamping passes became one — **2.9x**, exactly what the structure
predicts, and the 30,000x on the timer means only that *all* of what that timer
covered was the redundant part. `OnComponentAdded`'s stamp now reports as
**`CPU VoxelBaker::Occupy (added)`** (0.614 ms avg, 19.187 ms peak across the
load) so the two are never read in isolation again.

**Chunk streaming — the symptom originally reported — is not verified fixed.**
Every number here is a world load. A chunk-loaded renderer has
`IsChunkInstanceLoaded()` true, so `OnComponentAdded` skips it and `Bake` does
the stamping with `Positions == nullptr`, which means `bBakeCurrent` is false
and nothing is skipped. **The expected effect on a window shift is close to
neutral**, and phase 4's 74 ms per shift is probably still there. Measuring it
needs camera movement, so it needs either the user or a scripted `ViewPoint`.
Start the next round of this work there, not on the world load.

**What the bake did was almost entirely redundant, and that is the finding.** At a
world load `RenderSystem::OnComponentAdded` stamps every renderer,
`Chunk::UpdateRenderer` then calls `RequestUpdate` on every one of them on first
load, and the resulting clear-and-re-occupy reproduced the same voxels exactly.
Instrumented before changing anything: **218 of 218 re-stamps byte-identical**,
0 changed. Every voxel of it cost two writes to the ReBAR mapping, two to the
occupancy bitmap, two brick-count updates and a physics-grid lookup.

Three changes make a re-bake request answerable without performing it:

- **`BakeData::StampKey`** records what the stamped voxels are a function of:
  the `VoxelStampTransform`, plus the frame, override colour and render state,
  which is exactly what `ForEachStampedVoxel` reads. Equal keys mean an
  identical sequence of (voxel, colour) pairs. It is O(1) — it computes the
  stamp transform and compares seven values, it does not walk the model.
- **`RenderContext::GetVoxelGeneration`** bumps on every clear and resize of the
  voxel buffer. With the already-recorded `WorldOffset` it establishes that the
  buffer still holds the previous stamp, which is the precondition for skipping
  and is what a forced update actually means. A force now re-*examines* every
  renderer instead of re-*stamping* it.
- **A disabled renderer is no longer excluded from the skip test.** It used to
  be cleared on every frame for the rest of its life, and once the first clear
  had dropped its `Positions`, every later one took `Clear`'s fallback branch,
  which allocates and scans the renderer's whole bounding box out of the physics
  grid.

**Do not use the `Updated` flag for this.** It was the obvious candidate and it
is wrong: `CheckRendererChange` sets it from the transform, and a transform
reaches the stamp through a quantized rotation and a `floor`, so it can move
without moving a single voxel. **All 218 had `Updated` set** while producing
identical stamps. Gating the skip on `!Updated` took the win from 13x to zero.

**Locality was measured and is not the answer.** Worth knowing before anyone
optimizes the per-voxel path again. The write stream is genuinely scattered —
6.94 M distinct occupancy-bitmap words per 10.4 M `SetVoxel` calls, i.e. a new
cache line about two thirds of the time. Sorting each `.vox` frame into the
order every consumer indexes by, and separately forcing every stamp unrotated,
took that to 391 K distinct words — **17x fewer line touches, and the same time
per voxel**. Attribution by disabling parts of the write path was also
misleading: it suggested the bitmap was 509 ms of the 770, but those switches
perturb control flow, and an isolated benchmark of the same structures at the
same sizes runs the identical work at 10.7 ns coherent / 20 ns scattered against
the engine's ~110 ns. *The gap was never explained*, and it stopped mattering
once the work itself went away. If the per-voxel cost ever matters again, start
from that unexplained 5-10x, not from cache locality.

**The `.vox` sort landed anyway**, on its own merit: a `.vox` file lists voxels
in authoring order, and sorting each frame once at load into (z, y, x) takes the
far-field build from **243.0 → 213.9 ms** for a byte-identical result (168,656
cells both ways). `VoxModel::SortFrameVoxels`.

**`VOXAGINE_PROFILE=1`** now forces the frame profiler on or off regardless of
build type. The costs worth measuring behave differently in Release, which is
exactly where it defaults off; every number above needed it.

---

## Phase 4d — Shrink the CPU `Voxel` from 16 B to 4 B

*Done. **Opus**. Memory and cache footprint. Landed at 4 B of voxel plus 2 B of
ownership beside it rather than the flat 4 B the design asked for — the reason
is the most useful thing in this section.*

**What it was.** `struct Voxel` carried an owner and a redundant flag next to
the colour:

```cpp
struct Voxel                        // 16 B
{
    uintptr_t UserPointer = 0;      // 8
    bool Active = false;            // 1 (+3 pad)
    uint32_t Color = 0;             // 4
};
```

**What it is.** `Voxel` is the GPU word and nothing else, with a
`static_assert(sizeof(Voxel) == 4)` to keep it that way:

```cpp
struct Voxel { uint32_t Color = 0; bool IsActive() const { return (Color >> 24) != 0; } };
```

and ownership lives in a `VoxelOwnerVolume` beside each chunk's voxels — a
`uint16_t` slot per voxel, plus a per-world table mapping slot to entity id and
a sparse map for particle claims.

### Measured, before and after

Three interleaved A/B runs of the same binary pair, editor Release,
`Fishing_Village_Beat1`, autosave and V-Sync off, nothing else on the machine.
Interleaving mattered: consecutive runs of the *same* binary varied by more in
frame time than the change did.

| | before | after | |
|---|---|---|---|
| Peak RSS | 1,696,212 kB | 945,761 kB | **−750 MB, −44%** |
| `CPU VoxelBaker::Occupy (added)` avg | 0.624 ms | 0.592 ms | −5% |
| `CPU VoxelBaker::Occupy (added)` peak | 19.2–25.0 ms | 18.1–18.3 ms | −8% |
| Chunk encode | 15–18 ms | 7–8 ms | **−2.2x** |
| Chunk decode | 53–54 ms | 14–15 ms | **−3.7x** |
| Frame time p99 (idle editor) | 0.397 ms | 0.388 ms | within noise |

The RSS number is exactly what the structure predicts: 9 resident chunks x
(128 − 48) MiB = 720 MiB = 755 MB.

**Nothing got slower**, which was the constraint this phase was executed under.
The stamp is marginally *faster* — it writes 4 B of colour and 2 B of slot into
two arrays instead of 13 B into one, and the colour array is a quarter of the
size it was. The codec is much faster because a run is now 7 bytes instead of
18 *and* runs are longer: they used to break wherever a raw owner pointer
changed.

### Why not the flat 4 B

The design's first choice was to keep no per-voxel ownership at all and answer
it from the owning renderer's `BakeData::Positions`. The second was a sparse
`unordered_map<voxelID, owner>`. Both were rejected for the same measured
reason:

**`VoxelBaker::Occupy` writes an owner for every stamped voxel — 3.1 M of them
at a world load, in a pass that currently takes ~290 ms.** A hash insert per
voxel is 50–100 ns, so a sparse map adds 150–300 ms to the single largest
remaining stall in a world load. That is not a footprint trade, it is a
regression. The owner has to be a flat array indexed by voxel, and the only
question left is how wide the element is.

`uint16_t` is wide enough because the *ids* are wide, not numerous:
**970 K owned voxels over ~117 distinct entities** (408 in the level as
measured here). A per-world table turns the id into a slot, and slots are never
recycled — which also makes a slot a stable identity, so the combo streak and
the bake's "did I stamp this" test compare slots directly rather than resolving
them.

**Particles are the reason the array cannot be narrower and the reason it needs
an escape hatch.** They claim voxels too, from a pool of 150,000 — more than a
`uint16_t` can name. They get the reserved slot `0xFFFF` and a sparse map keyed
by voxel index. That is fine because a particle claim is rare where a static
stamp is not, and crucially the *slot value alone* answers the stamp's
question: `0` is unowned, anything else only matters if it equals mine, and a
particle slot never does. The hot path never touches the map.

**Generalise this.** *Check what writes the field before choosing how to store
it.* The read pattern here (970 K entries, ~117 distinct values, consulted on
rare paths) argues for a sparse map from every angle. The write pattern —
3.1 M stores in one burst — rules it out on its own, and the write pattern was
not in the original design notes at all.

### What the change actually touched

- **`Active` is gone**, derived as `(Color >> 24) != 0`. It was verified
  redundant over 75,497,472 voxels before the change and the audit agrees after
  it: identical active count (2,706,503) and owner count (2,116,679) on the same
  scene, before and after.
- **Every write that set `Active` alongside a colour is now just the colour.**
  Three of them (`Chunk::UpdateGroundPlane`, `VoxelBaker::Occupy`,
  `PhysicsSystem::SyncIntegrityJob`) already wrote a matching colour, so they
  are pure deletions.
- **`VoxelCell`** replaces a bare `Voxel*` wherever an owner is read or written:
  the voxel, its owner volume and its index, resolved by one pass of the index
  arithmetic that `GetVoxel` already did. Doing that arithmetic twice was the
  only real cost the split could have introduced.
- **`GetChunk` optionally fills a parallel `uint16_t` slot array.** That is what
  unblocked `Bullet::OnVoxelCollision`, which the design called out as the
  obstacle: it receives voxels with no coordinates, so it cannot look ownership
  up for itself. It gets the slots handed to it and uses one as the model
  identity, which is all the combo streak ever wanted.
- **The chunk RLE now encodes (colour, slot)** at 7 bytes a run rather than
  (bool, colour, uintptr_t, `';'`, count) at 18. Particle claims are
  deliberately dropped on encode — the old format restored raw `Particle*`
  values into a pool that had long since recycled them, and a chunk far enough
  away to unload has no debris worth keeping.

### The acceptance tests, and how to run them

`VOXAGINE_VOXEL_AUDIT=<seconds>` still exists and is still the only correctness
check here that does not depend on the camera. It now reports the slot
population as well, and it round-trips every loaded chunk through the RLE codec
in place, which is the only way to exercise a format that otherwise needs a walk
long enough to unload a chunk.

```bash
cd Game && VOXAGINE_VOXEL_AUDIT=15 ../Build/Linux/Editor/Release/bin/BitBuster
```

At rest on `Fishing_Village_Beat1`: 2,706,503 active of 75,497,472; 2,116,679
owners over 408 distinct slots, 0 naming a dead entity, 0 particle claims;
**0 diverging voxels** over 9 chunks of codec round trip. Run it again *during
destruction* — a static scene has no particle claims at all, and the
owner-set-but-inactive combination (debris in flight, which blocks a static
re-bake over that voxel) only exists then.

### Left alone deliberately

- **`FindEmtpyNeighbor` and its caller disagree about where the baked voxel
  goes.** `PhysicsSystem::SimulateParticles` picks `bakeVoxelPos`, asks
  `FindEmtpyNeighbor` for a different cell when that one is occupied, and then
  writes the colour at `bakeVoxelPos` anyway. Pre-existing, and visible now only
  because ownership needs coordinates: the function reports where it found the
  cell so the *owner* lands on the right voxel, while the colour writes stay
  exactly where they were. Fixing the mismatch is a behaviour change and belongs
  in its own change.
- **`Particle::Live.UserPointer` is never assigned anywhere**, so the bake at
  `SimulateParticles` has always written owner 0 rather than setting one. Kept
  as a clear, and commented as one.
- **Slots are never recycled.** 65533 are available and the largest level uses
  117; exhausting them logs once and bakes as unowned rather than wrapping.

### Ride-along still not taken

`Chunk` still never frees its decoded voxels for a chunk that has left the
window — `m_VoxelData` is resized once and `EncodeVoxels` shrinks it, but a
chunk that was visited and unloaded keeps its ≥10 MB encoded reserve. This phase
made each resident chunk 2.7x cheaper; it did not change how many are resident.

---

## Phase 5 — Point lights and specular highlights

*Design. **Opus**. Note: this is a new feature, not a restoration.*

Splody's shader had `LightPoint`, `pointLights : register(t1)` and `lightCount`
(`VoxelRendererForward.ps.hlsl:31-40, :19`) — but **no engine on this branch
ever fed them**; the only other trace is a commented-out mock in
`UnitTesting/UnitTests/Source/Engine/Rendering/Lighting.cpp:33,50`. The specular
function existed and its call site was already commented out. So the memory of
"voxel specular highlights" is probably of a build that ran this code briefly,
and reproducing it means building the engine side.

**Steps.**

1. **`LightComponent`** — position from the entity transform, plus colour,
   range, strength. Register with RTTR so it appears in the editor's
   `PropertyRenderer` (which is genuinely RTTR-driven and needs no changes).
2. **Gather** in `RenderSystem::PostTick`, exactly like the existing AABB list
   (`RenderSystem.cpp:138-143`): fill a structured buffer, upload per frame like
   `SortAABBs`/`RenderContext.cpp:448-457` does. Add `lightCount` to
   `CameraData.hlsl` — mind rule 2 (`-fvk-use-dx-layout`) for the new struct and
   rule 1 for the new binding.
3. **Cap and sort** — take the N (start at 16) strongest contributors per frame.
   No per-light shadows in v1.
4. **Shader** — port `GetPointLighting` (Splody `:222-228`) and
   `GetPointLightingSpecular` (`:203-220`). **Use the Blinn-Phong variant** (the
   commented-out one at `:205-210`): better highlight shape on flat voxel faces
   and one `normalize` cheaper than `reflect`.
5. **Specular on voxel art** reads best when it respects the faceting. Try it on
   the raw face normal with no smoothing first — that hard, quantized highlight
   is very likely the look being remembered. Keep the exponent a `#define` while
   tuning.
6. ~~Consider restoring the **edge "shine line"**~~ **Done — landed in Phase 1
   instead**, once it turned out to need nothing beyond what Phase 1's AO work
   already exposed (`res.UV`/`res.Normal`). See Phase 1's implementation
   notes.

**Acceptance.** A light-emitting entity lights nearby voxels with a visible
falloff and a highlight that tracks the camera. Cost scales sanely with light
count (measure at 0, 1, 8, 16). Editor can place and tune one. Zero validation
errors.

---

## Phase 6 — Beyond the original

Ordered by payoff per effort. Each is independent — do them one at a time, and
re-read the Progress table before starting.

**6.1 Fog / aerial perspective.** **DONE - 2026-08-09.** `Fog.hlsl`, applied to
linear radiance at the three shading sites that can be distant - both voxel
pixel shaders and `ShadeFarField`, which covers far-field geometry and the
endless ground plane together. **Two of the three reasons it was filed for do
not exist at this game's camera**; see the notes section below before tuning it.
`marchDiffuse.Distance` is *not* what it uses, and that is the one concrete
correction to the paragraph this replaces.

**6.0 A frame-time ceiling for the marcher.** **DONE — 2026-08-09.** The voxel
pass had no upper bound on cost: `VoxelRenderer.ps.hlsl` budgets the walk by the
window diagonal (~137 brick steps) and fires a shadow ray with the same budget
from every lit hit, and `MarchBricks` runs a 24-step voxel DDA in every occupied
brick. Worst case was ~6,600 voxel steps per pixel with nothing capping the
frame, which has reached the driver as **`NVRM: Xid 109 CTX SWITCH TIMEOUT`**
and the user as the `[stall]` line in `RenderContext` over a window that stops
updating. See the notes section below for what landed and, more usefully, for
what the measurement said about the *cause* — the per-pixel bound turns out not
to be large enough to explain a three-second frame on its own, so the trigger is
still open. Read that before assuming this closed it.

**6.2 Soft shadows.** **DONE — 2026-08-09.** Landed close to as written — a
jittered cone, penumbra scaling with distance to the occluder because the cone
diverges — but the jittered rays are spread *across the 2x2 quad* rather than
fired one after another from each pixel, so a quad gets four penumbra samples
at the cost of one ray per pixel. The distance fade is gone. See the notes
section below for the cost decomposition, which is the part worth reading: it
is what says a light-space shadow map is the wrong structure for this engine
and sends the remaining work to phase 7.

**6.3 Emissive voxels + bloom.** *Design.* The alpha byte is a `rendererState + 1`
tag (rule 3), so spare tag values can mark emissive voxels. Bloom goes into the
existing `PostProcessing` pass next to FXAA. **Audit the tag space first** —
find every producer and consumer of that byte before claiming a value.

**6.4 Linear-light pipeline.** *Design, invasive but shallow.* Lighting
currently happens in gamma space, which is why `AmbientOcclusion.hlsl:44`
hand-applies `pow(…, 2.2)`. Move lighting to linear and present through an sRGB
swapchain format. Everything composites more correctly afterwards — and it
should be done **before** 6.3 and 6.5, since bloom and any bounce lighting in
gamma space will look wrong in ways that are hard to diagnose. Expect to retune
`AMBIENT_VALUE` and the Phase 1 constants again.

> **A colour bug that looked like this phase, and was not.** The port presented
> through a `B8G8R8A8_SRGB` swapchain while every pass rendered into UNORM
> targets holding already-encoded values, so `vkCmdBlitImage` gamma-encoded the
> whole frame a *second* time — washed out and desaturated, ImGui and editor
> chrome included, since they composite into the same target before the blit.
> Fixed separately by presenting through UNORM; see `CLAUDE.md`, "The
> double-gamma present". **That fix does not do any of the work above** — the
> pipeline is still gamma-space and `pow(…, 2.2)` is still hand-applied. What
> it does is make the reference trustworthy: before it, this phase would have
> been tuned against an image that was already wrong.

**6.5 Stretch: cone-traced bounce lighting.** **Superseded by Phase 7**, which
is this item plus the shadow and AO work that turns out to share its structure.
The observation that made it a phase of its own rather than a stretch goal: the
Phase 4 far-field volume really is most of a GI pyramid, and once the pyramid
exists the sun shadow and the AO both want to be cone traces through it too.
Building a shadow map first would have meant maintaining two structures that
answer overlapping questions.

---

## Phase 6.0 — A frame-time ceiling for the marcher

*Taken out of order, on 2026-08-09, because it is the fix for the hang.*

### What landed

`MARCH_STEP_BUDGET` in `Defines.hlsl`, defaulting to **1024**: the number of DDA
crossings — brick-level and voxel-level alike — that one pixel may make across
**every ray it fires**. `SDFMarcher.hlsl`'s `numStepsTaken` was already there and
already incremented; it was a leftover of the cap phase 2 deleted and nothing
read it. It is a static global, so it is per-invocation, so the primary march and
the shadow ray that follows it accumulate into the same counter. That is the
point: what has to be bounded is the pixel's total work, and budgeting each ray
separately just doubles the ceiling.

Running out ends the march the same way running out of `iMaxBrickSteps` already
did — alpha 0, read by the caller as a miss. For the primary ray that hands the
pixel to the background (far field, ground, sky); for the shadow ray it means no
occluder, so the surface stays lit. Both are wrong pixels and both are cheap.

`FarField.hlsl` gets the same budget under its own counter — different pass, no
shared invocation. It fires one ray per pixel rather than two, but it runs over
the whole screen instead of only the pixels an AABB proxy covers, so its
unbounded worst case was worth about as much.

`MARCH_STEP_DEBUG`, commented out in `SDFMarcher.hlsl`, shades both voxel pixel
shaders by step count instead of by what they hit — blue through red, white for a
pixel that ran out. Both shaders apply it on the **miss** path too, which is the
one that matters: a ray that marches a long way and finds nothing is the
expensive kind and is invisible in the normal image.

### The measurement, and why it is the interesting part

Sweeping the budget against the voxel pass's own GPU timestamp, `game-release`,
`Valley_Path_To_Castle_Beat1`, camera still at the phase 0 baseline vantage,
30 s a point:

| `MARCH_STEP_BUDGET` | 1e9 | 1024 | 512 | 256 | 192 | 128 | 64 | 32 |
|---|---|---|---|---|---|---|---|---|
| Voxel pass (ms) | 2.50 | 2.58 | 2.58 | 2.58 | 2.58 | 2.50 | 2.10 | 1.61 |

The pass stops responding between 64 and 128 — by 128 there is effectively no
pixel left to clip, and the 2.50/2.58 split across the flat part is run-to-run
variance, not a response. 1024 is eight times the plateau, so it is free here and
has margin for the long sightlines this vantage does not contain. It is also
**not** a return of the 700-step cap phase 2 removed: that one bounded the
primary ray alone in a flat single-level walk, where a 768-voxel sightline cost
768 steps. In the two-level walk the same sightline costs ~96 brick steps.

**And this is where it stops being a story about the marcher.** The slope
between 32 and 64 is ~0.015 ms per step of the average, so a whole screen of
pixels all spending the full 1024 is roughly **16 ms** of voxel pass — and the
old unbounded ceiling of ~6,600 was worth roughly **100 ms**. The hang is a frame
of *seconds*. Per-pixel step count, even completely unbounded, is short by a
factor of about 25.

So the remaining factor is not in the marcher, and the candidates are the ones
that multiply it:

- **Resolution.** Everything above was measured in a 1365x2003 window with a 16:9
  target letterboxed inside it. Fullscreen at 2732x2048 is about 4x the pixels.
- **Overdraw.** The voxel pass rasterizes one AABB proxy per `VoxRenderer` and
  each covered fragment runs a full march. Early-z off the nearest proxy is what
  keeps this small in practice, and nothing bounds it in principle. 4x resolution
  and ~6x overdraw is the missing 25x, and it is the first thing to check.

The `[stall]` line already prints `voxel instances` and `aabbs` — the log quoted
in `CLAUDE.md` predates those fields, so the next reproduction will say straight
away whether the proxy count is what moved. **Reproduce with `MARCH_STEP_DEBUG`
on**, per the plan's original instruction; white pixels name the geometry, and
their absence during a slow frame would rule the marcher out entirely and point
at overdraw.

### Acceptance

Zero validation errors on a 45 s `game` (Debug) run of the same level. Voxel pass
2.47 ms, unchanged. The budget is a ceiling that nothing at this vantage
approaches, so there is nothing to see on screen — which is the intended result,
and also why it is **not** confirmation that the hang is gone. That needs a
reproduction attempt.

---

## Phase 6.2 — Soft shadows

*Done, 2026-08-09. The shadow ray was 52% of the voxel pass and had no
structural answer in the plan; this is that answer, plus the measurement that
redirects the rest of the work to phase 7.*

### Where the shadow ray's time actually went

`game-release`, `Valley_Path_To_Castle_Beat1`, camera still at the phase 0
vantage, 35 s a point, vsync off. Every number is the Voxel pass's own GPU
timestamp.

| shadow ray | Voxel pass | fps |
|---|---|---|
| none (`ShadowLess` variant) | 1.110 ms | 587 |
| brick level only, never descends | 1.339 ms | 502 |
| voxel-exact (what shipped) | 2.331 ms | 309 |

So the brick walk costs **0.23 ms** and descending into the occupied bricks it
crosses costs **0.99 ms** — 81% of the shadow ray. Bounding how far the exact
walk may go then says *where* that 0.99 ms is:

| exact walk bounded to | 0 bricks | 1 | 2 | 3 | 4 voxel steps | 8 | 16 | unbounded |
|---|---|---|---|---|---|---|---|---|
| Voxel pass | 1.583 | 1.804 | 1.965 | 2.204 | 1.714 | 1.840 | 2.264 | 2.331 |

**Descents beyond the first ~16 voxel steps are worth 0.07 ms.** Essentially the
whole cost is every lit pixel walking out of *its own brick* — a shaded
surface's brick is occupied by definition, so that first descent is unavoidable
and universal. Phase 3's note that the shadow ray is "near-field per-lit-pixel
work" is right, and this is the number behind it.

### What did not work, and why it is recorded

**Brick-density soft shadows.** Beyond a short exact contact region, attenuate
the ray by each brick's occupied-voxel count instead of descending —
Beer-Lambert through a density field, one buffer read per brick instead of up
to `BRICK_MAX_VOXEL_STEPS` voxel reads. It works and it is fast (2.331 → 1.840
at 8 contact steps) and the user rejected it on sight: **blotchy 8-unit patches,
and shadows that had lost their shape**. Both are the same cause — a brick is
8 voxels and a per-brick constant is visible as one. A trilinear brick field
would fix the blotchiness and make the shape *worse*.

Worth generalising: *brick resolution is fine for deciding where to look and
useless for deciding what something looks like.* The brick grid earns its keep
as an acceleration structure precisely because nothing shades from it.

There is also a second-order defect in it that would have bitten anyway: the
chunk ground plane is a solid voxel layer at y = 0, so every brick spanning
y = 0..7 carries 64/512 of density from the floor slab alone, and a shadow ray
leaving the ground is still inside that band when a short contact budget runs
out. The floor attenuates itself.

### What landed

- **`MarchShadow` (`SDFMarcher.hlsl`) replaces `MarchLight`**, and returns a
  `float` transmittance rather than a `MarchResult`. It is a separate function
  rather than a call into `MarchBricks` because that one computes a hit's whole
  surface frame — normal, UV, smooth position — and a shadow test discards all
  of it. Measured at 2.319 ms against `MarchBricks`' 2.331: the compiler was
  already eliminating most of it, so this is for clarity, not speed.
- **The distance fade is gone**, as asked. `SHADOW_FADE_K` is deleted. A
  transmittance multiplies the sun term directly. The fade existed to disguise a
  binary answer; a transmittance does not need disguising. (`AMBIENT_VALUE`
  itself is gone too — see the sun-and-sky model below, which replaced it.)
- **Softness is a cone of rays, and the sampling scheme took two attempts.**
  `GetSunVisibility` fires `SHADOW_CONE_SAMPLES` voxel-exact rays on a
  golden-angle spiral over the sun's disc, rotated per pixel by interleaved
  gradient noise so residual quantization becomes dither. Every sample is a real
  occlusion test against real voxels, and **the penumbra widens with distance to
  the occluder on its own** because a fixed-angle cone diverges — a box on the
  floor still casts a hard line where it touches. `SHADOW_CONE_SPREAD` is the
  tangent of the half-angle, 0.05.
  - **First attempt: one ray per pixel, averaged across the 2×2 quad** with
    `QuadReadAcrossX/Y`. Four directions per quad for the price of one ray per
    pixel. Rejected on sight twice over, and both reasons are worth keeping:
    four directions is *five discrete levels*, which the user described as
    "4 separate shadows, not smooth"; and once the sample count was raised, a
    quad average is a **2×2 box filter on the shadow term** — invisible on a
    broad penumbra, and it destroys most of the contrast of a shadow one or two
    pixels wide. Thin occluders (railings, posts, beams) are exactly the
    geometry that costs, reported as "thin shadows seem under-present".
  - **Generalise:** sharing a lighting term across a pixel footprint is free
    smoothness, and it is paid for in the highest frequencies of that term. If
    the feature you care about is thinner than the footprint, it is not a trade,
    it is a deletion.
  - It is **not free**: smoothness costs rays linearly. This is the brute-force
    version and phase 7.1 is what replaces it — a cone trace through a filtered
    mip pyramid integrates the same coverage in one trace. Kept until then as
    the ground truth that version has to match.
- **`-fspv-target-env=vulkan1.1` in `CMake/Shaders.cmake`.** Added for the quad
  intrinsics above and kept after they were removed: the target env should match
  the device regardless (`VKDevice` asks for `VK_API_VERSION_1_3`), and phase
  7.1's mip-pyramid compute pass will want wave intrinsics. Nothing uses a
  subgroup op at the moment.

### The sun-and-sky model, which is the bigger change

Landed alongside the shadow work because a shadow is only as good as the thing
it is subtracting light from. `Lighting.hlsl` is new and is the one place the
surface model lives; `AMBIENT_VALUE` and `FARFIELD_AMBIENT` are gone.

What it replaced: `difference * (1 - AMBIENT_VALUE) + AMBIENT_VALUE`, with
ambient occlusion multiplied over the whole result afterwards. One light and a
flat grey floor under it.

What it is: **two lights, and each has its own occlusion term.**

| light | colour | occluded by |
|---|---|---|
| sun | directional, warm | the shadow ray's transmittance |
| sky | hemispherical, cool above / warm below, lerped on `normal.y` | ambient occlusion |

**The part that matters is which occlusion applies to which light.** AO is
*skylight* occlusion. A crevice in direct sun is not dark, and under the old
model it was, because AO dimmed the sun term too. Splitting them is most of why
a path-traced voxel render reads as lit by a place rather than by a lamp, and it
is a bigger visual change than anything else in this phase.

`AmbientOcclusion.hlsl`'s `GetAmbientOcclusion` became `GetSkyVisibility` and
returns the raw [0, 1] visibility rather than a colour remapped into 0.75..1.0
with a `pow(x, 2.2)` on it. The range was arbitrary and the remap belongs to
whoever consumes it. The `pow(x, 1/3)` stays — it is a softening curve on the
interpolated corner term, not a gamma conversion.

**One model, four consumers, on purpose.** The window's two pixel shaders, the
far field, the endless ground plane and the particle vertex shader all call
`ShadeSurface`. The first three meet along visible boundaries and a second
formula is how the phase 4 seam got in; the fourth matters because debris is
made of voxels that were part of the world a moment ago, and lighting it
differently makes an explosion read as an effect pasted over the scene.

**Still gamma space.** 6.4 has not happened and `AmbientOcclusion.hlsl` still
hand-applies `pow(x, 2.2)` for that reason, so adding two light terms here is
the same class of error that phase warns about for bounce light. Done anyway
because the *shape* of the lighting was wrong and that is visible from across
the room, where the gamma error is not. Every constant in `Lighting.hlsl` wants
retuning once 6.4 lands.

### Brick size 8³ → 4³, and it is not a shadow fix

Swept because the decomposition above makes brick size the structural lever on
"walking out of your own brick". `BRICK_SHIFT` in `Defines.hlsl` and
`VoxelBrickGrid::k_uiBrickShift` are the paired constants; the far field reuses
both, so all three grids move together.

| brick | 2³ | 4³ | 8³ (was) | 16³ | 32³ |
|---|---|---|---|---|---|
| Voxel pass, exact shadow | 2.729 | **2.132** | 2.319 | 2.960 | 3.566 |

4³ is the optimum and is now the default. **The win is almost entirely on the
primary ray, not the shadow**: at 4³ the no-shadow floor is 0.938 ms against
8³'s 1.110, while the shadow's own share is 1.194 ms against 1.209 — unchanged.
Smaller bricks skip empty space more tightly and cost more brick steps; 2³
crosses over and is worse.

Cost: 1.18 M bricks for a 768×128×768 window rather than 147 K — 2.3 MiB of CPU
counts and 4.6 MiB of GPU mirror per buffer, against 288 KiB and 576 KiB. Still
negligible beside 288 MiB of voxels, and `uint16` counts still hold a full 4³
brick with room to spare.

*Method note, recorded because it wasted a build cycle.* The first sweep edited
`BRICK_SHIFT` with a `sed` expression that errored on the `BRICK_INV_SIZE` line,
so the shader defines never changed while `k_uiBrickShift` did. A brick grid
whose two sides disagree does not fail loudly — it reports empty bricks and gets
*faster*. 16³ appeared to cost 1.384 ms. **A performance number that improves
when a shared constant desynchronises is measuring a broken acceleration
structure, not an optimization.**

*And use Post Processing as the witness that two runs are comparable.* Hyprland
tiles the window and picks its size, so a run can silently come up in a smaller
tile than the one before it — one confirmation run here reported the Voxel pass
at 0.752 ms, *below* the shadow-less floor, which is what gave it away. Post
Processing is full-screen and does a fixed amount of work per pixel, so it is a
direct read of the target size: every run in the tables above sits at
0.112–0.126 ms and the bad one sat at 0.033. Check it before comparing anything,
and re-run rather than reasoning about the difference.

*And one invalid experiment.* Confining every voxel fetch to the first 64 K
voxels (`& 0xFFFF`), to test whether the walk is memory-bound, changes what the
rays hit and therefore the control flow — it came out *slower*, which says
nothing. The question is still open, and phase 4c's note about attribution by
disabling parts of a path is the same trap.

### Resolution scaling, which is what sends the rest to phase 7

Measured at `ResolutionScale 2.8`, giving a 3822×2150 target from the same
1365×768 one, same vantage:

| | 1× (1365×768) | 4K (3822×2150) |
|---|---|---|
| Voxel pass, no shadow | 0.938 ms | 5.602 ms |
| Voxel pass, cone soft shadow | 2.383 ms | 14.722 ms |
| **shadow ray alone** | **1.45 ms** | **9.12 ms** |

The shadow is 62% of the pass at 4K and scales with pixel count, because it is
one ray per lit pixel and nothing about it is shared between pixels. Both rows
give a consistent **2.2–2.8 ns per shadow ray**.

**A light-space shadow map was costed against this and rejected.** The window's
footprint perpendicular to `lightDirection` is ~1065 × 877 world units, so a map
at one texel per voxel is 934 K rays and at one per two voxels is 233 K —
fixed, whatever the screen resolution. Its rays are longer than the screen ones
(they enter the volume from outside; a screen shadow ray starts on a surface and
leaves the 128-tall window after ~160 units), so call it 2–4.6 ms and 0.6–1.2 ms.
Against 1.45 ms at 1× that is a wash; against 9.12 ms at 4K it is a 2–15x win.

It was still not built, and the reason is phase 7: **a shadow map answers
exactly one question and cannot carry bounce light.** Once GI is in scope the
same rays want a radiance pyramid, and that structure answers sun shadows, AO
and bounce together. Two structures answering overlapping questions is the thing
to avoid, not the extra pass.

---

## Phase 7 — Voxel cone tracing: shadows, AO and bounce from one pyramid

*Design, large. **Opus**. Supersedes 6.5 and takes over from 6.2.*

**Goal.** Replace the per-pixel shadow DDA, the 12-tap AO hack and the flat
ambient constant with cone traces through a single voxel radiance pyramid, so
that shadows are soft and correct, AO is real, and bounce light exists at all.

**Why one structure.** Cone tracing answers all three with the same traversal
and the same data:

- **Sun shadow** — one cone toward the light. Steps grow with the cone, so it is
  far cheaper than a per-voxel DDA, and the penumbra widens with distance to the
  occluder because the cone does.
- **AO** — the near-field occlusion those same cones accumulate.
  `AmbientOcclusion.hlsl`'s 12 `IsVoxel` samples go away.
- **Diffuse GI** — 4–6 wide cones over the hemisphere.
- **Emissive voxels (6.3)** — fall out for free once the pyramid carries
  radiance rather than only coverage.

**What already exists, and it is most of it.** `FarFieldVolume` is the whole
level at 4× downsample with its own brick grid — that is mip 2, built and
maintained. `MarchBricks` is the traversal. `VoxelBrickGrid` is the occupancy
source and already keeps a CPU-side bitmap. Missing: the intermediate mips,
radiance instead of coverage, and the cone tracer.

### Design constraints, settled with the user 2026-08-09

**The game is the constraint, and it decides more than the renderer does.**
Bit Buster is an arcade voxel shooter: an angled, distant camera, fast action,
and destruction rewriting the geometry continuously. Three consequences, and
each rules something out:

- **Geometry changes every frame.** That removes temporal accumulation, which
  is the usual way ray-sampled soft shadows are made affordable — it would
  ghost on debris and explosions, which is precisely what the game is made of.
- **The camera is far and features are small on screen.** Penumbra accuracy
  below a pixel is wasted. What has to read is silhouette and *grounding* —
  debris and characters looking like they sit on the ground. That is contact
  shadow and AO, not penumbra physics.
- **It is an action game's frame budget.** Even 4 shadow rays per pixel is
  ~4.8 ms at 1365x767, which is ~38 ms scaled to 4K. There is no sample count
  that is simultaneously smooth enough and cheap enough. That is the argument
  for cone tracing in one line.

**Budget: ~3 ms at 3840x2160 for the whole lighting solution** — sun shadow, AO
and bounce together, in gameplay. Everything below is sized against that. For
scale, the voxel pass alone is 5.6 ms at 4K today before any of this.

| | budget |
|---|---|
| Pyramid rebuild (incremental) | ~0.3 ms |
| Sun cone, full resolution | ~1.2 ms |
| Diffuse cones + AO, quarter resolution | ~1.2 ms |
| Bilateral upsample | ~0.3 ms |

**Rebuild is incremental, from the dirty-brick set.** `VoxelEditBatch` already
accumulates which bricks a burst touched — it exists for phase 2's seeding and
phase 4's connectivity — so the mip build consumes the same set and only
re-downsamples what moved. Cost then scales with how much was destroyed rather
than with the size of the window, which is the right shape here. A full rebuild
was the alternative and is ~10.8 M texels per frame, dominated by reading mip 1
out of the 288 MiB host-visible voxel buffer.

**The risk this takes on, and it needs the same answer phase 2 gave.** A missed
dirty mark shows as *stale lighting* — a wall that was blown open still casting
its shadow — and that is invisible in code review, exactly like a missed brick
count. Phase 2 shipped `RenderContext::ValidateBrickGrid()` behind **View →
Validate Occupancy Bricks** for this and it found the answer immediately. Build
the equivalent (recompute every mip from the voxel buffer, compare, report
disagreements) *in the same change*, not afterwards.

### The premise check that split phase 7 in two

**Done before writing any code, and it changed the phase.** 7.1 was written as
"the pyramid, and cone-traced sun shadows + AO", on the assumption that a cone
trace makes soft sun shadows cheap. It does not, for this light.

**A cone is only cheaper than a ray when the cone is wider than the data it
samples.** At `SHADOW_CONE_SPREAD` 0.02 the sun cone's radius is 0.02 x
distance: 1 voxel at 50, 2 at 100, 4 at 200. The finest pyramid level is 4-voxel
cells. So across the whole range over which this game actually casts shadows,
the sun cone is *narrower than the coarsest thing it would be sampling* — every
step resolves to the finest level and the trace degenerates into the exact march
that already exists. Cone tracing wins for **wide** cones: AO and diffuse GI are
tens of degrees and reach useful mip levels within a few voxels.

**And the budget rules out keeping the per-pixel ray.** Shadow at one ray per
pixel measures 1.194 ms at 1365x767 (phase 6.2's table), which is **9.45 ms**
scaled to 3840x2160. Against 3 ms for sun + AO + GI combined, *no* per-pixel
exact shadow fits — not at four samples, not at one.

So the sun needs a structure whose cost does not scale with pixel count, and the
light-space shadow map costed and rejected in 6.2 is that structure. The
rejection reasoning there ("answers one question, cannot carry bounce light") was
correct and is not a reason to prefer the pyramid *for this question*, because
the pyramid does not answer it either. They are complementary, not competing:

| | structure | cost at 4K |
|---|---|---|
| Sun shadow | light-space depth map, marched with the brick DDA, PCSS on lookup | ~0.9-1.4 ms, **fixed** |
| AO + bounce | mip pyramid, wide cones at quarter resolution | ~1.5 ms |

**A depth map also errs the right way on thin geometry.** It records the nearest
blocker per texel, so a one-voxel post claims its whole texel and casts a shadow
two voxels wide at one texel per two voxels. It *over*-represents thin occluders
where the cone *under*-represented them — and "thin shadows too weak" is the
complaint 6.2 could not satisfy at any setting.

**Generalise:** the technique that is right for a family of problems is not
automatically right for every member of it. Check the technique's precondition
against the actual parameters before building — here it is one line of
arithmetic (cone radius versus cell size at the distances that matter), and it
was worth more than the phase it rewrote.

### 7.1a — Light-space sun shadow map

- Light-space footprint of the 768x128x768 window perpendicular to
  `lightDirection` is ~1065 x 877 world units. At one texel per two voxels that
  is ~233 K texels, each a brick-DDA march through the volume.
- Rebuilt every frame: characters move and destruction rewrites geometry, so
  there is nothing to cache. Its cost does not depend on what changed, which is
  the opposite trade from the pyramid and is fine at this size.
- PCSS on lookup — blocker search, then a filter width from the blocker
  distance — gives a penumbra that widens with occluder distance without costing
  rays, which is the property 6.2 bought with sample count.
- Watch: depth bias and acne. Voxel geometry is axis-aligned, which makes
  normal-offset bias behave well.
- **Acceptance.** Thin occluders still cast (the test 6.2 failed). Penumbra
  widens with distance, contact stays sharp. Cost measured at 1x and 4K and
  shown to be flat between them, which is the entire point.

### 7.1a results — DONE, 2026-08-09

**It does the one thing it was built to do.** `game-release`,
`Valley_Path_To_Castle_Beat1`, headless, 900-1200 frames:

| | 1920x1080 | 3840x2160 |
|---|---|---|
| Voxel pass | 0.730 ms | 2.380-2.396 ms |
| **Sun Shadow** | **0.372 ms** | **0.368-0.382 ms** |

**The shadow pass is flat across a 4x change in pixel count**, which is the
entire design goal and the thing the per-pixel ray could never be. Whole
lighting cost at 4K is ~2.76 ms against the 3 ms budget. Zero validation errors.

Do **not** compare these against phase 6.2's 4K table. Those were windowed runs
at a different vantage, and gameplay is not deterministic — the plan's own phase
1 notes record the voxel pass ranging 1.48-6.0 ms on sightline alone. The
within-run resolution sweep above is the comparison that means something.

**Two bugs, and the second is the more interesting.**

- *The blocker search was sized off the whole window's depth range*
  (`shadowDepth.w`, ~1300 units), so it clamped to the maximum radius for every
  pixel. A handful of taps spread that wide makes the average blocker depth —
  and therefore the filter radius — jump from pixel to pixel. **The noise was
  never in the filter, it was in the estimate driving it.** The radius is now
  bounded by the receiver's own distance from the near plane, which is the
  furthest anything could possibly occlude it from.
- *The contact-hardening early-out returned "fully shadowed"* whenever the
  computed filter radius fell below half a texel, rather than testing. The
  blocker search finding *anything* is not the same as this pixel being
  occluded, so one stray tap out of sixteen became a hard black pixel. It hid
  until the search radius shrank and pushed most pixels down that path.

Measured on a fixed crop of the ground, isolated-speckle fraction: **11.53% ->
0.64%**, high-frequency energy 7.84 -> 1.44.

**Thin occluders cast, which is what phase 6.2 could not achieve at any
setting.** Bridge railings and fence posts read clearly. A depth map records the
nearest blocker per texel, so a one-voxel post claims its texel and casts a
shadow slightly *wider* than itself — erring thick where the cone erred thin.

**Known broken: `SUN_SHADOW_REFERENCE` does not run.** The switch compiles and
the process exits during startup with no timings and no validation error. The
first hypothesis — DXC stripping the then-unused `sunShadowMap` and leaving
`VoxelPass` binding a descriptor the reflected layout lacks — was wrong, or at
least insufficient: keeping the texture alive with a multiply-by-zero changed
nothing. Unfixed. It matters because it is the only way to A/B the map against
ground truth.

**Method note that paid for itself immediately.** Iterating on this by eye costs
a screenshot per attempt, which is expensive. Two cheap scalar metrics over a
fixed crop — mean luminance, and the fraction of pixels differing from *both*
horizontal neighbours by more than a threshold — ranked three candidate builds
correctly and caught the early-out bug as a *brightness* jump (90 -> 132) that no
amount of looking at a penumbra would have named. Look once to confirm; measure
to iterate.

### 7.1b — The pyramid, for AO and (later) bounce


Coverage only, no radiance. Safe in gamma space, because an occlusion term is a
*multiplier* and multipliers survive gamma; only additive bounce does not.

Wide cones only — the sun is 7.1a's problem now.

- Mips over the resident window as RGBA8 3D textures with hardware trilinear
  filtering, which is what makes a cone step one `SampleLevel`: 384x64x384,
  192x32x192, 96x16x96, … ≈ 10.8 M texels, **~43 MiB**. Alpha is the coverage
  fraction; a downsample is the mean of eight children.
- **The infrastructure exists** — `E_TEXTURE_3D` maps to `VK_IMAGE_TYPE_3D` in
  `VKTranslate.h`, `VKView.cpp` already handles 3D depth and
  `VK_IMAGE_VIEW_TYPE_3D`, and `ComputePass`/`VKComputePass` are there
  (`VoxelBakePass` is the dead one, but the machinery under it is not). Nothing
  here needs a new resource category, only wiring.
- **Mip 0 is not stored.** It would be a 288 MiB duplicate of the voxel buffer.
  The existing exact DDA covers the first few voxels of a cone, which is where
  contact shadows live and where mip 0 precision would have mattered; the cone
  switches to mip 1 once its footprint exceeds two voxels. That split is the
  same shape as phase 6.2's rejected contact/density experiment, and it works
  here for the reason that one failed: a trilinearly filtered mip is a smooth
  field, where a per-brick count is an 8-voxel constant.
- **Isotropic first.** Anisotropic (six directional channels) is the standard
  fix for light leaking through thin walls, at six times the memory and build
  cost. Start isotropic, look at it, then decide — thin walls are common in
  this game's buildings, so this may not survive contact.
- New bindings: rule 1 applies — `CMake/Shaders.cmake` and `VKShaderBindings.h`
  in lockstep.
- **Acceptance.** Soft sun shadows whose penumbra widens with distance to the
  occluder, contact still sharp, and **thin occluders still casting** — a
  railing or a fence post is the case phase 6.2 could not satisfy at any
  setting, and it is the acceptance test that matters. AO from the cones rather
  than `GetSkyVisibility`. Cost measured at 1x and at 4K against the 6.2 table.
  Validation path built in the same change. Zero validation errors.

### 7.1b first cut — cone AO against the *existing* brick counts

**The pyramid was not built, on purpose: its precondition was checked first.**
7.1b was scoped as "build the mip pyramid, then cone trace it". But an AO cone
is wide — at `AO_RING_SINE`/`AO_RING_COSINE` its footprint exceeds a brick
within a few voxels of the surface — so it is already sampling data coarser than
itself, which is the condition under which a cone beats a ray. The brick counts
`VoxelBrickGrid` has maintained since phase 2 are exactly that data. Trying them
costs a shader; the pyramid costs 3D textures, per-mip views, a compute pass,
barriers and a dirty-region upload.

`AmbientCone.hlsl`: five cones (one on the normal, four on a ring near the
cosine-weighted centroid), six geometrically growing steps each, front-to-back
occlusion accumulation against brick density. Steps grow because the cone's
footprint grows linearly with distance — a fixed step count then covers an
exponentially growing range without undersampling relative to the cone's own
width. Six steps from 4 voxels reach ~57, so a surface can feel a building; the
twelve-tap neighbour test it supplements only ever saw one voxel out.

Kept *alongside* `GetSkyVisibility` rather than replacing it, multiplied
together: the neighbour test carries voxel-scale contact detail the cone cannot
resolve, and the cone carries enclosure the neighbour test cannot reach.

**Measured**, 3840x2160, headless, same vantage:

| | Voxel pass |
|---|---|
| cone AO off (`AO_CONE_STRENGTH 0`) | 2.407 ms |
| cone AO on | 3.596 ms |

**1.19 ms at 4K.** With the sun map's 0.368 ms that is **1.57 ms of lighting**
against the 3 ms budget, leaving room for bounce. Zero validation errors. Mean
luminance over an open-ground crop moves 113.7 -> 106.3, which is the right
*small* magnitude for ground that is genuinely not enclosed.

### The answer: no. Brick resolution is not enough for AO.

**Seen on screen, twice, and the two failures bracket the problem with nothing
in between.**

- *Cone start under ~8 voxels* - a ring cone leaves at ~52 degrees, so near the
  surface it travels *along* it and samples the 4-voxel bricks containing the
  wall it started on. The wall occludes itself. On stepped geometry the origin
  landed inside the ledge above and the surface went black in wedges, reported
  as "full of black triangles".
- *Cone start at 8 voxels* - far enough out to escape, and now **nothing
  measures occlusion between 1 voxel and 8**. `GetSkyVisibility`'s neighbour
  test covers exactly one voxel; the cone starts at eight; the gap is precisely
  the scale of the gaps between stones in a wall. Reported as "flat and
  inconsistent" - flat from the missing near field, inconsistent because which
  samples land in an occupied brick depends on where a block happens to fall in
  a 4-voxel lattice that knows nothing about it.

There is no cone start that satisfies both, because the constraint is the data,
not the trace. **This is what justifies the pyramid**, and specifically levels
*finer* than the brick - a 2-voxel level with trilinear filtering, so a cone
samples its own scale instead of a fixed grid. `AmbientCone.hlsl` is kept and
gated off behind `AO_CONE_ENABLED`; the cone itself is correct and becomes the
pyramid version's body.

**The check was still worth running.** It cost one shader file and it converted
"the pyramid is probably needed for AO" into a demonstrated requirement with a
named mechanism - which is a better starting point for building it than a plan
paragraph was.

**A separate real defect it flushed out, which would have shipped otherwise:**
the sun shadow map's depth bias was a flat constant, and a vertical wall under a
48-degree sun is close to edge-on, where one shadow texel spans a long run of
the receiver and no constant can absorb the difference. Bias and normal offset
now scale with `(1 - N.L)` - `SUN_SHADOW_GRAZING_SCALE`. The black wedges were
*both* causes at once, which is why the first fix only half worked.

**Verification note.** Every visual regression this phase produced landed in
enclosed geometry, while the fixed capture crop was open ground. The valley's
left and right walls are in frame at the standard vantage - crop
`(60, 250, 900, 640)` of a 3840x2160 capture - and that is the crop to check
before handing over a shading change.

**Why this does not close 7.1b.** Two things are unverified and one is known:

- Blockiness in genuinely enclosed geometry — interiors, under bridges, against
  cliff faces — has not been looked at. Open ground was, and is clean. If it is
  blocky there, the pyramid with trilinear filtering is the fix and this becomes
  its motivation rather than its replacement.
- The strength, ring tilt and step growth are unturned by any eye.
- **Bounce still needs the pyramid regardless.** A count is coverage, not
  colour; 7.3 needs radiance per cell and no amount of counting supplies it. So
  the pyramid is deferred, not cancelled — this only establishes that *AO* may
  not need it.

### 7.1b proper — the pyramid. Designed here, built as route A below.

**Levels, and the finest one is the point.** The 4-voxel brick is what failed;
the pyramid has to go *finer*, or it reproduces the same gap between 1 voxel and
the first level it can sample.

| level | cell | cells (768x128x768) |
|---|---|---|
| 0 | 2³ | 384x64x384 = 9,437,184 |
| 1 | 4³ | 192x32x192 = 1,179,648 (**the existing brick counts**) |
| 2 | 8³ | 96x16x96 = 147,456 |
| 3 | 16³ | 48x8x48 = 18,432 |
| 4 | 32³ | 24x4x24 = 2,304 |

**Two routes, and the cheap one should be tried first.**

*A — extend `VoxelBrickGrid` to multi-level counts, in the existing mapper.*
It already maintains level 1 incrementally, with underflow reporting and a
`ValidateBrickGrid` path wired to the editor. Adding levels is the same code
per level: `SetVoxel` bumps five counters instead of one, indexed by shifting
the voxel coordinate. Append the levels into the *same* brick mapper — the
offsets are derivable in the shader from `worldSize` by the same ceil-div both
sides already do — so **no new binding, no contract change, no descriptor
work**. Incremental by construction, which is what was chosen.
  - Cost: 37.7 MiB of GPU mirror per buffer for level 0 (75 MiB for both), 9.4
    MiB of CPU counts at `uint8` (a 2³ cell holds at most 8).
  - Risk to measure first: `SetVoxel` goes from one counter update to five, and
    a world load stamps 3.1 M voxels. Watch peak `CPU VoxelBaker::Occupy
    (added)` — phase 4d has the baseline. The upper levels are tiny and stay in
    cache; level 0 is the one that could bite.
  - Costs 8 fetches per filtered sample, done by hand.

*B — GPU 3D textures with a compute mip build.* Hardware trilinear makes a cone
step one `SampleLevel` instead of eight fetches. Needs, in order: `View::Info`
to carry mip levels and `VK_IMAGE_USAGE_STORAGE_BIT` (`VKResource::CreateImage`
already takes a mip count, `View::Resize` hardcodes 1);
`GetOrCreateImageView` to cache per-mip views and use `VK_REMAINING_MIP_LEVELS`
for the sampled one; `ComputePass::Data` to declare storage-image outputs and
`VKBuildComputePassBindings` to emit `E_STORAGE_IMAGE` for them — **the kind
exists and nothing produces it today**; barriers between mip levels; and a
dirty-region buffer to keep the build incremental.

**Do A first.** The open question is whether levels finer than a brick fix AO,
and A answers it as well as B for a fraction of the work while reusing machinery
that is already proven and already has a validation path. Move to B only if the
manual trilinear fetch cost shows up in a measurement — which is a question A
can answer and speculation cannot.

*It did, and A was built: see the results section below. Levels finer than a
brick do fix AO, the filter costs 4.6x the rest of the cone, and everything A
built except the storage carries over to B unchanged. B is done too, and the
list of Vulkan work above is not quite what it needed — no compute pass and no
storage images, because a linear `vkCmdBlitImage` builds the chain and a
buffer-to-image copy fills mip 0.*

**Whichever route: build the validation in the same change.** A missed dirty
mark is stale lighting, invisible in review, exactly the shape of a missed brick
count — and phase 2's `ValidateBrickGrid` found that class immediately.

### 7.1b route A results — built, and it hands the phase to route B

**Done: the pyramid exists, is validated, and the cone against it is right.
Not done: it is off, because the hand-written trilinear filter costs 4.6x the
rest of the cone.** Three commits, and each of the three findings below is one
of them.

**The addressing is derived on both sides, not passed across.** Five levels laid
end to end in the buffer the bricks already bind, offsets recomputed from
`worldSize` by the same ceil-div in `VoxelBrickGrid::Resize` and
`VoxelPyramid.hlsl`. No new mapper, no new descriptor, no change to the SPIR-V/
C++ contract — and the bricks become `PYRAMID_BRICK_LEVEL` of it rather than a
structure beside it, so they no longer start at element zero. The far field
keeps its own `VoxelBrickGrid`, so it gets the pyramid and the same layout free.

**Maintaining it per voxel is the wrong shape, and the measurement is the
finding.** The plan above said `SetVoxel` bumps five counters instead of one and
named `CPU VoxelBaker::Occupy (added)` as the thing to watch. It went **1.86 →
7.20 ms avg, 23.6 → 89.2 ms peak** — over a second added to a world load whose
whole stamp cost was 400 ms. Attributed by building a variant that kept five
counters and wrote one mirror: **3.46 ms**, so four counters cost 1.6 ms and
four scattered stores into uncached host-visible memory cost 3.7 ms.

Nearly all of it is redundant: **a 32³ cell is written by up to 32768 voxels of
the same burst**, each recomputing and re-storing the same cell. So the bricks
stay incremental — gameplay reads them through `GetCount` inside the frame that
wrote them — and everything else is deferred:

- a write marks its brick in a dirty bitmap, one cached atomic OR over 1.2 M
  bits instead of four counters and four streaming stores;
- `FlushDirty`, from `RenderContext::Present`, rebuilds each dirty brick's eight
  level-0 cells from the **occupancy bitmap** — the only representation fine
  enough and already cached CPU memory — and marks its parent; each coarser
  level is then the sum of its eight children.

`Occupy (added)` is back to **1.85 ms**, indistinguishable from the bricks
alone. `FlushDirty` is **0.025 ms a frame** in gameplay and **21 ms once**, on
the frame a world finishes loading and the whole window is dirty.

Two things fall out of rebuilding rather than accumulating, both worth keeping.
`EndRegion`'s rule for an unaligned region — mark the straddling cell fully
occupied rather than undercount it — now applies to **bricks only**, where it
costs traversal; at 32³ it would have asserted a block of ambient occlusion from
one voxel of evidence. And the state hash folds the bricks and the bitmap alone,
because every other level is derived from those two and folding it in would only
make a determinism hash depend on when the last flush ran.

**Validation, in the same change as the plan asked.** `Validate` covers every
level and flushes first: in-game, **10,785,024 cells against 75,497,472 voxels,
zero disagreements**, alongside a sync audit reporting only the expected
dynamic-renderer differences. Four new checks in `voxagine_tests`, including the
bulk region path across every level in both directions.

**The cone against the pyramid is right.** It starts two voxels off the surface
rather than eight, because the finest level is two voxels — so the 1-to-8 voxel
gap that made stonework read flat is gone, and so are the black wedges. On the
valley-wall crop: wall mean **82.5 → 72.9**, open ground **158.7 → 152.6**,
which is the right shape (recesses and the underside of a shelf darken, flats do
not). `AO_CONE_STRENGTH` is unturned by any eye at 0.35.

**And it is unaffordable as route A.** Voxel pass, headless, same vantage:

| | 1920x1080 | 3840x2160 |
|---|---|---|
| cone off | 0.745 ms | 2.426 ms |
| cone, one fetch a step | 1.051 ms | 3.565 ms |
| cone, trilinear by hand | 2.320 ms | 7.666 ms |

**+5.24 ms at 4K against a 3 ms lighting budget**, of which the filter alone is
4.1 ms. The middle row is the same trace with `AO_CONE_TRILINEAR 0` — one fetch
a step, which is what a 3D texture's `SampleLevel` charges — and it costs
**+1.14 ms**, which fits with the sun map's 0.37 ms beside it. That is the
comparison route A was run to produce.

**So: do B, and it is now a cost change rather than a question.** The list of
Vulkan work under *B* above is unchanged and still accurate. What B does **not**
have to redo: the level geometry, the dirty-region build, `FlushDirty`, the
validation, the cone body, or any of the constants — B replaces where the counts
are stored and how a sample is filtered, and the rest is done.

Two smaller results kept here because they will otherwise be rediscovered:

- **Hoisting the level offsets is worth 0.4 ms at 4K.**
  `PyramidLevelOffset` runs the full depth and masks rather than stopping at the
  requested level: the level varies per sample while the grid sizes do not, so
  the early exit dragged five ceil-divs into the inner loop instead of letting
  them hoist out of the cone.
- **`--frames` counts main-loop iterations, not completed frames.** When the GPU
  is behind, `Present` early-returns and the loop spins — 4000 "frames" in
  0.8 s. Every capture taken that way is of the **startup fade**, which reads as
  a uniformly black image and looks exactly like a shading bug. Three
  diagnostics returning wildly different values produced byte-identical image
  statistics before that was spotted. Capture on wall time, and check the run
  produced an `[fps]` line at all. Worth fixing in phase 0b: count frames the
  GPU retired, or take `--seconds`.

### 7.1b route B results — DONE, 2026-08-09. The pyramid is a texture, and the cone is on

**Route A's finding was that the levels are right and the storage is wrong.**
It measured the hand-written trilinear filter at 4.6x the whole rest of the
cone, +5.24 ms at 4K against a 3 ms lighting budget, and predicted that a
hardware `SampleLevel` would cost what its one-fetch-a-step variant did. It
costs slightly less than that.

| Voxel pass, headless, same vantage | 1920x1080 | 3840x2160 |
|---|---|---|
| cone off | 0.749 ms | 2.442 ms |
| route A, one fetch a step | 1.051 ms | 3.565 ms |
| route A, trilinear by hand | 2.320 ms | 7.666 ms |
| **route B, 3D texture** | **0.994 ms** | **3.467 ms** |

**+1.03 ms at 4K**, and with 7.1a's sun map at 0.38 ms that is **1.40 ms of
lighting** against the 3 ms budget. Better than the one-fetch row because the
texture deletes more than the filter: a normalized coordinate is the same at
every level, so the per-level grid size and base offset the buffer version
recomputed per sample are gone too. `AO_CONE_ENABLED` is **1**.

Crop means at the standard vantage, cone off → on: open ground **144.4 →
142.0**, the valley wall **89.5 → 85.8**, rubble **81.9 → 77.3**. That ordering
is the whole check — flats barely move, detailed and enclosed geometry
darkens. (Route A's numbers were larger in relative terms and are not
comparable: 7.2 landed in between and AO now multiplies linear radiance.)

**The texture is R8, one channel, holding a density rather than a count.** A
2³ cell holds at most eight voxels, so count and unorm byte are exactly
inter-convertible and the quantization is free; an occlusion term needs nothing
else. Mip L is pyramid level L. 10.8 MiB for the whole chain.

**The mip chain is blitted, not uploaded, and that is the one design decision
worth reading.** A Vulkan mip is a floor-halving of the level below it; a
pyramid level is a ceil-div of the *window*. The two agree only where the
window is a multiple of the coarsest cell — 768x128x768 is, a level with an
odd chunk size would not be — and where they disagree, a level uploaded by
index lands on the wrong texels and nothing says so. `vkCmdBlitImage` with a
linear filter resamples whatever extents the image actually has, and for an
exact halving it *is* the eight-child average. So the CPU stages mip 0 only.

**What is uploaded is dirty boxes, and the bookkeeping is the risk.**
`FlushDirty` already walks the dirty bricks in ascending id order, so a run
along x merges into one box and a burst of a thousand bricks becomes about a
hundred. Over `k_uiMaxDensityRegions` (4096) boxes, or a buffer swap, or a
resize, falls back to uploading the whole level — 9.4 MiB, which beats issuing
a million copy regions. Measured: **0.23 ms once** when a world finishes
loading, **0.048 ms** for an incremental frame, and nothing at all when nothing
moved.

**The validator was built in the same change, as this phase was told to, and it
paid for itself on its first run.** `View → Validate Coverage Pyramid`, and
`VOXAGINE_PYRAMID_AUDIT=<seconds>` for a running session: it reads the texture
back and checks mip 0 against the staging mirror *exactly*, and every coarser
mip against the average of its children (±2, because the hardware filter rounds
its own way). It immediately reported 92 stale cells — the call recording dirty
regions had never been wired into `FlushDirty` at all, because a
search-and-replace silently did not match. That is precisely the failure this
phase warned about: the counts are right, the image looks plausible, and the
lighting describes geometry that is no longer there. Zero disagreements now,
across three audits of a Debug session, with zero validation errors.

Two method notes:

- **The audit must not compare across an outstanding upload.** It runs at the
  top of `Present`, before `FlushDirty`, and skips while
  `HasPendingDensityRegions()` — that is the one moment in the frame when the
  mirror and the texture are supposed to be identical. It also waits on the
  engine that records the upload before reading back: two submissions to the
  same queue carry no dependency on each other, and a readback that overtakes
  the frame reports the part of the upload that had not landed, which looks
  exactly like the bug it is hunting.
- **Bisecting with "upload everything every frame" separated the two halves in
  one build.** Zero disagreements that way and 92 with regions meant the copy,
  the offsets, the blit chain and the density encoding were all correct and
  only the bookkeeping was not. Worth remembering: an incremental structure
  always has a wholesale version to compare against, and it costs one line.

**Then the buffer went back to bricks.** With the pyramid in a texture, four of
its five levels were still being written into the host-visible buffer the
marcher binds and read by nothing — 76.8 MiB of mirror across the two window
buffers, and four streaming stores into uncached memory per rebuilt cell, which
is the very cost route A measured. Removing them took
`CPU VoxelBrickGrid::FlushDirty`'s world-load peak **25.4 → 19.4 ms**, below the
21 ms it cost before the texture existed; `Occupy (added)` is unmoved at 1.87 ms.
`VoxelPyramid.hlsl` loses `PyramidLevelOffset` and the bricks start at element
zero again. The CPU keeps counts for every level — they are what the density
mirror and `Validate` are computed from, and they are ordinary cached memory.

**Known and accepted: the density staging buffer is single-buffered against the
frame.** The copy is recorded in frame N's submission and the CPU writes the
mirror again in frame N+1's `FlushDirty`, with only the two-slot command-engine
fence between them — the same shape as `Docs/DESTRUCTION_PLAN.md`'s P16 on the
particle mapper. What can tear is one byte of one cell of an occlusion term,
and every value a byte can hold is a valid density, so the worst case is a
single texel of AO being one frame stale. Recorded rather than fixed; a back
buffer here would double 9.4 MiB and buy a frame of AO latency.

**`AO_CONE_STRENGTH` 0.35 is confirmed on screen**, so the one number this
phase could not settle headless is settled. The ring tilt and the step growth
are still unjudged, but they change the *shape* of the term rather than its
weight, and nothing has been reported wrong with either.

**What is deliberately not here.** The cone samples one mip per step, with a
point mip filter: blending two levels is smoother where a cone widens and costs
twice what this phase bought. Anisotropy (six directional channels) is still
unbuilt and still the standard answer to light leaking through thin walls — the
place to start if enclosed interiors read wrong.

### 7.2 — Linear light (this is 6.4, and the order is load-bearing)

*As written:* do 6.4 here, before any bounce. Lighting is gamma-space today —
`AmbientOcclusion.hlsl:44` hand-applies `pow(x, 2.2)` precisely because it is —
and additive bounce in gamma space is wrong in ways that are hard to attribute
to their cause. Expect to retune `AMBIENT_VALUE`, `SHADOW_CONE_SPREAD` and the
phase 1 constants once, here.

*(The `pow(x, 2.2)` citation was already stale: phase 1's sun/sky split deleted
it, along with the `pow(x, 1/3)` beside it. The pipeline was still gamma-space
for the reason the rest of the paragraph gives.)*

### 7.2 results — DONE, 2026-08-09

**Lighting is linear. The encode is at the end of scene shading, not at
present, and that is a deliberate departure from how 6.4 was written.**

**The boundary, which is the whole design.** `Color.hlsl` is new and holds
`SrgbToLinear`, `LinearToSrgb` and `EncodeSceneColor`; the last exists so that a
grep for it lists the entire end of the linear region. `ShadeSurface` decodes
albedo on the way in and returns **linear radiance**; its four callers —
`VoxelRenderer.ps.hlsl`, its ShadowLess variant, `FarField.hlsl`'s
`ShadeFarField` and `Particles.vs.hlsl` — encode. Nothing else in the frame
changes: the targets are still `R8G8B8A8_UNORM`, the present is still a byte
copy, and FXAA, the UI composite and ImGui still work on encoded values.

**Why not linear all the way to present, which is what 6.4 asked for.** Three
things downstream of the scene target are correct on encoded input and wrong on
linear input, and all three composite into the same image:

- **FXAA is a perceptual filter.** It thresholds on luma computed from the
  encoded value; on linear input it under-detects edges in shadow and
  over-detects them in highlights.
- **The UI target, ImGui and the debug lines are all authored sRGB** and blend
  into that same image. Each would need a decode on the way in, and the blend
  weights change with it.
- **8-bit linear bands in the darks.** sRGB's curve is what spends those codes
  where the eye is. The fix is an FP16 scene target, which is 2× the bandwidth
  of the largest target in the frame at 4K — a real cost, for no gain that
  reaches the screen. Revisit only if HDR output is ever wanted.

The region this leaves linear is exactly the region 7.3's bounce term lives in,
which is what the ordering note was protecting.

**Every constant was converted, not retuned.** Each carries the gamma-space
value it came from in a comment, because 0.0946 does not read as "the old
0.34" to anyone:

| | was (gamma) | is (linear) |
|---|---|---|
| `SUN_COLOR` | `(1.0, 0.96, 0.88) * 0.85` | `(1.0, 0.912, 0.750) * 0.692` |
| `SKY_LIGHT_COLOR` | `(0.588, 0.902, 1.0) * 0.34` | `(0.350, 0.809, 1.0) * 0.0946` |
| `SKY_GROUND_COLOR` | `(0.20, 0.17, 0.13)` | `(0.0331, 0.0245, 0.0153)` |
| `SKY_AO_CURVE` | `1.35` | `2.97` |

**`SKY_AO_CURVE` is the one that had to move rather than convert, and it is the
trap in this whole change.** It shapes a *visibility fraction*, and a fraction
that used to multiply an encoded value now multiplies radiance that is encoded
afterwards — so the old 1.35 would have arrived on screen as ≈0.61 and AO would
have all but disappeared. 2.97 is 1.35 × 2.2: the same image. Anything of this
shape — the shine line's gain is the other one here, hence
`GammaGainToLinear` — needs the exponent, not the value. **A multiplier tuned
against an encoded image is not a linear multiplier.**

**What changed on screen, measured** (`Valley_Path_To_Castle_Beat1`, headless
3840×2160, mean luminance over a fixed crop):

| crop | before | after |
|---|---|---|
| whole frame | 89.03 | 80.77 |
| valley walls `(60,250,900,640)` | 89.13 | 90.83 |
| sunlit centre | 162.12 | 139.20 |

That distribution *is* the correction and it predicts exactly: a surface lit by
one term is unchanged, because one term through one curve is the same number —
the enclosed valley-wall crop moves 1.9%. A surface lit by sun *and* sky was
over-bright, because the two were summed after encoding — the open sunlit ground
drops 14%, and the arithmetic for that surface predicts 14.7% before running it.
The frame is a little darker and a good deal more saturated; the sand stops
reading as washed out. **This wants Joey's eyes**, and no exposure gain was
invented to hide the shift: reproducing the old lit colour exactly needs
`SUN_COLOR` = `(0.84, 0.99, 0.92)`, i.e. a *green* sun, which is just fitting
the artefact.

**Cost is nothing.** Six `pow`s per shaded pixel against a marcher:

| | before | after |
|---|---|---|
| Voxel | 2.428–2.437 ms | 2.444–2.445 ms |
| Post Processing | 0.253–0.255 ms | 0.249 ms |

Same vantage, `game-release`, `VOXAGINE_PROFILE=1`, 3840×2160. The exact
piecewise sRGB curve was used rather than `pow(x, 2.2)` for that reason — the
present blit hands bytes to a compositor that applies sRGB, so sRGB is what the
display does, and matching it costs nothing measurable.

**Zero validation errors**, Debug build, headless.

**Left alone deliberately.** The `sceneFader` fade-to-black and the UI alpha
composite still happen on encoded values in `PostProcessing.ps.hlsl`. Both are
defensible — a fade is a stylistic curve and UI blending in the space the UI was
authored in is what an artist expects — but neither is *physically* right, and
if a fade ever reads as too fast in the middle, that is where it is.

### 7.3 — Radiance in the pyramid, and diffuse GI

- Mip 0 gains radiance: albedo × the direct lighting the voxel receives.
- 4–6 diffuse cones per pixel **at quarter resolution**, bilaterally upsampled.
  Irradiance is low-frequency so that upsample is nearly lossless, and it is
  what keeps GI affordable at 4K — where per-pixel work is the entire problem,
  as 6.2's table shows.
- **Acceptance.** Colour bleeding onto a nearby surface, visible and plausible.
  Cost measured at both resolutions.

### 7.3 results — DONE, 2026-08-09

**The pyramid carries albedo and the AO cones gather bounce out of the same
fetch.** Colour bleeds: the valley's cliff face takes a distinct green cast from
the vegetation growing on it, and the shadowed sand beside the wall warms.

**The design departs from this phase as written in one large way, and the reason
is the thing worth reading.** 7.3 was specified as *4–6 diffuse cones per pixel
at quarter resolution, bilaterally upsampled*, and budgeted at 1.2 ms + 0.3 ms
on that basis. None of that was built, because the premise under it is false
here: **the diffuse cones are not extra cones.** 7.1b already traces five wide
cosine-weighted cones over the hemisphere for ambient occlusion, and a bounce
gather wants the same five cones with the same weights. Making the texel RGBA
turns one `SampleLevel` into both answers — the coverage the occlusion needs and
the albedo the bounce needs — so the marginal cost of GI is the arithmetic and
one shadow tap, not a second traversal.

Quarter resolution would also have cost more than it saved. The voxel pass
shades *inside* the AABB proxies, so there is no G-buffer: a quarter-res GI pass
would need position and normal at quarter res, which means either a second
marching pass or a new prepass — and phase 3 is the recorded history of building
a prepass for this renderer and finding it redundant.

**Storage: RGBA8, premultiplied, linear.**

- Alpha is the density 7.1b already staged. RGB is the cell's albedo *times*
  that density. Premultiplied is what keeps the mip chain a plain box average —
  `vkCmdBlitImage` averages four channels as happily as one, and an average of
  premultiplied radiance is the right answer where an average of raw colours
  would let one voxel speak for eight empty neighbours.
- **Linear, decoded on the CPU through a 256-entry table.** The alternative is
  `SrgbToLinear` per cone sample, and there are thirty-five of those per pixel.
  8-bit linear quantizes the darks coarsely; a bounce term does not resolve it.
- **Colour is carried per *brick*, not per level-0 cell** — 9.4 MiB instead of
  75, for a difference no diffuse cone can see. `RebuildFineCells` loads it once
  and writes it into all eight cells under that brick, so mip 0 has fine
  coverage and coarse colour.
- Cost: staging 18.9 → 75.5 MiB, the texture 10.8 → 43.1 MiB of VRAM, plus 9.4
  MiB of brick colours. **+66 MiB of RSS and +32 MiB of VRAM**, by arithmetic —
  no A/B against master was run. Peak RSS measured **859 MB** on this level,
  which is not comparable to 4d's 946 MB figure without rerunning it.

**Lighting a cell that has no normal.** A pyramid cell is a density and a
colour, so there is no N·L to take and no hemisphere to lerp. `GI_SUN_WRAP` and
`GI_SKY_WRAP` are the averages that stand in for both. Sun *visibility* is real,
though: `GetSunVisibilityAt` is one tap of 7.1a's shadow map at the sample
point, no blocker search and no filter, because the cone averages tens of them
and the aggregate is smooth. That is also what keeps the bounce current without
re-injecting radiance — the pyramid stores albedo, which changes only when
geometry does, and the light is applied at sample time. **A baked-radiance
pyramid would have needed re-lighting every frame a shadow moved**, and a
compute pass with storage images to write it; this needs neither, and every
piece of route B's incremental upload path survives untouched.

`GI_SUN_TOLERANCE` is the one constant that had to be invented. The shadow map
records the front-most surface in a light-space column and a bouncing cell
usually *is* that surface, so without a tolerance every emitter shadows itself
and the bounce is black.

**Measured**, `game-release`, `Valley_Path_To_Castle_Beat1`, headless,
`--uncapped`, same vantage, three runs each:

| Voxel pass | 1920x1080 | 3840x2160 |
|---|---|---|
| cone off | 0.687 ms | 2.511 ms |
| AO only (7.1b) | 1.025 ms | 3.625 ms |
| **AO + bounce (7.3)** | **1.421 ms** | **4.913 ms** |

**Bounce is +0.40 ms at 1080p and +1.29 ms at 4K.** With the sun map at 0.338 ms
that is **2.74 ms of lighting at 4K against the 3 ms budget** — sun 0.34, AO
1.11, bounce 1.29. It fits, and there is no headroom left: 7.4's specular cone
has to come out of something else or be cheaper than these.

Everything scales properly with pixel count now (cone off 3.65x over a 4x change,
AO 3.3x, bounce 3.25x) and `Sun Shadow` is flat at 0.334–0.338 ms across both,
exactly as 7.1a claims. **That is new, and getting there is the other finding.**

#### The benchmark harness was measuring at two different GPU clocks

**Every GPU number in this plan before this line was taken vsync-locked at
60 Hz.** At 1080p that leaves the voxel pass as ~2 ms of a 16.7 ms frame, the
card clocks down to match, and the *same pass* at 4K — which keeps it busy —
runs at a higher clock. Costs measured that way are not comparable across
resolutions, and comparing them across resolutions is most of what this plan
does.

It surfaced as an impossibility: the AO cone measured **0.76 ms at 1080p and
0.67 ms at 4K**, i.e. a per-pixel cost that got *cheaper* as the pixels
quadrupled. The screenshots confirmed the resolutions really were 1920x1080 and
3840x2160, so the only free variable left was the clock.

**`--uncapped` is the fix** (`LaunchOptions`, overriding `EnableVSync` and
`FrameLimit` in `Application::LoadSettings`, writing nothing back). With it the
same build reproduces 7.1b route B's table to within 0.1 ms — which is the
reassuring part: **route B's numbers were right**, so whatever it ran under was
not clock-limited, and the defect is in the harness rather than in the record.
Use `--uncapped` for every GPU measurement from here.

Generalise: *a cost that does not move when you scale the input is the signature
of an optimization that is not addressing the thing that costs* — phase 3's
lesson — **and also the signature of a measurement rig that is not measuring
what it claims.** Sweeping the axis is what distinguishes them, and it is worth
running even when the result is expected to be boring.

**Two smaller things, both worth keeping:**

- **`VoxelBrickGrid::SetVoxel` takes a colour now, and the bool overload is
  `= delete`.** Every caller already spelled the old argument
  `(uiColor >> 24) != 0`; leaving that to the implicit conversion means passing
  `1` as a colour, whose alpha byte is zero, so **every voxel reads as empty**
  and nothing says so. Two call sites did exactly that and the suite caught them
  — but the deleted overload is what makes it a compile error next time.
- **The `AO_CONE_HAS_SUN_SHADOW` include-order contract.** `AmbientCone.hlsl`
  now needs `Lighting.hlsl`'s constants and, in the shadowed variant,
  `SunShadowLookup.hlsl`, so it is included last in both pixel shaders. The
  ShadowLess variant leaves the macro undefined and lights its cone samples with
  an unshadowed sun, which is the same assumption it already makes about the
  surface (rule 8).

**Acceptance.** Colour bleeding visible and plausible — yes, measured as a
*saturation* rise, which is the half mean luminance cannot see: pushing
`GI_STRENGTH` 1.6 → 6.0 takes the valley-wall crop from lum 93.1/sat 0.203 to
105.0/0.210 and the whole frame from sat 0.275 to 0.292. Light that only
brightened would have driven saturation the other way. Cost measured at both
resolutions. Zero validation errors, Debug, headless. `Validate Coverage
Pyramid` clean across all four channels: **10,785,024 texels over 5 mips, 0
disagree**, alongside `[bricks] 0 disagree` over 75.5 M voxels. 78 checks, 31
scenarios, 0 perf regressions.

**`GI_STRENGTH` 2.0 wants Joey's eyes.** 1.0 is one bounce off the albedo the
pyramid holds; above it stands in for the second and third bounces nobody
computes and for a cone that samples one point of a footprint. 2.0 was chosen
against three captures (1.6 / 2.0 / 6.0) — 6.0 makes the cliff read as mossy
rather than lit — but it is a judgement about how much bounce the game wants,
which is the kind of call 7.1b's `AO_CONE_STRENGTH` needed a person for.

**Not done, deliberately:** anisotropy (six directional channels) is still the
standard answer to light leaking through thin walls and is still unbuilt; with
radiance in the pyramid it now costs six times the *colour* too, so it is a
bigger decision than it was. Nothing has been reported leaking.

## Phase 6.1 — Aerial perspective

### 6.1 results - DONE, 2026-08-09

**Built as specified, then re-aimed by a measurement, and the measurement is the
part worth keeping.**

`Fog.hlsl` holds the whole thing: an offset squared-exponential fade toward the
sky colour, applied to **linear radiance before `EncodeSceneColor`**, so a fully
fogged pixel comes out as exactly `SKY_COLOR` and the horizon has no step in it.
One function, called from `VoxelRenderer.ps.hlsl`, its ShadowLess variant (rule
8) and `ShadeFarField` - if the window and the far field fogged differently the
seam would move rather than close.

**`marchDiffuse.Distance` is the wrong distance and the plan said to use it.**
This pass rasterizes AABB proxies, so a primary ray starts where its own box was
entered, not at the camera - that is the same fact phase 3 was deleted over. Fog
is a function of distance *from the camera*, which the pixel shader already
computes for the particle depth test.

#### The premise check: this camera has no horizon

6.1 promised three things. Measured against the game's own vantage, only one of
them is real:

- ~~*Hides the far-field LOD seam*~~ - **there is no far field on screen.**
- ~~*Sells the phase 4 scale*~~ - **there is no horizon.**
- *Depth cueing across the frame* - real, and now the whole justification.

Bit Buster's camera looks down into a valley from close above it. At the
standard vantage in `Valley_Path_To_Castle_Beat1`, and again in
`Fishing_Village_Beat1`, the frame contains no sky, no horizon line and no
far-field geometry: it is all resident window. The first attempt used
`FOG_START` 350 on the reasoning that fog should begin past the window, and it
moved the frame by **0.4 of a luminance code** - correctly, because nothing on
screen was that far away.

**How the range was measured, which took one capture.** Setting `FOG_START` 0
and `FOG_DENSITY` 1/200 turns the fog term itself into a distance
visualization - no debug shader, no new code path, and it is the same
instrument-rather-than-reason move phase 4 used for its boundary artefacts. The
answer: the visible depth range runs from about **120 units at the bottom of the
frame to about 450 at the top**. That is a room, not a landscape, and 3:1 is not
an aerial-perspective ratio.

**Retuned to what is actually there:** `FOG_START` 200, `FOG_DENSITY` **0.0014**.
Measured over three horizontal bands, fog off then on:

| band | luminance | saturation |
|---|---|---|
| top strip (furthest) | 70.66 -> **81.97** | 0.198 -> ~0.16 |
| upper third | 80.89 -> 84.00 | 0.250 -> ~0.22 |
| play area | 106.58 -> **106.59** | 0.394 -> 0.394 |

**0.002 was the first value and Joey called it too intense on screen** - it put
the top strip at 87.88, i.e. half again as much haze as this. 0.0014 keeps about
two thirds of the depth cue. It is one number if it wants to move further; the
band table above is the cheap way to check where it lands without a screenshot.

That distribution *is* the design: the far end of the valley recedes and
desaturates toward the sky, and the ground the player is standing on does not
move by one part in ten thousand. Falling saturation is the check that it is
fading toward the sky rather than merely brightening.

The curve still reaches full fade at real distance - 92% by 1000 units - so
wherever far-field geometry or the endless ground *is* visible, the original
seam-hiding job is done. It just is not the reason to keep this.

**Cost is nothing:** voxel pass at 4K 5.079 -> 5.133 ms, which is inside
run-to-run variance; post processing unmoved at 0.23. Zero validation errors,
Debug, headless; coverage-pyramid audit clean.

**Judged on screen.** The first value read as too heavy a blue over the far end
of the valley and was halved-ish to 0.0014 on that call. `FOG_START` and
`FOG_DENSITY` are the two numbers; `FOG_ENABLED 0` turns it off entirely.

**Not fogged: particles.** `Particles.vs.hlsl` shades debris and passes no
distance. Debris is spawned by gameplay and lives within a few tens of units of
the action, i.e. inside `FOG_START` where the term is identically zero, so
wiring it would change nothing measurable. If the fog start ever comes forward,
this is the first thing that will look wrong.

---

### 7.3b — Candidate: fold the far field into the pyramid as a clipmap

*Not scheduled. Filed here because the two are the same idea built twice, and
because the prerequisite is 7.3 rather than anything in 7.1.*

**The observation.** `FarFieldVolume` is the whole level at 4 voxels a cell,
marched where the resident window has no answer. The pyramid is the window at
2/4/8/16/32 voxels a cell. Both are *coarser data further away, sampled instead
of the fine grid* — which is one structure, a **clipmap**, where each level
covers a larger extent at a coarser resolution. Today it is two systems with two
coordinate spaces, two brick grids, and a DDA that `FarField.hlsl` says outright
is a copy of `MarchBricks` kept separate only because HLSL cannot swap which
buffer a function reads.

**Three things it would buy, none of them tidiness.**

- **Destruction visible at distance.** The far field is built once at load by
  `FarFieldBaker` and *never updated* — verified: nothing under `Core/Voxels/`
  or the physics system references it at all. Blow up a building and it is still
  standing on the horizon. A clipmap level maintained by `SetVoxel`, as the
  bricks already are, fixes that by construction.
- **AO and GI cones that do not stop at the window edge.** As 7.1b is specified,
  a cone reaching the window boundary reports open sky. Under a clipmap it
  continues into coarser levels — which is what a wide cone wants anyway, and it
  is the difference between a valley that feels enclosed and one that does not.
- **One traversal instead of two copies.**

**Two things that make it harder than it looks, and both are load-bearing.**

- **The maintenance paths are fundamentally different and both are needed.** The
  pyramid is maintained by voxel writes. The far field is built by deserializing
  *entity JSON*, because a chunk that has never been resident has no voxels to
  downsample — that is phase 4's central finding and it does not go away. A
  unified structure still needs both producers, and the seam between "levels the
  window maintains" and "levels the baker stamps" is where the bugs would live.
- **Far-field cells carry colour; pyramid cells carry counts.** They only
  converge once 7.3 puts radiance in the pyramid. Attempting this before then
  means inventing a shared cell format for a consumer that does not exist yet.

**Do not start this before 7.3 lands.** 7.1b has not yet established that the
fine levels work at all, and merging two working systems on the strength of an
unproven third is precisely the shape of the mistakes phase 7 already made twice
— see the premise check above, and 7.1b's first cut.

**What the chunk system is not.** Worth stating since the question comes up
together: none of this touches `ChunkSystem`. It streams entities, owns the CPU
voxels, and `Chunk::UpdateGroundPlane` writes the physics floor entities stand
on — phase 4 already records that replacing that floor with the analytic ground
plane is impossible for exactly that reason. A coverage structure knows how full
a region is; it cannot say what to simulate, collide against or load.

### 7.4 — Emissive voxels and a specular cone

6.3 lands here rather than on its own: a pyramid carrying radiance makes an
emissive voxel light its surroundings instead of needing bloom to fake it.
**Audit the alpha tag space first** (rule 3) — find every producer and consumer
of that byte before claiming a value. One narrow cone gives specular.

### 7.4 results — 2026-08-09. Tag space reclaimed, emissives glow, specular is on.
### One part is **NOT BUILT** and is costed below: emissive light spilling onto neighbours.

**The audit was the phase.** Rule 3 says the alpha byte packs `rendererState + 1`
and that occupancy is `a > 0`. The second half is true. The first half was
**not**, and had never been:

```
(pColors[i] | static_cast<unsigned char>(rendererState + 1) << 24)
```

is an **OR** onto a colour whose alpha the MagicaVoxel palette had already set to
255 (`VoxModel.h`'s default palette is all `0xff......`, and `VoxModel::Read`
stores the palette word whole). `255 | 1` is 255. **Every voxel in the world
carried a tag byte of 255, and the render state never reached the buffer at
all.** Nothing noticed, for two reasons that are worth separating:

- **No consumer reads the value.** `VoxelBaker`, `SDFMarcher`, `AmbientOcclusion`,
  `VoxelBrickGrid`, `VoxelEditBatch`, `ChunkSystem`, `FarFieldVolume` — every one
  tests occupancy alone. Which is also what made this safe to fix.
- **So `RS_GRID_LINES` and `RS_SELECTION_LINES` have had no rendering effect for
  the life of this branch.** They are not implemented in any shader either.
  Recorded, not fixed; it is not this phase's business.

Had the bit been claimed without the audit, `0x80` would already have been set on
every voxel and the entire world would have been emissive. That is the failure
rule 3 exists to prevent, and it was one line of code away.

**What landed instead:** the tag is masked in (`(colour & 0x00FFFFFF) | tag`), so
the byte holds what it claims and the bits above the state are genuinely free.
`VOXEL_STATE_MASK` (0x3F) and `VOXEL_EMISSIVE_TAG` (0x80) live in
`VoxRenderer.h` and `Defines.hlsl` and are a SPIR-V/C++ contract like
`BRICK_SHIFT`.

**Two ground-plane writers, and the second is why the first fix looked wrong.**
`Chunk::UpdateGroundPlane` and `RenderSystem::SetGroundPlane` are the same loop
twice, both writing a texture texel straight into a voxel. Fixing only the first
left the entire floor of the level glowing, because the second still wrote 255 —
every reserved bit set, `VOXEL_EMISSIVE_TAG` included.

**How it was found in one build, and this is the reusable part.** Colouring the
emissive branch magenta said *the ground takes it*; that was not enough, because
the ground has two producers and the reasoning pointed at the wrong one. A
second build coloured **every hit by its tag byte** — magenta 255, green 1, blue
3 — and the answer was immediate: the models were blue (`RS_SELECTION_LINES`, the
default) and the ground was still magenta. Same lesson as phase 4's boundary
artefacts and 6.1's distance range: *do not reason about which producer wrote a
value, render the value.*

**The regression check that mattered.** Reclaiming a byte every voxel carries
should change nothing on screen. Fixed crop, before and after: whole frame
luminance **82.68 → 82.68**, valley walls 102.87 → 102.90 — against a
two-runs-of-the-same-binary noise floor measured at 0.06. (Byte-identical
captures are not available: characters move, so two runs of one binary already
differ in 24 k of 25 M bytes.)

**Emissive voxels.** `VoxRenderer::IsEmissive` — an RTTR property, so the editor's
`PropertyRenderer` shows it with no work — sets the bit for every voxel the model
stamps. Both pixel shaders take an early branch **above** the shadow-map lookup
and the cones: everything between there and `ShadeSurface` measures light
*arriving*, and none of it applies to light that is leaving. A lantern is not
dimmer in shadow and ambient occlusion has nothing to occlude. It still fogs,
because a glowing thing far away is still seen through the same air.

Per renderer rather than per palette index because a `.vox` palette entry has
nowhere to say so — MagicaVoxel keeps emission in MATL chunks `VoxModel` does not
read. The tag byte has room for a per-index version later.

Verified by defaulting `m_bEmissive` to true for one capture: all model geometry
renders self-lit at its own colour, shadows and shading gone, while the ground
(tag 1) stays normally shaded. Reverted.

**The specular cone.** One narrow cone along the reflected view ray, through the
same pyramid, gathering with the same front-to-back accumulation as the diffuse
cones and adding whatever it does not hit as sky. Added to the shaded result
rather than folded into `ShadeSurface`'s parentheses: a reflection is light
bouncing *off* the surface, so the albedo must not multiply it. Schlick against
a dielectric `SPEC_F0` of 0.04 — there is no metal in this art — so it is a rim
effect that grows as a face turns edge-on.

**Cost, `--uncapped`, same vantage:**

| Voxel pass | 1920x1080 | 3840x2160 |
|---|---|---|
| all cones off | 0.795 ms | 2.563 ms |
| **shipped (AO + bounce + specular)** | **1.388 ms** | **5.184 ms** |
| Sun Shadow | 0.340 ms | 0.342 ms |

**Total lighting at 4K is 2.96 ms against the 3 ms budget.** There is no headroom
left at all.

**The specular cone measured within run-to-run variance (≤0.05 ms at 4K), and
that number should not be trusted as a general one.** `SPEC_CUTOFF` skips the
cone wherever Fresnel is under 0.06, which at F0 = 0.04 means any face within
~57° of head-on — and this game's camera looks almost straight down, so most of
the screen never traces it. **At a lower camera angle it would cost far more.**
That is a property of the vantage, not a free lunch, and it is the one number
here that would move on a different level.

Image, whole frame: luminance 82.68 → 83.62, saturation 0.251 → **0.238**. Falling
saturation is the check — a sky-coloured reflection is what should desaturate a
warm surface.

#### NOT BUILT: emissive light spilling onto its surroundings

This is the half of 7.4 the plan's own sentence leads with, and it does not fit
the texel. Recorded rather than half-landed, as the rules ask.

**Why it needs a channel that does not exist.** The pyramid texel is RGBA8: RGB
is albedo premultiplied by coverage, A *is* the coverage. The cone computes
`rgb * ConeIncidentLight(sample)`, and an emissive cell must contribute
regardless of incident light — a lantern indoors is the whole use case, and
indoors `ConeIncidentLight` is ~0.03. So the cone has to be able to tell an
emissive cell from a lit one, and there is nowhere to put the distinction:

- **Not in A.** Every coarser mip is a `vkCmdBlitImage` box average, so a flag
  bit averaged with its seven neighbours stops being a flag and starts
  corrupting the density it shares a byte with.
- **Not in RGB.** Same averaging, and unorm cannot carry a value above 1 to mark
  an overbright.
- **Not by pre-lighting on the CPU.** Storing `albedo x approximate light` would
  let emission ride along and would delete the shadow tap the bounce currently
  pays for — but it also deletes the per-sample sun visibility, so geometry in
  shadow would bleed as brightly as geometry in sun. That is a visible
  regression in the effect 7.3 just landed.

**The design, costed.** A second RGBA8 3D texture whose **mip 0 is the brick
level** — which is exactly the resolution the CPU already keeps brick colours at,
and exactly the granularity of the dirty set `FlushDirty` and
`RecordDensityRegion` already walk, so one brick is one texel and the whole
incremental upload path is reused unchanged. 1.18 M texels: **~5.4 MiB of VRAM
and 9.4 MiB of staging**, against the 43 + 75 MiB the coverage texture costs at
level-0 resolution.

The cone samples it at `max(level - 1, 0)` and adds the result unmodulated. One
extra `SampleLevel` per step, behind a **uniform** branch on "does this world
contain any emissive voxel", so a level with none pays nothing and the feature is
pay-for-what-you-use.

**Two reasons it stopped here.** The lighting budget is at 2.96 of 3 ms, and a
second fetch per cone step is of the order of the 1.29 ms the bounce itself
costs — so on a level that used it, lighting would be a third over budget and
that is Joey's call, not a thing to land quietly. And there is **no authored
emissive content to verify it against**: every acceptance test for it would have
to be run against a level edited for the purpose, which means changing shared
content while nobody is watching the screen.

**Bloom is not the fallback, and that is a plan decision rather than a
preference.** 6.3 paired emissive voxels with bloom, and 7.4's premise is that
pyramid radiance replaces it. With that half unbuilt, bloom is the obvious
consolation — except that this plan's own *Rejected / out of scope* section says
a **third** hardcoded pass means stopping and building the frame graph first.
Bloom would be that third pass. So it is gated behind the frame graph, not behind
this phase.

---


---

---

## Rejected / out of scope

Recorded so future sessions do not rediscover them as good ideas.

- **Reviving the GPU compute baker** (`VoxelBaker.cs.hlsl`, `VoxelBakePass`,
  bindless `voxelModelData[]`, `VoxModel.cpp:531-546`). Half-finished, and
  orthogonal to fidelity. It would remove the CPU stamping cost, which is a real
  win — but it is its own project with its own plan.
- **Widening the chunk window** — see Phase 4.
- **Batching texture uploads** — already tried and reverted; see `CLAUDE.md`,
  it needs an unshared upload engine.
- **A frame graph.** `CLAUDE.md` correctly notes pass order is hardcoded by name
  lookup and barriers come from fixed per-pass rules. This plan adds two passes
  (Phase 3 prepass, and possibly a brick-validation compute pass) the same
  hardcoded way. If a third or fourth arrives, stop and build the frame graph
  first.
- **Full staging-buffer uploads for the voxel buffer.** The ReBAR memory-type
  preference in Phase 2 captures most of the win for a fraction of the work.
