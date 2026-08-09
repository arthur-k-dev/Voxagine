#include "Framework/Check.h"

#include <cmath>

#include "Core/Math.h"
#include "Core/Utils/Utils.h"

/* The oldest tests in the tree, from the original coursework suite. They sat in
   the gtest entry point rather than in a test file, so they were never listed in
   the build and had not run since the port began. Ported here because they are
   still true and still cheap. */

VOXAGINE_CHECK(MathUtils, NormalizeProducesUnitLength)
{
	CHECK_NEAR(glm::length(glm::normalize(Vector3(1.f, 1.f, 1.f))), 1.f, 1e-5);
}

VOXAGINE_CHECK(MathUtils, AverageDividesInPlace)
{
	Vector3 v3Sum = Vector3(2.f, 4.f, 8.f) + Vector3(5.f, 10.f, 16.f) + Vector3(5.f, 10.f, 16.f);

	Utils::Average(v3Sum, 3);

	CHECK_NEAR(v3Sum.x, 4.f, 1e-4);
	CHECK_NEAR(v3Sum.y, 8.f, 1e-4);
	CHECK_NEAR(v3Sum.z, 13.3333f, 1e-3);
}

VOXAGINE_CHECK(MathUtils, LengthCountsArrayElements)
{
	const int elements[] = { 1, 2, 3, 4, 5 };

	CHECK_EQ(Utils::Length(elements), 5u);
}

VOXAGINE_CHECK(MathUtils, ClampHoldsTheBounds)
{
	CHECK_EQ(Utils::Clamp(0u, 1u, 3u), 1u);
	CHECK_EQ(Utils::Clamp(2u, 1u, 3u), 2u);
	CHECK_EQ(Utils::Clamp(9u, 1u, 3u), 3u);
}
