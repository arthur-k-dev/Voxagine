#include "Defines.hlsl"
#include "CameraData.hlsl" // register(b0)

/* See VoxelModel.ps.hlsl. Selected instead of it when Settings::IsShadowEnabled()
 * is false, same rule 8 pattern VoxelRenderer.ps.hlsl/VoxelRenderer.ShadowLess.ps.hlsl
 * already use: SunShadowCombinePass does not exist in that mode (RenderContext.cpp),
 * so this variant declares no sunShadowMap and assumes full, unoccluded sun -
 * exactly what VoxelRenderer.ShadowLess.ps.hlsl already assumes for the world.
 *
 * Registers are this file's contract with VoxelModelPass.cpp - see rule 1,
 * RENDERING_PLAN.md. s0 is the pyramid's own sampler; this variant never
 * samples anything else, so unlike the shadowed one it does not also need a
 * point sampler for a shadow-map lookup. */
SamplerState pyramidSampler : register(s0);
Texture3D<float4> voxelPyramid : register(t3);

struct PS_in
{
	float4 NormScreenPosition	: POS_OUT;
	float3 WorldPosition		: POSITION0;
	float3 Normal				: POSITION1;
	float SkyVisibility			: POSITION2;
	nointerpolation uint PackedColour : COLOR0;
};

struct PS_out
{
	float4 Color	: TAR_OUT0;
	float Depth		: TAR_OUT1;
};

#include "VectorMath.hlsl"
#include "Lighting.hlsl"
#include "AmbientCone.hlsl"
#include "Fog.hlsl"
#include "ShadeVoxelSurface.hlsl"

FORCE_DEPTH_TEST
PS_out main(PS_in IN)
{
	PS_out OUT;

	float4 colour = float4(
		float(0xFFu & IN.PackedColour),
		float(0xFFu & (IN.PackedColour >> 8)),
		float(0xFFu & (IN.PackedColour >> 16)),
		float(0xFFu & (IN.PackedColour >> 24))
	) / 255.0;

	float3 normal = normalize(IN.Normal);
	float3 gridLocalPosition = IN.WorldPosition - camOffset.xyz;
	float3 rayDirection = normalize(IN.WorldPosition - camPosition.xyz);

	if (IsEmissiveVoxel(colour.a))
	{
		float3 v3Emitted = SrgbToLinear(colour.xyz) * EMISSIVE_GAIN;

		v3Emitted = ApplyAerialPerspective(v3Emitted, distance(IN.WorldPosition, camPosition.xyz));

		OUT.Color = float4(EncodeSceneColor(v3Emitted), 1.0);
		OUT.Depth = distance(IN.WorldPosition, camPosition.xyz);
		return OUT;
	}

	float fSkyVisibility = saturate(IN.SkyVisibility);

	float3 bounce = 0.0;

	if (AO_CONE_ENABLED)
		fSkyVisibility *= GetConeAmbient(gridLocalPosition, normal, IN.NormScreenPosition.xy, bounce);

	float3 v3Encoded = ShadeVoxelHit(
		colour.xyz, normal, gridLocalPosition, rayDirection,
		1.0, fSkyVisibility, bounce, 1.0);

	OUT.Color = float4(v3Encoded, 1.0);
	OUT.Depth = distance(IN.WorldPosition, camPosition.xyz);

	return OUT;
}
