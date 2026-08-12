#include "Framework/Check.h"

#include "Core/GameTimer.h"

namespace
{
	/* GameTimer's accumulator is the interesting part and the clock is not, so
	   this subclass drives RunFixedUpdates directly. Testing ChronoGameTimer
	   would mean sleeping and would depend on steady_clock's native period. */
	class ManualGameTimer final : public GameTimer
	{
	public:
		const Time& GetCurrentSystemTime() const override { return m_Time; }
		void Update(const std::function<void()>&) override {}

		uint32_t Advance(uint64_t uiTicks, const std::function<void()>& update)
		{
			return RunFixedUpdates(uiTicks, update);
		}

		uint64_t GetLeftOverTicks() const { return m_uiLeftOverTicks; }
	};
}

/* The defect: a delta covering several fixed steps consumed and counted all of
   them but called back once, so at 30 display fps a 60 Hz fixed update ran half
   the steps it accounted for and the game simulated at half speed. */
VOXAGINE_CHECK(GameTimer, FixedAccumulatorRunsEveryElapsedStep)
{
	ManualGameTimer timer;
	timer.SetTargetElapsedTicks(10);

	uint32_t uiCallbacks = 0;
	const auto count = [&uiCallbacks]() { ++uiCallbacks; };

	CHECK_EQ(timer.Advance(25, count), 2u);
	CHECK_EQ(uiCallbacks, 2u);
	CHECK_EQ(timer.GetFrameCount(), 2u);
	CHECK_EQ(timer.GetTotalTicks(), 20u);
	CHECK_EQ(timer.GetElapsedTicks(), 10u);
	CHECK_EQ(timer.GetLeftOverTicks(), 5u);

	// The remainder carries, so the step it was short of arrives on the next call.
	CHECK_EQ(timer.Advance(5, count), 1u);
	CHECK_EQ(uiCallbacks, 3u);
	CHECK_EQ(timer.GetFrameCount(), 3u);
	CHECK_EQ(timer.GetTotalTicks(), 30u);
	CHECK_EQ(timer.GetLeftOverTicks(), 0u);
}

VOXAGINE_CHECK(GameTimer, FixedAccumulatorRunsNothingBelowOneStep)
{
	ManualGameTimer timer;
	timer.SetTargetElapsedTicks(10);

	uint32_t uiCallbacks = 0;
	CHECK_EQ(timer.Advance(9, [&uiCallbacks]() { ++uiCallbacks; }), 0u);
	CHECK_EQ(uiCallbacks, 0u);
	CHECK_EQ(timer.GetFrameCount(), 0u);
	CHECK_EQ(timer.GetLeftOverTicks(), 9u);
}

/* A period of zero is not reachable through Settings, but the loop now has a
   callback in it and an unbounded spin would be an unbounded spin with side
   effects rather than a hang the debugger can name. */
VOXAGINE_CHECK(GameTimer, AZeroFixedPeriodRunsNothingRatherThanSpinning)
{
	ManualGameTimer timer;
	timer.SetTargetElapsedTicks(0);

	uint32_t uiCallbacks = 0;
	CHECK_EQ(timer.Advance(1000, [&uiCallbacks]() { ++uiCallbacks; }), 0u);
	CHECK_EQ(uiCallbacks, 0u);
}
