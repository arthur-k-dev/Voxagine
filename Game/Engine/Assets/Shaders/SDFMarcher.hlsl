#define CLEAR_COLOR_COMPONENT 0.1
#define CLEAR_COLOR float3(CLEAR_COLOR_COMPONENT, CLEAR_COLOR_COMPONENT, CLEAR_COLOR_COMPONENT)
#define COLOR_CONVERTER 1 / 255.0

// glsl style mod
#define mod(x, y) (x - y * floor(x / y))

struct MarchResult
{
	float3 Position;
	float3 SmoothPosition;
	
	float Distance;
	float3 Normal;
	float2 UV;
	
	float3 Mask;
	float3 SRDirection;
	
	float4 Color;
};

/* Every DDA crossing this pixel has made, at either level, across every ray it
   has fired. A static global is per-invocation, so the primary march and the
   shadow ray that follows it accumulate into the same counter - which is the
   point. What has to be bounded is the pixel's total work; bounding each ray
   separately just doubles the ceiling.

   MARCH_STEP_BUDGET is the ceiling and Defines.hlsl says why. */
static uint numStepsTaken = 0;

inline bool HasStepBudget() {
	return numStepsTaken < MARCH_STEP_BUDGET;
}

/* Step-count heatmap - RENDERING_PLAN.md phase 6.0. Uncomment, rebuild, and
   both voxel pixel shaders shade every fragment by how much marching it did
   rather than by what it hit: black at rest, then blue, green, yellow, red,
   and full white for a pixel that ran out of MARCH_STEP_BUDGET.

   This exists because the trigger for the Xid 109 hang is not identified, and
   a frame that takes seconds is one nobody can screenshot. The heatmap is the
   thing to leave running while reproducing it - white appearing anywhere names
   the geometry responsible in one look, which is what CLAUDE.md's "colour the
   suspects" note asks for. It is also how the budget itself was calibrated.

   Both shaders deliberately apply it to the miss path too: a ray that marches
   a long way and finds nothing is exactly the expensive kind, and it is
   invisible in the normal image. */
// #define MARCH_STEP_DEBUG

#ifdef MARCH_STEP_DEBUG
float4 GetStepHeatmap()
{
	float fLoad = saturate(float(numStepsTaken) / float(MARCH_STEP_BUDGET));

	/* blue -> green -> yellow -> red over the first three quarters, so the
	   ordinary range stays readable, then to white over the last one. */
	float3 v3Color = lerp(
		lerp(
			lerp(float3(0.0, 0.0, 0.5), float3(0.0, 1.0, 0.0), saturate(fLoad * 4.0)),
			float3(1.0, 1.0, 0.0),
			saturate(fLoad * 4.0 - 1.0)
		),
		float3(1.0, 0.0, 0.0),
		saturate(fLoad * 4.0 - 2.0)
	);

	v3Color = lerp(v3Color, float3(1.0, 1.0, 1.0), saturate(fLoad * 4.0 - 3.0));

	return float4(v3Color, 1.0);
}
#endif

inline uint PosToVoxelID(uint3 v3Position)
{
	return v3Position.x + v3Position.y * worldSize.x + worldSize.x * worldSize.y * v3Position.z;
}

inline float4 GetVoxel(float3 v3Position) {
	uint ID = PosToVoxelID(v3Position);
	
#ifdef __PSSL__
    uint uiColor = voxelWorldData[ID];

	return float4(
        0xFF & (uiColor),
        0xFF & (uiColor >> 8),
        0xFF & (uiColor >> 16),
        0xFF & (uiColor >> 24)
    ) / 255.0;
#else
	return voxelWorldData[ID];
#endif
}

inline bool IsInChunk(int3 v3Position) {
	return (
		v3Position.x >= 0.0 && v3Position.y >= 0.0 && v3Position.z >= 0.0 &&
		v3Position.x < worldSize.x && v3Position.y < worldSize.y && v3Position.z < worldSize.z
	);
}

