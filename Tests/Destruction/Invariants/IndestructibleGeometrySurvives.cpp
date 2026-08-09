#include <string>

#include "Framework/Invariant.h"

/* An indestructible entity's voxels are never cleared, by any path.
 *
 * Island conversion did not check this and never had, which only stopped being
 * harmless once the seed set widened enough to reach such geometry - and then a
 * bullet clipping some rubble collapsed a whole non-destructible building. */
class IndestructibleGeometrySurvives : public Invariant
{
public:
	const char* Name() const override { return "indestructible-survives"; }

	std::string Check(const DestructionResult& result) const override
	{
		if (result.uiProtectedCleared == 0)
			return std::string();

		return std::to_string(result.uiProtectedCleared) + " of " +
			std::to_string(result.uiProtectedAtStart) +
			" voxels belonging to an indestructible owner were cleared";
	}
};

VOXAGINE_INVARIANT(IndestructibleGeometrySurvives)
