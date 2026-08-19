#include "Defines.hlsl"
#include "CameraData.hlsl" // register(b0)

static const float3 vertices[] = {
    float3(-1.0,	 1.0,	 1.0),    // Front Top Left
    float3( 1.0,	 1.0,	 1.0),    // Front Top Right
    float3(-1.0,	-1.0,	 1.0),    // Front Bottom Left
    float3( 1.0,	-1.0,	 1.0),    // Front Bottom Right
    float3(-1.0,	 1.0,	-1.0),   // Back Top Left
    float3( 1.0,	 1.0,	-1.0),   // Back Top Right
    float3(-1.0,	-1.0,	-1.0),   // Back Bottom Left
    float3( 1.0,	-1.0,	-1.0),   // Back Bottom Right
};

static int indices[14] =
{
    3, 2, 1, 0,
	4, 2, 6, 3,
	7, 1, 5, 4,
	7, 6
};

static float3 CUBE_VERTS[24] = {
	float3(-1, 0, -1),	float3(0, -1, 0),	float3(-1, 0,  1),
	float3(-1, 0,  1),	float3(0, -1, 0),	float3( 1, 0,  1),
	float3(1,  0,  1),	float3(0, -1, 0),	float3( 1, 0, -1),
	float3(1,  0, -1),	float3(0, -1, 0),	float3(-1, 0, -1),
	float3(-1, 0, -1),	float3(0,  1, 0),	float3(-1, 0,  1),
	float3(-1, 0,  1),	float3(0,  1, 0),	float3( 1, 0,  1),
	float3(1,  0,  1),	float3(0,  1, 0),	float3( 1, 0, -1),
	float3(1,  0, -1),	float3(0,  1, 0),	float3(-1, 0, -1)
};

VOXEL_RW_BUFFER voxelWorldData : register(u0);

/* Scalarised for the same reason UIRenderer.vs.hlsl's SpriteData is, and the
   consequence of not doing it is larger here. The engine memcpys a packed
   32-byte StructuredVoxelBuffer (Vector3 Position, Vector3 Extents, uint32
   MapperID, float Distance) straight into this buffer. Spelled as float3 the
   members get std430's 16-byte alignment, which makes the stride 48 - so every
   AABB after [0] was decoded from the wrong offset.

   That is what left the world unrendered: this buffer is the voxel pass's
   instance list, one proxy box per submitted region, so garbage extents mean
   nothing rasterises and only the far field survives. Regions re-submitted
   after a destruction happened to land readably, which is why shooting
   "revealed" parts of the world.

   Eight 4-byte scalars are laid out identically by std430 and by DX packing,
   so this is correct under glslc and DXC alike rather than compensating for
   one of them. */
struct AABB
{
	float PositionX;
	float PositionY;
	float PositionZ;

	float ExtentsX;
	float ExtentsY;
	float ExtentsZ;

	int TextureID;
	float Distance;
};

STRUCTURED_BUFFER(AABB) AABBs : register(t0);
Texture2D<float4> particlePass : register(t1);

/* The unbounded `VOXEL_BUFFER voxelModelData[] : register(tN)` that used to be
   here is gone. It was never read - it belonged to a GPU-baker path that was
   abandoned - but it compiled into the live SPIR-V as an unbounded typed-buffer
   descriptor array, which is exactly the shape mobile drivers and MoltenVK are
   pickiest about, and it would have been bound to nothing. Deleting dead code
   beats working around a descriptor-indexing limitation for it.
   MOBILE_PORT_PLAN.md phase 4, step 3. */

struct VS_out
{
    float4 NormScreenPosition	: POS_OUT;
	float4 Direction			: POSITION0;
    float3 WorldPosition		: POSITION1;
};

VS_out main(uint IDvert : VERT_ID, uint IDinst : INST_ID)
{
    VS_out OUT;

    // Get relevant AABB data
    AABB tAABB = AABBs[IDinst];

    // Get cube model data
    // Store world-space coordinates in the vertex
	float3 aabbExtents = float3(tAABB.ExtentsX, tAABB.ExtentsY, tAABB.ExtentsZ);
	float3 aabbPosition = float3(tAABB.PositionX, tAABB.PositionY, tAABB.PositionZ);

	OUT.WorldPosition = vertices[indices[IDvert]] * aabbExtents + aabbPosition;
	OUT.WorldPosition = clamp(OUT.WorldPosition.xyz, float3(0.0, 0.0, 0.0), float3(worldSize.xyz)) + camOffset.xyz;

    // Multiply the position with the MVP matrix
    OUT.NormScreenPosition = mul(mvp, float4(OUT.WorldPosition, 1.0));
	
	OUT.Direction = float4(OUT.WorldPosition.xyz - camPosition.xyz, 0.0);
	
    return OUT;
}