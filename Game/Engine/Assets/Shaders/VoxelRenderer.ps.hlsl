#include "Defines.hlsl"
#include "CameraData.hlsl" // register(b0)

SamplerState s0 : register(s0);

/* Linear, point across mips, black outside - the coverage pyramid's sampler.
   See RenderContext's "Coverage pyramid texture" block for why each of the
   three matters. */
SamplerState pyramidSampler : register(s1);

VOXEL_RW_BUFFER voxelWorldData : register(u0);

/* Occupancy counts per BRICK_SIZE^3 block - see SDFMarcher.hlsl. Declared
   read-write because the pass binds it as a read-write mapper, which is what
   keeps it out of the t register range the textures below already use. */
RW_STRUCTURED_BUFFER(uint) voxelBrickData : register(u1);

Texture2D<float4> particlePass : register(t1);
Texture2D<float> particleDepthPass : register(t2);

/* Dynamic model pass output - DYNAMIC_MODELS_PLAN.md phase 2, VoxelModelPass.
   Same composited-by-depth idea as the particle pair above, t3/t4, pushed
   unconditionally by VoxelPass so every register after this shifted by two
   from what it was before this pass existed. */
Texture2D<float4> modelPass : register(t3);
Texture2D<float> modelDepthPass : register(t4);

/* Light-space sun shadow map - RENDERING_PLAN.md 7.1a. Pushed after the model
   pair, so t5; the bindless model array below stays at t6. */
Texture2D<float> sunShadowMap : register(t5);

/* Coverage and radiance pyramid - RENDERING_PLAN.md 7.1b route B and 7.3. Mip
   L is pyramid level L over the resident window: alpha is the occupied fraction
   of a cell and RGB its linear albedo premultiplied by that fraction, so one
   SampleLevel is a cone step for both the occlusion and the bounce, filter
   included. Pushed after the shadow map, so t6, and the bindless model array
   moves up to t7. */
Texture3D<float4> voxelPyramid : register(t6);

/* The unbounded `VOXEL_BUFFER voxelModelData[] : register(tN)` that used to be
   here is gone. It was never read - it belonged to a GPU-baker path that was
   abandoned - but it compiled into the live SPIR-V as an unbounded typed-buffer
   descriptor array, which is exactly the shape mobile drivers and MoltenVK are
   pickiest about, and it would have been bound to nothing. Deleting dead code
   beats working around a descriptor-indexing limitation for it.
   MOBILE_PORT_PLAN.md phase 4, step 3. */

struct PS_in
{
    float4 NormScreenPosition	: POS_OUT;
	float4 Direction			: POSITION0;
    float3 WorldPosition		: POSITION1;
};

/* AmbientCone.hlsl last, and the two above it are what it needs: the bounce it
   gathers is lit by the sun (SunShadowLookup) with the same constants the direct
   term uses (Lighting). This variant has the shadow map, so its cone samples are
   shadowed; the ShadowLess one does not define this and lights them fully. */
#define AO_CONE_HAS_SUN_SHADOW 1

#include "SDFMarcher.hlsl"
#include "AmbientOcclusion.hlsl"
#include "SunShadowLookup.hlsl"
#include "Lighting.hlsl"
#include "AmbientCone.hlsl"
#include "Fog.hlsl"
#include "ShadeVoxelSurface.hlsl"

