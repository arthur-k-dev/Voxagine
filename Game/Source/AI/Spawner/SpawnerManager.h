#pragma once

#include "Core/Math.h"
#include "Core/ECS/Entity.h"

class BoundingWall;
class Spawner;
class BoxCollider;
class SpawnerManager : public Entity
{
public:
	SpawnerManager(World* pWorld) : Entity(pWorld) { }

	Vector3 GetBoxSize() const { return v3InitializeBoxSize; }
	void SetBoxSize(Vector3 v3BoxSize);

	void Awake() override;
	void Start() override;
	void FixedTick(const ::GameTimer&) override;

private:
	/* Subscribes to any spawner in the list that has appeared since the last
	   call, and reports whether the list holds a live one.
	 *
	 * The list is a serialized link and three of Fishing_Village_Beat1's four
	 * spawners live in a different chunk from their manager, so "null" at Start
	 * means "not resolved yet" far more often than it means "not there". It has
	 * to be re-asked every tick rather than answered once. */
	bool RefreshSpawnerLinks();

	/* The live spawners, resolved from ids. Never holds a pointer across a
	   frame - see RefreshSpawnerLinks. */
	std::vector<Spawner*> ResolveLiveSpawners();

	/* VOXAGINE_GAMEPLAY_DEBUG=1 - see the definitions. */
	static bool GameplayDebugEnabled();
	void ReportState(const char* pWhere);

public:

	void OnCollisionEnter(Collider*, const Manifold&) override;
	void OnCollisionStay(Collider*, const Manifold&) override;
	void OnCollisionExit(Collider*, const Manifold&) override;
	
	Event<> OnSpawningCompleted;

protected:
	/**
	 * @brief - Collider which the player can collide to
	 * to activate all the linked spawners.
	 */
	BoxCollider* m_pCollider = nullptr;
	/**
	 * @brief - Current Players inside the trigger box.
	*/
	Entity* m_pPlayer1 = nullptr, * m_pPlayer2 = nullptr;

	/**
	 * @brief Linked Spawners in the scene
	 */
	std::vector<Spawner*> m_vSpawnerEntities = {};

	/* **The spawners this manager owns, by identity.**
	 *
	 * The serialized Spawners list hands out raw Spawner* and chunk streaming
	 * frees the entities under them. Guarding by dereferencing - "is the owner
	 * destroyed?" - crashed *inside the guard*, because the owner pointer was
	 * not null, it was freed, and a pointer you must dereference to validate
	 * cannot be validated.
	 *
	 * So each link is adopted exactly once, while it is known good, and reduced
	 * to its entity id; the pointer is then dropped from the list so nothing can
	 * read it again. Everything afterwards resolves ids through the world, which
	 * is the only thing that knows what is alive. Same rule as PlayerSlot, and
	 * it self-heals when a chunk comes back. */
	std::vector<uint64_t> m_vSpawnerOwnerIds = {};

	/* Whether a live spawner has ever been seen. The manager finishes - opens
	   its walls and destroys itself - when it has seen one and none are left;
	   without this it would finish on its first frame, while its cross-chunk
	   links are still null. */
	bool m_bHasSeenSpawners = false;

	/* Diagnostics only: what the last report said, so a per-tick callback
	   reports once per change rather than once per tick. */
	uint32_t m_uiReportedLive = 0xFFFFFFFFu;
	bool m_bReportedStay = false;

	/**
	 * @brief - Initial size of the trigger box
	*/
	Vector3 v3InitializeBoxSize = Vector3(50.0f);

	/**
	 * @brief - Bounding wall that needs to block the player
	 * from progressing
	 */
	std::vector<BoundingWall*> m_BoundWalls = {};

	RTTR_ENABLE(Entity)
	RTTR_REGISTRATION_FRIEND
};