/* --- Occupancy bricks (RENDERING_PLAN.md phase 2) -------------------------
   voxelBrickData holds one count of occupied voxels per BRICK_SIZE^3 block of
   the resident window, maintained on the CPU by VoxelBrickGrid. Only "is it
   zero" is read here; the count exists so that destroying one voxel can
   decrement its brick without rescanning the other 511.

   Linearization is the voxel convention (rule 4) scaled down:
   x + y*B.x + z*B.x*B.y with B = ceil(worldSize / BRICK_SIZE). The C++ side
   computes B the same way - VoxelBrickGrid::Resize. */

inline uint3 GetBrickGridSize() {
	return (worldSize.xyz + (BRICK_SIZE - 1)) >> BRICK_SHIFT;
}

inline uint PosToBrickID(int3 v3Brick) {
	uint3 v3Grid = GetBrickGridSize();
	return uint(v3Brick.x) + uint(v3Brick.y) * v3Grid.x + uint(v3Brick.z) * v3Grid.x * v3Grid.y;
}

inline bool IsBrickInWorld(int3 v3Brick) {
	int3 v3Grid = int3(GetBrickGridSize());

	return (
		v3Brick.x >= 0 && v3Brick.y >= 0 && v3Brick.z >= 0 &&
		v3Brick.x < v3Grid.x && v3Brick.y < v3Grid.y && v3Brick.z < v3Grid.z
	);
}

inline bool IsBrickOccupied(int3 v3Brick) {
	return voxelBrickData[PosToBrickID(v3Brick)] != 0;
}

float3 v3LessThan(float3 v3First, float3 v3Second) {
	return float3(v3First.x < v3Second.x, v3First.y < v3Second.y, v3First.z < v3Second.z);
}

float Sum(float3 v3Vector) {
	return dot(v3Vector, float3(1.0, 1.0, 1.0));
}

float GetDistanceToWorld(float3 v3Position, float3 v3InvDirection, float3 v3ChunkSize) {
	float3 tMin = (float3(0.0, 0.0, 0.0) - v3Position) * v3InvDirection;
    float3 tMax = (v3ChunkSize - v3Position) * v3InvDirection;
    
	float3 t1 = min(tMin, tMax);
    float3 t2 = max(tMin, tMax);
	
    float tNear = max(max(t1.x, t1.y), t1.z);
    float tFar = min(min(t2.x, t2.y), t2.z);
	
    return ((tNear <= tFar) ? tNear : -1.0);
}

// Returns a vector that is orthogonal to u.
float3 GetOrthogonal(float3 u){
	u = normalize(u);
	float3 v = float3(0.99146, 0.11664, 0.05832); // Pick any normalized vector.
	return abs(dot(u, v)) > 0.99999 ? cross(u, float3(0, 1, 0)) : cross(u, v);
}

/* Two-level DDA over the resident window: an outer walk that steps whole
   BRICK_SIZE^3 bricks and skips empty ones for free, and an inner voxel walk
   that only ever runs inside a brick the CPU says holds something.

   Both levels share the ray's *original* origin, so every hit-time value below
   - Distance, SmoothPosition, UV - stays origin-relative no matter which brick
   the fine walk restarted in. That is what makes restarting cheap: the fine
   DDA state is a closed-form function of (origin, direction, voxel), not an
   accumulation, so it can be re-derived at any point along the ray.

   Replaces the flat single-level walk that the 700-step cap and the shadow
   ray's 2x stride existed to bound - see RENDERING_PLAN.md phase 2. */
