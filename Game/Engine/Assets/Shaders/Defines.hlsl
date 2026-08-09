/* Global */
#define OPTIMIZED 1
// #define DIRECT_LIGHTING 2

/* --- Soft shadows (RENDERING_PLAN.md phase 6.2) ---------------------------
   How far the four cone rays of a quad spread, as a tangent: a sample at the
   pattern's full radius leaves the light direction by about this much, so the
   penumbra is roughly 2 * SHADOW_CONE_SPREAD * (distance to occluder) wide.

   0.05 was tried first and is too wide for this art. It is ~10x the sun's real
   angular radius, which puts a 4-voxel gradient on anything 40 voxels out - so
   a one-voxel post occludes a quarter of the cone and its shadow all but
   disappears. Reported as "thin shadows too weak". 0.02 is ~4x the real sun:
   still far softer than physical, and a post still reads.

   It is also the noise knob, which is not obvious. All the sampling variance
   lives in partially-occluded pixels, and the penumbra *is* the set of
   partially-occluded pixels - so halving the spread roughly halves the area
   that can be noisy at all. Widening this without also raising
   SHADOW_CONE_SAMPLES trades thin shadows and grain for softness. */
#define SHADOW_CONE_SPREAD 0.02

/* Shadow rays per pixel, all of them this pixel's own.

   Smoothness costs rays linearly here, which is the whole reason
   RENDERING_PLAN.md phase 7.1 exists - a cone trace through a filtered mip
   pyramid integrates the same coverage analytically, in one trace. Until then
   this is the brute-force version and the knob is honest about it: raise it
   for a smoother penumbra and a proportionally more expensive frame.

   Sharing rays across the 2x2 quad was tried and removed. It bought four times
   the directions for free, but averaging over a quad is a box filter on the
   shadow term, and thin occluders - railings, posts, beams - cast shadows only
   a pixel or two wide, which is exactly what a box filter erases. */
#define SHADOW_CONE_SAMPLES 4

/* Occupancy bricks (RENDERING_PLAN.md phase 2). One count of occupied voxels
   per BRICK_SIZE^3 block of the resident window, maintained CPU-side by
   VoxelBrickGrid; the marcher walks bricks and only descends into ones that
   hold something. BRICK_SHIFT is the C++ side's k_uiBrickShift - change both
   or neither. */
#define BRICK_SHIFT 2
#define BRICK_SIZE 4
#define BRICK_SIZE_F 4.0
#define BRICK_INV_SIZE 0.25

/* Coverage pyramid (RENDERING_PLAN.md 7.1b). The bricks above are one level of
   it: PYRAMID_LEVELS counts over the same window, each level's cell twice the
   edge of the one below, the finest at 1 << PYRAMID_FINE_SHIFT voxels. They
   share the brick buffer, laid end to end, and VoxelPyramid.hlsl derives where
   each one starts. These three are VoxelBrickGrid's k_uiFineShift,
   k_uiPyramidLevels and k_uiBrickLevel - change both sides or neither.

   A level finer than the brick is the whole point: a cone traced against the
   4-voxel bricks alone cannot serve ambient occlusion, which was established on
   screen rather than argued (see AO_CONE_ENABLED below). */
#define PYRAMID_FINE_SHIFT 1
#define PYRAMID_LEVELS 5
#define PYRAMID_BRICK_LEVEL (BRICK_SHIFT - PYRAMID_FINE_SHIFT)

/* Voxel crossings a ray can make inside one brick before it has to leave:
   3 axes x BRICK_SIZE cells. The inner walk also breaks the moment the ray
   leaves the brick, so this only bounds the pathological case. */
#define BRICK_MAX_VOXEL_STEPS 12

/* Safety ceiling on how much marching one pixel may do, counted in DDA
   crossings - brick-level and voxel-level alike - and shared by every ray the
   pixel fires (RENDERING_PLAN.md phase 6.0).

   Without it the worst case is the product of the two loop bounds above and
   the ray count: ~137 brick steps on a 768x128x768 window, 24 voxel steps in
   every occupied brick entered, twice over for the shadow ray. About 6,600
   voxel steps per pixel, at 2732x2048, with nothing capping the frame. That
   has reached the driver as `NVRM: Xid 109 CTX SWITCH TIMEOUT` - a draw it
   could not preempt - and the user as a window that stops updating.

   Note this is a *safety* bound, not the quality bound phase 2 deleted. That
   one was 700 voxel steps on the primary ray alone, and it cut every long
   sightline. This is a budget an ordinary pixel never approaches. Swept
   against the voxel pass's own GPU time at the phase 0 baseline vantage
   (game-release, Valley_Path_To_Castle_Beat1, camera still):

     budget    1e9    1024    512    256    192    128     64     32
     voxel ms  2.50   2.58    2.58   2.58   2.58   2.50   2.10   1.61

   The pass stops responding somewhere between 64 and 128, so by 128 there is
   effectively no pixel left to clip, and 1024 is eight times that. What it
   does bound is the ceiling: the marginal cost measured between 32 and 64 is
   ~0.015 ms per step of the average, which puts a whole screen of pixels all
   spending the full 1024 at roughly 16 ms of voxel pass - a bad frame, but
   three orders of magnitude away from a frame the driver kills.

   Running out of it ends the march exactly as running out of brick steps
   already does - alpha 0, which the caller reads as a miss. */
