#include "Defines.hlsl"
#include "CameraData.hlsl" // register(b0)
#include "VectorMath.hlsl" // QuatRotate

/* DYNAMIC_MODELS_PLAN.md phase 2. One quad instance per SV_InstanceID, six
 * vertices each (two triangles, no index buffer - same "procedural from
 * SV_VertexID" idiom every other pass in this engine already uses, see
 * DYNAMIC_MODELS_PLAN.md's "Nothing in this engine uses a vertex buffer").
 *
 * Register order is this file's contract with VoxelModelPass.cpp - see rule 1,
 * RENDERING_PLAN.md. b0 camera (shared), t0/t1 the two per-frame instance
 * buffers, t2 the shared mesh quad store. */

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
	
	uint _Pad0;
	uint _Pad1;
};

STRUCTURED_BUFFER(ModelInstance) modelInstances : register(t0);
STRUCTURED_BUFFER(QuadInstanceRef) quadInstances : register(t1);

/* The shared quad store (VoxelMesher.h): 3 uint words per quad, raw - not a
 * struct, because every model frame's quads share one flat buffer built once
 * and grown as new frames are meshed, and a struct stride would have to match
 * exactly anyway. */
STRUCTURED_BUFFER(uint) modelMeshQuads : register(t2);

struct VS_out
{
	float4 NormScreenPosition			: POS_OUT;

	/* True world space, unlike the AABB-proxy vertex shader's WorldPosition -
	   that one starts grid-local (an AABB the CPU already put in window space)
	   and adds camOffset to reach world space for the MVP multiply; this one
	   already has true world coordinates from ModelInstance.WorldOrigin, so
	   there is nothing to add. The pixel shader subtracts camOffset itself
	   wherever it needs the grid-local convention SDFMarcher's functions and
	   the coverage pyramid expect - see VoxelModel.ps.hlsl. */
	float3 WorldPosition				: POSITION0;
	float3 Normal						: POSITION1;

	/* Baked per-corner AO (VoxelMesher.h), already converted from occlusion
	   level to visibility and bilinearly resolved by picking the correct
	   diagonal below - the pixel shader just receives an already-correct
	   interpolated scalar, no AmbientOcclusion.hlsl call needed. */
	float SkyVisibility					: POSITION2;

	/* Integer interpolants must be nointerpolation in HLSL - harmless here
	   since it is genuinely constant across one quad's six vertices. Combines
	   the quad's stored colour (or the renderer's override) with its
	   VoxelStateTag exactly as VoxelStamp.h's ForEachStampedVoxel does for a
	   baked voxel, so IsEmissiveVoxel and the alpha-is-occupancy convention
	   (rule 3) both still hold for this word. */
	nointerpolation uint PackedColour	: COLOR0;
};

