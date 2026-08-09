#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Selftest/SelftestWorld.h"

/* Something that must be true of every run, of every scenario.
 *
 * Separate from the scenarios on purpose: a new invariant should apply to every
 * case that already exists without touching any of them, and a new scenario
 * should be checked by every invariant without knowing they exist.
 */
class SelftestInvariant
{
public:
	virtual ~SelftestInvariant() = default;

	virtual const char* Name() const = 0;

	/* Empty string means it held. Anything else is the failure, phrased so a
	   reader who has never seen this file knows what broke. */
	virtual std::string Check(const SelftestResult& result) const = 0;
};

class SelftestInvariantRegistry
{
public:
	static SelftestInvariantRegistry& Get();

	bool Add(std::unique_ptr<SelftestInvariant> pInvariant);

	const std::vector<std::unique_ptr<SelftestInvariant>>& Invariants() const { return m_Invariants; }

private:
	std::vector<std::unique_ptr<SelftestInvariant>> m_Invariants;
};

#define VOXAGINE_SELFTEST_INVARIANT(Type) \
	namespace { const bool s_bRegisteredInvariant_##Type = \
		SelftestInvariantRegistry::Get().Add(std::unique_ptr<SelftestInvariant>(new Type())); }
