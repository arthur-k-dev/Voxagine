#include "pch.h"
#include "Core/ECS/WorldStreamingReadiness.h"

#include "Core/ECS/World.h"
#include "Core/ECS/Systems/Chunk/ChunkSystem.h"
#include "Core/ECS/Systems/Rendering/RenderSystem.h"

void DefaultWorldStreamingReadiness::Advance(
	World& world, const GameTimer& fixedTimer, uint32_t uiFixedSteps)
{
	ChunkSystem* pChunks = world.GetChunkSystem();
	RenderSystem* pRenderSystem = world.GetRenderSystem();

	/* PreTick is the whole of what this world is allowed to do with its
	   entities: run the add/remove queues, register components, resolve links.
	   It is also what admits the roots an incoming chunk staged, so leaving it
	   out would stall the load rather than merely slow it.

	   Between catch-up steps as well as before them, because a step's admission
	   has to be exposed to the next one - that is what keeps the loader's
	   bounded-work contract while nothing is watching the frame rate. */
	if (uiFixedSteps == 0)
		world.PreTick();

	for (uint32_t uiStep = 0; uiStep < uiFixedSteps; ++uiStep)
	{
		world.PreTick();

		if (pChunks != nullptr)
			pChunks->FixedTick(fixedTimer);
	}

	/* An active world advances its chunk system from World::Tick once per
	   display frame. This one does not tick, so that cadence is reproduced
	   explicitly - group advance is once per displayed frame, one state at a
	   time (R2), and driving it from the fixed loop instead would change how
	   many states a transition passes through. */
	if (pChunks != nullptr)
		pChunks->Tick(0.f);

	/* Only on a frame that ran a fixed step, for the same reason
	   Application::Run gates the active world's Render: RenderSystem::Render
	   swaps the particle mapper, and swapping twice for one simulated tick
	   presents the tick before last. The stamp itself is budgeted per renderer
	   (phase 5), so this is bounded work whatever the level. */
	if (pRenderSystem != nullptr && uiFixedSteps > 0)
		pRenderSystem->Render(fixedTimer);
}

bool DefaultWorldStreamingReadiness::HasArrived(World& world)
{
	ChunkSystem* pChunks = world.GetChunkSystem();

	/* A world with no chunk system has nothing to wait for. */
	if (pChunks == nullptr)
		return true;

	/* Both halves, and the second one is free - which is the measurement that
	   decided it rather than the argument.

	   IsInitialWindowReady is the condition gameplay itself waits on (R1): the
	   window committed, its roots were admitted, every admitted renderer was
	   stamped. IsStreaming adds the far-field build, and the obvious worry is
	   that waiting for a horizon doubles the load. It does not: behind the
	   loading screen this world takes **832 / 848 / 882 ms** with both halves
	   and **867 / 914 ms** with only the first, because the far field builds in
	   4 ms slices *alongside* the window rather than after it. So the strict
	   test costs nothing and closes phase 4's open judgement call - the horizon
	   arriving 1.4 seconds into gameplay - by putting it in front of the
	   loading screen instead. */
	return pChunks->IsInitialWindowReady() && !pChunks->IsStreaming();
}
