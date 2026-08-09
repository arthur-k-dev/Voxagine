#include "Framework/Benchmark.h"
#include "Framework/Check.h"
#include "Framework/Invariant.h"
#include "Framework/Scenario.h"

/* Function-local statics, not namespace-scope objects: a check, scenario,
   invariant or benchmark in another translation unit registers itself from a
   static initialiser, and whether the registry object had been constructed by
   then would otherwise be link order. */

CheckRegistry& CheckRegistry::Get()
{
	static CheckRegistry s_Registry;
	return s_Registry;
}

bool CheckRegistry::Add(std::unique_ptr<Check> pCheck)
{
	m_Checks.push_back(std::move(pCheck));
	return true;
}

void CheckContext::Fail(const char* pFile, int iLine, const std::string& detail)
{
	/* Only the last two path components: the full build path is noise, and the
	   system directory plus the file name is exactly what identifies it. */
	std::string file(pFile);

	size_t uiCut = file.find_last_of("/\\");

	if (uiCut != std::string::npos && uiCut > 0)
	{
		const size_t uiPrevious = file.find_last_of("/\\", uiCut - 1);

		if (uiPrevious != std::string::npos)
			uiCut = uiPrevious;

		file = file.substr(uiCut + 1);
	}

	m_Failures.push_back(file + ":" + std::to_string(iLine) + ": " + detail);
}

ScenarioRegistry& ScenarioRegistry::Get()
{
	static ScenarioRegistry s_Registry;
	return s_Registry;
}

bool ScenarioRegistry::Add(std::unique_ptr<Scenario> pScenario)
{
	m_Scenarios.push_back(std::move(pScenario));
	return true;
}

InvariantRegistry& InvariantRegistry::Get()
{
	static InvariantRegistry s_Registry;
	return s_Registry;
}

bool InvariantRegistry::Add(std::unique_ptr<Invariant> pInvariant)
{
	m_Invariants.push_back(std::move(pInvariant));
	return true;
}

BenchmarkRegistry& BenchmarkRegistry::Get()
{
	static BenchmarkRegistry s_Registry;
	return s_Registry;
}

bool BenchmarkRegistry::Add(std::unique_ptr<Benchmark> pBenchmark)
{
	m_Benchmarks.push_back(std::move(pBenchmark));
	return true;
}