MarchResult MarchBricks(float3 v3Origin, float3 v3Direction, int iMaxBrickSteps) {
	float3 v3InvDirection = 1.0 / v3Direction;
	float3 v3SignedRayDirection = sign(v3Direction);

	MarchResult result;
	result.Color = float4(CLEAR_COLOR, 0.0);

	/* Brick space is world space scaled by 1/BRICK_SIZE, so its cells are unit
	   sized and the same DDA applies; a parameter t in brick space is
	   BRICK_SIZE * t in world space. */
	float3 v3BrickOrigin = v3Origin * BRICK_INV_SIZE;
	int3 v3Brick = int3(floor(v3BrickOrigin));

	float3 v3BrickDistance = (float3(v3Brick) - v3BrickOrigin + 0.5 + v3SignedRayDirection * 0.5) * v3InvDirection;
	float3 v3BrickMask = float3(0.0, 0.0, 0.0);

	/* World-space parameter at which the ray entered v3Brick. Unused for the
	   brick the ray starts inside. */
	float fBrickEntry = 0.0;
	bool bFirstBrick = true;

	for (int b = 0; b < iMaxBrickSteps; b++) {
		if (!IsBrickInWorld(v3Brick))
			break;

		/* Out of budget. Falling out of the loop reports a miss, which is the
		   same answer this already gives a ray that uses up iMaxBrickSteps. */
		if (!HasStepBudget())
			break;

		if (IsBrickOccupied(v3Brick)) {
			int3 v3Position;
			float3 v3Mask;

			if (bFirstBrick) {
				v3Position = int3(floor(v3Origin));

				/* A march that begins *inside* an occupied voxel has no entry
				   face, so there is no normal to report. The DDA below would
				   derive one anyway, from the next crossing rather than a
				   crossing already made - and for an origin sitting on a voxel
				   face, the nearest crossing is that same face. The result is a
				   horizontal normal on a piece of floor.

				   That is what draws the strip along the resident window's
				   boundary. VoxelRenderer.vs.hlsl clamps the proxy cube's
				   surface to worldSize, so the edge fragments start the march
				   exactly on the window's face and immediately inside the
				   ground layer. Every pixel there came out +/-X or +/-Z where
				   its neighbours were +Y: dark, because it faces away from the
				   light, with GetShineLine's vertical-wall branch adding a lit
				   rim along the top.

				   This is the place for the test rather than MarchDiffuse's
				   out-of-window path, which only sees the fragments whose
				   clamped origin floors to *outside* the window. The ones that
				   land a fraction inside come straight here, and fixing only
				   the first turned the strip into a dashed version of itself.

				   Reporting a miss hands the pixel to the background, which is
				   the honest answer at a boundary that is a cut through the
				   world rather than a surface in it: the endless ground plane
				   and the far-field volume both already draw what is on the
				   other side, so the world continues instead of ending in a
				   wall. */
				if (IsInChunk(v3Position) && GetVoxel(float3(v3Position)).a > 0.0) {
					result.Color = float4(CLEAR_COLOR, 0.0);
					return result;
				}
			}
			else {
				/* Reconstruct the entry point from the crossing parameter. It
				   lands exactly on a brick face, so floor() of it can name the
				   voxel on the far side for a negative direction component -
				   and float error can push it either way. Clamping into the
				   brick fixes both: the voxel it would otherwise pick belongs
				   to a brick this walk has already cleared as empty. */
				float3 v3Entry = v3Origin + v3Direction * fBrickEntry;

				v3Position = clamp(
					int3(floor(v3Entry)),
					v3Brick << BRICK_SHIFT,
					(v3Brick << BRICK_SHIFT) + (BRICK_SIZE - 1)
				);
			}

			float3 v3Distance = (float3(v3Position) - v3Origin + 0.5 + v3SignedRayDirection * 0.5) * v3InvDirection;

			/* The face the ray crossed to enter the brick is the same face it
			   crosses to enter that brick's first voxel, so the outer mask is
			   the correct normal for a hit on the very first sample. */
			v3Mask = bFirstBrick
				? step(v3Distance.xyz, v3Distance.yxy) * step(v3Distance.xyz, v3Distance.zzx)
				: v3BrickMask;

			for (int v = 0; v < BRICK_MAX_VOXEL_STEPS; v++) {
				/* An edge brick can cover voxels past worldSize when the window
				   is not a whole number of bricks. The window is a box, so a ray
				   that leaves it never re-enters - this ends the whole march,
				   not just the inner walk. */
				if (!IsInChunk(v3Position)) {
					result.Color = float4(CLEAR_COLOR, 0.0);
					return result;
				}

				result.Color = GetVoxel(v3Position);

				if (result.Color.a > 0) {
					v3Distance = (float3(v3Position) - v3Origin + 0.5 - v3SignedRayDirection * 0.5) * v3InvDirection;
					result.Distance = max(v3Distance.x, max(v3Distance.y, v3Distance.z));
					result.SmoothPosition = v3Origin + v3Direction * result.Distance;

					result.Position = v3Position;
					result.Normal = -v3Mask * v3SignedRayDirection;

					float3 v3IntersectPlane = float3(v3Position) + v3LessThan(v3Direction, float3(0, 0, 0));
					float3 v3EndRayPos = v3Direction / Sum(v3Mask * v3Direction) * Sum(v3Mask * (v3IntersectPlane - v3Origin)) + v3Origin;

					result.UV = mod(
						float2(
							dot(v3Mask * v3EndRayPos.zxy, float3(1.0, 1.0, 1.0)),
							dot(v3Mask * v3EndRayPos.yzx, float3(1.0, 1.0, 1.0))
						),
						float2(1.0, 1.0)
					);

					if (abs(result.Normal.b) > 0.5)
						result.UV = float2(result.UV.y, result.UV.x);

					result.Mask = v3Mask;
					result.SRDirection = v3SignedRayDirection;

					return result;
				}

				v3Mask = step(v3Distance.xyz, v3Distance.yxy) * step(v3Distance.xyz, v3Distance.zzx);
				v3Distance += v3Mask * v3SignedRayDirection * v3InvDirection;
				v3Position += int3(v3Mask * v3SignedRayDirection);

				numStepsTaken++;

				if (any((v3Position >> BRICK_SHIFT) != v3Brick))
					break;

				/* The outer loop re-tests this and breaks too; leaving the
				   inner walk first is what stops a single dense brick from
				   overrunning the budget by up to BRICK_MAX_VOXEL_STEPS. */
				if (!HasStepBudget())
					break;
			}
		}

		/* A brick crossing is cheaper than a voxel one but not free, and a ray
		   can spend the whole of iMaxBrickSteps here without descending once.
		   Counting both into one budget is what makes the bound hold for every
		   shape of ray rather than only the dense ones. */
		numStepsTaken++;

		/* Step to the next brick. The crossing parameter has to be read before
		   the step, since that is where the next brick is entered. */
		v3BrickMask = step(v3BrickDistance.xyz, v3BrickDistance.yxy) * step(v3BrickDistance.xyz, v3BrickDistance.zzx);
		fBrickEntry = min(v3BrickDistance.x, min(v3BrickDistance.y, v3BrickDistance.z)) * BRICK_SIZE_F;

		v3BrickDistance += v3BrickMask * v3SignedRayDirection * v3InvDirection;
		v3Brick += int3(v3BrickMask * v3SignedRayDirection);

		bFirstBrick = false;
	}

	result.Color = float4(CLEAR_COLOR, 0.0);
	return result;
}

