#pragma once

#include <atomic>
#include <array>
#include <vector>
#include "Core/Math.h"
#include "Core/ECS/Entity.h"
#include "Core/ECS/Systems/Pathfinding/Grid/PathfindingChunk.h"

namespace pathfinding
{
	struct Node;
	class ContinuumCrowdsGroup;
	class PathfindingObstacle;

	/* A value copy of one obstacle, taken on the main thread. Same reason as
	   PathfinderGroup::AgentState, and the same hazard: a PathfindingObstacle is
	   a component, buildDiscomfortField runs on a worker thread, and a chunk
	   unload destroys the entity it belongs to. */
	struct ObstacleState
	{
		Vector3 m_position = Vector3(0.f);
		Vector3 m_halfBoxSize = Vector3(0.f);
		float m_fDiscomfort = 0.f;
	};

	class ChunkGrid : public Entity
	{
	public:
		// Rendering
		enum RenderMode
		{
			NONE,
			ONLY_CENTER_CHUNK,
			ALL_CHUNKS
		};
		RenderMode m_renderMode;
		ContinuumCrowdsGroup* m_groupToDraw;

		// Grid
		enum GridJob
		{
			BUILD_SHARED_FIELD,
			BUILD_GROUP_FIELDS,
			REBUILD_GRID,
			RECONNECT_GRID
		};
		GridJob m_currentGridJob;
		static const unsigned int g_GRIDSIZE = 3;
		std::array<Chunk, g_GRIDSIZE * g_GRIDSIZE> m_grid;
		
		// Editor variables
		Entity* m_gridCenterEntity;
		bool m_bGenerateVerticalNodes;
		int m_iGridCoarseness;
		float m_fDensityExponent;
		float m_fMaxAgentsPerNode;
		float m_fMinDensity;
		float m_fMaxDensity;

		/* How many jobs are currently locking the grid; the grid may only be
		 * rebuilt at zero, because rebuilding frees the chunks a job is walking.
		 *
		 * **Atomic, and it was a plain int.** It is incremented on the main
		 * thread and decremented from job-completion callbacks on worker
		 * threads, so a non-atomic read-modify-write loses decrements - and a
		 * lost decrement is not the dangerous direction. The dangerous one is
		 * the main thread reading a stale zero while a job is still iterating
		 * m_grid, calling rebuildGrid, and freeing the chunks underneath it:
		 * ContinuumCrowdsGroup::updatePaths then segfaults on a node access with
		 * a perfectly valid `this` and a perfectly valid m_pGrid, which is
		 * exactly how it was reported. The default sequential consistency also
		 * supplies the release/acquire edge that made the job's writes visible
		 * here by luck rather than by rule. */
		std::atomic<int> m_iGridLocks;
		bool m_rebuildGrid;
		std::vector<PathfinderGroup*> m_groups;
		std::vector<PathfindingObstacle*> m_obstacles;
		std::vector<ObstacleState> m_obstacleStates;
		std::vector<std::pair<int, ChunkConnections>> m_chunksNeedingConnecting;

	private:
		IVector2 m_gridCenter;
		IVector2 m_gridCenterTemp;
		// (group, its id). See addGroup for why the id is carried separately.
		std::vector<std::pair<PathfinderGroup*, int>> m_groupsToAdd;
		std::vector<std::pair<PathfinderGroup*, int>> m_groupsToRemove;
		float m_fTimer = 0;
		float m_fRebuildInterval = 0.f;
		int m_iRebuildCount = 0;

	public:
		ChunkGrid(World* pWorld);
		virtual ~ChunkGrid();

		void Awake() override;
		void Start() override;
		void Tick(float deltaTime) override;

		void addGroup(PathfinderGroup& group);
		void removeGroup(PathfinderGroup& group);
		
		void addObstacle(PathfindingObstacle& obstacle);
		void removeObstacle(PathfindingObstacle& obstacle);

		static IVector2 getGridPos(const IVector3& worldPos);
		int getChunkIdx(const IVector2& gridPos);

		Chunk* getChunk(const IVector2& gridPos);
		Node* getNode(const IVector3& worldPos);

		RTTR_ENABLE(Entity)
		RTTR_REGISTRATION_FRIEND

	private:
		void addAndRemoveGroups();

		/* Rebuilds every snapshot the jobs read, from the live lists. Main
		   thread, and only while m_iGridLocks is zero. See the definition. */
		void syncJobSnapshots();

		void rebuildGrid(const IVector2& gridCenter);

		// Calculate shared fields
		void buildDiscomfortField();
		void splatEnity(const Vector3& position, const Vector3& halfBoxSize, const Vector3& velocity);
		void buildAvgVelocityField();
	};
}