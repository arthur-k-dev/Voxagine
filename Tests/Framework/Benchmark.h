#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

/* A benchmark: one measurable piece of engine work, reported so that two runs
 * can be compared.
 *
 * The problem this framework exists to solve is that "is it slower?" is two
 * questions with very different answers.
 *
 * **Work** is deterministic. How many voxels a burst clears, how many cells the
 * flood fill visits, how many seeds survive filtering, how many islands are
 * emitted - none of that depends on the machine, the compiler, the build type
 * or what else is running. A change to any of those numbers is a change to the
 * *algorithm*, it is exactly reproducible, and it can be gated in CI without a
 * single flake. This is the signal that actually catches regressions, and it is
 * the one a stopwatch cannot give you: a rewrite that walks twice as many cells
 * is invisible in wall time on a fast machine and fatal on a slow one.
 *
 * **Time** is not. It moves with the hardware, the build type, the scheduler
 * and whatever else the machine is doing - CLAUDE.md's own note is that numbers
 * taken on a busy machine lie. So timings are recorded, compared and printed,
 * but they do not fail a run unless --strict says so, and the baseline they are
 * compared against has to have been recorded on the same machine to mean
 * anything at all.
 *
 * Hence two metric kinds, and hence the workflow:
 *
 *   voxagine_tests perf --record before.txt      # on your machine, before
 *   ...make the change...
 *   voxagine_tests perf --baseline before.txt    # same machine, after
 *
 * while CI runs `perf --baseline Tests/Baselines/perf.txt`, which holds only
 * the Work metrics and therefore holds on every machine.
 */
enum class MetricKind
{
	/* Exact and machine-independent. Any increase over the baseline is a
	   regression; any decrease is an improvement worth noticing. */
	Work,

	/* Wall clock. Compared with a wide tolerance and reported, not enforced. */
	Time
};

struct Metric
{
	std::string name;
	double fValue = 0.0;
	MetricKind kind = MetricKind::Work;
	std::string unit;
};

class BenchmarkResult
{
public:
	/* A count of something the algorithm did. Integral in practice, held as a
	   double so one accessor covers both. */
	void AddWork(const std::string& name, double fValue, const std::string& unit = "")
	{
		m_Metrics.push_back(Metric{ name, fValue, MetricKind::Work, unit });
	}

	void AddTime(const std::string& name, double fMilliseconds)
	{
		m_Metrics.push_back(Metric{ name, fMilliseconds, MetricKind::Time, "ms" });
	}

	/* A derived rate - nanoseconds per particle per tick, say. Timing-kind
	   because it is a stopwatch reading underneath. */
	void AddRate(const std::string& name, double fValue, const std::string& unit)
	{
		m_Metrics.push_back(Metric{ name, fValue, MetricKind::Time, unit });
	}

	/* Free-text the runner prints under the table but never compares. State
	   hashes and pass/fail notes go here. */
	void AddNote(const std::string& note) { m_Notes.push_back(note); }

	const std::vector<Metric>& Metrics() const { return m_Metrics; }
	const std::vector<std::string>& Notes() const { return m_Notes; }

private:
	std::vector<Metric> m_Metrics;
	std::vector<std::string> m_Notes;
};

class Benchmark
{
public:
	virtual ~Benchmark() = default;

	/* The system under test, and the directory this file lives in. */
	virtual const char* System() const = 0;
	virtual const char* Name() const = 0;

	virtual void Run(BenchmarkResult& result) const = 0;
};

class BenchmarkRegistry
{
public:
	static BenchmarkRegistry& Get();

	bool Add(std::unique_ptr<Benchmark> pBenchmark);

	const std::vector<std::unique_ptr<Benchmark>>& Benchmarks() const { return m_Benchmarks; }

private:
	std::vector<std::unique_ptr<Benchmark>> m_Benchmarks;
};

class Stopwatch
{
public:
	Stopwatch() : m_Start(std::chrono::steady_clock::now()) {}

	void Restart() { m_Start = std::chrono::steady_clock::now(); }

	double Milliseconds() const
	{
		const std::chrono::duration<double, std::milli> span =
			std::chrono::steady_clock::now() - m_Start;

		return span.count();
	}

private:
	std::chrono::steady_clock::time_point m_Start;
};

/* Total and peak over many samples, which is the same pair FrameProfiler keeps
   and for the same reason: a one-off cost buried in an average of 240 ticks
   says nothing. */
class PhaseTimer
{
public:
	void Add(double fMilliseconds)
	{
		m_fTotalMs += fMilliseconds;

		if (fMilliseconds > m_fPeakMs)
			m_fPeakMs = fMilliseconds;

		++m_uiSamples;
	}

	double TotalMs() const { return m_fTotalMs; }
	double PeakMs() const { return m_fPeakMs; }
	uint32_t Samples() const { return m_uiSamples; }

private:
	double m_fTotalMs = 0.0;
	double m_fPeakMs = 0.0;
	uint32_t m_uiSamples = 0;
};

#define VOXAGINE_BENCHMARK(SystemName, CaseName)                                                        \
	struct VoxBench_##SystemName##_##CaseName final : public Benchmark                                  \
	{                                                                                                   \
		const char* System() const override { return #SystemName; }                                     \
		const char* Name() const override { return #CaseName; }                                         \
		void Run(BenchmarkResult& result) const override;                                               \
	};                                                                                                  \
                                                                                                        \
	static const bool s_bVoxBench_##SystemName##_##CaseName =                                           \
		BenchmarkRegistry::Get().Add(std::unique_ptr<Benchmark>(new VoxBench_##SystemName##_##CaseName())); \
                                                                                                        \
	void VoxBench_##SystemName##_##CaseName::Run(BenchmarkResult& result) const
