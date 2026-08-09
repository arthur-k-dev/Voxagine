#include "Defines.hlsl"
#include "CameraData.hlsl" // register(b0)

SamplerState s0 : register(s0);

VOXEL_RW_BUFFER voxelWorldData : register(u0);

/* Occupancy counts per BRICK_SIZE^3 block - see SDFMarcher.hlsl. */
RW_STRUCTURED_BUFFER(uint) voxelBrickData : register(u1);

Texture2D<float4> particlePass : register(t1);

/* t2 is the particle depth target, which this variant does not read; the pass
   binds it either way, so the register is spoken for and t3 stays free. */

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
#include "Lighting.hlsl"

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

    /* Same sun-plus-sky model as VoxelRenderer.ps.hlsl (rule 8: non-shadow
       changes land in both), with the sun unoccluded because this variant
       casts no shadow ray at all. */
    float difference = clamp(dot(marchDiffuse.Normal, -lightDirection.xyz), 0.0, 1.0);

    float skyVisibility = GetSkyVisibility(marchDiffuse.Position, marchDiffuse.Mask, marchDiffuse.SRDirection, marchDiffuse.Normal, marchDiffuse.UV);

#if AO_CONE_ENABLED
    skyVisibility *= GetConeSkyVisibility(marchDiffuse.SmoothPosition, marchDiffuse.Normal, IN.NormScreenPosition.xy);
#endif

    marchDiffuse.Color.xyz = ShadeSurface(marchDiffuse.Color.xyz, marchDiffuse.Normal, 1.0, skyVisibility);

    /* Fake specular "shine line" on lit voxel edges - see GetShineLine in
       AmbientOcclusion.hlsl. */
    marchDiffuse.Color.xyz *= GetShineLine(marchDiffuse.Position, marchDiffuse.Normal, marchDiffuse.UV, lightDirection.xyz, difference);
    marchDiffuse.Color.a = 1.0;

#ifdef MARCH_STEP_DEBUG
    return GetStepHeatmap();
#endif

    return marchDiffuse.Color;
}