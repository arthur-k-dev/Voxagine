#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

/* RENDERING_PLAN.md Phase 0: aggregates named GPU-pass and CPU-section
 * timings and logs a rolling average once per second, mirroring
 * RenderContext's existing [fps] log.
 *
 * Disabled by default outside _DEBUG (Settings::IsGPUProfilingEnabled) so a
 * Release build pays nothing beyond the IsEnabled() branch at each call
 * site - callers are expected to skip their own measurement work (a
 * std::chrono span, a GPU query) when this is false rather than call
 * Report() unconditionally. */
class FrameProfiler
{
public:
	static FrameProfiler& Get();

	void SetEnabled(bool bEnabled) { m_bEnabled = bEnabled; }
	bool IsEnabled() const { return m_bEnabled; }

	void Report(const std::string& name, double fMilliseconds);

	/* A count of things done, not a duration - voxels stamped, renderers
	   re-baked, quads submitted.
	 *
	 * Separate from Report because it answers a different question and answers
	 * it *reliably*: a count is exact and machine-independent, so it is valid
	 * on a machine that is running something else, and two builds that disagree
	 * on it disagree about the work rather than about the weather. Timings on
	 * this tree have already been wrong twice for environmental reasons - a
	 * vsync-locked GPU clock (RENDERING_PLAN.md 7.3) and a compositor-chosen
	 * window size (CLAUDE.md) - and `Tests/` is built on exactly this split:
	 * work metrics gate CI, timings only report.
	 *
	 * Logged under [work] with a per-second total and peak, so a per-frame
	 * counter reads as a rate and a one-shot cost still shows its spike. */
	void ReportCount(const std::string& name, double fCount);

	/* Call once per frame; logs and resets every accumulator roughly once a
	   second. No-op when disabled. */
	void Tick(float fDeltaTime);

private:
	struct Accumulator
	{
		double fTotalMs = 0.0;

		/* The one-off costs are the interesting ones - a bake or a chunk load
		   happens on a single frame of a second and the average over 200 of
		   them buries it. Reported alongside the average, reset with it. */
		double fPeakMs = 0.0;

		uint32_t uiSamples = 0;

		/* Reported by ReportCount rather than Report, so Tick knows to print it
		   as a total under [work] instead of an average under [timing]. A name
		   belongs to one channel or the other; mixing them would average counts
		   and total milliseconds, both of which are meaningless. */
		bool bIsCount = false;
	};

	bool m_bEnabled = false;
	float m_fTimer = 0.0f;

	std::unordered_map<std::string, Accumulator> m_Accumulators;
};

/* Times a scope and reports it, or does nothing at all when profiling is off.
 *
 * The existing call sites all spell this out by hand - read IsEnabled() once,
 * take a chrono stamp, take another, subtract, Report - which is fine for the
 * three or four of them that measure something with an early return in it, and
 * pure noise for the dozen DESTRUCTION_PLAN.md phase 0 adds. The contract is
 * the same one the class comment states: when disabled, no clock is read.
 *
 * The name is captured by pointer, so it must outlive the scope. Every caller
 * passes a string literal. */
class ScopedFrameTimer
{
public:
	explicit ScopedFrameTimer(const char* pName) :
		m_pName(FrameProfiler::Get().IsEnabled() ? pName : nullptr)
	{
		if (m_pName)
			m_Start = std::chrono::high_resolution_clock::now();
	}

	~ScopedFrameTimer()
	{
		if (!m_pName)
			return;

		const std::chrono::duration<double, std::milli> span =
			std::chrono::high_resolution_clock::now() - m_Start;

		FrameProfiler::Get().Report(m_pName, span.count());
	}

	ScopedFrameTimer(const ScopedFrameTimer&) = delete;
	ScopedFrameTimer& operator=(const ScopedFrameTimer&) = delete;

private:
	const char* m_pName;
	std::chrono::high_resolution_clock::time_point m_Start;
};
