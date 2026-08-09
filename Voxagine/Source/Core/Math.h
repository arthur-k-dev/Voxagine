#pragma once
#include <cfloat>

/* Explicit, because <emmintrin.h> used to sit here and drag it in - so the
   float overloads of abs/floor/sqrt were visible to every translation unit
   that included this, by accident. Removing the SSE header for the arm64 build
   turned one call site into `int abs(int)` on a float, which compiles and
   silently truncates. Keeping <cmath> here keeps that from happening again to
   the other ninety-odd files that assume it. */
#include <cmath>

// GLM
#define GLM_FORCE_LEFT_HANDED
#define GLM_ENABLE_EXPERIMENTAL

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <External/glm/glm.hpp>
#include <External/glm/gtx/transform.hpp>
#include <External/glm/gtx/norm.hpp>
#include <External/glm/gtc/random.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

typedef glm::vec4 Vector4;
typedef glm::vec3 Vector3;
typedef glm::vec2 Vector2;

typedef glm::ivec4 IVector4;
typedef glm::ivec3 IVector3;
typedef glm::ivec2 IVector2;

typedef glm::uvec4 UVector4;
typedef glm::uvec3 UVector3;
typedef glm::uvec2 UVector2;

typedef glm::quat Quaternion;
typedef glm::mat4 Matrix4;

struct VColor
{
	VColor() { inst.Color = 0; }
	VColor(uint32_t uiColor) { inst.Color = uiColor; }
	VColor(unsigned char r, unsigned char g, unsigned char b, unsigned char a)
	{
		inst.Colors.r = r;
		inst.Colors.g = g;
		inst.Colors.b = b;
		inst.Colors.a = a;
	}

	VColor(float r, float g, float b, float a)
	{
		inst.Colors.r = static_cast<char>(255.f * r);
		inst.Colors.g = static_cast<char>(255.f * g);
		inst.Colors.b = static_cast<char>(255.f * b);
		inst.Colors.a = static_cast<char>(255.f * a);
	}
	VColor(Vector4 color)
	{
		inst.Colors.r = static_cast<char>(255.f * color.r);
		inst.Colors.g = static_cast<char>(255.f * color.g);
		inst.Colors.b = static_cast<char>(255.f * color.b);
		inst.Colors.a = static_cast<char>(255.f * color.a);
	}

	Vector4 GetVector4() const
	{
		Vector4 ret;
		ret.r = static_cast<float>(inst.Colors.r) / 255.f;
		ret.g = static_cast<float>(inst.Colors.g) / 255.f;
		ret.b = static_cast<float>(inst.Colors.b) / 255.f;
		ret.a = static_cast<float>(inst.Colors.a) / 255.f;
		return ret;
	}


	union RGBAUnion
	{
		uint32_t Color;
		struct RBGAColor
		{
			unsigned char r, g, b, a;
		} Colors;
	} inst;
};

/* Truncating float->int. This used to be _mm_cvtt_ss2si, which is exactly what
   a C cast compiles to on x86 anyway - and the <emmintrin.h> it needed was the
   single thing stopping this header, and so most of the engine, from compiling
   for arm64. It has no callers; kept because the name is referenced in the
   original coursework notes. */
inline int ftoi_sse1(float f)
{
	return static_cast<int>(f);
}
/* glm::normalize divides by the length, so normalizing a zero-length vector
   produces NaN rather than failing. That NaN then spreads through whatever it
   feeds - a transform, a knockback, a facing direction - and the entity
   silently stops rendering, which is a long way from where the mistake was.
   Zero-length inputs are ordinary here: a monster standing still has zero
   velocity, and a collision can report a zero impact normal. */
inline Vector3 SafeNormalize(const Vector3& v, const Vector3& fallback = Vector3(0.f, 0.f, 0.f))
{
	const float fLengthSquared = glm::dot(v, v);

	if (fLengthSquared <= FLT_MIN)
		return fallback;

	return v * glm::inversesqrt(fLengthSquared);
}
