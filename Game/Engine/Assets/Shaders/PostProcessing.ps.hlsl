#include "Defines.hlsl"
#include "CameraData.hlsl" // register(b0)
#include "Camera.hlsl"

SamplerState s0 : register(s0);

/* Point-sampled, and this pass is where it matters.
 *
 * The scene target is rendered at Settings::ResolutionScale - half size on a
 * phone by default - and read back here at the window's full resolution, so
 * every fetch of it is an upscale. Bilinear turns a hard voxel edge into a
 * two-pixel gradient, which on this art is not "softer", it is wrong: the
 * subject is cubes, and the thing a player sees at half resolution should be
 * bigger cubes rather than blurred ones.
 *
 * FXAA keeps s0. It is built on bilinear taps - the whole method is reading
 * *between* texels - and handing it a point sampler would not sharpen it, it
 * would break it. */
SamplerState pointSampler : register(s1);

Texture2D<float4> targetTexture : register(t0);
Texture2D<float4> uiTexture : register(t1);

VOXEL_RW_BUFFER voxelWorldData : register(u0);

/* Far-field LOD volume and its occupancy bricks - see FarField.hlsl. Bound
   read-write for the same reason the window's brick counts are: the u range is
   what is free, and taking a t would renumber the textures above. Nothing
   writes to either from the GPU. */
VOXEL_RW_BUFFER farFieldData : register(u1);
RW_STRUCTURED_BUFFER(uint) farFieldBrickData : register(u2);

#ifndef __PSSL__
#define FXAA_HLSL_5 1
#define FXAA_QUALITY__PRESET 39
#include "FXAA3_11.hlsl"
#endif

/* Minimal standalone copy of SDFMarcher.hlsl's voxel lookup - just enough for
   the ground sample below. Not #include-ing SDFMarcher.hlsl itself since this
   pass has no march loop and doesn't need it. */
inline uint PostFxPosToVoxelID(uint3 v3Position) {
	return v3Position.x + v3Position.y * worldSize.x + worldSize.x * worldSize.y * v3Position.z;
}

inline float4 PostFxGetVoxel(float3 v3Position) {
	/* The post pass can run before the first world upload has supplied a
	   resident grid.  Never turn a negative/NaN/out-of-world position into an
	   unsigned buffer index: strict Vulkan/Metal implementations fault the GPU
	   for that access instead of returning zero like several desktop drivers. */
	if (worldSize.x == 0 || worldSize.y == 0 || worldSize.z == 0)
		return float4(0.0, 0.0, 0.0, 0.0);

	int3 v3Voxel = int3(floor(v3Position));
	if (any(v3Voxel < int3(0, 0, 0)) || any(v3Voxel >= int3(worldSize.xyz)))
		return float4(0.0, 0.0, 0.0, 0.0);

	uint ID = PostFxPosToVoxelID(uint3(v3Voxel));
#ifdef __PSSL__
	uint uiColor = voxelWorldData[ID];
	return float4(0xFF & (uiColor), 0xFF & (uiColor >> 8), 0xFF & (uiColor >> 16), 0xFF & (uiColor >> 24)) / 255.0;
#else
	return voxelWorldData[ID];
#endif
}

/* Background for pixels the Voxel pass left untouched - either no AABB proxy
   covered them, or MarchDiffuse found nothing (see VoxelRenderer.ps.hlsl).
   Far-field geometry, endless ground and sky all come from GetBackground; this
   pass runs full-screen unconditionally, so unlike the proxy-box approach it
   always covers every pixel. */
#include "FarField.hlsl"

/* FXAA has to be kept away from the scene's silhouette against the background.
   The Voxel pass writes float4(0, 0, 0, 0) where its march found nothing, so a
   neighbourhood straddling a silhouette contains transparent *black* - and FXAA
   blends colour, not coverage, so it pulls the surface toward that black and
   leaves a one-pixel dark fringe along the edge. Against the sky that has
   always been there and read as an outline; along the resident window's
   boundary, where the ground continues on the far side at a matching colour, it
   reads as a seam across the middle of a flat plane.

   Nothing is lost by skipping it there. FXAA smooths high-contrast edges
   *within* the scene, and a pixel on the silhouette is composited against the
   background immediately below - there is no second scene sample to blend with.

   Load with clamped coordinates rather than Sample with offsets: R_DEF_WRAP_MODE
   is E_WRAP, so a neighbour offset past the screen edge would answer with a
   pixel from the far side. */