VS_out main(uint IDvert : VERT_ID, uint IDinst : INST_ID)
{
	VS_out OUT;

	QuadInstanceRef quadRef = quadInstances[IDinst];
	ModelInstance inst = modelInstances[quadRef.InstanceIndex];

	uint word0 = modelMeshQuads[quadRef.QuadIndex * 3u + 0u];
	uint word1 = modelMeshQuads[quadRef.QuadIndex * 3u + 1u];
	uint word2 = modelMeshQuads[quadRef.QuadIndex * 3u + 2u];

	int3 origin = int3(
		int(word0 & 0xFFu),
		int((word0 >> 8) & 0xFFu),
		int((word0 >> 16) & 0xFFu));

	int axis = int((word0 >> 24) & 0x3u);
	int faceSign = ((word0 >> 26) & 0x1u) != 0u ? 1 : -1;

	int extentU = int(word1 & 0xFFu);
	int extentV = int((word1 >> 8) & 0xFFu);

	/* Packing order from VoxelMesher.cpp: mm, pm, mp, pp - matched here to
	   the corner table below (c0=mm, c1=pm, c2=pp, c3=mp), which is why the
	   AO array is built in that same mm/pm/pp/mp order rather than reading
	   the bits out in ascending order. */
	int aoMM = int((word1 >> 16) & 0x3u);
	int aoPM = int((word1 >> 18) & 0x3u);
	int aoMP = int((word1 >> 20) & 0x3u);
	int aoPP = int((word1 >> 22) & 0x3u);

	/* The face plane sits one voxel past `origin` for a positive-sign face -
	   VoxelMesher.h's "a voxel at index v occupies [v, v+1)" convention. */
	int3 faceBase = origin;
	faceBase[axis] += (faceSign > 0) ? 1 : 0;

	int u = (axis + 1) % 3;
	int v = (axis + 2) % 3;

	int3 d1 = int3(0, 0, 0);
	int3 d2 = int3(0, 0, 0);
	d1[u] = 1;
	d2[v] = 1;

	/* Rectangle corners in (u, v), and the AO baked for each - c0..c3 go
	   around the quad, matching VoxelMesher.cpp's word1 packing. */
	int2 cornerUV[4] = { int2(0, 0), int2(extentU, 0), int2(extentU, extentV), int2(0, extentV) };
	int cornerAO[4] = { aoMM, aoPM, aoPP, aoMP };

	/* Two ways to split this rectangle into triangles; a diagonal chosen
	   without looking at AO can put the seam through the *open* corner of a
	   locally-occluded quad, which reads as a bright triangle where a soft
	   corner shadow belongs. Routing the split through the diagonal with the
	   larger combined occlusion keeps the shading's only genuine
	   discontinuity - a greedy-meshed quad has no interior gradient data -
	   out of the corner an eye would actually call "the shadow". */
	bool bFlipDiagonal = (cornerAO[0] + cornerAO[2]) < (cornerAO[1] + cornerAO[3]);

	/* d1 x d2 is always +axisUnit (u=(axis+1)%3, v=(axis+2)%3 is a cyclic,
	   right-handed choice for every axis), so walking cornerUV in ascending
	   order is counter-clockwise as seen from the +axis side regardless of
	   which face this is. A negative-sign face's outward normal is -axisUnit,
	   so that same winding is clockwise as seen from where this face is
	   actually visible from - backwards. Reversing each triangle's vertex
	   order for a negative-sign face is what keeps both faces front-facing
	   from their own outward side without changing cornerUV/extent meaning
	   (which is anchored to the true u,v axes, not to which way the face
	   points) or which voxels the quad covers. */
	int cornerIndex;
	if (faceSign > 0)
	{
		if (!bFlipDiagonal) { int tri[6] = { 0, 1, 2, 0, 2, 3 }; cornerIndex = tri[IDvert]; }
		else                { int tri[6] = { 1, 2, 3, 1, 3, 0 }; cornerIndex = tri[IDvert]; }
	}
	else
	{
		if (!bFlipDiagonal) { int tri[6] = { 2, 1, 0, 3, 2, 0 }; cornerIndex = tri[IDvert]; }
		else                { int tri[6] = { 3, 2, 1, 0, 3, 1 }; cornerIndex = tri[IDvert]; }
	}

	int2 cUV = cornerUV[cornerIndex];
	int cAO = cornerAO[cornerIndex];

	int3 cornerLocal = faceBase + d1 * cUV.x + d2 * cUV.y;

	float3 scaledLocal = float3(cornerLocal) * inst.Scale;
	float3 worldPos = inst.WorldOrigin + QuatRotate(inst.Rotation, scaledLocal);

	/* Face normal under non-uniform scale needs the inverse-transpose, which
	   for a diagonal scale matrix is just its componentwise reciprocal - see
	   ModelInstanceData.h. Axis-aligned before rotation, by construction. */
	float3 axisUnit = float3(0.0, 0.0, 0.0);
	axisUnit[axis] = float(faceSign);
	float3 invScale = 1.0 / max(abs(inst.Scale), float3(1e-5, 1e-5, 1e-5));
	float3 worldNormal = normalize(QuatRotate(inst.Rotation, axisUnit * invScale));

	uint hasOverride = (inst.OverrideColor >> 24) > 0u ? 1u : 0u;
	uint baseColour = hasOverride != 0u ? inst.OverrideColor : word2;

	OUT.WorldPosition = worldPos;
	OUT.NormScreenPosition = mul(mvp, float4(worldPos, 1.0));
	OUT.Normal = worldNormal;
	OUT.SkyVisibility = 1.0 - (float(cAO) / 3.0);
	OUT.PackedColour = (baseColour & 0x00FFFFFFu) | inst.Tag;

	return OUT;
}
