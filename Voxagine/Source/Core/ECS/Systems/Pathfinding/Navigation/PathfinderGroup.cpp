#include "pch.h"
#include "PathfinderGroup.h"

#include <rttr/registration>
#include "Core/MetaData/PropertyTypeMetaData.h"
#include "Core/ECS/Systems/Pathfinding/Navigation/Pathfinder.h"
#include "Core/ECS/Systems/Pathfinding/Grid/PathfindingChunkGrid.h"
#include "Core/ECS/Systems/Pathfinding/Navigation/PathfinderGoal.h"

RTTR_REGISTRATION
{
	rttr::registration::class_<pathfinding::PathfinderGroup>("PathfinderGroup")
	.property("Path Smoothing", &pathfinding::PathfinderGroup::m_fPathSmoothing)(RTTR_PUBLIC);
}

namespace pathfinding
{
	int PathfinderGroup::s_nextId = 0;

	PathfinderGroup::PathfinderGroup(World * pWorld) :
		Entity(pWorld),
		m_pGrid(nullptr),
		m_id(s_nextId)
	{
		s_nextId++;
	}

	/* This destructor is where the reported segfault came from, two steps
	 * removed from where it landed.
	 *
	 * It used to null m_group on *every* Pathfinder and PathfinderGoal in the
	 * world, unconditionally, rather than on the ones that name this group. A
	 * level has more than one group, so destroying one - which a chunk unload
	 * does - told the other group's agents they had no group while leaving them
	 * in its m_agents. ~Pathfinder then read m_group to decide what to
	 * deregister from, found null, and did nothing; the surviving group's list
	 * accumulated freed pointers, and the next thing to walk it crashed.
	 *
	 * Two changes. Registration is no longer keyed off m_group at all (see
	 * Pathfinder::m_pRegisteredGroup), and this walk is restricted to links
	 * that actually point here. */
	PathfinderGroup::~PathfinderGroup()
	{
		/* Everything registered here has to forget it before m_agents/m_goals
		   go away, or its destructor deregisters from freed memory. This is the
		   exact set, unlike the world walk below - it does not depend on
		   GetEntities seeing the owner or GetComponent reaching the component. */
		for (Pathfinder* pAgent : m_agents)
		{
			if (pAgent != nullptr)
				pAgent->ForgetGroup(this);
		}
		m_agents.clear();

		for (PathfinderGoal* pGoal : m_goals)
		{
			if (pGoal != nullptr)
				pGoal->ForgetGroup(this);
		}
		m_goals.clear();

		/* And anything that names this group without being registered with it -
		   a component whose Start has not run yet. Guarded by the pointer
		   actually being this one. */
		if (GetWorld() != nullptr)
		{
			auto entities = GetWorld()->GetEntities();
			for (auto& entity : entities)
			{
				if (entity == this)
					continue;

				Pathfinder* pathfinder = entity->GetComponent<Pathfinder>();
				if (pathfinder != nullptr)
					pathfinder->ForgetGroup(this);

				PathfinderGoal* pathfinderGoal = entity->GetComponent<PathfinderGoal>();
				if (pathfinderGoal != nullptr)
					pathfinderGoal->ForgetGroup(this);
			}
		}

		if (m_pGrid != nullptr)
			m_pGrid->removeGroup(*this);
	}

	void PathfinderGroup::Awake()
	{
		Entity::Awake();
	}

	void PathfinderGroup::Start()
	{
		Entity::Start();

		/* No assert: with chunk streaming there may be no grid *yet*. Start runs
		   when this group is admitted, and the PathfindingGrid entity is admitted
		   on its own schedule, so "not found" here is an ordinary early state
		   rather than a broken level. Tick keeps asking. */
		ResolveGrid();

		for (auto& agent : m_agents)
			agent->updatePositionVelocitySize();
	}

	bool PathfinderGroup::ResolveGrid()
	{
		if (m_pGrid != nullptr)
			return true;

		m_pGrid = dynamic_cast<ChunkGrid*>(GetWorld()->FindEntity("PathfindingGrid"));

		if (m_pGrid == nullptr)
			return false;

		m_pGrid->addGroup(*this);

		return true;
	}

	void PathfinderGroup::Tick(float deltaTime)
	{
		Entity::Tick(deltaTime);

		/* Re-resolved rather than looked up once at Start, and this is the same
		 * rule the rest of the tree has had to learn: a link that is null now is
		 * not a link that will always be null.
		 *
		 * Two ways this group loses its grid. It can be admitted before the grid
		 * is - Start then found nothing and the group was orphaned for the whole
		 * level, with no path updates and no complaint. And ~ChunkGrid nulls
		 * m_pGrid on every group in the world, so a world switch or a level
		 * ending leaves every group pointing at nothing; when a grid comes back
		 * the group has to register with it or it is never ticked again.
		 *
		 * The find is by name and therefore not free, which is why it only runs
		 * while there is no grid. */
		ResolveGrid();

		if (m_goals.size() == 0 || m_agents.size() == 0)
			return;
	}

