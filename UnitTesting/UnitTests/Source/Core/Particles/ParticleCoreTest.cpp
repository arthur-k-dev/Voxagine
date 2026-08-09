#include <gtest/gtest.h>

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
TEST(ParticleCore, ZeroCapacityIsEmptyRatherThanUndefined)
{
	ParticleCore core;
	core.Create(0);

	EXPECT_EQ(core.GetCapacity(), 0u);
	EXPECT_EQ(core.GetCount(), 0u);
	EXPECT_TRUE(core.IsFull());
	EXPECT_FALSE(core.Spawn(Spawn(1.f)).IsValid());
	EXPECT_TRUE(core.Audit().IsSound());

	/* And so is a default-constructed one, which is what a PhysicsSystem built
	   without a World holds. */
	ParticleCore fresh;
	EXPECT_EQ(fresh.GetCount(), 0u);
	EXPECT_FALSE(fresh.Spawn(Spawn(1.f)).IsValid());
}

TEST(ParticleCore, SpawnsUpToCapacityAndThenRefuses)
{
	ParticleCore core;
	core.Create(4);

	for (uint32_t i = 0; i < 4; ++i)
		EXPECT_TRUE(core.Spawn(Spawn(static_cast<float>(i))).IsValid());

	EXPECT_EQ(core.GetCount(), 4u);
	EXPECT_TRUE(core.IsFull());
	EXPECT_FALSE(core.Spawn(Spawn(99.f)).IsValid());
	EXPECT_TRUE(core.Audit().IsSound());
}

TEST(ParticleCore, RetiringSwapsTheLastParticleIn)
{
	ParticleCore core;
	core.Create(8);

	for (uint32_t i = 0; i < 4; ++i)
		core.Spawn(Spawn(static_cast<float>(i)));

	/* Retire the first; the last must land in its place, and the caller must
	   not advance - which is exactly what the simulation loop relies on. */
	core.Retire(0);

	EXPECT_EQ(core.GetCount(), 3u);
	EXPECT_EQ(core.Position[0].x, 3.f);
	EXPECT_EQ(core.Velocity[0].y, 3.f);
	EXPECT_EQ(core.Color[0], 3u);
	EXPECT_TRUE(core.Audit().IsSound());
}

TEST(ParticleCore, RetiringTheLastParticleIsNotASwap)
{
	ParticleCore core;
	core.Create(8);

	for (uint32_t i = 0; i < 3; ++i)
		core.Spawn(Spawn(static_cast<float>(i)));

	core.Retire(2);

	EXPECT_EQ(core.GetCount(), 2u);
	EXPECT_EQ(core.Position[0].x, 0.f);
	EXPECT_EQ(core.Position[1].x, 1.f);
	EXPECT_TRUE(core.Audit().IsSound());
}

/* Ledger P2/P3. Retiring in every order has to leave the tables sound - the old
   pool's head/tail repair was four asymmetric branches and a second
   DestroyParticle on the same particle cycled the free list onto itself. */
TEST(ParticleCore, StaysSoundThroughEveryRetireOrder)
{
	for (uint32_t uiVictim = 0; uiVictim < 6; ++uiVictim)
	{
		ParticleCore core;
		core.Create(6);

		for (uint32_t i = 0; i < 6; ++i)
			core.Spawn(Spawn(static_cast<float>(i)));

		core.Retire(uiVictim);
		ASSERT_TRUE(core.Audit().IsSound()) << "victim " << uiVictim;

		while (core.GetCount() > 0)
		{
			core.Retire(0);
			ASSERT_TRUE(core.Audit().IsSound()) << "victim " << uiVictim;
		}

		EXPECT_EQ(core.GetCount(), 0u);
		EXPECT_EQ(core.Audit().uiFreeListSize, 6u);

		/* Every slot is reusable afterwards. */
		for (uint32_t i = 0; i < 6; ++i)
			EXPECT_TRUE(core.Spawn(Spawn(static_cast<float>(i))).IsValid());
	}
}

TEST(ParticleCore, RetiringOutOfRangeDoesNothing)
{
	ParticleCore core;
	core.Create(4);
	core.Spawn(Spawn(1.f));

	core.Retire(1);
	core.Retire(99);

	EXPECT_EQ(core.GetCount(), 1u);
	EXPECT_TRUE(core.Audit().IsSound());
}

