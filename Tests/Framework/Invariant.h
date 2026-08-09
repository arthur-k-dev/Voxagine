#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Harness/DestructionRun.h"

/* Something that must be true of every run, of every scenario.
 *
 * Separate from the scenarios on purpose: a new invariant should apply to every
 * case that already exists without touching any of them, and a new scenario
 * should be checked by every invariant without knowing they exist.
 *
 * One file per invariant, under Tests/Destruction/Invariants/, named for what
 * it asserts.
 */
class Invariant
{
public:
	virtual ~Invariant() = default;

	virtual const char* Name() const = 0;

	/* Empty string means it held. Anything else is the failure, phrased so a
	   reader who has never seen this file knows what broke. */
	virtual std::string Check(const DestructionResult& result) const = 0;
};

class InvariantRegistry
{
public:
	static InvariantRegistry& Get();

	bool Add(std::unique_ptr<Invariant> pInvariant);

	const std::vector<std::unique_ptr<Invariant>>& Invariants() const { return m_Invariants; }

private:
	std::vector<std::unique_ptr<Invariant>> m_Invariants;
};

#define VOXAGINE_INVARIANT(Type) \
	namespace { const bool s_bVoxInvariant_##Type = \
		InvariantRegistry::Get().Add(std::unique_ptr<Invariant>(new Type())); }
