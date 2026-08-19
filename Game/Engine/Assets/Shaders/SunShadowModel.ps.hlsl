#include "Defines.hlsl"
#include "CameraData.hlsl" // register(b0)

/* DYNAMIC_MODELS_PLAN.md phase 4. Writes this fragment's absolute
 * light-space depth - see SunShadowModel.vs.hlsl. Every texel this pass's
 * geometry covers gets a real value; texels it does not cover keep the
 * clear value (SUN_SHADOW_FAR, SunShadowModelPass.cpp), which is what makes
 * combining it with the world map via min() correct - "no character here"
 * must never look like "something is extremely close to the light". */

struct PS_in
{
	float4 ClipPosition	: POS_OUT;
	float LightDepth		: POSITION0;
};

float main(PS_in IN) : TAR_OUT
{
	return IN.LightDepth;
}
