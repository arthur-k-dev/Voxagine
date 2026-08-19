#pragma once
#include <vector>
#include <unordered_map>
#include "Core/Math.h"
#include "Core/ECS/Entity.h"

namespace pathfinding
{
	class Pathfinder;
	class PathfinderGoal;
	class ChunkGrid;
	struct Node;

	/* A value copy of one agent, taken on the main thread.
	 *
	 * The grid's jobs used to walk m_agents and dereference the Pathfinder*s
	 * themselves, on a worker thread, while the main thread was free to destroy
	 * the entities those components belong to - which is exactly what a chunk
	 * unload does to the enemies that spawned inside it. Two ways that crashes,
	 * and the second is why a null check in the job cannot help: ~Pathfinder
	 * erases from m_agents, so a job iterating the vector walks past the end of
	 * a container that shrank under it; and until that erase runs the entry is a
	 * perfectly non-null pointer to freed memory.
	 *
	 * m_pAgent is an identity, not a pointer to use: it is compared against the
	 * live list when the results are applied and never dereferenced. */
	struct AgentState
	{
		const Pathfinder* m_pAgent = nullptr;

		Vector3 m_position = Vector3(0.f);
		Vector3 m_transformPosition = Vector3(0.f);
		Vector3 m_halfBoxSize = Vector3(0.f);
		Vector3 m_velocity = Vector3(0.f);
		float m_fMinVelocity = 0.f;
		float m_fMaxVelocity = 0.f;
		bool m_bCohesion = false;

		// Written by the job; applied back to the live agent by the main thread.
		float m_flockVelocityX = 0.f;
		float m_flockVelocityY = 0.f;
		bool m_bHasFlockVelocity = false;
	};

	// The same, for a goal. See AgentState - a goal is a component too.
	struct GoalState
	{
		IVector3 m_worldPos = IVector3(0);
		float m_fPotential = 0.f;
		bool m_bProjectPosition = true;
	};

	class PathfinderGroup : public Entity
	{
	public:
		ChunkGrid* m_pGrid;
		std::vector<PathfinderGoal*> m_goals;
		std::vector<Pathfinder*> m_agents;
		float m_fPathSmoothing = 1.f;

		/* What the jobs read instead of m_goals and m_agents. Only ChunkGrid::
		   Tick may touch these, and only while m_iGridLocks is zero - see
		   syncJobSnapshots. */
		std::vector<AgentState> m_agentStates;
		std::vector<GoalState> m_goalStates;

	private:
		int m_id;
		static int s_nextId;

	public:
		PathfinderGroup(World* pWorld);
		~PathfinderGroup();
		virtual void Awake() override;
		virtual void Start() override;
		virtual void Tick(float deltaTime) override;

		/* Finds the world's PathfindingGrid and registers with it, if it has not
		   already. False while there is none - an ordinary state under chunk
		   streaming, both before the grid is admitted and after one is
		   destroyed. See the definition. */
		bool ResolveGrid();

		/* Applies the results of the last job to the agents that are still here,
		   then rebuilds the snapshots from the live lists. Main thread only, and
		   only while no job is reading them. See the definition. */
		void syncJobSnapshots();

		virtual void updatePaths() = 0;
		virtual void updateAgents(AgentState& agent) {};
		int getId() const;

		void addAgent(Pathfinder& pathfinder);
		void removeAgent(Pathfinder& pathfinder);

		void addGoal(PathfinderGoal& goal);
		void removeGoal(PathfinderGoal& goal);
		bool isGoal(const IVector3 & nodeWorldPos) const;

		// Get the desired velocity and height given an agent.
		virtual void getDesiredVeclocityAndHeight(Vector2& o_velocity, float& o_height, Node** o_node, Pathfinder& pathfinder) const;
		virtual Vector2 getDesiredVeclocity(Pathfinder& pathfinder) const;
		virtual float getDesiredHeight(Pathfinder& pathfinder) const;

		// Get the desired velocity and height given an position.
		virtual void getDesiredVeclocityAndHeight(Vector2& o_velocity, float& o_height, Node** o_node, const IVector3& worldPos) const = 0;
		virtual Vector2 getDesiredVeclocity(const IVector3& worldPos) const = 0;
		virtual float getDesiredHeight(const IVector3& worldPos) const = 0;

		RTTR_ENABLE(Entity)
		RTTR_REGISTRATION_FRIEND
	};
}