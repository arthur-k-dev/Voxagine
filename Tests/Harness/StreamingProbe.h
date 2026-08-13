#pragma once

#include <cstdint>

#include "Core/ECS/Entity.h"

/* A reflected entity that exists only in the test binary, so that two things
 * the streaming phases have to prove become expressible at all
 * (CHUNK_STREAMING_PLAN.md R7 - "a property only a human can check is a property
 * the phase must first make checkable"):
 *
 *   - **a cross-chunk link.** `LinkTarget` is a plain reflected `Entity*`, which
 *     is what the serializer records as a `WorldConnectionInformation` and what
 *     `ResolveWorldLinks` has to reconnect after streaming rebuilt both ends in
 *     different frames. Nothing in the engine has such a property; every one is
 *     in game code, which the suite does not link.
 *   - **whether gameplay ticked.** R1 says entities must not advance against a
 *     world whose initial window has not arrived, and the only way to observe
 *     that is an entity that counts its own ticks.
 *
 * Registered from the test binary rather than from `voxagine`, which works
 * because RTTR registrations are static initialisers and the executable's own
 * objects are always linked in - the archive problem CMake/TestSources.cmake
 * describes does not apply here.
 *
 * The counters are static because streaming *destroys and rebuilds* these
 * entities: a chunk that unloads takes its probes with it and a chunk that
 * comes back deserializes new ones, so an instance member would be reset by the
 * thing being measured.
 */
class StreamingProbeEntity : public Entity
{
	RTTR_ENABLE(Entity)

public:
	explicit StreamingProbeEntity(World* pWorld) : Entity(pWorld) {}

	void Tick(float fDeltaTime) override;
	void FixedTick(const GameTimer& fixedTimer) override;

	Entity* GetLinkTarget() const { return m_pLinkTarget; }
	void SetLinkTarget(Entity* pTarget) { m_pLinkTarget = pTarget; }

	/* Ticks and fixed ticks taken by *any* probe since ResetCounters. */
	static uint64_t TicksTaken();
	static uint64_t FixedTicksTaken();
	static void ResetCounters();

private:
	Entity* m_pLinkTarget = nullptr;
};