/* Directional shadow, as a transmittance in [0, 1] rather than a hit result.
   1 is fully lit, 0 fully shadowed.

   The same two-level walk MarchBricks does, with everything a shading hit
   needs stripped out - no normal, no UV, no smooth position, and no colour
   fetched once occupancy is known. All it answers is "does anything stand
   between this surface and the sun", which is why it is a separate function
   rather than a call into MarchBricks: that one computes a hit's whole
   surface frame, and none of it survives the caller's `is it occluded` test.

   There is deliberately no distance fade. The fade existed to disguise a
   binary answer by brightening shadows away from their occluder, and it was
   bright enough at range to read as AO being cancelled out near shadow edges
   (RENDERING_PLAN.md phase 1's deferred item, retuned in phase 2 and now
   simply gone). A shadowed surface is the ambient term and nothing else. */
float MarchShadow(float3 v3Origin, float3 v3Direction, int iMaxBrickSteps) {
	float3 v3InvDirection = 1.0 / v3Direction;
	float3 v3SignedRayDirection = sign(v3Direction);

	float3 v3BrickOrigin = v3Origin * BRICK_INV_SIZE;
	int3 v3Brick = int3(floor(v3BrickOrigin));

	float3 v3BrickDistance = (float3(v3Brick) - v3BrickOrigin + 0.5 + v3SignedRayDirection * 0.5) * v3InvDirection;

	/* World-space parameter at which the ray entered v3Brick; 0 for the brick
	   it starts inside. */
	float fBrickEntry = 0.0;
	bool bFirstBrick = true;

	for (int b = 0; b < iMaxBrickSteps; b++) {
		if (!IsBrickInWorld(v3Brick))
			break;

		/* Out of budget. Reporting lit is the same answer the primary ray's
		   miss gives - both are wrong pixels and both are cheap. */
		if (!HasStepBudget())
			break;

		if (IsBrickOccupied(v3Brick)) {
			int3 v3Position;

			if (bFirstBrick) {
				v3Position = int3(floor(v3Origin));

				/* The ray starts one unit off the surface toward the light, and
				   for a wall that unit can round back into the voxel being
				   shaded. Treating an occupied origin voxel as lit is what the
				   MarchBricks walk this replaces did - it reported a miss - and
				   it is what keeps a lit wall from shadowing itself. */
				if (IsInChunk(v3Position) && GetVoxel(float3(v3Position)).a > 0.0)
					return 1.0;
			}
			else {
				/* Same reconstruction as MarchBricks: the entry point lands
				   exactly on a brick face, where floor() can name the voxel on
				   the far side for a negative direction component, so clamp it
				   into the brick this walk has actually reached. */
				float3 v3Entry = v3Origin + v3Direction * fBrickEntry;

				v3Position = clamp(
					int3(floor(v3Entry)),
					v3Brick << BRICK_SHIFT,
					(v3Brick << BRICK_SHIFT) + (BRICK_SIZE - 1)
				);
			}

			float3 v3Distance = (float3(v3Position) - v3Origin + 0.5 + v3SignedRayDirection * 0.5) * v3InvDirection;

			for (int v = 0; v < BRICK_MAX_VOXEL_STEPS; v++) {
				/* Leaving the window ends the whole march: it is a box, so a
				   ray that leaves never re-enters. Nothing outside it can cast
				   into it either - the endless ground plane is flat. */
				if (!IsInChunk(v3Position))
					return 1.0;

				if (GetVoxel(float3(v3Position)).a > 0.0)
					return 0.0;

				float3 v3Mask = step(v3Distance.xyz, v3Distance.yxy) * step(v3Distance.xyz, v3Distance.zzx);
				v3Distance += v3Mask * v3SignedRayDirection * v3InvDirection;
				v3Position += int3(v3Mask * v3SignedRayDirection);

				numStepsTaken++;

				if (any((v3Position >> BRICK_SHIFT) != v3Brick))
					break;

				if (!HasStepBudget())
					break;
			}
		}

		numStepsTaken++;

		/* Step to the next brick. The crossing parameter has to be read before
		   the step, since that is where the next brick is entered. */
		float3 v3BrickMask = step(v3BrickDistance.xyz, v3BrickDistance.yxy) * step(v3BrickDistance.xyz, v3BrickDistance.zzx);
		fBrickEntry = min(v3BrickDistance.x, min(v3BrickDistance.y, v3BrickDistance.z)) * BRICK_SIZE_F;

		v3BrickDistance += v3BrickMask * v3SignedRayDirection * v3InvDirection;
		v3Brick += int3(v3BrickMask * v3SignedRayDirection);

		bFirstBrick = false;
	}

	return 1.0;
}