/* Scale this with SHADOW_CONE_SAMPLES. The budget is shared by every ray a
   pixel fires, and **running out reports lit** - so a budget set too low does
   not degrade gracefully, it puts bright speckle exactly where geometry is
   densest and the rays are longest. That reads as noise and is easy to blame
   on the sampling instead.

   Sizing, at BRICK_SHIFT 2 on a 768x128x768 window: a shadow ray climbing out
   of the window is ~43 brick steps plus descents, call it ~100, so 8 of them
   plus a primary ray is ~1400 for a typical pixel - but the *worst* case is
   iMaxBrickSteps (273) x BRICK_MAX_VOXEL_STEPS (12) = ~3300 per ray. 4096 sat
   between the two, which is the wrong place for a safety net to sit: never
   binding on an ordinary pixel, frequently binding on a dense one. */
#define MARCH_STEP_BUDGET 16384

/* Uncomment to shade the sun with GetSunVisibilityByRays - the brute-force
   cone of exact shadow rays - instead of sampling the shadow map. Ground truth
   to compare the map against: it is correct by construction and far too slow
   to ship. Differences are the map's, not the light's. */
// #define SUN_SHADOW_REFERENCE

/* --- Sun shadow map (RENDERING_PLAN.md 7.1a) ------------------------------
   Side of the square light-space depth map. Must equal
   RenderContext::k_uiSunShadowResolution - change both or neither; they are
   the SPIR-V/C++ contract for this pass exactly as BRICK_SHIFT is for the
   brick grid. */
#define SUN_SHADOW_RESOLUTION 1024.0

/* Written for a light-space column that hits nothing. Has to exceed any depth
   a receiver can compute for itself, including one pushed a little past the
   window's far plane by rounding. */
#define SUN_SHADOW_FAR 1.0e9

/* Depth slack, in world units, between a receiver and the blocker recorded for
   its own texel. A texel covers up to a couple of voxels of light-space
   footprint, so a lit surface's own depth can differ from what its texel
   recorded by about that much; without the slack every lit surface shadows
   itself in stripes. Voxel geometry is axis aligned, which is why a constant
   works here where a sloped-surface renderer would need a slope-scaled one. */
#define SUN_SHADOW_BIAS 1.5

/* World units the lookup pushes its sample point out along the surface normal
   before projecting it into light space. Voxel faces are axis aligned so one
   voxel is exact; raise it if acne survives, lower it if shadows visibly
   detach from the geometry casting them. */
#define SUN_SHADOW_NORMAL_OFFSET 1.0

/* How much the bias and the normal offset grow as a surface turns edge-on to
   the light, as a multiplier on (1 - N.L).

   A flat bias is only ever correct for a surface facing the light. At a grazing
   angle one shadow texel spans a long run of the receiver, so the depth
   recorded for the texel differs from this pixel's own depth by far more than a
   constant can absorb, and the surface stripes itself in shadow. A wall lit
   near edge-on is the worst case and is extremely common here - the light sits
   at ~48 degrees elevation, so every vertical face is close to grazing. */
#define SUN_SHADOW_GRAZING_SCALE 6.0

/* Tangent of the sun's angular radius, as SHADOW_CONE_SPREAD was. The PCSS
   filter width is this times the distance between receiver and blocker, so the
   penumbra widens with occluder distance without costing any rays - which is
   the property phase 6.2 had to buy with sample count. */
#define SUN_ANGULAR_RADIUS 0.03

/* Taps for the blocker search and for the filter itself. Both are 2D texture
   samples of a map that is at most 4 MiB, so they are nothing like the cost of
   the rays they replace. */
#define SUN_SHADOW_BLOCKER_TAPS 16
#define SUN_SHADOW_FILTER_TAPS 24

/* Largest filter radius, in shadow-map texels. Caps how far the penumbra can
   spread when a blocker is very distant, which is both a quality choice and
   what stops the tap pattern from undersampling into noise. */
#define SUN_SHADOW_MAX_RADIUS 12.0

