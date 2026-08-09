#include <string>

#include "Selftest/SelftestInvariant.h"

/* One class per thing that must be true. They are in one file because each is
   four lines; splitting them further would be filing, not structure. Adding one
   is adding a class and a registration line - no runner or scenario changes. */

namespace
{
	std::string Count(uint64_t uiValue) { return std::to_string(uiValue); }
}

/* Rule 3: a voxel exists in four places and a write maintains all of them. A
   path that updates some is the whole reason VoxelEditBatch exists. */
class RepresentationInvariant : public SelftestInvariant
{
public:
	const char* Name() const override { return "representation"; }

	std::string Check(const SelftestResult& result) const override
	{
		if (result.uiRepresentation == 0)
			return std::string();

		return Count(result.uiRepresentation) +
			" voxels disagree between the mapped words, the occupancy bitmap and the brick counts";
	}
};

VOXAGINE_SELFTEST_INVARIANT(RepresentationInvariant)

/* An indestructible entity's voxels are never cleared, by any path. Island
   conversion did not check this and never had, which only stopped being
   harmless once the seed set widened enough to reach such geometry. */
class ProtectionInvariant : public SelftestInvariant
{
public:
	const char* Name() const override { return "protection"; }

	std::string Check(const SelftestResult& result) const override
	{
		if (result.uiProtectedCleared == 0)
			return std::string();

		return Count(result.uiProtectedCleared) + " of " + Count(result.uiProtectedAtStart) +
			" voxels belonging to an indestructible owner were cleared";
	}
};

VOXAGINE_SELFTEST_INVARIANT(ProtectionInvariant)

/* The particle core's sparse slot table and its dense arrays agree in both
   directions, and every slot is accounted for exactly once. */
class PoolInvariant : public SelftestInvariant
{
public:
	const char* Name() const override { return "pool"; }

	std::string Check(const SelftestResult& result) const override
	{
		if (result.bPoolSound)
			return std::string();

		return "the particle core's slot table and dense arrays disagree";
	}
};

VOXAGINE_SELFTEST_INVARIANT(PoolInvariant)

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
 * is a new component too. */
class OracleInvariant : public SelftestInvariant
{
public:
	const char* Name() const override { return "oracle"; }

	std::string Check(const SelftestResult& result) const override
	{
		if (result.uiStandingComponents > result.uiStandingComponentsAtStart)
		{
			return Count(result.uiStandingComponents - result.uiStandingComponentsAtStart) +
				" new ungrounded components are standing (" + Count(result.uiStandingComponents) +
				" holding " + Count(result.uiStanding) + " voxels, was " +
				Count(result.uiStandingComponentsAtStart) + ") - either the checker missed an "
				"island or debris was left in mid-air";
		}

		/* The other direction, for scenarios whose floating geometry is meant
		   to be untouched: collapsing it means seeding reached something the
		   bursts never went near. */
		if (result.uiStanding < result.uiMinStanding)
		{
			return Count(result.uiMinStanding - result.uiStanding) +
				" voxels of untouched ungrounded geometry were collapsed - seeding reached "
				"something no burst went near";
		}

		return std::string();
	}
};

VOXAGINE_SELFTEST_INVARIANT(OracleInvariant)

/* Same scenario twice, same world. Everything else here is worthless without
   it, because a run that cannot be reproduced cannot be bisected. */
class DeterminismInvariant : public SelftestInvariant
{
public:
	const char* Name() const override { return "determinism"; }

	std::string Check(const SelftestResult& result) const override
	{
		if (result.uiHash == result.uiRepeatHash)
			return std::string();

		return "two identical runs produced different state hashes";
	}
};

VOXAGINE_SELFTEST_INVARIANT(DeterminismInvariant)
