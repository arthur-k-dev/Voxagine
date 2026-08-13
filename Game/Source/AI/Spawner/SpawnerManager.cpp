#include "SpawnerManager.h"

#include "Core/ECS/World.h"
#include "Core/ECS/Entity.h"
#include "Core/ECS/Components/BoxCollider.h"
#include "Core/MetaData/PropertyTypeMetaData.h"

#include "AI/Spawner/Spawner.h"

#include "Gameplay/Wall/BoundingWall.h"
#include <Core/Application.h>
#include "Core/LoggingSystem/LoggingSystem.h"

#include <cstdlib>

RTTR_REGISTRATION
{
	rttr::registration::class_<SpawnerManager>("SpawnerManager")
		.constructor<World*>()(rttr::policy::ctor::as_raw_ptr)
		.property("Spawners", &SpawnerManager::m_vSpawnerEntities)(RTTR_PUBLIC, RTTR_TOOLTIP("Spawners connected to this manager"))
		.property("Bounding Walls", &SpawnerManager::m_BoundWalls)(RTTR_PUBLIC, RTTR_TOOLTIP("Walls connected to this manager"))
		.property("Trigger Box Size", &SpawnerManager::GetBoxSize, &SpawnerManager::SetBoxSize)(RTTR_PUBLIC, RTTR_TOOLTIP("Trigger box size"));
}

void SpawnerManager::SetBoxSize(Vector3 v3BoxSize)
{
	if (v3BoxSize == v3InitializeBoxSize)
		return;

	v3InitializeBoxSize = v3BoxSize;

	if (auto pCollider = GetComponentAll<BoxCollider>())
		pCollider->SetBoxSize(v3InitializeBoxSize);
}


void SpawnerManager::Awake()
{
	// Collider
	m_pCollider = AddComponent<BoxCollider>();
	if (!m_pCollider)
		m_pCollider = GetComponent<BoxCollider>();
	m_pCollider->SetBoxSize(v3InitializeBoxSize);
	m_pCollider->SetTrigger(true);
	m_pCollider->SetIgnoreVoxels(true);

	const std::string sName = "SpawnerManager" + std::to_string(GetId());
	SetName(sName);

	SetPersistent(true);
}

void SpawnerManager::Start()
{
	Entity::Start();

	if (m_pCollider)
		m_pCollider->SetBoxSize(v3InitializeBoxSize);

	RefreshSpawnerLinks();

	if (!m_BoundWalls.empty())
	{
		// Set all walls to false
		for (auto pWall : m_BoundWalls)
		{
			if (pWall)
				pWall->SetEnabled(false);
		}
	}
}

/* The live spawners, resolved from the ids adopted below. A pointer is never
   held across a frame, so there is nothing here that streaming can leave
   dangling - see m_vSpawnerOwnerIds. */
std::vector<Spawner*> SpawnerManager::ResolveLiveSpawners()
{
	std::vector<Spawner*> live;
	live.reserve(m_vSpawnerOwnerIds.size());

	for (uint64_t uiId : m_vSpawnerOwnerIds)
	{
		Entity* pOwner = GetWorld()->FindEntity(uiId);

		if (pOwner == nullptr || pOwner->IsDestroyed())
			continue;

		if (Spawner* pSpawner = pOwner->GetComponent<Spawner>())
			live.push_back(pSpawner);
	}

	return live;
}

bool SpawnerManager::RefreshSpawnerLinks()
{
	/* An unresolved link is not a missing spawner.
	 *
	 * m_vSpawnerEntities is a serialized list of entity links, and three of
	 * Fishing_Village_Beat1's four spawners are in a different chunk from their
	 * manager - so at Start they are null and they resolve over the following
	 * frames, as the world's link resolution retries (World::
	 * k_uiMaxWorldLinkRetries). Start used to erase them on sight, which threw
	 * the link away permanently; with all four gone the list was empty and
	 * FixedTick destroyed the manager on its first frame, so walking into the
	 * arena did nothing at all. Nulls are therefore left in place and simply
	 * skipped by every reader.
	 *
	 * Subscribing has to happen here rather than once in Start for the same
	 * reason: a spawner that arrives on frame 200 still needs its Destroyed
	 * handler, or defeating it never removes it from the list and the manager
	 * never finishes. */
	/* Adopt whatever the link resolution has filled in since the last pass, and
	   consume it: the entry is set to null once its id is taken, so this is the
	   only code that ever dereferences one of those pointers and it does so on
	   the frame they were written. */
	for (Spawner*& pSpawner : m_vSpawnerEntities)
	{
		if (pSpawner == nullptr)
			continue;

		Entity* pOwner = pSpawner->GetOwner();

		pSpawner = nullptr;

		if (pOwner == nullptr)
			continue;

		const uint64_t uiId = pOwner->GetId();

		if (std::find(m_vSpawnerOwnerIds.begin(), m_vSpawnerOwnerIds.end(), uiId) == m_vSpawnerOwnerIds.end())
			m_vSpawnerOwnerIds.push_back(uiId);
	}

	/* A spawner that has been *defeated* is gone for good and must leave the
	   list, or the manager never finishes. A spawner whose chunk merely
	   unloaded is not gone and keeps its id, which is why this asks the spawner
	   rather than the world. */
	std::vector<Spawner*> live = ResolveLiveSpawners();

	for (Spawner* pSpawner : live)
	{
		if (pSpawner->IsAlive())
			continue;

		Entity* pOwner = pSpawner->GetOwner();

		if (pOwner == nullptr)
			continue;

		const auto it = std::find(m_vSpawnerOwnerIds.begin(), m_vSpawnerOwnerIds.end(), pOwner->GetId());

		if (it != m_vSpawnerOwnerIds.end())
			m_vSpawnerOwnerIds.erase(it);
	}

	const bool bAnyLive = !live.empty();

	m_bHasSeenSpawners = m_bHasSeenSpawners || bAnyLive;

	return bAnyLive;
}