/* Fraction of the sun's disc this point can see, sampled by shooting
   SHADOW_CONE_SAMPLES voxel-exact rays into a cone around the light. The
   caller averages across the 2x2 quad on top, so a quad resolves
   SHADOW_CONE_SAMPLES * 4 directions.

   Why a cone of rays rather than blurring a single hard shadow: the penumbra
   then widens with distance to the occluder on its own, because a cone of
   fixed angle diverges. A box on the floor still casts a hard line where it
   touches, and a tower's shadow across the valley is soft. A post-hoc blur
   cannot do that without knowing the occluder distance, and a distance fade -
   which is what this engine used to do - fakes it with brightness instead,
   which is not the same thing and looks it.

   **This is the reference implementation, not the shipping one.** It is
   ground truth - every sample is an exact occlusion test - and it is priced
   accordingly: smoothness costs rays linearly, and one ray per pixel is
   ~9.45 ms at 3840x2160 against a 3 ms budget for all lighting. The sun is a
   shadow map now (SunShadowLookup.hlsl); this stays so the map can be A/B'd
   against something known-correct. SUN_SHADOW_REFERENCE switches between them.

   The directions are a golden-angle spiral over the disc - sqrt radius for
   equal-area coverage, 2.39996 rad between consecutive points - rotated per
   *pixel* by interleaved gradient noise, so what is left of the quantization
   breaks up as fine dither rather than as visible steps.

   An earlier version fired one ray per pixel and averaged the four lanes of
   each 2x2 quad together, which buys four times the directions for free. Two
   things were wrong with it and the second is why it is gone: four directions
   is five discrete levels and reads as four separate shadows rather than one
   soft edge; and averaging across a quad is a 2x2 box filter on the shadow
   term, which is invisible on a broad penumbra and destroys most of the
   contrast of a shadow only a pixel or two wide. Thin occluders - railings,
   fence posts, beams - are exactly the geometry that costs. Sampling stays
   per-pixel now, and the smoothness comes from sample count instead. */
