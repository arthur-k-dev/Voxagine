#pragma once
#include "Core/Math.h"
#include "Core/ECS/Systems/Pathfinding/Navigation/ContinuumCrowdsGroup.h"

namespace pathfinding
{
	class PathfinderGoal : public Component
	{
	public:
		pathfinding::ContinuumCrowdsGroup* m_group;
		bool m_bProjectPosition;
		float m_fPotential;

	private:
		// See Pathfinder::m_pRegisteredGroup; a goal has the same asymmetry.
		pathfinding::ContinuumCrowdsGroup* m_pRegisteredGroup;

	public:
		PathfinderGoal(Entity* pOwner);
		~PathfinderGoal();
		void Start() override;

		void SetGroup(ContinuumCrowdsGroup* pGroup);
		void SyncGroupRegistration();
		void ForgetGroup(const PathfinderGroup* pGroup);

		IVector3 getGoalWorldPos() const;

		RTTR_ENABLE(Component)
		RTTR_REGISTRATION_FRIEND
	};
}