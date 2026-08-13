#include "pch.h"
#include "Core/ECS/Systems/Pathfinding/Navigation/PathfinderGoal.h"

#include <rttr/registration>
#include "Core/MetaData/PropertyTypeMetaData.h"

RTTR_REGISTRATION
{
	rttr::registration::class_<pathfinding::PathfinderGoal>("PathfinderGoal")
	.constructor<Entity*>()(rttr::policy::ctor::as_raw_ptr)
	.property("Pathfinder Group", &pathfinding::PathfinderGoal::m_group) (RTTR_PUBLIC)
	.property("Project Goal On Grid", &pathfinding::PathfinderGoal::m_bProjectPosition) (RTTR_PUBLIC)
	.property("Potential", &pathfinding::PathfinderGoal::m_fPotential) (RTTR_PUBLIC);
}

namespace pathfinding
{
	PathfinderGoal::PathfinderGoal(Entity * pOwner) :
		Component(pOwner),
		m_group(nullptr),
		m_pRegisteredGroup(nullptr),
		m_bProjectPosition(true),
		m_fPotential(0)
	{}

	PathfinderGoal::~PathfinderGoal()
	{
		if (m_pRegisteredGroup != nullptr)
			m_pRegisteredGroup->removeGoal(*this);
	}

	void PathfinderGoal::SetGroup(ContinuumCrowdsGroup* pGroup)
	{
		m_group = pGroup;
		SyncGroupRegistration();
	}

	void PathfinderGoal::SyncGroupRegistration()
	{
		if (m_pRegisteredGroup == m_group)
			return;

		if (m_pRegisteredGroup != nullptr)
			m_pRegisteredGroup->removeGoal(*this);

		m_pRegisteredGroup = m_group;

		if (m_pRegisteredGroup != nullptr)
			m_pRegisteredGroup->addGoal(*this);
	}

	void PathfinderGoal::ForgetGroup(const PathfinderGroup* pGroup)
	{
		if (m_pRegisteredGroup == pGroup)
			m_pRegisteredGroup = nullptr;

		if (m_group == pGroup)
			m_group = nullptr;
	}

	void PathfinderGoal::Start()
	{
		Component::Start();

		SyncGroupRegistration();
	}

	IVector3 PathfinderGoal::getGoalWorldPos() const
	{
		return ((Component*)this)->GetOwner()->GetTransform()->GetPosition();
	}
}