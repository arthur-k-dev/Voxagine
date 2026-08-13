#pragma once

#include <cstdint>

class GameTimer;
class World;

/* How a world being brought up *behind* something else is advanced, and how
 * "it has arrived" is answered. Docs/CHUNK_STREAMING_PLAN.md phase 8.
 *
 * There are exactly two implementations and there should stay exactly two.
 * `DefaultWorldStreamingReadiness` drives the real systems; a check installs a
 * stub so that `WorldManager`'s pending-world state machine - one pending world
 * at a time, a second request refused, activation only once the world is there,
 * ClearWorlds discarding a pending world - can be driven with no render context,
 * no chunks and no clock. That state machine is the part that can be wrong in a
 * way nobody sees until a level switch strands the player on a black screen, and
 * it is the part that has nothing to do with rendering.
 *
 * **This is a test seam, not an abstraction layer**, and the same warning
 * `IVoxelWindow` carries applies: nothing else should implement it, and it
 * should not grow a method because some caller would find one convenient.
 */
class IWorldStreamingReadiness
{
public:
	virtual ~IWorldStreamingReadiness() = default;

	/* One display frame's worth of progress for a world that is not the active
	   one. Deliberately not World::Tick/FixedTick: scripts, AI, physics and
	   player input must not run behind a loading screen. */
	virtual void Advance(World& world, const GameTimer& fixedTimer, uint32_t uiFixedSteps) = 0;

	/* Is this world complete enough to be the one you are looking at? */
	virtual bool HasArrived(World& world) = 0;
};

class DefaultWorldStreamingReadiness : public IWorldStreamingReadiness
{
public:
	void Advance(World& world, const GameTimer& fixedTimer, uint32_t uiFixedSteps) override;
	bool HasArrived(World& world) override;
};
