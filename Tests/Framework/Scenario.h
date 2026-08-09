#pragma once

#include <memory>
#include <vector>

#include "Harness/DestructionRun.h"

/* A world with a shape somebody chose, and a script fired at it.
 *
 * Every one of the four defects the first play session found was a *scenario
 * that did not exist* rather than an assertion that was too weak - no test had
 * a dynamic renderer's voxels in it, or geometry that was ungrounded by design,
 * or an indestructible owner, or a diagonal-only support. So adding a case has
 * to be as cheap as possible: one file, one class, one registration line, no
 * edit to the runner.
 *
 * Scenarios live under Tests/Destruction/Scenarios/ and are named for the
 * situation they set up, not for the code they happen to reach.
 */
class Scenario
{
public:
	virtual ~Scenario() = default;

	virtual const char* Name() const = 0;

	/* Window size, tick count, seed. Defaults are usually fine. */
	virtual void Configure(DestructionConfig& config) const { (void)config; }

	virtual void Build(VoxelWorldHarness& world, const DestructionConfig& config) const = 0;
	virtual void Script(const DestructionConfig& config, std::vector<Burst>& o_bursts) const = 0;

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
class ScenarioRegistry
{
public:
	static ScenarioRegistry& Get();

	bool Add(std::unique_ptr<Scenario> pScenario);

	const std::vector<std::unique_ptr<Scenario>>& Scenarios() const { return m_Scenarios; }

private:
	std::vector<std::unique_ptr<Scenario>> m_Scenarios;
};

#define VOXAGINE_SCENARIO(Type) \
	namespace { const bool s_bVoxScenario_##Type = \
		ScenarioRegistry::Get().Add(std::unique_ptr<Scenario>(new Type())); }