	void PathfinderGroup::syncJobSnapshots()
	{
		/* Apply what the last job computed, to the agents that are still here.
		 *
		 * m_pAgent is compared and never dereferenced, so an agent destroyed
		 * while the job ran simply fails to match and its result is dropped -
		 * which is the whole point, because it is the only thing that can be
		 * done safely with a result computed for something that no longer
		 * exists. (An address reused by a Pathfinder added since would take one
		 * frame of somebody else's flocking velocity. It is a float that is
		 * recomputed next slot, not a lifetime hazard.) */
		for (const AgentState& state : m_agentStates)
		{
			if (!state.m_bHasFlockVelocity)
				continue;

			auto it = std::find(m_agents.begin(), m_agents.end(), state.m_pAgent);
			if (it == m_agents.end())
				continue;

			(*it)->m_flockVelocityX.store(state.m_flockVelocityX);
			(*it)->m_flockVelocityY.store(state.m_flockVelocityY);
		}

		// Rebuild from the live lists.
		m_agentStates.clear();
		m_agentStates.reserve(m_agents.size());
		for (Pathfinder* pAgent : m_agents)
		{
			if (pAgent == nullptr)
				continue;

			Transform* pTransform = pAgent->GetTransform();
			if (pTransform == nullptr)
				continue;

			AgentState state;
			state.m_pAgent = pAgent;
			/* Two positions, because the two readers disagreed and both are kept
			   as they were: the shared field splats the cached m_position, which
			   Pathfinder::PostTick deliberately holds still while that job runs,
			   and separation used the live transform. */
			state.m_position = pAgent->getPosition();
			state.m_transformPosition = pTransform->GetPosition();
			state.m_halfBoxSize = pAgent->getHalfBoxSize();
			state.m_velocity = pAgent->getVelocity();
			state.m_fMinVelocity = pAgent->m_fMinVelocity;
			state.m_fMaxVelocity = pAgent->m_fMaxVelocity;
			state.m_bCohesion = pAgent->m_bCohesion;
			m_agentStates.push_back(state);
		}

		m_goalStates.clear();
		m_goalStates.reserve(m_goals.size());
		for (PathfinderGoal* pGoal : m_goals)
		{
			if (pGoal == nullptr || pGoal->GetOwner() == nullptr)
				continue;

			GoalState state;
			state.m_worldPos = pGoal->getGoalWorldPos();
			state.m_fPotential = pGoal->m_fPotential;
			state.m_bProjectPosition = pGoal->m_bProjectPosition;
			m_goalStates.push_back(state);
		}
	}

	int PathfinderGroup::getId() const
	{
		return m_id;
	}

	void PathfinderGroup::addAgent(Pathfinder & pathfinder)
	{
		m_agents.push_back(&pathfinder);
	}

	void PathfinderGroup::removeAgent(Pathfinder & pathfinder)
	{
		if (m_agents.size() > 0)
			m_agents.erase(std::remove(m_agents.begin(), m_agents.end(), &pathfinder), m_agents.end());
	}

	void PathfinderGroup::addGoal(PathfinderGoal & goal)
	{
		m_goals.push_back(&goal);
	}

	void PathfinderGroup::removeGoal(PathfinderGoal & goal)
	{
		if (m_goals.size() > 0)
			m_goals.erase(std::remove(m_goals.begin(), m_goals.end(), &goal), m_goals.end());
	}

	bool PathfinderGroup::isGoal(const IVector3 & nodeWorldPos) const
	{
		auto it = std::find_if(m_goals.begin(), m_goals.end(), 
							   [&nodeWorldPos](const PathfinderGoal* goal) 
							   { return goal->getGoalWorldPos() == nodeWorldPos; });

		return it != m_goals.end();
	}

	void PathfinderGroup::getDesiredVeclocityAndHeight(Vector2 & o_velocity, float & o_height, Node** o_node, Pathfinder & pathfinder) const
	{
		getDesiredVeclocityAndHeight(o_velocity, o_height, o_node, pathfinder.getPosition());
	}

	Vector2 PathfinderGroup::getDesiredVeclocity(Pathfinder & pathfinder) const
	{
		return getDesiredVeclocity(pathfinder.getPosition());
	}

	float PathfinderGroup::getDesiredHeight(Pathfinder & pathfinder) const
	{
		return getDesiredHeight(pathfinder.getPosition());
	}
}