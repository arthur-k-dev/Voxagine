CONSTANT_BUFFER Data : register(b0)
{
	matrix mvp;
    matrix mv;
	
    float4 camPosition;
    float4 camOffset;
	
	float4 viewport;
    float4 lightDirection;
	
    uint4 worldSize;
	
    float voxelRenderScale;
    float sceneFader;

	uint particleCount;
	uint sdfCount;

	/* Far-field cell grid (RENDERING_PLAN.md phase 4), or zero when there is no
	   far field to march - see RenderContext::GetFarFieldShaderGridSize.

	   Appended, never inserted: RenderContext::Present fills this buffer field
	   by field in declaration order, so anything added above shifts everything
	   after it. The four scalars above pack into one 16-byte register, so this
	   lands aligned. */
	uint4 farFieldSize;

	/* The camera the *scene* was rendered with, which is not the camera in the
	   fields above.
	   RenderContext::Present copies the voxel pass's target at the top of the
	   frame and post processing samples that copy - so the image it composites
	   was rendered one submission ago, while camPosition above has already
	   moved on to the render being recorded now. Anything in post processing
	   that reconstructs a world-space ray has to use these instead, or it
	   slides against the image underneath it whenever the camera moves.
	   Sky and ground never noticed, because they read only the ray's Y.
	   Appended, never inserted - see farFieldSize. */
	matrix sceneInvMvp;

	float4 sceneCamPosition;
	float4 sceneCamOffset;

	/* --- Light-space frame for the sun shadow map (RENDERING_PLAN.md 7.1a) ---
	   An orthonormal basis around lightDirection, plus the rectangle the
	   resident window projects onto in it. RenderContext::Present computes all
	   of this on the CPU: lightDirection is a fixed engine constant and
	   worldSize only changes when the grid is resized, so there is nothing
	   per-pixel about it.

	   A window-space point p maps to light space as
	     (dot(p, shadowTangent.xyz), dot(p, shadowBitangent.xyz), dot(p, lightDirection.xyz))
	   where the third component is depth along the light and the first two are
	   normalized against shadowRect into the map's UV.

	   Appended, never inserted - see farFieldSize. shadowRect is
	   (minU, minV, sizeU, sizeV); shadowDepth is (nearW, invTexelsPerUnitU,
	   invTexelsPerUnitV, unused). */
	float4 shadowTangent;
	float4 shadowBitangent;
	float4 shadowRect;
	float4 shadowDepth;

	/* --- Render quality, from Settings ---------------------------------------
	   The player-facing graphics settings, uploaded every frame so that changing
	   one takes effect on the next without a pipeline rebuild. Defines.hlsl
	   wraps each component in a named accessor - read those rather than these.

	     x  ShadowQuality  0 off, 1 hard, 2 soft (PCSS)
	     y  AmbientQuality 0 off, 1 neighbour test, 2 neighbour test + cones
	     z  a bitmask of QUALITY_FLAG_*: bounce light, reflections, FXAA
	     w  side of the sun shadow map in texels, which follows x

	   Floats rather than a uint4 because w is a resolution the UV maths divides
	   by, and every other value is a small integer that a float holds exactly -
	   so this is one register instead of two.

	   Appended, never inserted - see farFieldSize. */
	float4 renderQuality;

	/* Room for the quality values that do not fit in the four components above.
	   Appended, never inserted - see farFieldSize.

	     x  SHQ_RAY's maximum shadow ray length in world units, 0 for unbounded
	     yzw unused

	   A second register rather than repacking the first: the components of
	   renderQuality are read by name all over the shaders and shifting them to
	   make room is the kind of change that compiles and then draws the wrong
	   thing. */
	float4 renderTuning;
};