/* A handle survives compaction moving its particle, and stops resolving once
   the particle is gone - which is the whole reason it is not a bare index. */
TEST(ParticleCore, HandlesSurviveCompactionAndDieWithTheirParticle)
{
	ParticleCore core;
	core.Create(8);

	const ParticleHandle first = core.Spawn(Spawn(0.f));
	const ParticleHandle second = core.Spawn(Spawn(1.f));
	const ParticleHandle third = core.Spawn(Spawn(2.f));

	ASSERT_EQ(core.Resolve(first), 0u);
	ASSERT_EQ(core.Resolve(third), 2u);

	/* Retiring the first moves the third into index 0. Its handle must follow
	   it, and the first's must stop resolving. */
	core.Retire(0);

	EXPECT_FALSE(core.IsAlive(first));
	EXPECT_EQ(core.Resolve(first), UINT32_MAX);
	EXPECT_EQ(core.Resolve(third), 0u);
	EXPECT_TRUE(core.IsAlive(second));
}

TEST(ParticleCore, AStaleHandleDoesNotNameItsSlotsNextOccupant)
{
	ParticleCore core;
	core.Create(2);

	const ParticleHandle first = core.Spawn(Spawn(0.f));
	core.Retire(0);

	const ParticleHandle reused = core.Spawn(Spawn(1.f));

	/* Same slot, different generation. A bare index would have made these
	   indistinguishable. */
	EXPECT_EQ(first.uiSlot, reused.uiSlot);
	EXPECT_NE(first.uiGeneration, reused.uiGeneration);
	EXPECT_FALSE(core.IsAlive(first));
	EXPECT_TRUE(core.IsAlive(reused));
}

TEST(ParticleCore, AnInvalidHandleNeverResolves)
{
	ParticleCore core;
	core.Create(4);
	core.Spawn(Spawn(0.f));

	ParticleHandle bogus;
	EXPECT_FALSE(core.IsAlive(bogus));

	bogus.uiSlot = 0;
	bogus.uiGeneration = 12345;
	EXPECT_FALSE(core.IsAlive(bogus));

	bogus.uiSlot = 99;
	EXPECT_FALSE(core.IsAlive(bogus));
}

/* Ledger P10: world pause cleared the integrity state and left the pool full of
   debris whose positions referred to a world about to stop existing. */
TEST(ParticleCore, ClearRetiresEverythingAndInvalidatesHandles)
{
	ParticleCore core;
	core.Create(8);

	std::vector<ParticleHandle> handles;

	for (uint32_t i = 0; i < 5; ++i)
		handles.push_back(core.Spawn(Spawn(static_cast<float>(i))));

	core.Clear();

	EXPECT_EQ(core.GetCount(), 0u);
	EXPECT_TRUE(core.Audit().IsSound());

	for (const ParticleHandle& handle : handles)
		EXPECT_FALSE(core.IsAlive(handle));

	/* And it is immediately reusable. */
	for (uint32_t i = 0; i < 8; ++i)
		EXPECT_TRUE(core.Spawn(Spawn(static_cast<float>(i))).IsValid());
}

TEST(ParticleCore, SpawnStoresEveryField)
{
	ParticleCore core;
	core.Create(2);

	ParticleSpawn spawn;
	spawn.v3Position = Vector3(1.f, 2.f, 3.f);
	spawn.v3Velocity = Vector3(4.f, 5.f, 6.f);
	spawn.uiColor = 0xFF112233u;
	spawn.bBakeOnImpact = false;
	spawn.fTimer = 2.5f;

	ASSERT_TRUE(core.Spawn(spawn).IsValid());

	EXPECT_EQ(core.Position[0], Vector3(1.f, 2.f, 3.f));
	EXPECT_EQ(core.Velocity[0], Vector3(4.f, 5.f, 6.f));
	EXPECT_EQ(core.Color[0], 0xFF112233u);
	EXPECT_EQ(core.BakeOnImpact[0], 0u);
	EXPECT_EQ(core.Timer[0], 2.5f);
}
