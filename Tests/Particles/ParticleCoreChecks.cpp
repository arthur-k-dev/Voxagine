#include "Framework/Check.h"

#include <vector>

#include "Core/Particles/ParticleCore.h"

namespace
{
	ParticleSpawn Spawn(float fX)
	{
		ParticleSpawn spawn;
		spawn.v3Position = Vector3(fX, 0.f, 0.f);
		spawn.v3Velocity = Vector3(0.f, fX, 0.f);
		spawn.uiColor = static_cast<uint32_t>(fX);

		return spawn;
	}
}

/* Ledger P11: `ParticleLinkedList(0)` indexed an empty vector to seed its free
   list and then ran `for (i = 0; i < uiReserveSize - 1; ...)` with an unsigned
   zero, so it walked four billion elements. A zero-capacity pool is just an
   empty pool. */
VOXAGINE_CHECK(ParticleCore, ZeroCapacityIsEmptyRatherThanUndefined)
{
	ParticleCore core;
	core.Create(0);

	CHECK_EQ(core.GetCapacity(), 0u);
	CHECK_EQ(core.GetCount(), 0u);
	CHECK_TRUE(core.IsFull());
	CHECK_FALSE(core.Spawn(Spawn(1.f)).IsValid());
	CHECK_TRUE(core.Audit().IsSound());

	/* And so is a default-constructed one, which is what a PhysicsSystem built
	   without a World holds. */
	ParticleCore fresh;
	CHECK_EQ(fresh.GetCount(), 0u);
	CHECK_FALSE(fresh.Spawn(Spawn(1.f)).IsValid());
}

VOXAGINE_CHECK(ParticleCore, SpawnsUpToCapacityAndThenRefuses)
{
	ParticleCore core;
	core.Create(4);

	for (uint32_t i = 0; i < 4; ++i)
		CHECK_TRUE(core.Spawn(Spawn(static_cast<float>(i))).IsValid());

	CHECK_EQ(core.GetCount(), 4u);
	CHECK_TRUE(core.IsFull());
	CHECK_FALSE(core.Spawn(Spawn(99.f)).IsValid());
	CHECK_TRUE(core.Audit().IsSound());
}

VOXAGINE_CHECK(ParticleCore, RetiringSwapsTheLastParticleIn)
{
	ParticleCore core;
	core.Create(8);

	for (uint32_t i = 0; i < 4; ++i)
		core.Spawn(Spawn(static_cast<float>(i)));

	/* Retire the first; the last must land in its place, and the caller must
	   not advance - which is exactly what the simulation loop relies on. */
	core.Retire(0);

	CHECK_EQ(core.GetCount(), 3u);
	CHECK_EQ(core.Position[0].x, 3.f);
	CHECK_EQ(core.Velocity[0].y, 3.f);
	CHECK_EQ(core.Color[0], 3u);
	CHECK_TRUE(core.Audit().IsSound());
}

VOXAGINE_CHECK(ParticleCore, RetiringTheLastParticleIsNotASwap)
{
	ParticleCore core;
	core.Create(8);

	for (uint32_t i = 0; i < 3; ++i)
		core.Spawn(Spawn(static_cast<float>(i)));

	core.Retire(2);

	CHECK_EQ(core.GetCount(), 2u);
	CHECK_EQ(core.Position[0].x, 0.f);
	CHECK_EQ(core.Position[1].x, 1.f);
	CHECK_TRUE(core.Audit().IsSound());
}

/* Ledger P2/P3. Retiring in every order has to leave the tables sound - the old
   pool's head/tail repair was four asymmetric branches and a second
   DestroyParticle on the same particle cycled the free list onto itself. */
VOXAGINE_CHECK(ParticleCore, StaysSoundThroughEveryRetireOrder)
{
	for (uint32_t uiVictim = 0; uiVictim < 6; ++uiVictim)
	{
		ParticleCore core;
		core.Create(6);

		for (uint32_t i = 0; i < 6; ++i)
			core.Spawn(Spawn(static_cast<float>(i)));

		core.Retire(uiVictim);
		REQUIRE_TRUE(core.Audit().IsSound()) << "victim " << uiVictim;

		while (core.GetCount() > 0)
		{
			core.Retire(0);
			REQUIRE_TRUE(core.Audit().IsSound()) << "victim " << uiVictim;
		}

		CHECK_EQ(core.GetCount(), 0u);
		CHECK_EQ(core.Audit().uiFreeListSize, 6u);

		/* Every slot is reusable afterwards. */
		for (uint32_t i = 0; i < 6; ++i)
			CHECK_TRUE(core.Spawn(Spawn(static_cast<float>(i))).IsValid());
	}
}

