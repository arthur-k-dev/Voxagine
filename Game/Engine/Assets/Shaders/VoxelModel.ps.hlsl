#include "Defines.hlsl"
#include "CameraData.hlsl" // register(b0)

/* DYNAMIC_MODELS_PLAN.md phase 2, with self-shadowing added as a phase 4
 * follow-up. Shades a dynamic model's rasterized quad - no marching, no
 * world voxel buffer, no world AABB proxy involved. Composed from the same
 * building blocks the marcher uses (AmbientCone.hlsl for world ambient/
 * bounce, SunShadowLookup.hlsl for the sun term, ShadeVoxelSurface.hlsl for
 * the shared tail), but with the self-occlusion term baked into the mesh
 * instead of sampled from voxelWorldData.
 *
 * sunShadowMap here is SunShadowCombinePass's output (RenderContext.cpp),
 * not the world-only map - it already folds in every dynamic renderer's own
 * cast shadow (SunShadowModel.ps.hlsl), so a character standing in another
 * character's shadow, or in its own contact shadow, reads correctly with no
 * extra plumbing here.
 *
 * Registers are this file's contract with VoxelModelPass.cpp - see rule 1,
 * RENDERING_PLAN.md. s0 is the point sampler SunShadowLookup.hlsl expects by
 * that exact name; s1 is the pyramid's own (linear, point across mips, black
 * outside - RenderContext's "Coverage pyramid texture" block). */
SamplerState s0 : register(s0);
SamplerState pyramidSampler : register(s1);

Texture3D<float4> voxelPyramid : register(t3);
Texture2D<float> sunShadowMap : register(t4);

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

	/* True-world distance from the camera, exactly the convention
	   VoxelRenderer.ps.hlsl already reads particleDepthPass in - see its
	   composite branch and DYNAMIC_MODELS_PLAN.md piece 3. */
	float Depth		: TAR_OUT1;
};

/* GetOrthogonal (VectorMath.hlsl, pulled in by AmbientCone.hlsl's tangent
   frame) has no dependency on the voxel buffer; SDFMarcher.hlsl itself is
   deliberately NOT included here - this pass has no world to march, and
   pulling it in would mean declaring voxelWorldData/voxelBrickData bindings
   that would never be read. See VectorMath.hlsl's header comment. */
#define AO_CONE_HAS_SUN_SHADOW 1

#include "VectorMath.hlsl"
#include "Lighting.hlsl"
#include "SunShadowLookup.hlsl"
#include "AmbientCone.hlsl"
#include "Fog.hlsl"
#include "ShadeVoxelSurface.hlsl"

FORCE_DEPTH_TEST
PS_out main(PS_in IN)
{
	PS_out OUT;

	/* Same byte layout GetVoxel decodes the world buffer's words into -
	   VoxelMesher.h's word2 is deliberately the same 0xAABBGGRR shape, just
	   not the same *meaning* for occupancy (there is none left to encode
	   here; existence of the quad already says solid). */
	float4 colour = float4(
		float(0xFFu & IN.PackedColour),
		float(0xFFu & (IN.PackedColour >> 8)),
		float(0xFFu & (IN.PackedColour >> 16)),
		float(0xFFu & (IN.PackedColour >> 24))
	) / 255.0;

	float3 normal = normalize(IN.Normal);

	/* Grid-local (window-relative) position, matching what SDFMarcher's
	   MarchResult.Position/SmoothPosition and the pyramid sampling already
	   assume - IN.WorldPosition is true-world (VoxelModel.vs.hlsl), and
	   camOffset is the resident window's own world-space origin. */
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

	/* Same gate the marcher's shadowed variant uses: a face angled away from
	   the sun gets no benefit from the lookup (its own NdotL already zeroes
	   the direct term in ShadeSurface), so skip the PCSS blocker search and
	   filter for it. */
	float fDifference = clamp(dot(normal, -lightDirection.xyz), 0.0, 1.0);
	float fSunVisibility = 1.0;

	if (fDifference > 0.1)
		fSunVisibility = GetSunVisibility(gridLocalPosition, normal, IN.NormScreenPosition.xy);

	/* Baked per-corner AO from the vertex shader, standing in for
	   AmbientOcclusion.hlsl's GetSkyVisibility - DYNAMIC_MODELS_PLAN.md's
	   design piece 4. World cones still apply on top, exactly as they do for
	   the marched surface. */
	float fSkyVisibility = saturate(IN.SkyVisibility);

	float3 bounce = 0.0;

	if (AO_CONE_ENABLED)
		fSkyVisibility *= GetConeAmbient(gridLocalPosition, normal, IN.NormScreenPosition.xy, bounce);

	/* No GetShineLine - see ShadeVoxelSurface.hlsl on why a freely rotated
	   face has no coherent equivalent, rather than an approximated one. */
	float3 v3Encoded = ShadeVoxelHit(
		colour.xyz, normal, gridLocalPosition, rayDirection,
		fSunVisibility, fSkyVisibility, bounce, 1.0);

	OUT.Color = float4(v3Encoded, 1.0);
	OUT.Depth = distance(IN.WorldPosition, camPosition.xyz);

	return OUT;
}