void SpawnerManager::FixedTick(const ::GameTimer& gameTimer)
{
	Entity::FixedTick(gameTimer);

	const bool bAnyLive = RefreshSpawnerLinks();

	// if we defeated all the enemies and destroyed all the spawners
	if(m_bHasSeenSpawners && !bAnyLive)
	{
		if (!m_BoundWalls.empty())
		{
			// Set all walls to false
			for (auto pWall : m_BoundWalls)
			{
				if (pWall)
					pWall->SetEnabled(false);
			}
		}


		Destroy();
	}
}

void SpawnerManager::OnCollisionEnter(Collider* pCollider, const Manifold&)
{
	if (GameplayDebugEnabled())
		fprintf(stderr, "[spawner] enter '%s' by '%s' (tagged Player: %d)\n",
			GetName().c_str(),
			pCollider->GetOwner() ? pCollider->GetOwner()->GetName().c_str() : "?",
			pCollider->GetOwner() && pCollider->GetOwner()->HasTag("Player") ? 1 : 0);

	if (pCollider->GetOwner()->HasTag("Player"))
	{
		// If we don;t have the first player grab it
		if (!m_pPlayer1)
		{
			m_pPlayer1 = pCollider->GetOwner();
		}
		else if(m_pPlayer1 && !m_pPlayer2)
		{
			m_pPlayer2 = pCollider->GetOwner();
		}

		// if we have either first or second player
		if ((m_pPlayer1 || m_pPlayer2) && !m_BoundWalls.empty())
		{
			for (auto pWall : m_BoundWalls)
			{
				if (pWall && pWall->bEndWall)
					pWall->SetEnabled(true);
			}
		}
	}
}

/* VOXAGINE_GAMEPLAY_DEBUG=1. Why this exists rather than a debugger session:
   the two reports it serves - "the spawners never appear" and "I cannot recall
   the bullet" - are both a *state* question ("what did the manager think was in
   the box, and what was in its list?") that no headless script in this tree can
   reach, because --ui-script cannot fire a weapon or walk into an arena
   reliably. One line per state change, never per tick. */
bool SpawnerManager::GameplayDebugEnabled()
{
	static const bool s_bEnabled = std::getenv("VOXAGINE_GAMEPLAY_DEBUG") != nullptr;
	return s_bEnabled;
}

void SpawnerManager::ReportState(const char* pWhere)
{
	if (!GameplayDebugEnabled())
		return;

	uint32_t uiLive = 0;
	uint32_t uiStarted = 0;

	for (Spawner* pSpawner : ResolveLiveSpawners())
	{
		++uiLive;

		if (pSpawner->HasStarted())
			++uiStarted;
	}

	fprintf(stderr, "[spawner] %s '%s': players %d/%d, list %zu (%u resolved, %u started), walls %zu\n",
		pWhere, GetName().c_str(),
		m_pPlayer1 ? 1 : 0, m_pPlayer2 ? 1 : 0,
		m_vSpawnerOwnerIds.size(), uiLive, uiStarted, m_BoundWalls.size());
}

void SpawnerManager::OnCollisionStay(Collider*, const Manifold&)
{
	/* Once per change, not once per tick. */
	if (GameplayDebugEnabled())
	{
		const uint32_t uiLive = static_cast<uint32_t>(ResolveLiveSpawners().size());

		if (uiLive != m_uiReportedLive || !m_bReportedStay)
		{
			m_uiReportedLive = uiLive;
			m_bReportedStay = true;

			ReportState("stay");
		}
	}

	/* One player in the box is enough to start the wave. It used to need both,
	   so a solo player could walk into an arena and nothing would ever happen -
	   reported on Fishing_Village_Beat1, where the bound walls close (that half
	   only needs one player, OnCollisionEnter) and the spawners never fire.
	   m_pPlayer1 is filled by whichever player arrives first. */
	if(m_pPlayer1 || m_pPlayer2)
	{
		/* Resolved from ids on the spot rather than read out of the serialized
		   link array, so nothing here can be a freed pointer - see
		   m_vSpawnerOwnerIds. This is the loop that crashed: it walked the raw
		   links, which chunk streaming had left pointing at destroyed
		   components. */
		for (Spawner* pSpawner : ResolveLiveSpawners())
		{
			if (pSpawner->HasStarted())
				continue;

			pSpawner->StartWave();
		}

		if(!m_BoundWalls.empty())
		{
			for (auto pWall : m_BoundWalls)
			{
				if (pWall && !pWall->IsEnabled())
				{
					pWall->SetEnabled(true);
				}
			}
		}
	}
}

void SpawnerManager::OnCollisionExit(Collider* pCollider, const Manifold&)
{
	// If one of the players is walking out of the trigger box.
	if (m_pPlayer1 == pCollider->GetOwner())
	{
		m_pPlayer1 = nullptr;
	}
	
	if (m_pPlayer2 == pCollider->GetOwner())
	{
		m_pPlayer2 = nullptr;
	}
}
