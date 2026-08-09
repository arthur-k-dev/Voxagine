#include "Selftest/SelftestInvariant.h"
#include "Selftest/SelftestScenario.h"

/* Function-local statics, not namespace-scope objects: a scenario in another
   translation unit registers itself from a static initialiser, and whether the
   registry object had been constructed by then would otherwise be link order. */
SelftestRegistry& SelftestRegistry::Get()
{
	static SelftestRegistry s_Registry;
	return s_Registry;
}

bool SelftestRegistry::Add(std::unique_ptr<SelftestScenario> pScenario)
{
	m_Scenarios.push_back(std::move(pScenario));
	return true;
}

SelftestInvariantRegistry& SelftestInvariantRegistry::Get()
{
	static SelftestInvariantRegistry s_Registry;
	return s_Registry;
}

bool SelftestInvariantRegistry::Add(std::unique_ptr<SelftestInvariant> pInvariant)
{
	m_Invariants.push_back(std::move(pInvariant));
	return true;
}
