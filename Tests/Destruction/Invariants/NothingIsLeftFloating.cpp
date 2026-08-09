#include <string>

#include "Framework/Invariant.h"

/* The connectivity oracle, and the phrasing took three attempts to get right.
 *
 * "Nothing ungrounded may be left standing" is wrong as an absolute: a level
 * legitimately contains geometry that never touched the ground - a pristine
 * Fishing_Village has 14,532 such voxels in 211 components - and nothing should
 * ever seed it.
 *
 * "No more ungrounded *voxels* than at the start" is wrong too, and more
 * subtly: debris that settles on a permanently floating shelf adds voxels to a
 * component that was always there, which is a particle coming to rest on
 * something solid rather than a missed island.
 *
 * The question that discriminates is whether a *new component* appeared.
 * Debris joining an existing floating component is fine; debris forming its own
 * is the floating-debris defect, and a structure the checker failed to convert
 * is a new component too.
 */
class NothingIsLeftFloating : public Invariant
{
public:
	const char* Name() const override { return "nothing-left-floating"; }

	std::string Check(const DestructionResult& result) const override
	{
		if (result.uiStandingComponents > result.uiStandingComponentsAtStart)
		{
			return std::to_string(result.uiStandingComponents - result.uiStandingComponentsAtStart) +
				" new ungrounded components are standing (" + std::to_string(result.uiStandingComponents) +
				" holding " + std::to_string(result.uiStanding) + " voxels, was " +
				std::to_string(result.uiStandingComponentsAtStart) + ") - either the checker missed an "
				"island or debris was left in mid-air";
		}

		/* The other direction, for scenarios whose floating geometry is meant
		   to be untouched: collapsing it means seeding reached something the
		   bursts never went near. */
		if (result.uiStanding < result.uiMinStanding)
		{
			return std::to_string(result.uiMinStanding - result.uiStanding) +
				" voxels of untouched ungrounded geometry were collapsed - seeding reached "
				"something no burst went near";
		}

		return std::string();
	}
};

VOXAGINE_INVARIANT(NothingIsLeftFloating)