bool IsSceneNeighbourhoodOpaque(float2 v2PixelPosition)
{
	/* This pass is full-resolution while targetTexture is rendered at
	   Settings::ResolutionScale (0.35 on the iPad).  Using the output pixel
	   coordinate directly for Texture2D.Load therefore reads far beyond the
	   smaller scene image.  Desktop drivers commonly return zero for that
	   undefined access; Metal reports it correctly as a GPU address fault. */
	/* Keep this calculation identical to VKRenderPass::Recreate: the target
	   dimensions are uint32_t(viewport * ResolutionScale).  The glslc HLSL
	   frontend used by the iOS build silently discards Texture2D.GetDimensions
	   output arguments, so querying the image here is not portable. */
	int2 v2Size = max(int2(viewport.xy * voxelRenderScale), int2(1, 1));
	float2 v2UV = saturate(v2PixelPosition / max(viewport.xy, float2(1.0, 1.0)));
	int2 v2Texel = min(int2(v2UV * float2(v2Size)), v2Size - 1);

	[unroll]
	for (int y = -1; y <= 1; y++)
	{
		[unroll]
		for (int x = -1; x <= 1; x++)
		{
			int2 v2Sample = clamp(v2Texel + int2(x, y), int2(0, 0), v2Size - 1);

			if (targetTexture.Load(int3(v2Sample, 0)).a == 0.0)
				return false;
		}
	}

	return true;
}

float4 main(float4 position : POS_OUT) : TAR_OUT
{
    /* The UI target is already full resolution, so this is a 1:1 fetch either
       way - point keeps it exact rather than trusting that to stay true. */
    float4 uiColor = uiTexture.Sample(pointSampler, position.xy / viewport.xy);
    if (uiColor.a == 1.0) return lerp(float4(0.0, 0.0, 0.0, 1.0), uiColor, sceneFader);

	/* The upscale. See pointSampler above. */
	float4 rawScene = targetTexture.Sample(pointSampler, position.xy / viewport.xy);
	float4 sceneColor;

	if (rawScene.a == 0.0)
	{
		sceneColor = GetBackground(position.xy);
	}
	/* Settings::FXAAEnabled, and the flag is tested before the neighbourhood
	   test rather than after: those nine Loads exist only to decide whether FXAA
	   is safe here, so with FXAA off they are nine texture reads a pixel
	   answering a question nobody asks. */
	else if (!FXAA_ENABLED || !IsSceneNeighbourhoodOpaque(position.xy))
	{
		sceneColor = rawScene;
	}
	else
	{
#ifndef __PSSL__
		FxaaTex inputFXAATex = { s0, targetTexture };

		sceneColor =  FxaaPixelShader(
			position.xy / viewport.xy,						// FxaaFloat2 pos,
			FxaaFloat4(0.0f, 0.0f, 0.0f, 0.0f),				// FxaaFloat4 fxaaConsolePosPos,
			inputFXAATex,									// FxaaTex tex,
			inputFXAATex,									// FxaaTex fxaaConsole360TexExpBiasNegOne,
			inputFXAATex,									// FxaaTex fxaaConsole360TexExpBiasNegTwo,
			FxaaFloat2(1.0 / viewport.x, 1.0 / viewport.y),	// FxaaFloat2 fxaaQualityRcpFrame,
			FxaaFloat4(0.0f, 0.0f, 0.0f, 0.0f),				// FxaaFloat4 fxaaConsoleRcpFrameOpt,
			FxaaFloat4(0.0f, 0.0f, 0.0f, 0.0f),				// FxaaFloat4 fxaaConsoleRcpFrameOpt2,
			FxaaFloat4(0.0f, 0.0f, 0.0f, 0.0f),				// FxaaFloat4 fxaaConsole360RcpFrameOpt2,
			0.75f,											// FxaaFloat fxaaQualitySubpix,
			0.166f,											// FxaaFloat fxaaQualityEdgeThreshold,
			0.0833f,										// FxaaFloat fxaaQualityEdgeThresholdMin,
			8.0f,											// FxaaFloat fxaaConsoleEdgeSharpness,
			0.125f,											// FxaaFloat fxaaConsoleEdgeThreshold,
			0.05f,											// FxaaFloat fxaaConsoleEdgeThresholdMin,
			FxaaFloat4(0.0f, 0.0f, 0.0f, 0.0f)				// FxaaFloat fxaaConsole360ConstDir,
		);
#else
		sceneColor = targetTexture.Sample(pointSampler, position.xy / viewport.xy);
#endif
	}

    float uiAlpha = uiColor.a;
    uiColor.a = 1.0;

	return lerp(float4(0.0, 0.0, 0.0, 1.0), lerp(sceneColor, uiColor, uiAlpha), sceneFader);
}