VOXAGINE_CHECK(ParticleCore, RetiringOutOfRangeDoesNothing)
{
	ParticleCore core;
	core.Create(4);
	core.Spawn(Spawn(1.f));

	core.Retire(1);
	core.Retire(99);

	CHECK_EQ(core.GetCount(), 1u);
	CHECK_TRUE(core.Audit().IsSound());
}

/* A handle survives compaction moving its particle, and stops resolving once
   the particle is gone - which is the whole reason it is not a bare index. */
VOXAGINE_CHECK(ParticleCore, HandlesSurviveCompactionAndDieWithTheirParticle)
{
	ParticleCore core;
	core.Create(8);

	const ParticleHandle first = core.Spawn(Spawn(0.f));
	const ParticleHandle second = core.Spawn(Spawn(1.f));
	const ParticleHandle third = core.Spawn(Spawn(2.f));

	REQUIRE_EQ(core.Resolve(first), 0u);
	REQUIRE_EQ(core.Resolve(third), 2u);

	/* Retiring the first moves the third into index 0. Its handle must follow
	   it, and the first's must stop resolving. */
	core.Retire(0);

	CHECK_FALSE(core.IsAlive(first));
	CHECK_EQ(core.Resolve(first), UINT32_MAX);
	CHECK_EQ(core.Resolve(third), 0u);
	CHECK_TRUE(core.IsAlive(second));
}

VOXAGINE_CHECK(ParticleCore, AStaleHandleDoesNotNameItsSlotsNextOccupant)
{
	ParticleCore core;
	core.Create(2);

	const ParticleHandle first = core.Spawn(Spawn(0.f));
	core.Retire(0);

	const ParticleHandle reused = core.Spawn(Spawn(1.f));

	/* Same slot, different generation. A bare index would have made these
	   indistinguishable. */
	CHECK_EQ(first.uiSlot, reused.uiSlot);
	CHECK_NE(first.uiGeneration, reused.uiGeneration);
	CHECK_FALSE(core.IsAlive(first));
	CHECK_TRUE(core.IsAlive(reused));
}

VOXAGINE_CHECK(ParticleCore, AnInvalidHandleNeverResolves)
{
	ParticleCore core;
	core.Create(4);
	core.Spawn(Spawn(0.f));

	ParticleHandle bogus;
	CHECK_FALSE(core.IsAlive(bogus));

	bogus.uiSlot = 0;
	bogus.uiGeneration = 12345;
	CHECK_FALSE(core.IsAlive(bogus));

	bogus.uiSlot = 99;
	CHECK_FALSE(core.IsAlive(bogus));
}

/* Ledger P10: world pause cleared the integrity state and left the pool full of
   debris whose positions referred to a world about to stop existing. */
VOXAGINE_CHECK(ParticleCore, ClearRetiresEverythingAndInvalidatesHandles)
{
	ParticleCore core;
	core.Create(8);

	std::vector<ParticleHandle> handles;

	for (uint32_t i = 0; i < 5; ++i)
		handles.push_back(core.Spawn(Spawn(static_cast<float>(i))));

	core.Clear();

	CHECK_EQ(core.GetCount(), 0u);
	CHECK_TRUE(core.Audit().IsSound());

	for (const ParticleHandle& handle : handles)
		CHECK_FALSE(core.IsAlive(handle));

	/* And it is immediately reusable. */
	for (uint32_t i = 0; i < 8; ++i)
		CHECK_TRUE(core.Spawn(Spawn(static_cast<float>(i))).IsValid());
}

VOXAGINE_CHECK(ParticleCore, SpawnStoresEveryField)
{
	ParticleCore core;
	core.Create(2);

	ParticleSpawn spawn;
	spawn.v3Position = Vector3(1.f, 2.f, 3.f);
	spawn.v3Velocity = Vector3(4.f, 5.f, 6.f);
	spawn.uiColor = 0xFF112233u;
	spawn.bBakeOnImpact = false;
	spawn.fTimer = 2.5f;

	REQUIRE_TRUE(core.Spawn(spawn).IsValid());

	CHECK_EQ(core.Position[0], Vector3(1.f, 2.f, 3.f));
	CHECK_EQ(core.Velocity[0], Vector3(4.f, 5.f, 6.f));
	CHECK_EQ(core.Color[0], 0xFF112233u);
	CHECK_EQ(core.BakeOnImpact[0], 0u);
	CHECK_EQ(core.Timer[0], 2.5f);
}
