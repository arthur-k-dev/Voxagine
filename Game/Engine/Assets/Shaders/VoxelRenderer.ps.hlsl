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

/* Light-space sun shadow map - RENDERING_PLAN.md 7.1a. Third texture pushed by
   VoxelPass, so t3; the bindless model array below stays at t4. */
Texture2D<float> sunShadowMap : register(t3);

/* Coverage and radiance pyramid - RENDERING_PLAN.md 7.1b route B and 7.3. Mip
   L is pyramid level L over the resident window: alpha is the occupied fraction
   of a cell and RGB its linear albedo premultiplied by that fraction, so one
   SampleLevel is a cone step for both the occlusion and the bounce, filter
   included. Fourth texture pushed by VoxelPass, so t4, and the bindless model
   array moves up to t5. */
Texture3D<float4> voxelPyramid : register(t4);

VOXEL_BUFFER voxelModelData[] : register(t5);

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

    /* Bounce light from the same cones - RENDERING_PLAN.md 7.3. */
    float3 bounce = 0.0;

#if AO_CONE_ENABLED
    skyVisibility *= GetConeAmbient(marchDiffuse.SmoothPosition, marchDiffuse.Normal, IN.NormScreenPosition.xy, bounce);
#endif

    /* Linear radiance from here to EncodeSceneColor below - RENDERING_PLAN.md
       phase 7.2, and Color.hlsl for why the encode is here rather than at
       present. */
    float3 v3Radiance = ShadeSurface(marchDiffuse.Color.xyz, marchDiffuse.Normal, sunVisibility, skyVisibility, bounce);

    /* Environment specular - RENDERING_PLAN.md 7.4. Added rather than folded
       into ShadeSurface's parentheses: it is light reflected *off* this
       surface, so the albedo does not multiply it. */
    v3Radiance += GetConeSpecular(marchDiffuse.SmoothPosition, marchDiffuse.Normal, rayDirection);

    /* Fake specular "shine line" on lit voxel edges - see GetShineLine in
       AmbientOcclusion.hlsl. It returns 1.0 for anything the sun does not
       reach, so applying it to the whole shaded result rather than to the sun
       term alone only ever brightens a rim that is already lit. Its gain was
       tuned against the encoded image, hence the conversion. */
    v3Radiance *= GammaGainToLinear(GetShineLine(marchDiffuse.Position, marchDiffuse.Normal, marchDiffuse.UV, lightDirection.xyz, difference));

    /* Aerial perspective - RENDERING_PLAN.md 6.1. From the *camera*, not
       marchDiffuse.Distance: this pass rasterizes AABB proxies and the ray
       started where its own box was entered, so the marcher's distance is not
       the one fog is a function of. */
    v3Radiance = ApplyAerialPerspective(v3Radiance, distance(marchDiffuse.SmoothPosition + camOffset.xyz, camPosition.xyz));

    marchDiffuse.Color.xyz = EncodeSceneColor(v3Radiance);
    marchDiffuse.Color.a = 1.0;

#ifdef MARCH_STEP_DEBUG
    /* After the shadow ray, so the heatmap shows the pixel's whole cost - the
       shadow ray is 53% of this pass (RENDERING_PLAN.md phase 3 notes). */
    return GetStepHeatmap();
#endif

    return marchDiffuse.Color;
}