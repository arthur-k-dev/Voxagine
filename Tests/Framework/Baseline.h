#pragma once

#include <map>
#include <string>
#include <vector>

#include "Framework/Benchmark.h"

/* Recorded metrics, and the comparison against them.
 *
 * The file is one metric per line and nothing else:
 *
 *   <system>/<benchmark>/<metric>  <value>  <kind>  [unit]
 *
 * Plain text on purpose - it lands in a diff as a readable list of what got
 * faster and what got slower, which is most of the value of having it.
 */
class Baseline
{
public:
	struct Entry
	{
		double fValue = 0.0;
		MetricKind kind = MetricKind::Work;
	};

	struct Comparison
	{
		std::string key;
		std::string unit;

		MetricKind kind = MetricKind::Work;

		double fValue = 0.0;
		double fBaseline = 0.0;

		bool bHasBaseline = false;
		bool bRegressed = false;
		bool bImproved = false;
	};

	/* Work metrics are exact, so the only tolerance is for floating-point
	   representation of what are really integers. */
	static constexpr double k_fWorkTolerance = 1e-6;

	/* Timings on a quiet machine repeat to a few percent; on a shared CI runner
	   they do not repeat at all. Wide enough that only a real change trips it. */
	static constexpr double k_fTimeTolerance = 0.25;

	bool Load(const std::string& path, std::string& o_error);
	/* bWorkOnly drops the timings, which is what a *shared* recording wants:
	   a millisecond figure from one machine is noise on another, and a baseline
	   full of them turns every CI run into a wall of meaningless verdicts. The
	   checked-in Tests/Baselines/perf.txt is work-only for exactly that reason;
	   a local before/after recording keeps both. */
	bool Save(const std::string& path, std::string& o_error, bool bWorkOnly = false) const;

	void Record(const std::string& system, const std::string& name, const BenchmarkResult& result);

	std::vector<Comparison> Compare(const std::string& system, const std::string& name,
	                                const BenchmarkResult& result) const;

	bool Empty() const { return m_Entries.empty(); }

	static std::string Key(const std::string& system, const std::string& name, const std::string& metric);

private:
	/* Ordered, so a re-recorded file is stable rather than reshuffled by a hash
	   seed - the diff is the product. */
	std::map<std::string, Entry> m_Entries;
	std::map<std::string, std::string> m_Units;
};