float GetSunVisibilityByRays(float3 v3Origin, float2 v2ScreenPosition, int iMaxBrickSteps) {
	float3 v3Light = -lightDirection.xyz;

	float3 v3Tangent = normalize(GetOrthogonal(v3Light));
	float3 v3Bitangent = cross(v3Light, v3Tangent);

	/* Interleaved gradient noise, so neighbouring pixels rotate their sample
	   set differently and the residual banding becomes dither. */
	float fRotation = frac(52.9829189 * frac(dot(v2ScreenPosition, float2(0.06711056, 0.00583715)))) * 6.2831853;

	float fSum = 0.0;

	for (uint i = 0; i < SHADOW_CONE_SAMPLES; i++) {
		float fRadius = sqrt((float(i) + 0.5) / float(SHADOW_CONE_SAMPLES));
		float fAngle = float(i) * 2.39996323 + fRotation;

		float2 v2Offset = float2(cos(fAngle), sin(fAngle)) * fRadius * SHADOW_CONE_SPREAD;

		fSum += MarchShadow(
			v3Origin,
			normalize(v3Light + v3Tangent * v2Offset.x + v3Bitangent * v2Offset.y),
			iMaxBrickSteps
		);
	}

	return fSum / float(SHADOW_CONE_SAMPLES);
}

MarchResult MarchDiffuse(float3 v3Origin, float3 v3Direction, int iMaxBrickSteps) {
	/* Callers may pass an origin outside worldSize bounds - the AABB proxy
	   cubes are clamped to the window, but a camera sitting on the boundary
	   still produces one. MarchBricks' own bounds tests cover the walk; this
	   covers the origin voxel it starts from.

	   Entering the window and marching from there - rather than reporting a
	   miss, as this did - is what keeps a grazing ray from punching holes
	   along the window's silhouette. */
	if (!IsInChunk(int3(floor(v3Origin)))) {
		MarchResult miss;
		miss.Color = float4(CLEAR_COLOR, 0.0);

		float fToWorld = GetDistanceToWorld(v3Origin, 1.0 / v3Direction, float3(worldSize.xyz));

		/* Negative means the window is behind the ray, or it misses entirely. */
		if (fToWorld < 0.0)
			return miss;

		float3 v3Entry = clamp(
			v3Origin + v3Direction * fToWorld,
			float3(WORLD_ENTRY_EPSILON, WORLD_ENTRY_EPSILON, WORLD_ENTRY_EPSILON),
			float3(worldSize.xyz) - WORLD_ENTRY_EPSILON
		);

		if (!IsInChunk(int3(floor(v3Entry))))
			return miss;

		MarchResult result = MarchBricks(v3Entry, v3Direction, iMaxBrickSteps);

		/* Distance stays relative to the origin the caller passed;
		   SmoothPosition is absolute already. */
		if (result.Color.a > 0.0)
			result.Distance += fToWorld;

		return result;
	}

	return MarchBricks(v3Origin, v3Direction, iMaxBrickSteps);
}