/* --- Cone-traced ambient occlusion (RENDERING_PLAN.md 7.1b) ---------------
   Five cones over the hemisphere - one on the normal, four on a ring - marched
   against the coverage pyramid, each step sampling the level whose cell is
   about as wide as the cone is there.

   The first cut marched the bricks alone and it failed in two ways that
   bracket the problem with no setting in between: started nearer than ~8
   voxels a ring cone samples the 4-voxel bricks holding the wall it began on
   and the surface occludes itself; started at 8, nothing measures occlusion
   between one voxel and eight, which is the scale of the gaps between stones
   in a wall. Both were seen on screen, both are the data rather than the
   trace, and the pyramid's finer levels are the answer to both. Against the
   pyramid the image is right: no self-occlusion wedges, no lattice, and a wall
   that reads as stone with gaps in it.

   OFF for cost, which is a different reason from the first cut's. The voxel
   pass, headless, same vantage:

                      1920x1080   3840x2160
     off                0.745       2.426
     one fetch a step   1.051       3.565
     trilinear          2.320       7.666

   The filter is 4.6x the whole rest of the cone, because eight fetches by hand
   is what a storage buffer costs where a 3D texture would charge one sample -
   and +5.24 ms at 4K is the entire lighting budget several times over. That is
   the measurement 7.1b said would decide route B, and it decides it. */
#define AO_CONE_ENABLED 0

/* Trilinear filtering of a pyramid sample, which costs eight fetches by hand
   because the levels live in a storage buffer rather than a 3D texture. Set to
   0 to price a step at one fetch - the cost a hardware SampleLevel would have
   - which is the measurement 7.1b route B turns on. Not a quality setting: at
   0 the lattice artefact the pyramid exists to remove comes back. */
#define AO_CONE_TRILINEAR 1

/* Half-angle of a cone as a radius-over-distance ratio. Five cones covering a
   hemisphere is ~45 degrees each; a little under that keeps them from
   overlapping into a term that saturates on open ground. This is what picks
   the level per step, so it is the knob that decides how coarse the data a
   given step reads is. */
#define AO_CONE_APERTURE 0.55

/* Cones in the ring around the normal, plus one along it. Five total. */
#define AO_RING_CONES 4

/* The ring's tilt from the normal, as a cosine/sine pair. ~52 degrees puts the
   ring near the centroid of the cosine-weighted hemisphere, so five cones cover
   it about as evenly as five can. */
#define AO_RING_COSINE 0.615
#define AO_RING_SINE 0.788

/* Samples per cone, and how fast the step grows. Geometric, so seven steps
   from AO_CONE_START reach ~66 voxels - far enough to feel a building, which
   the twelve-tap neighbour test never could.

   AO_CONE_START is two voxels now rather than eight, and that is the whole
   point of having built the pyramid. Eight was forced by the brick: a ring cone
   leaves at ~52 degrees, so a sample taken nearer than that still landed in a
   4-voxel brick containing the wall the cone started on. The finest level is
   two voxels, so a sample two voxels out is already reading somewhere else -
   and the 1-to-8 voxel gap that made walls read flat closes. */
#define AO_CONE_STEPS 7
#define AO_CONE_START 2.0
#define AO_CONE_GROWTH 1.7

/* How much a fully solid cell occludes per step. Under 1 because a cone step
   samples one point of a footprint that is mostly not that point; tune by eye
   against how heavy enclosed areas look. */
#define AO_CONE_STRENGTH 0.35

/* Where a cone starts, along the surface normal. Enough to leave the finest
   cell the surface is itself part of - which is two voxels, the same as
   AO_CONE_START, where under the bricks these had to be different numbers.

   It was a full brick (4.0) once and that was actively wrong on detailed
   geometry: on a stepped wall, four voxels along a recessed face's normal
   lands *inside* the ledge above it, so the cone began in solid rock and the
   surface came out black. The artefact followed the steps, which is what
   "full of black triangles" was. */
#define AO_SURFACE_OFFSET 2.0

/* Sky colour for rays that leave the world without hitting anything or the
   ground plane. Becomes fog input in RENDERING_PLAN.md phase 6.1. */
#define SKY_COLOR float4(150.0 / 255.0, 230.0 / 255.0, 255.0 / 255.0, 1.0)

/* How far inside the resident window an entry point computed on one of its
   faces is nudged, so that floor() cannot name the cell on the far side. */
#define WORLD_ENTRY_EPSILON 0.001

/* Far-field LOD volume (RENDERING_PLAN.md phase 4). The whole level at
   FARFIELD_SIZE voxels per cell, marched where the 3x3 detail window does not
   reach. FARFIELD_SHIFT is FarFieldVolume::k_uiShift on the C++ side - change
   both or neither.

   The far field reuses BRICK_SHIFT for its own occupancy bricks, over its cell
   grid rather than the window's voxels, so one cell-space brick spans
   BRICK_SIZE * FARFIELD_SIZE = 32 world units. */
