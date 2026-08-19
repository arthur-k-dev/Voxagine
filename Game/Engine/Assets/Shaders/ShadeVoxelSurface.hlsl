/* DYNAMIC_MODELS_PLAN.md phase 2. The common tail of VoxelRenderer.ps.hlsl and
 * VoxelRenderer.ShadowLess.ps.hlsl, factored out so the planned dynamic-model
 * pass shades through the exact same code rather than a third hand-copied
 * version - rule 8's two-shaders-in-lockstep problem, about to gain a third
 * call site.
 *
 * Deliberately NOT the whole lit path. sunVisibility and skyVisibility (with
 * its ambient-quality gate and cone multiply) are computed differently by the
 * two existing callers: one runs a shadow-map lookup and gates sky visibility
 * on Settings::AmbientQuality, the other assumes full sun and never gates.
 * Folding those differences into one shared branch would be a real behaviour
 * change disguised as a refactor. Callers keep computing both visibilities
 * themselves and pass the finished numbers in.
 *
 * fRawRimBoost is GetShineLine's result for the two existing callers - and,
 * deliberately, NOT something the dynamic model pass computes. GetShineLine
 * tests neighbouring *world* voxels (IsVoxel, in AmbientOcclusion.hlsl) and
 * its floor/wall branches assume a face aligned to world axes; a freely
 * rotated character face is generally neither in the world buffer nor
 * axis-aligned, so there is no coherent equivalent to compute, not merely an
 * inconvenient one. The model pass passes the neutral value 1.0 rather than
 * approximate it - see DYNAMIC_MODELS_PLAN.md phase 2 notes.
 *
 * Include after Lighting.hlsl (ShadeSurface), AmbientOcclusion.hlsl (Color.hlsl's
 * GammaGainToLinear, which it pulls in), AmbientCone.hlsl (GetConeSpecular) and
 * Fog.hlsl (ApplyAerialPerspective) - the same include-order contract the rest
 * of this pass already relies on. Reads camOffset/camPosition straight from
 * CameraData.hlsl's cbuffer, same as everything else in this chain. */
float3 ShadeVoxelHit(
	float3 v3Colour,
	float3 v3Normal,
	float3 v3SmoothPosition,
	float3 v3RayDirection,
	float fSunVisibility,
	float fSkyVisibility,
	float3 v3Bounce,
	float fRawRimBoost)
{
	float3 v3Radiance = ShadeSurface(v3Colour, v3Normal, fSunVisibility, fSkyVisibility, v3Bounce);

	/* Environment specular - RENDERING_PLAN.md 7.4. Added rather than folded
	   into ShadeSurface's parentheses: it is light reflected *off* this
	   surface, so the albedo does not multiply it. */
	v3Radiance += GetConeSpecular(v3SmoothPosition, v3Normal, v3RayDirection);

	/* Whatever rim boost the caller decided on - GetShineLine's result for the
	   marcher, 1.0 (a no-op multiplier) for the model pass. Its gain was
	   tuned against the encoded image, hence the conversion, and that applies
	   regardless of source. */
	v3Radiance *= GammaGainToLinear(fRawRimBoost);

	/* Aerial perspective - RENDERING_PLAN.md 6.1. From the *camera*, not a
	   marched distance: every caller of this function rasterizes a proxy and
	   the ray started where its own box was entered, so the caller's own
	   traversal distance is not the one fog is a function of. */
	v3Radiance = ApplyAerialPerspective(v3Radiance, distance(v3SmoothPosition + camOffset.xyz, camPosition.xyz));

	return EncodeSceneColor(v3Radiance);
}