/* Single-level walk, kept as-is. Its only caller is SDFDepth.ps.hlsl, which
   compiles but is loaded by nothing (see RENDERING_PLAN.md, "Ground truth") -
   there was no behaviour to preserve by porting it onto the brick walk, and
   no way to verify the port either. */
MarchResult March(float3 v3Origin, float3 v3Direction, float3 v3ChunkSize, int iMaxSteps, bool bDetailedResult) {
	float3 v3InvDirection = 1.0 / v3Direction;

	float fDistanceToWorld = GetDistanceToWorld(v3Origin, v3InvDirection, v3ChunkSize);
	bool bIsInChunk = IsInChunk(v3Origin);
	
	MarchResult result;
		
	if (bIsInChunk || fDistanceToWorld >= 0.0)
	{
		v3Origin += !bIsInChunk * (fDistanceToWorld * v3Direction);
		
		int3 v3Position = floor(v3Origin);
		float3 v3SignedRayDirection = sign(v3Direction);
		float3 v3Distance = (v3Position - v3Origin + 0.5 + v3SignedRayDirection * 0.5) * v3InvDirection;
		float3 v3Mask = step(v3Distance.xyz, v3Distance.yxy) * step(v3Distance.xyz, v3Distance.zzx);
		
		for (int i = 0; i < iMaxSteps; i++) {	
			result.Color = GetVoxel(v3Position);
			
			if (result.Color.a > 0)
			{
				v3Distance = (v3Position - v3Origin + 0.5 - v3SignedRayDirection * 0.5) * v3InvDirection;
				result.Distance = max(v3Distance.x, max(v3Distance.y, v3Distance.z));
	
				result.SmoothPosition = v3Origin + v3Direction * result.Distance;

				if (bDetailedResult) {
					result.Position = v3Position;
					result.SmoothPosition = v3Origin + v3Direction * result.Distance;
			
					result.Normal = -v3Mask * v3SignedRayDirection;
					
					float3 v3IntersectPlane = v3Position + v3LessThan(v3Direction, float3(0, 0, 0));
					float3 v3EndRayPos = v3Direction / Sum(v3Mask * v3Direction) * Sum(v3Mask * (v3IntersectPlane - v3Origin)) + v3Origin;
					
					result.UV = mod(
						float2(
							dot(v3Mask * v3EndRayPos.zxy, float3(1.0, 1.0, 1.0)),
							dot(v3Mask * v3EndRayPos.yzx, float3(1.0, 1.0, 1.0))
						),
						float2(1.0, 1.0)
					);
					
					if (abs(result.Normal.b) > 0.5)
						result.UV = float2(result.UV.y, result.UV.x);
					
					result.Mask = v3Mask;
					result.SRDirection = v3SignedRayDirection;
				}
				
				return result;
			}
			
			v3Mask = step(v3Distance.xyz, v3Distance.yxy) * step(v3Distance.xyz, v3Distance.zzx);
			v3Distance += v3Mask * v3SignedRayDirection * v3InvDirection;
			v3Position += v3Mask * v3SignedRayDirection;

			numStepsTaken++;
			
			if (!IsInChunk(v3Position))
				break;
		}
	}
	
	result.Color = float4(CLEAR_COLOR, 0.0);
	return result;
}