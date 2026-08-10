#ifndef VOXAGINE_VECTORMATH_HLSL
#define VOXAGINE_VECTORMATH_HLSL

/* Pure vector helpers with no dependency on the voxel buffer or any binding -
 * split out of SDFMarcher.hlsl (DYNAMIC_MODELS_PLAN.md phase 2) because
 * AmbientCone.hlsl needs GetOrthogonal for its tangent frame but the dynamic
 * model pass has no world to march and must not declare voxelWorldData/
 * voxelBrickData bindings it never reads just to satisfy that dependency. */

// Returns a vector that is orthogonal to u.
float3 GetOrthogonal(float3 u){
	u = normalize(u);
	float3 v = float3(0.99146, 0.11664, 0.05832); // Pick any normalized vector.
	return abs(dot(u, v)) > 0.99999 ? cross(u, float3(0, 1, 0)) : cross(u, v);
}

/* Standard quaternion vector rotation (q * v * q^-1 for a unit q, expanded).
 * DYNAMIC_MODELS_PLAN.md phase 2: the dynamic model pass's vertex shader is
 * the first thing in this engine that rotates geometry by a free (not
 * axis-quantized) transform on the GPU, so nothing like it existed yet. */
float3 QuatRotate(float4 q, float3 v)
{
	float3 t = 2.0 * cross(q.xyz, v);
	return v + q.w * t + cross(q.xyz, t);
}

#endif
