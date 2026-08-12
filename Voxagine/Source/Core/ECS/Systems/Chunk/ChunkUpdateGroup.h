#pragma once
#include "Core/Math.h"
#include <chrono>
#include <vector>

class Chunk;
class ChunkUpdateGroup
{
public:
	/* The order is the order they run in. CHUNK_STREAMING_PLAN.md's design
	   paragraph is the reference; the short version is that everything before
	   US_COMMIT is preparation nobody can observe, US_COMMIT is the one
	   main-thread transaction that publishes the new window (R2), and
	   everything after it operates on a world that has already moved. */
	enum class UpdateState
	{
		US_INIT,
		US_WAIT,
		US_RENDERING,
		/* The atomic publish: physics volumes, world offset, buffer swap and
		   camera, together or not at all. */
		US_COMMIT,
		/* Deserializing incoming roots and refreshing moved ones. After the
		   commit, so a static renderer's stamp lands against the offset it will
		   actually be drawn at. Still unbounded - phases 2 and 3. */
		US_LOADING_ENTITIES,
		US_UNLOADING
	};

	struct Item
	{
		enum class Target
		{
			T_ASYNC_LOAD,
			T_LOAD,
			T_ASYNC_UNLOAD,
			T_MOVE
		};

		Item(Target _target, Chunk* _pChunk, UVector2 _gridIndex, bool bDone = true) :
			ItemTarget(_target),
			bIsDone(bDone),
			pChunk(_pChunk),
			GridTargetIndex(_gridIndex) {}

		Target ItemTarget;
		bool bIsDone = true;
		Chunk* pChunk = nullptr;
		UVector2 GridTargetIndex = UVector2(0, 0);
	};

	ChunkUpdateGroup(uint32_t gridTargetIndex, Vector3 worldOffset);
	~ChunkUpdateGroup() {};

	UpdateState GetState() const { return m_updateState; }
	void SetState(UpdateState state) { m_updateState = state; }

	void AddItem(ChunkUpdateGroup::Item newItem);
	std::vector<Item>& GetItems() { return m_UpdateItems; }

	uint32_t GetId() const { return m_uiUpdateId; }
	Vector3 GetWorldOffset() const { return m_worldOffset; }

	bool IsChunkScheduledFor(Chunk* pChunk, Item::Target target);
	bool IsRendering() const { return m_bRendering; }
	void SetRendering(bool bRendering) { m_bRendering = bRendering; }

	/* Has this group published its window? Exactly one transition to true per
	   group, ever (R2) - ChunkSystem asserts it and StreamingCounters counts
	   it, so the invariant is checkable in Release too. */
	bool HasCommitted() const { return m_bCommitted; }
	void MarkCommitted() { m_bCommitted = true; }

	/* End-to-end latency of a window transition: how long the player waits
	   between crossing a boundary and the new chunks being there. Distinct from
	   the *frame* cost the hitch gate measures, and the two move in opposite
	   directions - spreading work across frames removes the hitch and adds
	   latency - so a phase that improves one has to be able to see the other.
	   Reported under VOXAGINE_CHUNK_IO_TIMINGS, never by default (rule R9). */
	double MillisecondsSinceCreated() const
	{
		return std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - m_Created).count();
	}

	uint32_t GetAdvanceCount() const { return m_uiAdvances; }
	void CountAdvance() { ++m_uiAdvances; }

	inline bool operator()(const ChunkUpdateGroup& group) const { return group.m_uiUpdateId == m_uiUpdateId; }

private:
	std::vector<Item> m_UpdateItems;
	UpdateState m_updateState = UpdateState::US_INIT;
	uint32_t m_uiUpdateId = UINT32_MAX;
	Vector3 m_worldOffset = Vector3(0);
	bool m_bRendering = false;
	bool m_bCommitted = false;

	std::chrono::steady_clock::time_point m_Created = std::chrono::steady_clock::now();
	uint32_t m_uiAdvances = 0;
};