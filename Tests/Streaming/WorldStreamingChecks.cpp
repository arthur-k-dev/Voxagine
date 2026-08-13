#include "Framework/Check.h"
#include "Harness/StreamingHarness.h"

#include "Core/Application.h"
#include "Core/ECS/World.h"
#include "Core/ECS/WorldManager.h"
#include "Core/ECS/WorldStreamingReadiness.h"
#include "Core/GameTimer.h"

/* What CHUNK_STREAMING_PLAN.md phase 8 claims about the *state machine*,
 * asserted with no render context, no chunks and no clock.
 *
 * The phase's own testability note is why this file exists: bringing a world up
 * behind a loading screen is two separable things, and only one of them is
 * about rendering. "Does the loading artwork animate, does the music start at
 * the right moment, does the fade survive" needs the real renderer and is
 * verified by the headless game runs in the phase notes. "Is there exactly one
 * pending world, is a second request refused, is the visible world replaced
 * only once the pending one has arrived, does ClearWorlds take a pending world
 * with it" is ordinary bookkeeping that can strand a player on a black screen,
 * and it is what IWorldStreamingReadiness lets a check drive directly.
 *
 * These worlds are built with PreLoad(false) - no RenderSystem, and therefore
 * no chunk system either - so the stub readiness below is the *only* thing that
 * decides when a world has arrived. That is deliberate: the real readiness rule
 * is measured in the game, not asserted here.
 */

namespace
{
	/* Arrives when the check says so, and counts how often it was advanced -
	   which is the other half of the contract: a pending world must be driven
	   every frame until it is activated and never afterwards. */
	class StubReadiness final : public IWorldStreamingReadiness
	{
	public:
		void Advance(World&, const GameTimer&, uint32_t) override { ++m_uiAdvances; }
		bool HasArrived(World&) override { return m_bArrived; }

		void SetArrived(bool bArrived) { m_bArrived = bArrived; }
		uint32_t Advances() const { return m_uiAdvances; }

	private:
		bool m_bArrived = false;
		uint32_t m_uiAdvances = 0;
	};

	/* GameTimer is abstract and the streaming path only passes it through. */
	class UnusedGameTimer final : public GameTimer
	{
	public:
		const Time& GetCurrentSystemTime() const override { return m_Time; }
		void Update(const std::function<void()>&) override {}
	};

	const GameTimer& FixedTimer()
	{
		static UnusedGameTimer s_Timer;
		return s_Timer;
	}

	World* MakeBareWorld(Application& app, const std::string& sName)
	{
		World* pWorld = new World(&app);
		pWorld->PreLoad(false);
		pWorld->SetWorldName(sName);

		return pWorld;
	}

	/* Installs a stub for the duration of a check and puts the default back,
	   because the readiness provider is manager state and the next check must
	   not inherit it. Clears the world manager on both ends for the same reason:
	   these checks are the only things that put worlds in it. */
	class StreamingFixture
	{
	public:
		StreamingFixture()
			: m_App(StreamingHarness::SharedApplication())
		{
			m_App.GetWorldManager().ClearWorlds();
			m_App.GetWorldManager().SetStreamingReadiness(&m_Readiness);
		}

		~StreamingFixture()
		{
			m_App.GetWorldManager().ClearWorlds();
			m_App.GetWorldManager().SetStreamingReadiness(nullptr);
		}

		Application& App() { return m_App; }
		WorldManager& Worlds() { return m_App.GetWorldManager(); }
		StubReadiness& Readiness() { return m_Readiness; }

		/* One display frame of the world manager: the deferred queue runs at the
		   top of a frame (Application::Run), the pending world is advanced in the
		   middle of it. Same order, so a check sees the same interleaving the
		   game does - including the frame of lag between "it has arrived" and
		   "it is the top world", which is the activation transaction. */
		void Frame()
		{
			if (Worlds().RequiresSwap())
				Worlds().SwapWorlds();

			Worlds().UpdateStreamingWorld(FixedTimer(), 1);
		}

	private:
		Application& m_App;
		StubReadiness m_Readiness;
	};
}

