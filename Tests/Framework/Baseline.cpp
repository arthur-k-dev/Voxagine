#include "Framework/Baseline.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

std::string Baseline::Key(const std::string& system, const std::string& name, const std::string& metric)
{
	return system + "/" + name + "/" + metric;
}

bool Baseline::Load(const std::string& path, std::string& o_error)
{
	std::ifstream file(path);

	if (!file.is_open())
	{
		o_error = "cannot open '" + path + "'";
		return false;
	}

	m_Entries.clear();
	m_Units.clear();

	std::string line;
	

	while (std::getline(file, line))
	{


		const size_t uiComment = line.find('#');

		if (uiComment != std::string::npos)
			line.erase(uiComment);

		std::istringstream stream(line);

		std::string key;
		std::string kind;
		double fValue = 0.0;

		if (!(stream >> key >> fValue >> kind))
			continue;

		Entry entry;
		entry.fValue = fValue;
		entry.kind = kind == "time" ? MetricKind::Time : MetricKind::Work;

		m_Entries[key] = entry;

		std::string unit;

		if (stream >> unit)
			m_Units[key] = unit;
	}

	return true;
}

bool Baseline::Save(const std::string& path, std::string& o_error, bool bWorkOnly) const
{
	std::ofstream file(path, std::ios::trunc);

	if (!file.is_open())
	{
		o_error = "cannot write '" + path + "'";
		return false;
	}

	file << "# Recorded by `voxagine_tests perf --record-work <file>` (work only, which is\n"
	     << "# what the checked-in Tests/Baselines/perf.txt holds) or `--record <file>`\n"
	     << "# (with timings, for a local before/after comparison on one machine).\n"
	     << "#\n"
	     << "# `work` metrics are exact and machine-independent: any increase is a\n"
	     << "# regression and CI enforces them. `time` metrics only mean something\n"
	     << "# against a recording from the same machine and build type, so they are\n"
	     << "# reported rather than enforced unless --strict is passed.\n"
	     << "#\n"
	     << "# <system>/<benchmark>/<metric>  <value>  <kind>  [unit]\n\n";

	for (const std::pair<const std::string, Entry>& entry : m_Entries)
	{
		if (bWorkOnly && entry.second.kind != MetricKind::Work)
			continue;

		char line[512];

		const std::map<std::string, std::string>::const_iterator unit = m_Units.find(entry.first);

		std::snprintf(line, sizeof(line), "%-60s %16.4f  %s %s",
		              entry.first.c_str(), entry.second.fValue,
		              entry.second.kind == MetricKind::Time ? "time" : "work",
		              unit != m_Units.end() ? unit->second.c_str() : "");

		std::string row(line);

		while (!row.empty() && row.back() == ' ')
			row.pop_back();

		file << row << "\n";
	}

	return true;
}

void Baseline::Record(const std::string& system, const std::string& name, const BenchmarkResult& result)
{
	for (const Metric& metric : result.Metrics())
	{
		const std::string key = Key(system, name, metric.name);

		Entry entry;
		entry.fValue = metric.fValue;
		entry.kind = metric.kind;

		m_Entries[key] = entry;

		if (!metric.unit.empty())
			m_Units[key] = metric.unit;
	}
}

std::vector<Baseline::Comparison> Baseline::Compare(const std::string& system, const std::string& name,
                                                    const BenchmarkResult& result) const
{
	std::vector<Comparison> comparisons;

	for (const Metric& metric : result.Metrics())
	{
		Comparison comparison;
		comparison.key = metric.name;
		comparison.unit = metric.unit;
		comparison.kind = metric.kind;
		comparison.fValue = metric.fValue;

		const std::map<std::string, Entry>::const_iterator entry =
			m_Entries.find(Key(system, name, metric.name));

		if (entry != m_Entries.end())
		{
			comparison.bHasBaseline = true;
			comparison.fBaseline = entry->second.fValue;

			const double fTolerance = metric.kind == MetricKind::Work
				? k_fWorkTolerance
				: k_fTimeTolerance * std::fabs(entry->second.fValue);

			/* Both directions are reported. A metric that improved by more than
			   the tolerance is worth seeing in the same table - half the time a
			   "regression somewhere else" is this number having moved, and a
			   change that halves the work is usually either very good news or a
			   sign that something stopped running at all. */
			if (comparison.fValue > entry->second.fValue + fTolerance)
				comparison.bRegressed = true;
			else if (comparison.fValue < entry->second.fValue - fTolerance)
				comparison.bImproved = true;
		}

		comparisons.push_back(comparison);
	}

	return comparisons;
}
