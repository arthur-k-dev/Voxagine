#include <string>

#include "Framework/Invariant.h"

/* Same scenario twice, same world.
 *
 * Everything else here is worthless without it, because a run that cannot be
 * reproduced cannot be bisected. It is also why particle velocities come from a
 * seeded DeterministicRandom rather than glm::linearRand: the latter draws on a
 * process-global engine that every other caller perturbs, so a replay diverged
 * on the first unrelated call anywhere in the process. */
class RunsAreReproducible : public Invariant
{
public:
	const char* Name() const override { return "reproducible"; }

	std::string Check(const DestructionResult& result) const override
	{
		if (result.uiHash == result.uiRepeatHash)
			return std::string();

		return "two identical runs produced different state hashes";
	}
};

VOXAGINE_INVARIANT(RunsAreReproducible)
