#include <string>

#include "Framework/Invariant.h"

/* The particle core's sparse slot table and its dense arrays agree in both
   directions, and every slot is accounted for exactly once.
 *
 * The pool this replaced was a union of live state and a free-list link, so a
 * retired particle was drawn one more frame from its own link reinterpreted as
 * a float3. A structure that can be audited is what stops that shape returning. */
class ParticlePoolIsSound : public Invariant
{
public:
	const char* Name() const override { return "particle-pool-sound"; }

	std::string Check(const DestructionResult& result) const override
	{
		if (result.bPoolSound)
			return std::string();

		return "the particle core's slot table and dense arrays disagree";
	}
};

VOXAGINE_INVARIANT(ParticlePoolIsSound)
