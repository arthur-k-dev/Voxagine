#include "Defines.hlsl"
#include "CameraData.hlsl" // register(b0)

SamplerState s0 : register(s0);

VOXEL_RW_BUFFER voxelWorldData : register(u0);

/* Occupancy counts per BRICK_SIZE^3 block - see SDFMarcher.hlsl. Declared
   read-write because the pass binds it as a read-write mapper, which is what
   keeps it out of the t register range the textures below already use. */
RW_STRUCTURED_BUFFER(uint) voxelBrickData : register(u1);

Texture2D<float4> particlePass : register(t1);
Texture2D<float> particleDepthPass : register(t2);

/* Light-space sun shadow map - RENDERING_PLAN.md 7.1a. Third texture pushed by
   VoxelPass, so t3; the bindless model array below stays at t4. */
Texture2D<float> sunShadowMap : register(t3);

VOXEL_BUFFER voxelModelData[] : register(t4);

struct PS_in
{
    float4 NormScreenPosition	: POS_OUT;
	float4 Direction			: POSITION0;
    float3 WorldPosition		: POSITION1;
};

#include "SDFMarcher.hlsl"
#include "AmbientOcclusion.hlsl"
#include "AmbientCone.hlsl"
#include "SunShadowLookup.hlsl"
#include "Lighting.hlsl"

FORCE_DEPTH_TEST
float4 main(PS_in IN) : TAR_OUT
{
    /* Check particle color */
    float2 particleUV = IN.NormScreenPosition.xy / (viewport.xy * voxelRenderScale);
    float4 particleColor = particlePass.Sample(s0, particleUV);
    float particleDepth = particleDepthPass.Sample(s0, particleUV);

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

    if (particleDepth < distance(marchDiffuse.SmoothPosition + camOffset.xyz, camPosition.xyz) && particleColor.a != 0.0)
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
        sunVisibility = GetSunVisibility(
            marchDiffuse.SmoothPosition,
            marchDiffuse.Normal,
            IN.NormScreenPosition.xy
        );
#endif
    }

    /* Sky visibility, which is what ambient occlusion actually measures.
       Hit-time only, zero added per-step cost. Phase 7.1 replaces this with a
       cone trace through the radiance pyramid. */
    float skyVisibility = GetSkyVisibility(marchDiffuse.Position, marchDiffuse.Mask, marchDiffuse.SRDirection, marchDiffuse.Normal, marchDiffuse.UV);

#if AO_CONE_ENABLED
    skyVisibility *= GetConeSkyVisibility(marchDiffuse.SmoothPosition, marchDiffuse.Normal, IN.NormScreenPosition.xy);
#endif

    marchDiffuse.Color.xyz = ShadeSurface(marchDiffuse.Color.xyz, marchDiffuse.Normal, sunVisibility, skyVisibility);

    /* Fake specular "shine line" on lit voxel edges - see GetShineLine in
       AmbientOcclusion.hlsl. It returns 1.0 for anything the sun does not
       reach, so applying it to the whole shaded result rather than to the sun
       term alone only ever brightens a rim that is already lit. */
    marchDiffuse.Color.xyz *= GetShineLine(marchDiffuse.Position, marchDiffuse.Normal, marchDiffuse.UV, lightDirection.xyz, difference);
    marchDiffuse.Color.a = 1.0;

#ifdef MARCH_STEP_DEBUG
    /* After the shadow ray, so the heatmap shows the pixel's whole cost - the
       shadow ray is 53% of this pass (RENDERING_PLAN.md phase 3 notes). */
    return GetStepHeatmap();
#endif

    return marchDiffuse.Color;
}