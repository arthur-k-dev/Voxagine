#pragma once

#include <memory>
#include <vector>

#include "Selftest/SelftestWorld.h"

/* A world with a shape somebody chose, and a script fired at it.
 *
 * Every one of the four defects the first play session found was a *scenario
 * that did not exist* rather than an assertion that was too weak - no test had
 * a dynamic renderer's voxels in it, or geometry that was ungrounded by design,
 * or an indestructible owner, or a diagonal-only support. So adding a case has
 * to be as cheap as possible: one file, one class, one registration line, no
 * edit to the runner.
 */
class SelftestScenario
{
public:
	virtual ~SelftestScenario() = default;

	virtual const char* Name() const = 0;

	/* Window size, tick count, seed. Defaults are usually fine. */
	virtual void Configure(SelftestConfig& config) const { (void)config; }

	virtual void Build(VoxelWorldHarness& world, const SelftestConfig& config) const = 0;
	virtual void Script(const SelftestConfig& config, std::vector<Burst>& o_bursts) const = 0;

	/* How much ungrounded geometry must *still* be standing at the end.
	   Defaults to none required - a scenario whose floating decoration is meant
	   to survive untouched returns what it started with. */
	virtual uint64_t MinStandingAtEnd(uint64_t uiStandingAtStart) const
	{
		(void)uiStandingAtStart;
		return 0;
	}
};

/* Self-registering, so a scenario file is self-contained. The executable links
   these translation units directly rather than through a static archive, so
   nothing has to reference them for their initialisers to run - see the
   WHOLE_ARCHIVE note on BitBuster in CMakeLists.txt for what goes wrong when
   that is not true. */
class SelftestRegistry
{
public:
	static SelftestRegistry& Get();

	bool Add(std::unique_ptr<SelftestScenario> pScenario);

	const std::vector<std::unique_ptr<SelftestScenario>>& Scenarios() const { return m_Scenarios; }

private:
	std::vector<std::unique_ptr<SelftestScenario>> m_Scenarios;
};

#define VOXAGINE_SELFTEST_SCENARIO(Type) \
	namespace { const bool s_bRegistered_##Type = \
		SelftestRegistry::Get().Add(std::unique_ptr<SelftestScenario>(new Type())); }
