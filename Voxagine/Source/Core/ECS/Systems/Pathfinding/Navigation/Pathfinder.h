#pragma once
#include "Core/ECS/Components/BehaviorScript.h"
#include "Core/Math.h"
#include "Core/ECS/Systems/Pathfinding/Navigation/ContinuumCrowdsGroup.h"

namespace pathfinding
{
	class Pathfinder : public BehaviorScript
	{
	public:
		bool m_findPath;
		bool m_applyVelocity;
		bool m_applyHeight;
		ContinuumCrowdsGroup* m_group;
		float m_fMinVelocity;
		float m_fMaxVelocity;
		bool m_bCanMoveDiagonal;
		bool m_bCohesion;
		bool m_bClampVelocity;

		std::atomic<float> m_flockVelocityX;
		std::atomic<float> m_flockVelocityY;

	private:
		/* The group this agent is actually in the m_agents list of, which is
		 * not the same question as m_group and must not be confused with it.
		 *
		 * Registration used to be keyed off m_group at both ends: Start added
		 * to m_group, ~Pathfinder removed from m_group. That is only correct
		 * while nothing changes m_group in between, and two things do -
		 * Spawner::SpawnEnemies writes it directly, and ~PathfinderGroup nulls
		 * it on *every* Pathfinder in the world rather than on its own agents.
		 * So one group being destroyed set m_group to null on the other
		 * group's agents while leaving them in its m_agents; each of those
		 * monsters then died without deregistering, and the surviving group's
		 * list filled with freed pointers. Confirmed under ASan: a Pathfinder
		 * allocated by Spawner::SpawnEnemies, freed by ~Pathfinder, and still
		 * read out of m_agents afterwards.
		 *
		 * Deregistration goes through this, so it cannot be aimed at the wrong
		 * list or skipped because somebody else moved m_group. */
		ContinuumCrowdsGroup* m_pRegisteredGroup;

		/* Zero, not stack garbage. GLM does not default-initialise and
		   GLM_FORCE_CTOR_INIT is not set, and Spawner::SpawnEnemies registers
		   an agent before Start has run updatePositionVelocitySize - so these
		   were splatted into the density field as whatever was on the stack. */
		Vector3 m_position;
		Vector3 m_velocity;
		Vector3 m_halfBoxSize;

		bool m_bIsOnGrid;
		Vector3 m_desiredVelocity;

	public:
		Pathfinder(Entity* pOwner);
		~Pathfinder();
		void Start() override;
		void FixedTick(const GameTimer& time) override;
		void PostTick(float fDeltaTime) override;

		/* Sets the group and moves the registration with it. This is the only
		   supported way to change an agent's group from outside; writing
		   m_group and calling addAgent by hand is what Spawner::SpawnEnemies
		   did, and it is how the two could disagree. */
		void SetGroup(ContinuumCrowdsGroup* pGroup);

		/* Makes the registration match m_group, adding or removing as needed.
		   Idempotent, so Start and SetGroup can both call it. */
		void SyncGroupRegistration();

		/* The group is being destroyed: drop both pointers without touching
		   its lists. Called from ~PathfinderGroup. */
		void ForgetGroup(const PathfinderGroup* pGroup);

		void updatePositionVelocitySize();
		bool IsOnGrid() const;

		virtual Vector3 getPosition();
		virtual Vector3 getHalfBoxSize();
		virtual Vector3 getVelocity();

		RTTR_ENABLE(BehaviorScript)
		RTTR_REGISTRATION_FRIEND

	private:
		std::vector<IVector3> path;
		void calculatePath();
	};
}