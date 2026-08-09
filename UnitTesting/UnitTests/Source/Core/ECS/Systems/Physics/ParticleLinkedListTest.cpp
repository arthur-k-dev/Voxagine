#include <gtest/gtest.h>

#include <vector>

#include "Core/ECS/Systems/Physics/ParticleLinkedList.h"

/* The pool as it is today, pinned so phase 3's SoA replacement can be held to
   the same observable behaviour. The defects the ledger records (P2's missing
   double-free guard, P3's asymmetric head/tail repair) are deliberately *not*
   asserted as bugs here - a test that locks in a defect makes the fix look like
   a regression. What is asserted is what callers actually rely on. */
TEST(ParticleLinkedList, SpawnsUpToCapacityAndThenReportsFailure)
{
	ParticleLinkedList pool(4);

	std::vector<Particle*> spawned;

	for (uint32_t i = 0; i < 4; ++i)
	{
		Particle* pParticle = pool.SpawnParticle();
		ASSERT_NE(pParticle, nullptr);
		spawned.push_back(pParticle);
	}

	EXPECT_EQ(pool.SpawnParticle(), nullptr);

	const ParticleLinkedList::AuditResult result = pool.Audit();
	EXPECT_TRUE(result.IsSound());
	EXPECT_EQ(result.uiAlive, 4u);
	EXPECT_EQ(result.uiFree, 0u);
}

TEST(ParticleLinkedList, ADestroyedParticleIsReusable)
{
	ParticleLinkedList pool(2);

	Particle* pFirst = pool.SpawnParticle();
	Particle* pSecond = pool.SpawnParticle();

	ASSERT_NE(pFirst, nullptr);
	ASSERT_NE(pSecond, nullptr);
	EXPECT_EQ(pool.SpawnParticle(), nullptr);

	pool.DestroyParticle(pFirst);

	EXPECT_TRUE(pool.Audit().IsSound());
	EXPECT_NE(pool.SpawnParticle(), nullptr);
}

TEST(ParticleLinkedList, InitialisesSpawnedState)
{
	ParticleLinkedList pool(2);

	Particle* pParticle = pool.SpawnParticle();
	ASSERT_NE(pParticle, nullptr);

	pParticle->Live.Position = Vector3(1.f, 2.f, 3.f);
	pParticle->Live.BakeOnImpact = false;

	pool.DestroyParticle(pParticle);

	Particle* pReused = pool.SpawnParticle();
	ASSERT_EQ(pReused, pParticle);

	EXPECT_EQ(pReused->Live.Position, Vector3(0.f));
	EXPECT_TRUE(pReused->Live.BakeOnImpact);
}

/* The alive list is walked newest-first by SimulateParticles (GetLastAlive then
   ->Prev), and the order matters for P13's starvation. Pinned so the
   replacement can be compared against it. */
TEST(ParticleLinkedList, TheAliveListIsWalkableFromBothEnds)
{
	ParticleLinkedList pool(8);

	std::vector<Particle*> spawned;

	for (uint32_t i = 0; i < 5; ++i)
		spawned.push_back(pool.SpawnParticle());

	uint32_t uiForward = 0;

	for (Particle* p = pool.GetFirstAlive(); p != nullptr; p = p->Next)
		++uiForward;

	uint32_t uiBackward = 0;

	for (Particle* p = pool.GetLastAlive(); p != nullptr; p = p->Prev)
		++uiBackward;

	EXPECT_EQ(uiForward, 5u);
	EXPECT_EQ(uiBackward, 5u);

	/* Newest spawn is the head; oldest is the tail. */
	EXPECT_EQ(pool.GetFirstAlive(), spawned.back());
	EXPECT_EQ(pool.GetLastAlive(), spawned.front());
}

TEST(ParticleLinkedList, DestroyingEveryParticleLeavesTheListsSound)
{
	ParticleLinkedList pool(16);

	std::vector<Particle*> spawned;

	for (uint32_t i = 0; i < 16; ++i)
		spawned.push_back(pool.SpawnParticle());

	/* Out of order on purpose: head, tail and middle each take a different
	   branch of DestroyParticle's repair. */
	for (uint32_t i : { 0u, 15u, 7u, 1u, 14u, 8u, 2u, 3u, 4u, 5u, 6u, 9u, 10u, 11u, 12u, 13u })
	{
		pool.DestroyParticle(spawned[i]);
		EXPECT_TRUE(pool.Audit().IsSound()) << "after destroying " << i;
	}

	EXPECT_EQ(pool.GetFirstAlive(), nullptr);
	EXPECT_EQ(pool.GetLastAlive(), nullptr);

	const ParticleLinkedList::AuditResult result = pool.Audit();
	EXPECT_EQ(result.uiAlive, 0u);
	EXPECT_EQ(result.uiFree, 16u);
}
