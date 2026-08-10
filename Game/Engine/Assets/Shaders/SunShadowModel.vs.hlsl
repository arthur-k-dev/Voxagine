#include "Defines.hlsl"
#include "CameraData.hlsl" // register(b0)
#include "VectorMath.hlsl" // QuatRotate

/* DYNAMIC_MODELS_PLAN.md phase 4. Projects the same quad list
 * VoxelModel.vs.hlsl draws into light-space clip instead of camera clip, so
 * dynamic renderers cast into their own light-space depth target
 * (SunShadowModelPass) rather than the world one. Deliberately a *second*
 * target combined afterward (SunShadowCombine.ps.hlsl) rather than writing
 * into SunShadowPass's own target - RenderPass::Data::m_PassOutput is an
 * input binding only, this engine has no "write into a pass I don't own"
 * mechanism, and this way the well-tested PCSS blocker search/filter in
 * SunShadowLookup.hlsl (RENDERING_PLAN.md 7.1a) needs no changes at all: it
 * keeps reading one `sunShadowMap`, now pointed at the combined result.
 *
 * Registers match VoxelModel.vs.hlsl's first three exactly (same buffers,
 * reused): b0 camera, t0 instances, t1 quad instances, t2 mesh quads. No
 * pyramid here - this pass does not shade anything. */

struct ModelInstance
{
	float4 Rotation;
	float3 Scale;
	float _Pad0;
	float3 WorldOrigin;
	float _Pad1;
	uint OverrideColor;
	uint Tag;
	uint _Pad2;
	uint _Pad3;
};

struct QuadInstanceRef
{
	uint InstanceIndex;
	uint QuadIndex;
};

STRUCTURED_BUFFER(ModelInstance) modelInstances : register(t0);
STRUCTURED_BUFFER(QuadInstanceRef) quadInstances : register(t1);
STRUCTURED_BUFFER(uint) modelMeshQuads : register(t2);

struct VS_out
{
	float4 ClipPosition	: POS_OUT;

	/* Absolute light-space depth of this vertex - SunShadowToLightSpace's
	   third component, dot(position, lightDirection) - which is exactly what
	   SunShadow.ps.hlsl's world march stores (shadowDepth.x + hit.Distance
	   reduces to the same dot product; see this pass's own notes). Written
	   here rather than recomputed per pixel since it is linear across a flat
	   quad and free to interpolate. */
	float LightDepth		: POSITION0;
};

VS_out main(uint IDvert : VERT_ID, uint IDinst : INST_ID)
{
	VS_out OUT;

	QuadInstanceRef quadRef = quadInstances[IDinst];
	ModelInstance inst = modelInstances[quadRef.InstanceIndex];

	uint word0 = modelMeshQuads[quadRef.QuadIndex * 3u + 0u];
	uint word1 = modelMeshQuads[quadRef.QuadIndex * 3u + 1u];

	int3 origin = int3(
		int(word0 & 0xFFu),
		int((word0 >> 8) & 0xFFu),
		int((word0 >> 16) & 0xFFu));

	int axis = int((word0 >> 24) & 0x3u);
	int faceSign = ((word0 >> 26) & 0x1u) != 0u ? 1 : -1;

	int extentU = int(word1 & 0xFFu);
	int extentV = int((word1 >> 8) & 0xFFu);

	int3 faceBase = origin;
	faceBase[axis] += (faceSign > 0) ? 1 : 0;

	int u = (axis + 1) % 3;
	int v = (axis + 2) % 3;

	int3 d1 = int3(0, 0, 0);
	int3 d2 = int3(0, 0, 0);
	d1[u] = 1;
	d2[v] = 1;

	/* Same corner table and diagonal-flip winding as VoxelModel.vs.hlsl -
	   shadow casting does not care about winding (no culling here, see
	   SunShadowModelPass.cpp), but using the same table keeps this file
	   readable against that one rather than inventing a second convention. */
	int2 cornerUV[4] = { int2(0, 0), int2(extentU, 0), int2(extentU, extentV), int2(0, extentV) };

	int tri[6] = { 0, 1, 2, 0, 2, 3 };
	int2 cUV = cornerUV[tri[IDvert]];

	int3 cornerLocal = faceBase + d1 * cUV.x + d2 * cUV.y;

	float3 scaledLocal = float3(cornerLocal) * inst.Scale;
	float3 worldPos = inst.WorldOrigin + QuatRotate(inst.Rotation, scaledLocal);

	/* Grid-local (window-relative), matching the world shadow pass's own
	   space - VoxelModel.ps.hlsl's header explains the same conversion. */
	float3 gridLocalPos = worldPos - camOffset.xyz;

	float lightU = dot(gridLocalPos, shadowTangent.xyz);
	float lightV = dot(gridLocalPos, shadowBitangent.xyz);
	float lightW = dot(gridLocalPos, lightDirection.xyz);

	float2 uv = (float2(lightU, lightV) - shadowRect.xy) / shadowRect.zw;

	/* Negative-height viewport (VKRenderPass.cpp: viewport.y = height,
	   viewport.height = -height, applied to every pass) maps clip.y=+1 to
	   window row 0 and clip.y=-1 to row `height` - the Vulkan viewport
	   transform is yw = height*(1-yndc)/2, not the yw = height*(1+yndc)/2 a
	   "positive height" viewport would give. So v=0 (top row, matching the
	   world pass's own v4Position.xy/SUN_SHADOW_RESOLUTION convention) has
	   to land at clip.y=+1, the opposite of the naive uv*2-1 mapping - which
	   is what the first version of this shader used, and it shipped a
	   Y-mirrored shadow: reported as "moves the opposite way" and "lands a
	   chunk away", both symptoms of one flipped sign, not two bugs. X has no
	   such flip (viewport.x/width are never negated), so clip.x = uv.x*2-1
	   stays as-is. */
	float2 clipXY = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

	/* Depth range normalized to [0,1] across the window's own light-space
	   extent (shadowDepth.x=near, shadowDepth.w=range) - an orthographic
	   projection along the light, so w=1 and no perspective divide is
	   needed. Used only for this pass's own depth attachment, resolving
	   overlapping characters against each other; the light-space *value*
	   this pass writes to its colour target is LightDepth below, unrelated
	   to this normalization. */
	float depth01 = saturate((lightW - shadowDepth.x) / max(shadowDepth.w, 1e-5));

	OUT.ClipPosition = float4(clipXY, depth01, 1.0);
	OUT.LightDepth = lightW;

	return OUT;
}
