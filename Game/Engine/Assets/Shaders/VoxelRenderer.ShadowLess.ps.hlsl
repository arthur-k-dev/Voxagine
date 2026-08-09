#include "Defines.hlsl"
#include "CameraData.hlsl" // register(b0)

SamplerState s0 : register(s0);

/* See VoxelRenderer.ps.hlsl. */
SamplerState pyramidSampler : register(s1);

VOXEL_RW_BUFFER voxelWorldData : register(u0);

/* Occupancy counts per BRICK_SIZE^3 block - see SDFMarcher.hlsl. */
RW_STRUCTURED_BUFFER(uint) voxelBrickData : register(u1);

Texture2D<float4> particlePass : register(t1);

/* t2 is the particle depth target, which this variant does not read; the pass
   binds it either way, so the register is spoken for. */

/* Coverage and radiance pyramid - RENDERING_PLAN.md 7.1b route B and 7.3, and
   t3 rather than t4 because this variant has no shadow map in front of it. */
Texture3D<float4> voxelPyramid : register(t3);

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

/* AmbientCone.hlsl after Lighting.hlsl, which supplies the constants its bounce
   term is lit by. AO_CONE_HAS_SUN_SHADOW is left undefined: there is no map
   here, so a cone sample is lit by an unshadowed sun - the same assumption this
   variant already makes about the surface it is shading. */
#include "SDFMarcher.hlsl"
#include "AmbientOcclusion.hlsl"
#include "Lighting.hlsl"
#include "AmbientCone.hlsl"
#include "Fog.hlsl"

FORCE_DEPTH_TEST
float4 main(PS_in IN) : TAR_OUT
{
    /* Check particle color */
    float4 particleColor = particlePass.Sample(s0, IN.NormScreenPosition.xy / (viewport.xy * voxelRenderScale));

    if (particleColor.a != 0.0)
        return particleColor;

    /* March diffuse color */
	float3 rayOrigin = IN.WorldPosition - camOffset.xyz;
	float3 rayDirection = normalize(IN.Direction.xyz);

    /* The window diagonal in bricks - see VoxelRenderer.ps.hlsl for why the
       hardcoded 700 is gone. */
    int primaryBrickSteps = int(length(float3(worldSize.xyz)) * BRICK_INV_SIZE) + 2;

    MarchResult marchDiffuse = MarchDiffuse
    (
		rayOrigin,
		rayDirection,
		primaryBrickSteps
	);
	
    /* Return transparent when not marched against anything. Sky and endless
       ground are composited in PostProcessing.ps.hlsl instead of here - see
       VoxelRenderer.ps.hlsl. */
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

    /* Same sun-plus-sky model as VoxelRenderer.ps.hlsl (rule 8: non-shadow
       changes land in both), with the sun unoccluded because this variant
       casts no shadow ray at all. */
    float difference = clamp(dot(marchDiffuse.Normal, -lightDirection.xyz), 0.0, 1.0);

    float skyVisibility = GetSkyVisibility(marchDiffuse.Position, marchDiffuse.Mask, marchDiffuse.SRDirection, marchDiffuse.Normal, marchDiffuse.UV);

    /* Bounce light from the same cones - RENDERING_PLAN.md 7.3. */
    float3 bounce = 0.0;

#if AO_CONE_ENABLED
    skyVisibility *= GetConeAmbient(marchDiffuse.SmoothPosition, marchDiffuse.Normal, IN.NormScreenPosition.xy, bounce);
#endif

    /* Linear radiance until EncodeSceneColor - RENDERING_PLAN.md phase 7.2. */
    float3 v3Radiance = ShadeSurface(marchDiffuse.Color.xyz, marchDiffuse.Normal, 1.0, skyVisibility, bounce);

    /* Environment specular - RENDERING_PLAN.md 7.4. Added rather than folded
       into ShadeSurface's parentheses: it is light reflected *off* this
       surface, so the albedo does not multiply it. */
    v3Radiance += GetConeSpecular(marchDiffuse.SmoothPosition, marchDiffuse.Normal, rayDirection);

    /* Fake specular "shine line" on lit voxel edges - see GetShineLine in
       AmbientOcclusion.hlsl. */
    v3Radiance *= GammaGainToLinear(GetShineLine(marchDiffuse.Position, marchDiffuse.Normal, marchDiffuse.UV, lightDirection.xyz, difference));

    /* Aerial perspective - RENDERING_PLAN.md 6.1. From the *camera*, not
       marchDiffuse.Distance: this pass rasterizes AABB proxies and the ray
       started where its own box was entered, so the marcher's distance is not
       the one fog is a function of. */
    v3Radiance = ApplyAerialPerspective(v3Radiance, distance(marchDiffuse.SmoothPosition + camOffset.xyz, camPosition.xyz));

    marchDiffuse.Color.xyz = EncodeSceneColor(v3Radiance);
    marchDiffuse.Color.a = 1.0;

#ifdef MARCH_STEP_DEBUG
    return GetStepHeatmap();
#endif

    return marchDiffuse.Color;
}