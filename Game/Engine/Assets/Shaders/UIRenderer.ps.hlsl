#include "Defines.hlsl"

/* Keep this in sync with VKPassBinding::m_uiBindlessCapacity.  glslc treats an
   unsized HLSL resource array as a one-element array (rather than a Vulkan
   runtime descriptor array), so every sprite whose texture ID was not zero
   used to read beyond the Metal argument buffer and fault the GPU.  A fixed
   cross-platform size also makes the shader/layout contract explicit. */
static const uint BindlessTextureCapacity = 96;
Texture2D<float4> Textures[BindlessTextureCapacity] : register(t1);

SamplerState s0 : register(s0);

// Register (b0)
#include "CameraData.hlsl"

struct PS_in
{
    float4	Position	: POS_OUT;
    float2	UVs			: TEXCOORD0;
    uint 	TextureID 	: POSITION;
	float4	Color		: COLOR;
};

float4 main(PS_in IN) : TAR_OUT
{
#ifndef __PSSL__
	/* Clamp corrupt/stale IDs before indexing the descriptor table. */
	uint textureID = min(IN.TextureID, BindlessTextureCapacity - 1);
	float4 outColor = Textures[textureID].Sample(s0, IN.UVs) * IN.Color;
	if (outColor.a == 0.0) discard;
	return outColor;
#else
	return float4(0.0, 0.0, 0.0, 1.0);
#endif
}