#define FARFIELD_SHIFT 2
#define FARFIELD_SIZE 4
#define FARFIELD_SIZE_F 4.0
#define FARFIELD_INV_SIZE 0.25

/* Cell crossings a ray can make inside one far-field brick: 3 axes x
   BRICK_SIZE cells, as BRICK_MAX_VOXEL_STEPS is for the window. */
#define FARFIELD_MAX_CELL_STEPS 24

/* Pushed off the window's face before the far-field march starts, in world
   units, so that a ray leaving the detail window cannot re-enter the cell it
   just left through rounding. One far-field cell. */
#define FARFIELD_ENTRY_EPSILON 4.0

/* Height of the endless ground plane GetBackground draws, which has to be the
   *top* of the chunk ground plane rather than its base: Chunk::UpdateGroundPlane
   fills the voxel layer at integer y = 0, and a voxel at integer y occupies
   [y, y+1]. */
#define GROUND_PLANE_HEIGHT 1.0

#define DEG2RAD 0.0174532925
#define RAD2DEG 57.2957795

/* Platform specific */
#ifdef __PSSL__

#define FORCE_DEPTH_TEST [FORCE_EARLY_DEPTH_STENCIL]

#define STRUCTURED_BUFFER(x) RegularBuffer<x>
#define RW_STRUCTURED_BUFFER(x) RW_RegularBuffer<x>
#define BUFFER(x) RegularBuffer<x>
#define RW_BUFFER(x) RW_RegularBuffer<x>

#define VOXEL_FORMAT uint

#define VOXEL_BUFFER BUFFER(VOXEL_FORMAT)
#define VOXEL_RW_BUFFER RW_BUFFER(VOXEL_FORMAT)
#define VOXEL_BUFFER_LOCAL BUFFER(VOXEL_FORMAT)

#define EMPTY_VOXEL 0

#define CONSTANT_BUFFER ConstantBuffer

#define VERT_ID S_VERTEX_ID
#define INST_ID S_INSTANCE_ID
#define POS_OUT S_POSITION

#define TAR_OUT S_TARGET_OUTPUT
#define TAR_OUT0 S_TARGET_OUTPUT0
#define TAR_OUT1 S_TARGET_OUTPUT1
#define TAR_OUT2 S_TARGET_OUTPUT2
#define TAR_OUT3 S_TARGET_OUTPUT3
#define TAR_OUT4 S_TARGET_OUTPUT4
#define TAR_OUT5 S_TARGET_OUTPUT5
#define TAR_OUT6 S_TARGET_OUTPUT6
#define TAR_OUT7 S_TARGET_OUTPUT7
#define TAR_OUT8 S_TARGET_OUTPUT8

#else

#define FORCE_DEPTH_TEST [earlydepthstencil]

#define STRUCTURED_BUFFER(x) StructuredBuffer<x>
#define RW_STRUCTURED_BUFFER(x) RWStructuredBuffer<x>
#define BUFFER(x) Buffer<x>
#define RW_BUFFER(x) RWBuffer<x>

#define VOXEL_FORMAT float4

/* The voxel buffers are float4 in the shader but the engine stores one 32-bit
   RGBA8 texel per voxel and relies on the view converting on read. D3D12 took
   that from the UAV format; SPIR-V carries the format on the declaration, and
   without this DXC assumes Rgba32f and the view no longer matches. */
#ifdef __spirv__
#define VOXEL_IMAGE_FORMAT [[vk::image_format("rgba8")]]
#else
#define VOXEL_IMAGE_FORMAT
#endif

#define VOXEL_BUFFER VOXEL_IMAGE_FORMAT BUFFER(VOXEL_FORMAT)
#define VOXEL_RW_BUFFER VOXEL_IMAGE_FORMAT RW_BUFFER(VOXEL_FORMAT)

/* Same type without the annotation, for local aliases - the attribute is only
   valid on a resource declaration. */
#define VOXEL_BUFFER_LOCAL BUFFER(VOXEL_FORMAT)

#define EMPTY_VOXEL float4(0.0, 0.0, 0.0, 0.0)

#define CONSTANT_BUFFER cbuffer

#define VERT_ID SV_VERTEXID
#define INST_ID SV_INSTANCEID
#define POS_OUT SV_POSITION

#define TAR_OUT SV_TARGET
#define TAR_OUT0 SV_TARGET0
#define TAR_OUT1 SV_TARGET1
#define TAR_OUT2 SV_TARGET2
#define TAR_OUT3 SV_TARGET3
#define TAR_OUT4 SV_TARGET4
#define TAR_OUT5 SV_TARGET5
#define TAR_OUT6 SV_TARGET6
#define TAR_OUT7 SV_TARGET7
#define TAR_OUT8 SV_TARGET8

#endif