VOXAGINE_CHECK(WorldStreaming, APendingWorldIsNotTheTopWorldUntilItArrives)
{
	StreamingFixture fixture;

	World* pVisible = MakeBareWorld(fixture.App(), "visible");
	fixture.Worlds().LoadWorld(pVisible);
	fixture.Frame();

	REQUIRE_EQ(fixture.Worlds().GetTopWorld(), pVisible)
		<< "the ordinary load path did not put the world on top";

	World* pPending = MakeBareWorld(fixture.App(), "pending");
	fixture.Worlds().LoadWorldAfterStreaming(pPending);

	/* Queued, not applied: LoadWorldAfterStreaming is deferred like every other
	   world change, so nothing has happened yet. */
	CHECK_EQ(fixture.Worlds().GetStreamingWorld(), static_cast<World*>(nullptr))
		<< "the pending world was adopted before the deferred queue ran";

	fixture.Frame();

	CHECK_EQ(fixture.Worlds().GetStreamingWorld(), pPending);
	CHECK_EQ(fixture.Worlds().GetTopWorld(), pVisible)
		<< "a world nobody can see must not be the world the game is in";
	CHECK_EQ(fixture.Worlds().GetWorldCount(), size_t(1))
		<< "the pending world must not be in the world list";

	/* Frames pass and it stays pending, because it has not arrived. */
	for (int i = 0; i < 8; ++i)
		fixture.Frame();

	CHECK_EQ(fixture.Worlds().GetTopWorld(), pVisible);
	CHECK_TRUE(fixture.Readiness().Advances() >= 8u)
		<< "a pending world must be advanced every frame, not once";

	fixture.Readiness().SetArrived(true);

	/* One frame to queue the activation, the next to run it. */
	fixture.Frame();
	fixture.Frame();

	CHECK_EQ(fixture.Worlds().GetTopWorld(), pPending);
	CHECK_EQ(fixture.Worlds().GetStreamingWorld(), static_cast<World*>(nullptr));
	CHECK_EQ(fixture.Worlds().GetWorldCount(), size_t(1))
		<< "activation replaces the visible world rather than stacking on it";
}

VOXAGINE_CHECK(WorldStreaming, ASecondPendingWorldIsRefusedAndTheFirstIsUndisturbed)
{
	StreamingFixture fixture;

	fixture.Worlds().LoadWorld(MakeBareWorld(fixture.App(), "visible"));
	fixture.Frame();

	World* pFirst = MakeBareWorld(fixture.App(), "first");
	fixture.Worlds().LoadWorldAfterStreaming(pFirst);
	fixture.Frame();

	REQUIRE_EQ(fixture.Worlds().GetStreamingWorld(), pFirst);

	/* The refusal is the point. This is not a cancellation API: replacing a
	   world that is already half-streamed would strand its chunk jobs and its
	   staged roots, so the second request is dropped and the first carries on. */
	fixture.Worlds().LoadWorldAfterStreaming(MakeBareWorld(fixture.App(), "second"));
	fixture.Frame();

	CHECK_EQ(fixture.Worlds().GetStreamingWorld(), pFirst)
		<< "a second request replaced a world that was already being prepared";

	fixture.Readiness().SetArrived(true);
	fixture.Frame();
	fixture.Frame();

	CHECK_EQ(fixture.Worlds().GetTopWorld(), pFirst);
}

VOXAGINE_CHECK(WorldStreaming, ClearWorldsDiscardsAPendingWorld)
{
	StreamingFixture fixture;

	fixture.Worlds().LoadWorld(MakeBareWorld(fixture.App(), "visible"));
	fixture.Frame();

	fixture.Worlds().LoadWorldAfterStreaming(MakeBareWorld(fixture.App(), "pending"));
	fixture.Frame();

	REQUIRE_TRUE(fixture.Worlds().GetStreamingWorld() != nullptr);

	/* A pending world is the one World that exists and is not in the world
	   list, so nothing else would ever free it - this is what ASan catches when
	   it is missed, and it is why the case is a check rather than a comment. */
	fixture.Worlds().ClearWorlds();

	CHECK_EQ(fixture.Worlds().GetStreamingWorld(), static_cast<World*>(nullptr));
	CHECK_EQ(fixture.Worlds().GetWorldCount(), size_t(0));

	/* And advancing afterwards is a no-op rather than a use-after-free. */
	fixture.Frame();
	fixture.Frame();
}

VOXAGINE_CHECK(WorldStreaming, AnArrivedWorldIsNotAdvancedAfterActivationIsQueued)
{
	StreamingFixture fixture;

	fixture.Worlds().LoadWorld(MakeBareWorld(fixture.App(), "visible"));
	fixture.Frame();

	fixture.Worlds().LoadWorldAfterStreaming(MakeBareWorld(fixture.App(), "pending"));
	fixture.Frame();

	fixture.Readiness().SetArrived(true);
	fixture.Frame();

	/* Activation is queued now and runs at the top of the next frame. The gap is
	   real and the pending world must not be driven across it: it is already
	   spoken for, and advancing it would run a streaming slice against a world
	   that is about to become the active one and be ticked properly. */
	const uint32_t uiAtQueue = fixture.Readiness().Advances();

	fixture.Frame();

	CHECK_EQ(fixture.Readiness().Advances(), uiAtQueue)
		<< "the pending world was advanced after its activation was queued";
}
