#include "Defines.hlsl"
#include "CameraData.hlsl" // register(b0) - SUN_SHADOW_RESOLUTION reads renderQuality.w from here

/* DYNAMIC_MODELS_PLAN.md phase 4. The world shadow map (SunShadow.ps.hlsl)
 * and the dynamic-model one (SunShadowModel.ps.hlsl) each hold, per
 * light-space column, the light-space depth of the nearest thing *that pass
 * knows about*. Smaller is nearer the light, so the true nearest occluder
 * across both is the smaller of the two - a plain min(), full-screen, once.
 *
 * Kept as its own tiny pass rather than folded into either producer so
 * SunShadowLookup.hlsl's PCSS blocker search and filter (RENDERING_PLAN.md
 * 7.1a, tuned and load-bearing) need no changes at all: every existing
 * reader keeps sampling one `sunShadowMap`, now bound to this pass's output
 * instead of the raw world one. */

SamplerState s0 : register(s0);

Texture2D<float> worldShadow : register(t0);
Texture2D<float> modelShadow : register(t1);

float main(float4 position : POS_OUT) : TAR_OUT
{
	float2 uv = position.xy / SUN_SHADOW_RESOLUTION;

	return min(
		worldShadow.SampleLevel(s0, uv, 0),
		modelShadow.SampleLevel(s0, uv, 0));
}