FORCE_DEPTH_TEST
float4 main(PS_in IN) : TAR_OUT
{
    /* Check particle color */
    float2 particleUV = IN.NormScreenPosition.xy / (viewport.xy * voxelRenderScale);
    float4 particleColor = particlePass.Sample(s0, particleUV);
    float particleDepth = particleDepthPass.Sample(s0, particleUV);

    /* Dynamic model pass output - DYNAMIC_MODELS_PLAN.md phase 2. Same UV as
       the particle pair: VoxelModelPass follows resolution scale identically
       (VoxelModelPass.cpp), so the footprint the two were rendered at agrees. */
    float4 modelColor = modelPass.Sample(s0, particleUV);
    float modelDepth = modelDepthPass.Sample(s0, particleUV);

    /* March diffuse color */
    float3 rayOrigin = IN.WorldPosition - camOffset.xyz;
	float3 rayDirection = normalize(IN.Direction.xyz);

    /* Budget the walk by the window's diagonal in bricks, so nothing distant
       is truncated. This replaces the hardcoded 700-voxel cap the scale-up
       introduced - on a 768x128x768 window the diagonal is ~1094 voxels, so
       700 cut every long sightline short. The brick walk is what makes the
       full diagonal affordable (RENDERING_PLAN.md phase 2). */
    int maxBrickSteps = int(length(float3(worldSize.xyz)) * BRICK_INV_SIZE) + 2;

    int primaryBrickSteps = maxBrickSteps;

    MarchResult marchDiffuse = MarchDiffuse
    (
		rayOrigin,
		rayDirection,
		primaryBrickSteps
	);

    /* Nearest of {model, particle, world hit} wins - DYNAMIC_MODELS_PLAN.md
       piece 3. bWorldHit gates worldDistance out of every comparison on a
       miss, structurally rather than by relying on its value - the original
       particle-only check compared against worldDistance unconditionally,
       trusting a MarchResult on the miss path whose SmoothPosition nothing
       sets. That is a latent quirk of the code being extended, not something
       this change is here to fix, but a three-way version cannot silently
       inherit "depends on uninitialized memory" without saying so: gating on
       bWorldHit is the minimal, deliberate correction, not a rewrite of the
       miss path's behaviour. */
    float worldDistance = distance(marchDiffuse.SmoothPosition + camOffset.xyz, camPosition.xyz);
    bool bWorldHit = marchDiffuse.Color.a != 0.0;

    bool bHasModel = modelColor.a != 0.0;
    bool bHasParticle = particleColor.a != 0.0;

    bool bModelWins = bHasModel && (!bWorldHit || modelDepth < worldDistance) && (!bHasParticle || modelDepth <= particleDepth);
    bool bParticleWins = !bModelWins && bHasParticle && (!bWorldHit || particleDepth < worldDistance);

    if (bModelWins)
        return modelColor;

    if (bParticleWins)
    {
        return particleColor;
    }

    /* Return transparent when not marched against anything. Sky and endless
       ground are composited in PostProcessing.ps.hlsl instead of here - this
       pass only rasterizes AABB proxy cubes (see RENDERING_PLAN.md phase 1
       notes), so it never covers the whole screen the way a full-screen sky
       needs. */
    if (marchDiffuse.Color.a == 0.0)
    {
#ifdef MARCH_STEP_DEBUG
        return GetStepHeatmap();
#endif
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    /* Emissive voxels are light sources, not surfaces - RENDERING_PLAN.md 7.4.
       Everything between here and ShadeSurface below measures how much light
       *arrives* at a surface, and none of it applies to light that is leaving:
       a lantern is not dimmer in shadow, and ambient occlusion has nothing to
       occlude. Placed above the shadow-map lookup and the cones so an emissive
       voxel does not pay for either.

       It still fogs. A glowing thing far enough away is still seen through the
       same air as everything else, and skipping that would make emissives the
       one class of geometry that refuses to recede. */
    if (IsEmissiveVoxel(marchDiffuse.Color.a))
    {
        float3 v3Emitted = SrgbToLinear(marchDiffuse.Color.xyz) * EMISSIVE_GAIN;

        v3Emitted = ApplyAerialPerspective(
            v3Emitted, distance(marchDiffuse.SmoothPosition + camOffset.xyz, camPosition.xyz));

        return float4(EncodeSceneColor(v3Emitted), 1.0);
    }

    /* Sun visibility. The shadow map is a fixed cost whatever the screen
       resolution is, and PCSS on lookup gives a penumbra that widens with
       distance to the occluder without firing a single ray - see
       SunShadowLookup.hlsl. SUN_SHADOW_REFERENCE swaps in the exact cone of
       rays it replaced, which is correct and unaffordable. */
    float difference = clamp(dot(marchDiffuse.Normal, -lightDirection.xyz), 0.0, 1.0);

    float sunVisibility = 1.0;

    if (difference > 0.1)
	{
#ifdef SUN_SHADOW_REFERENCE
        sunVisibility = GetSunVisibilityByRays(
            marchDiffuse.SmoothPosition - lightDirection.xyz,
            IN.NormScreenPosition.xy,
            maxBrickSteps
        );

        /* Keeps t3 alive. This path never reads the shadow map, DXC strips an
           unused texture out of the SPIR-V, and VoxelPass still pushes it - so
           the pass binds a descriptor the reflected layout no longer has and
           the run dies at startup. Multiplying by zero is the cheapest way to
           make the reference switch a *shader* change rather than one that has
           to be matched on the C++ side. */
        sunVisibility += sunShadowMap.SampleLevel(s0, float2(0.5, 0.5), 0) * 0.0;
#else
        /* SHQ_RAY - Settings.h. One exact occlusion test from the surface toward
           the sun, through the same brick DDA the primary ray just used, instead
           of a map: no resolution, no depth bias, no acne, and a silhouette that
           is right to the voxel.

           It is here rather than in SunShadowLookup.hlsl because the step budget
           the march needs is a property of this pixel and is in scope here.

           The origin is pushed one unit along the light exactly as
           GetSunVisibilityByRays does - MarchShadow also treats an occupied
           origin voxel as lit, which is what keeps a wall from shadowing
           itself. */
        if (GetShadowQuality() == QUALITY_SHADOW_RAY)
        {
            /* Settings::ShadowRayDistance, converted from world units into the
               brick steps MarchShadow counts in. A ray that runs out reports
               lit, which is what makes the limit a *distance* rather than a
               truncation artefact: past it, nothing casts.

               The +2 covers the partial bricks at each end - without it a limit
               that is an exact multiple of the brick size stops one brick short
               of what the player asked for. Zero means unbounded, and then this
               is the window's own diagonal as before. */
            int shadowBrickSteps = maxBrickSteps;

            if (SHADOW_RAY_DISTANCE > 0.0)
            {
                shadowBrickSteps = min(maxBrickSteps,
                    int(SHADOW_RAY_DISTANCE * BRICK_INV_SIZE) + 2);
            }

            sunVisibility = MarchShadow(
                marchDiffuse.SmoothPosition - lightDirection.xyz,
                -lightDirection.xyz,
                shadowBrickSteps
            );
        }
        else
        {
            sunVisibility = GetSunVisibility(
                marchDiffuse.SmoothPosition,
                marchDiffuse.Normal,
                IN.NormScreenPosition.xy
            );
        }
#endif
    }

    /* Sky visibility, which is what ambient occlusion actually measures, in the
       three tiers Settings::AmbientQuality names.

       AMQ_SIMPLE is GetSkyVisibility alone: the twelve-neighbour test, hit-time
       only, no added per-step cost, and it sees exactly one voxel out. AMQ_CONE
       multiplies it by the cone trace through the radiance pyramid, which is
       what knows about a wall two metres away. AMQ_OFF leaves the term at 1 and
       the sky lights everything equally.

       The cones *multiply* rather than replace, which is why the two tiers are
       nested rather than exclusive - see AmbientCone.hlsl's header. */
    float skyVisibility = 1.0;

    if (GetAmbientQuality() >= QUALITY_AMBIENT_SIMPLE)
        skyVisibility = GetSkyVisibility(marchDiffuse.Position, marchDiffuse.Mask, marchDiffuse.SRDirection, marchDiffuse.Normal, marchDiffuse.UV);

    /* Bounce light from the same cones - RENDERING_PLAN.md 7.3. */
    float3 bounce = 0.0;

    if (AO_CONE_ENABLED)
        skyVisibility *= GetConeAmbient(marchDiffuse.SmoothPosition, marchDiffuse.Normal, IN.NormScreenPosition.xy, bounce);

    /* Linear radiance until ShadeVoxelHit's EncodeSceneColor at the end -
       RENDERING_PLAN.md phase 7.2, and Color.hlsl for why the encode happens
       here rather than at present. The tail from here on is shared with
       VoxelRenderer.ShadowLess.ps.hlsl and the dynamic model pass
       (DYNAMIC_MODELS_PLAN.md phase 2, ShadeVoxelSurface.hlsl) - only
       sunVisibility and skyVisibility above it differ per caller. */
    float fRawRimBoost = GetShineLine(marchDiffuse.Position, marchDiffuse.Normal, marchDiffuse.UV, lightDirection.xyz, difference);

    marchDiffuse.Color.xyz = ShadeVoxelHit(
        marchDiffuse.Color.xyz, marchDiffuse.Normal, marchDiffuse.SmoothPosition,
        rayDirection, sunVisibility, skyVisibility, bounce, fRawRimBoost);
    marchDiffuse.Color.a = 1.0;

#ifdef MARCH_STEP_DEBUG
    /* After the shadow ray, so the heatmap shows the pixel's whole cost - the
       shadow ray is 53% of this pass (RENDERING_PLAN.md phase 3 notes). */
    return GetStepHeatmap();
#endif

    return marchDiffuse.Color;
}