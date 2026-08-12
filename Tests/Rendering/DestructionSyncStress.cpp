#include "VoxApp.h"

#include "Core/ECS/Entity.h"
#include "Core/ECS/Entities/Camera.h"
#include "Core/ECS/Systems/Chunk/Chunk.h"
#include "Core/ECS/Systems/Chunk/ChunkSystem.h"
#include "Core/ECS/Systems/Physics/PhysicsSystem.h"
#include "Core/ECS/Systems/Physics/VoxelGrid.h"
#include "Core/ECS/World.h"
#include "Core/LaunchOptions.h"
#include "Core/Platform/Rendering/CommandEngine.h"
#include "Core/Platform/Rendering/RenderContext.h"

#include <SDL3/SDL_main.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
	class DestructionSyncStress final : public VoxApp
	{
	public:
		bool Passed() const
		{
			if (m_bStreamingOnly)
			{
				return m_bFinished && m_bPassed &&
				       m_uiChunkTransitions == 1 &&
				       m_uiChunkTransitionFrameSamples != 0 &&
				       m_uiChunkTransitionHitchViolations == 0;
			}

			return m_bFinished && m_bPassed &&
			       m_uiBurstsIssued == kBurstCount &&
			       m_uiChunkTransitions == kExpectedChunkTransitions &&
			       m_uiLevelSwitches == kWorldCount - 1 &&
			       m_uiSteadyHitchViolations == 0 &&
			       m_uiChunkTransitionHitchViolations == 0 &&
			       m_uiLevelSwitchHitchViolations == 0 &&
			       m_DestroyedChunkIds.size() >= kMinDestroyedChunks &&
			       m_DestroyedOwnerIds.size() >= kMinDestroyedModels;
		}

	protected:
		void OnCreate() override
		{
			VoxApp::OnCreate();
			m_bStreamingOnly = std::getenv("VOXAGINE_GPU_STREAMING_ONLY") != nullptr;
			if (m_bStreamingOnly)
			{
				/* Only so ObserveExpectedWorld expects Beat2; this mode does not
				   use the phase route. */
				m_uiWorldIndex = 1;
				m_uiPhase = kWorldFirstPhase[m_uiWorldIndex];
				fprintf(stderr,
				        "[gpu-test] streaming gate armed: timing one Beat2 window transition "
				        "driven by --ui-script, %.2f ms frame limit\n",
				        kMaxStreamingTransitionFrameMs);
				return;
			}

			fprintf(stderr,
			        "[gpu-test] armed: %u productive destruction bursts, %u resident-window phases, "
			        "%u real level switch; CPU hitch limits %.0f/%.0f/%.0f ms "
			        "(steady/chunk/level)\n",
			        kBurstCount, kPhaseCount, kWorldCount - 1,
			        kMaxSteadyFrameMs, kMaxChunkTransitionFrameMs, kMaxLevelSwitchFrameMs);
		}

		void OnUpdate() override
		{
			MeasureFrameInterval();
			if (m_bFinished)
				return;

			WatchDirectTimeline();
			if (m_bFinished)
				return;

			if (m_bDraining)
			{
				DrainGPU();
				return;
			}

			DriveDestruction();
		}

		void OnExit() override
		{
			if (!m_bFinished)
			{
				if (m_bStreamingOnly)
				{
					fprintf(stderr,
					        "[gpu-test] FAIL: application exited before the focused chunk "
					        "transition completed\n");
					m_bPassed = false;
					VoxApp::OnExit();
					return;
				}

				fprintf(stderr,
				        "[gpu-test] FAIL: application exited before the fixture completed "
				        "(%u/%u bursts, %u/%u chunk switches, %u/%u level switches)\n",
				        m_uiBurstsIssued, kBurstCount,
				        m_uiChunkTransitions, kExpectedChunkTransitions,
				        m_uiLevelSwitches, kWorldCount - 1);
				m_bPassed = false;
			}

			VoxApp::OnExit();
		}

	private:
		/* Each world alternates between positions two chunk columns apart. Every
		   window switch therefore unloads two columns and loads two different
		   columns. Halfway through, the normal asynchronous level-switch path
		   tears the first World down and initializes the second one. */
		static constexpr uint32_t kWorldCount = 2;
		static constexpr uint32_t kPhaseCount = 8;
		/* Beat 1 has enough geometry for repeated alternation. Beat 2 contributes
		   a fresh initial window and one fully productive switch; returning after
		   that would measure geometry already depleted in its initial phase. */
		static constexpr uint32_t kWorldFirstPhase[kWorldCount + 1] = { 0, 6, 8 };
		static constexpr uint32_t kWindowBaseX[kPhaseCount] = { 0, 2, 0, 2, 0, 2, 0, 2 };
		static constexpr uint32_t kWindowBaseZ[kWorldCount] = { 0, 2 };
		static constexpr const char* kWorldPaths[kWorldCount] = {
			"Content/Worlds/Fishing_Village/Fishing_Village_Beat1.wld",
			"Content/Worlds/Fishing_Village/Fishing_Village_Beat2.wld"
		};
		static constexpr uint32_t kBurstsPerPhase = 32;
		static constexpr uint32_t kBurstCount = kPhaseCount * kBurstsPerPhase;
		static constexpr uint32_t kExpectedChunkTransitions = 6;
		static constexpr uint32_t kFramesBetweenBursts = 6;
		static constexpr uint32_t kResidentStableFrames = 30;
		static constexpr uint32_t kDrainFrames = 120;
		static constexpr uint32_t kStallFrameLimit = 600;
		static constexpr uint32_t kMinDestroyedChunks = 4;
		static constexpr uint32_t kMinDestroyedModels = 8;
		static constexpr uint32_t kMinDestroyedChunksPerPhase = 2;
		static constexpr uint32_t kMinDestroyedModelsPerPhase = 2;
		static constexpr float kDestructionRadius = 6.f;
		/* Debug + validation is intentionally used for this GPU test. These are
		   hard hitch limits, not frame-rate targets: they allow ordinary Debug
		   variance while rejecting a visible main-thread stop. Whole-loop timing
		   includes ChunkSystem ticks, GPU waits and WorldManager::SwapWorlds. */
		static constexpr double kMaxSteadyFrameMs = 100.0;
		static constexpr double kMaxChunkTransitionFrameMs = 250.0;
		static constexpr double kMaxLevelSwitchFrameMs = 2000.0;

		/* The streaming gate's own budget, and the whole point of the mode.
		   250 ms certifies that a window transition happened, not that it was
		   invisible - it is a hitch limit for a fixture whose subject is
		   destruction. Docs/CHUNK_STREAMING_PLAN.md drives this one down to a
		   frame across phases 1-5; on master it fails, which is recorded as the
		   baseline rather than hidden. */
		static constexpr double kMaxStreamingTransitionFrameMs = 1000.0 / 60.0;

		double ChunkTransitionLimitMs() const
		{
			return m_bStreamingOnly
				? kMaxStreamingTransitionFrameMs
				: kMaxChunkTransitionFrameMs;
		}

		enum class IntervalKind
		{
			Ignore,
			Steady,
			ChunkTransition,
			LevelSwitch
		};

		struct ChunkCursor
		{
			Chunk* pChunk = nullptr;
			uint32_t uiChunkKey = 0;
			size_t uiNextVoxel = 0;
			size_t uiFallbackVoxel = std::numeric_limits<size_t>::max();
		};

		struct Candidate
		{
			Vector3 v3GridPosition = Vector3(0.f);
			uint16_t uiOwnerSlot = VoxelOwnerVolume::k_uiNoOwnerSlot;
			uint64_t uiOwnerId = 0;
			uint32_t uiChunkKey = 0;
			UVector2 v2ChunkIndex = UVector2(0);
			std::string sOwnerName;
		};

		enum class CandidateSearchResult
		{
			Found,
			Pending,
			Exhausted
		};

		struct ScopedOwnerId
		{
			uint32_t uiWorld = 0;
			uint64_t uiOwner = 0;

			bool operator==(const ScopedOwnerId& other) const
			{
				return uiWorld == other.uiWorld && uiOwner == other.uiOwner;
			}
		};

		struct ScopedOwnerIdHash
		{
			size_t operator()(const ScopedOwnerId& id) const
			{
				const size_t uiOwnerHash = std::hash<uint64_t>{}(id.uiOwner);
				return uiOwnerHash ^ (static_cast<size_t>(id.uiWorld) +
				       0x9e3779b9u + (uiOwnerHash << 6u) + (uiOwnerHash >> 2u));
			}
		};

		template <typename... TArgs>
		void Fail(const char* pFormat, TArgs... args)
		{
			if (m_bFinished)
				return;

			fprintf(stderr, "[gpu-test] FAIL: ");
			fprintf(stderr, pFormat, args...);
			fprintf(stderr, "\n");
			m_bPassed = false;
			m_bFinished = true;
			Exit();
		}

		uint32_t PhaseInWorld() const
		{
			return m_uiPhase - kWorldFirstPhase[m_uiWorldIndex];
		}

		uint64_t ScopedChunkKey(uint32_t uiChunkKey) const
		{
			return (static_cast<uint64_t>(m_uiWorldIndex) << 32u) | uiChunkKey;
		}

		void MeasureFrameInterval()
		{
			const auto now = std::chrono::steady_clock::now();
			if (!m_bHaveFrameTimestamp)
			{
				m_LastFrameTimestamp = now;
				m_bHaveFrameTimestamp = true;
				return;
			}

			const double fMilliseconds =
				std::chrono::duration<double, std::milli>(now - m_LastFrameTimestamp).count();
			m_LastFrameTimestamp = now;

			const char* pKind = nullptr;
			double fLimit = 0.0;
			double* pPeak = nullptr;
			uint64_t* pSamples = nullptr;
			uint32_t* pViolations = nullptr;
			switch (m_eExpectedInterval)
			{
			case IntervalKind::Steady:
				pKind = "steady destruction";
				fLimit = kMaxSteadyFrameMs;
				pPeak = &m_fPeakSteadyFrameMs;
				pSamples = &m_uiSteadyFrameSamples;
				pViolations = &m_uiSteadyHitchViolations;
				break;
			case IntervalKind::ChunkTransition:
				pKind = "chunk-window transition";
				fLimit = ChunkTransitionLimitMs();
				pPeak = &m_fPeakChunkTransitionFrameMs;
				pSamples = &m_uiChunkTransitionFrameSamples;
				pViolations = &m_uiChunkTransitionHitchViolations;
				break;
			case IntervalKind::LevelSwitch:
				pKind = "level switch";
				fLimit = kMaxLevelSwitchFrameMs;
				pPeak = &m_fPeakLevelSwitchFrameMs;
				pSamples = &m_uiLevelSwitchFrameSamples;
				pViolations = &m_uiLevelSwitchHitchViolations;
				break;
			case IntervalKind::Ignore:
				return;
			}

			*pPeak = std::max(*pPeak, fMilliseconds);
			++*pSamples;
			if (fMilliseconds > fLimit)
			{
				++*pViolations;
				fprintf(stderr,
				        "[gpu-test] CPU HITCH: %s took %.2f ms (limit %.2f ms, "
				        "world %u phase %u, violation %u)\n",
				        pKind, fMilliseconds, fLimit, m_uiWorldIndex, PhaseInWorld(),
				        *pViolations);
			}
		}

		bool ObserveExpectedWorld(World& world)
		{
			const std::string sWorldName = world.GetName();
			if (!m_bInitialWorldObserved)
			{
				if (sWorldName != kWorldPaths[m_uiWorldIndex])
				{
					Fail("initial world is '%s', expected '%s'",
					     sWorldName.c_str(), kWorldPaths[m_uiWorldIndex]);
					return false;
				}

				m_bInitialWorldObserved = true;
				fprintf(stderr, "[gpu-test] world %u ready: %s\n",
				        m_uiWorldIndex, sWorldName.c_str());
				return true;
			}

			if (!m_bAwaitingWorldSwitch)
			{
				if (sWorldName != kWorldPaths[m_uiWorldIndex])
				{
					Fail("unexpected active world '%s' while testing '%s'",
					     sWorldName.c_str(), kWorldPaths[m_uiWorldIndex]);
					return false;
				}
				return true;
			}

			const uint32_t uiNextWorld = m_uiWorldIndex + 1;
			if (sWorldName == kWorldPaths[m_uiWorldIndex])
				return false;

			if (uiNextWorld >= kWorldCount || sWorldName != kWorldPaths[uiNextWorld])
			{
				Fail("level switch activated '%s', expected '%s'",
				     sWorldName.c_str(), uiNextWorld < kWorldCount ? kWorldPaths[uiNextWorld] : "<none>");
				return false;
			}

			m_uiWorldIndex = uiNextWorld;
			++m_uiLevelSwitches;
			m_bAwaitingWorldSwitch = false;
			m_bCameraPinned = false;
			m_bPhaseReady = false;
			m_uiResidentStableFrames = 0;
			m_uiBurstCadence = 0;
			m_uiNextChunkCursor = 0;
			m_PreviousResidentChunkIds.clear();
			m_ChunkCursors.clear();
			m_PhaseOwnerIds.clear();
			m_PhaseDestroyedChunkIds.clear();

			fprintf(stderr,
			        "[gpu-test] level switch %u/%u committed: %s; old World destroyed, "
			        "new World initialized\n",
			        m_uiLevelSwitches, kWorldCount - 1, sWorldName.c_str());
			return true;
		}

		void BeginLevelSwitch(World& world)
		{
			if (m_bAwaitingWorldSwitch || m_uiWorldIndex + 1 >= kWorldCount)
			{
				Fail("invalid level-switch request after world %u", m_uiWorldIndex);
				return;
			}

			m_bAwaitingWorldSwitch = true;
			m_eExpectedInterval = IntervalKind::LevelSwitch;
			fprintf(stderr, "[gpu-test] requesting asynchronous level switch: %s -> %s\n",
			        world.GetName().c_str(), kWorldPaths[m_uiWorldIndex + 1]);
			world.OpenWorldAsync(kWorldPaths[m_uiWorldIndex + 1], true);
		}

		void WatchDirectTimeline()
		{
			PCommandEngine* pDirect = GetPlatform().GetRenderContext()->GetEngine("Direct");
			if (pDirect == nullptr)
				return;

			const uint64_t uiSubmitted = pDirect->GetValue();
			const uint64_t uiCompleted = pDirect->GetCompletedValue();

			if (!m_bHaveTimelineSample)
			{
				m_bHaveTimelineSample = true;
				m_uiTimelineStart = uiCompleted;
				m_uiLastCompletedTimeline = uiCompleted;
			}

			m_uiMaxSubmittedTimeline = std::max(m_uiMaxSubmittedTimeline, uiSubmitted);
			m_uiMaxCompletedTimeline = std::max(m_uiMaxCompletedTimeline, uiCompleted);

			if (uiCompleted < m_uiLastCompletedTimeline)
			{
				Fail("direct timeline regressed from %llu to %llu",
				     static_cast<unsigned long long>(m_uiLastCompletedTimeline),
				     static_cast<unsigned long long>(uiCompleted));
				return;
			}

			/* Being one or more submissions behind is normal. A stall is no
			   completed-value progress while work remains outstanding. */
			if (uiCompleted > m_uiLastCompletedTimeline || uiSubmitted <= uiCompleted)
			{
				m_uiLastCompletedTimeline = uiCompleted;
				m_uiTimelineNoProgressFrames = 0;
				return;
			}

			if (++m_uiTimelineNoProgressFrames >= kStallFrameLimit)
			{
				Fail("direct timeline made no progress at %llu/%llu for %u rendered frames",
				     static_cast<unsigned long long>(uiCompleted),
				     static_cast<unsigned long long>(uiSubmitted), kStallFrameLimit);
			}
		}

		bool ValidateResidentWindow(VoxelGrid& grid, ChunkSystem& chunks,
		                            std::unordered_set<uint32_t>& o_resident)
		{
			o_resident.clear();
			if (chunks.IsStreaming())
				return false;

			const std::unordered_map<uint32_t, Chunk*>& allChunks = chunks.GetChunks();
			if (allChunks.empty())
				return false;

			Chunk* pFirstChunk = allChunks.begin()->second;
			if (pFirstChunk == nullptr)
				return false;

			const UVector3 v3ChunkSize = pFirstChunk->GetChunkSize();
			const UVector2 v2WorldSize = chunks.GetWorldSize();
			if (v3ChunkSize.x == 0 || v3ChunkSize.z == 0 ||
			    v2WorldSize.x < v3ChunkSize.x * 5 || v2WorldSize.y < v3ChunkSize.z * 3)
			{
				Fail("map is too small for the stress route");
				return false;
			}

			const uint32_t uiNumChunksX = v2WorldSize.x / v3ChunkSize.x;
			const uint32_t uiBaseX = kWindowBaseX[m_uiPhase];
			const uint32_t uiBaseZ = kWindowBaseZ[m_uiWorldIndex];
			const Vector3 v3ExpectedOffset(
				static_cast<float>(uiBaseX * v3ChunkSize.x), 0.f,
				static_cast<float>(uiBaseZ * v3ChunkSize.z));
			const Vector3 v3ActualOffset = grid.GetWorldOffset();

			if (v3ActualOffset != v3ExpectedOffset)
				return false;

			for (uint32_t z = 0; z < 3; ++z)
			{
				for (uint32_t x = 0; x < 3; ++x)
				{
					const uint32_t uiWorldX = uiBaseX + x;
					const uint32_t uiWorldZ = uiBaseZ + z;
					const uint32_t uiChunkKey = uiWorldX + uiWorldZ * uiNumChunksX;
					const auto chunkIt = allChunks.find(uiChunkKey);

					if (chunkIt == allChunks.end() || chunkIt->second == nullptr)
						return false;

					Chunk* pChunk = chunkIt->second;
					if (!pChunk->IsLoaded() || !pChunk->IsTargetLoaded() ||
					    pChunk->IsLoading() || pChunk->IsUnloading() ||
					    pChunk->GetGridTarget() != UVector2(x, z))
					{
						return false;
					}

					std::vector<Voxel>& voxels = pChunk->GetVoxelData();
					VoxelOwnerVolume& owners = pChunk->GetOwnerVolume();
					const size_t uiExpectedVoxels = static_cast<size_t>(v3ChunkSize.x) *
					                                v3ChunkSize.y * v3ChunkSize.z;
					if (voxels.size() != uiExpectedVoxels || owners.Size() != uiExpectedVoxels)
						return false;

					/* This pointer equality proves that the VoxelGrid slot used by
					   destruction names this exact newly loaded chunk storage. */
					const Voxel* pGridVoxel = grid.GetVoxel(x * v3ChunkSize.x, 0, z * v3ChunkSize.z);
					if (pGridVoxel != voxels.data())
						return false;

					o_resident.insert(uiChunkKey);
				}
			}

			return o_resident.size() == 9;
		}

		void RequestCurrentPhase(ChunkSystem& chunks, Camera& camera)
		{
			const std::unordered_map<uint32_t, Chunk*>& allChunks = chunks.GetChunks();
			if (allChunks.empty() || allChunks.begin()->second == nullptr)
				return;

			const UVector3 v3ChunkSize = allChunks.begin()->second->GetChunkSize();
			const Vector3 v3CameraPosition = camera.GetTransform()->GetPosition();
			const Vector3 v3TargetPosition(
				(static_cast<float>(kWindowBaseX[m_uiPhase]) + 1.5f) * v3ChunkSize.x,
				v3CameraPosition.y,
				(static_cast<float>(kWindowBaseZ[m_uiWorldIndex]) + 1.5f) * v3ChunkSize.z);

			/* This is the same camera-driven switch gameplay uses. Keeping the
			   real camera untouched avoids fighting CameraMultiplayer on this map. */
			chunks.SetCameraLoadOffset(v3TargetPosition - v3CameraPosition);
		}

		bool PreparePhase(World& world, VoxelGrid& grid, ChunkSystem& chunks)
		{
			std::unordered_set<uint32_t> resident;
			if (!ValidateResidentWindow(grid, chunks, resident))
				return false;

			std::unordered_set<uint32_t> eligible;
			if (PhaseInWorld() == 0)
			{
				eligible = resident;
			}
			else
			{
				for (uint32_t uiChunkKey : resident)
				{
					if (m_PreviousResidentChunkIds.count(uiChunkKey) == 0)
						eligible.insert(uiChunkKey);
				}

				if (eligible.empty())
				{
					Fail("phase %u completed without loading a new chunk", m_uiPhase);
					return false;
				}

				++m_uiChunkTransitions;
			}

			m_ChunkCursors.clear();
			const std::unordered_map<uint32_t, Chunk*>& allChunks = chunks.GetChunks();
			for (uint32_t uiChunkKey : eligible)
			{
				const auto chunkIt = allChunks.find(uiChunkKey);
				if (chunkIt != allChunks.end() && chunkIt->second != nullptr)
					m_ChunkCursors.push_back({chunkIt->second, uiChunkKey, 0,
						std::numeric_limits<size_t>::max()});
			}

			if (m_ChunkCursors.empty())
			{
				Fail("phase %u has no eligible resident chunk storage", m_uiPhase);
				return false;
			}

			m_PreviousResidentChunkIds = std::move(resident);
			m_PhaseOwnerIds.clear();
			m_PhaseDestroyedChunkIds.clear();
			m_uiPhaseDestroyedVoxels = 0;
			m_uiNextChunkCursor = 0;
			m_bCandidateSearchPending = false;
			m_uiBurstCadence = 0;
			m_uiResidentStableFrames = 0;
			m_bPhaseReady = true;
			m_eExpectedInterval = IntervalKind::Steady;

			const Vector3 v3Offset = grid.GetWorldOffset();
			fprintf(stderr,
			        "[gpu-test] world %u phase %u committed at window %.0f %.0f %.0f; "
			        "%zu %s chunks eligible\n",
			        m_uiWorldIndex, PhaseInWorld(), v3Offset.x, v3Offset.y, v3Offset.z,
			        m_ChunkCursors.size(), PhaseInWorld() == 0 ? "initially resident" : "newly loaded");

			return true;
		}

		bool CandidateAt(World& world, VoxelGrid& grid, ChunkCursor& cursor,
		                 size_t uiIndex, Candidate& o_candidate)
		{
			Chunk* pChunk = cursor.pChunk;
			std::vector<Voxel>& voxels = pChunk->GetVoxelData();
			VoxelOwnerVolume& owners = pChunk->GetOwnerVolume();
			if (uiIndex >= voxels.size() || uiIndex >= owners.Size() || !voxels[uiIndex].IsActive())
				return false;

			const uint16_t uiOwnerSlot = owners.GetSlot(static_cast<uint32_t>(uiIndex));
			const uint64_t uiOwnerId = grid.ResolveOwnerSlot(uiOwnerSlot);
			Entity* pOwner = uiOwnerId != 0 ? world.FindEntity(uiOwnerId) : nullptr;
			if (pOwner == nullptr || !pOwner->IsDestructible())
				return false;

			const UVector3 v3ChunkSize = pChunk->GetChunkSize();
			const uint32_t uiLocalX = static_cast<uint32_t>(uiIndex % v3ChunkSize.x);
			const uint32_t uiLocalY = static_cast<uint32_t>((uiIndex / v3ChunkSize.x) % v3ChunkSize.y);
			const uint32_t uiLocalZ = static_cast<uint32_t>(uiIndex /
				(static_cast<size_t>(v3ChunkSize.x) * v3ChunkSize.y));
			if (uiLocalY == 0)
				return false;

			const UVector2 v2GridTarget = pChunk->GetGridTarget();
			const UVector3 v3GridPosition(
				v2GridTarget.x * v3ChunkSize.x + uiLocalX,
				uiLocalY,
				v2GridTarget.y * v3ChunkSize.z + uiLocalZ);
			const VoxelCell gridCell = grid.GetCell(
				v3GridPosition.x, v3GridPosition.y, v3GridPosition.z);

			if (!gridCell.IsActive() || gridCell.GetSlot() != uiOwnerSlot ||
			    gridCell.pVoxel != &voxels[uiIndex])
			{
				Fail("phase %u candidate from chunk (%u,%u) is not the storage mapped into VoxelGrid",
				     m_uiPhase, pChunk->GetChunkIndex().x, pChunk->GetChunkIndex().y);
				return false;
			}

			o_candidate.v3GridPosition = Vector3(v3GridPosition);
			o_candidate.uiOwnerSlot = uiOwnerSlot;
			o_candidate.uiOwnerId = uiOwnerId;
			o_candidate.uiChunkKey = cursor.uiChunkKey;
			o_candidate.v2ChunkIndex = pChunk->GetChunkIndex();
			o_candidate.sOwnerName = pOwner->GetName();
			return true;
		}

		CandidateSearchResult FindFreshCandidate(
			World& world, VoxelGrid& grid, Candidate& o_candidate)
		{
			if (m_ChunkCursors.empty())
				return CandidateSearchResult::Exhausted;

			/* A resident chunk contains more than eight million voxels. The old
			   fixture searched every remaining voxel in one OnUpdate call, adding
			   its own 400 ms "steady" hitches to the production hitch gate. Keep
			   the cursor and fallback persistent and spend a small fixed amount of
			   search work per rendered frame. */
			static constexpr size_t kCandidateChecksPerFrame = 32768;
			const size_t uiCursorCount = m_ChunkCursors.size();
			size_t uiChecksRemaining = kCandidateChecksPerFrame;
			while (uiChecksRemaining > 0)
			{
				const bool bAnyRemaining = std::any_of(
					m_ChunkCursors.begin(), m_ChunkCursors.end(), [](const ChunkCursor& cursor)
					{
						return cursor.uiNextVoxel < cursor.pChunk->GetVoxelData().size() ||
							cursor.uiFallbackVoxel != std::numeric_limits<size_t>::max();
					});
				if (!bAnyRemaining)
					return CandidateSearchResult::Exhausted;

				const size_t uiCursorIndex = m_uiNextChunkCursor;
				ChunkCursor& cursor = m_ChunkCursors[uiCursorIndex];
				const std::vector<Voxel>& voxels = cursor.pChunk->GetVoxelData();

				if (cursor.uiNextVoxel < voxels.size())
				{
					const size_t uiIndex = cursor.uiNextVoxel++;
					--uiChecksRemaining;
					Candidate candidate;
					if (!CandidateAt(world, grid, cursor, uiIndex, candidate))
					{
						if (m_bFinished)
							return CandidateSearchResult::Exhausted;
						continue;
					}

					/* Prefer an owner the test has not hit yet. Once eight distinct
					   models have been proven, the first fresh active voxel wins. */
					const ScopedOwnerId scopedOwner{m_uiWorldIndex, candidate.uiOwnerId};
					if (m_DestroyedOwnerIds.size() < kMinDestroyedModels &&
					    m_DestroyedOwnerIds.count(scopedOwner) != 0)
					{
						if (cursor.uiFallbackVoxel == std::numeric_limits<size_t>::max())
							cursor.uiFallbackVoxel = uiIndex;
						continue;
					}

					m_uiNextChunkCursor = (uiCursorIndex + 1) % uiCursorCount;
					o_candidate = std::move(candidate);
					return CandidateSearchResult::Found;
				}

				if (cursor.uiFallbackVoxel != std::numeric_limits<size_t>::max())
				{
					const size_t uiFallback = cursor.uiFallbackVoxel;
					cursor.uiFallbackVoxel = std::numeric_limits<size_t>::max();
					Candidate candidate;
					if (!CandidateAt(world, grid, cursor, uiFallback, candidate))
					{
						if (m_bFinished)
							return CandidateSearchResult::Exhausted;
						m_uiNextChunkCursor = (uiCursorIndex + 1) % uiCursorCount;
						continue;
					}

					m_uiNextChunkCursor = (uiCursorIndex + 1) % uiCursorCount;
					o_candidate = std::move(candidate);
					return CandidateSearchResult::Found;
				}

				m_uiNextChunkCursor = (uiCursorIndex + 1) % uiCursorCount;
			}

			return CandidateSearchResult::Pending;
		}

		static uint32_t CountActiveInSphere(VoxelGrid& grid, const Vector3& v3Center,
		                                    float fRadius)
		{
			const UVector3 v3Dimensions = grid.GetDimensions();
			const int32_t iRadius = static_cast<int32_t>(fRadius);
			const int32_t iCenterX = static_cast<int32_t>(std::floor(v3Center.x));
			const int32_t iCenterY = static_cast<int32_t>(std::floor(v3Center.y));
			const int32_t iCenterZ = static_cast<int32_t>(std::floor(v3Center.z));
			const int32_t iMinX = std::max(iCenterX - iRadius, 0);
			const int32_t iMinY = std::max(iCenterY - iRadius, 1);
			const int32_t iMinZ = std::max(iCenterZ - iRadius, 0);
			const int32_t iMaxX = std::min(iCenterX + iRadius, static_cast<int32_t>(v3Dimensions.x) - 1);
			const int32_t iMaxY = std::min(iCenterY + iRadius, static_cast<int32_t>(v3Dimensions.y) - 1);
			const int32_t iMaxZ = std::min(iCenterZ + iRadius, static_cast<int32_t>(v3Dimensions.z) - 1);
			const float fRadiusSquared = fRadius * fRadius;
			uint32_t uiActive = 0;

			for (int32_t z = iMinZ; z <= iMaxZ; ++z)
			{
				for (int32_t y = iMinY; y <= iMaxY; ++y)
				{
					for (int32_t x = iMinX; x <= iMaxX; ++x)
					{
						const float fDX = static_cast<float>(x - iCenterX);
						const float fDY = static_cast<float>(y - iCenterY);
						const float fDZ = static_cast<float>(z - iCenterZ);
						if (fDX * fDX + fDY * fDY + fDZ * fDZ <= fRadiusSquared &&
						    grid.GetCell(static_cast<uint32_t>(x), static_cast<uint32_t>(y),
						                 static_cast<uint32_t>(z)).IsActive())
						{
							++uiActive;
						}
					}
				}
			}

			return uiActive;
		}

		void IssueBurst(World& world, VoxelGrid& grid, const Candidate& candidate)
		{
			const VoxelCell beforeCell = grid.GetCell(
				static_cast<uint32_t>(candidate.v3GridPosition.x),
				static_cast<uint32_t>(candidate.v3GridPosition.y),
				static_cast<uint32_t>(candidate.v3GridPosition.z));
			if (!beforeCell.IsActive() || beforeCell.GetSlot() != candidate.uiOwnerSlot)
			{
				Fail("phase %u selected a stale candidate before burst %u",
				     m_uiPhase, m_uiBurstsIssued);
				return;
			}

			const uint32_t uiActiveBefore = CountActiveInSphere(
				grid, candidate.v3GridPosition, kDestructionRadius);
			const uint64_t uiGenerationBefore = grid.GetWriteGeneration();
			const Vector3 v3WorldPosition = grid.GridToWorld(candidate.v3GridPosition);

			world.ApplySphericalDestruction(
				v3WorldPosition, kDestructionRadius, 30.f, 80.f, true);

			const uint32_t uiActiveAfter = CountActiveInSphere(
				grid, candidate.v3GridPosition, kDestructionRadius);
			const uint64_t uiGenerationAfter = grid.GetWriteGeneration();
			const VoxelCell afterCell = grid.GetCell(
				static_cast<uint32_t>(candidate.v3GridPosition.x),
				static_cast<uint32_t>(candidate.v3GridPosition.y),
				static_cast<uint32_t>(candidate.v3GridPosition.z));

			if (uiGenerationAfter <= uiGenerationBefore ||
			    uiActiveAfter >= uiActiveBefore || afterCell.IsActive())
			{
				Fail("burst %u did not destroy its fresh voxel (active %u -> %u, generation %llu -> %llu)",
				     m_uiBurstsIssued, uiActiveBefore, uiActiveAfter,
				     static_cast<unsigned long long>(uiGenerationBefore),
				     static_cast<unsigned long long>(uiGenerationAfter));
				return;
			}

			const uint32_t uiDestroyed = uiActiveBefore - uiActiveAfter;
			const bool bNewModel = m_DestroyedOwnerIds.insert(
				ScopedOwnerId{m_uiWorldIndex, candidate.uiOwnerId}).second;
			const bool bNewChunk = m_DestroyedChunkIds.insert(
				ScopedChunkKey(candidate.uiChunkKey)).second;
			m_PhaseOwnerIds.insert(candidate.uiOwnerId);
			m_PhaseDestroyedChunkIds.insert(candidate.uiChunkKey);
			m_uiDestroyedVoxels += uiDestroyed;
			m_uiPhaseDestroyedVoxels += uiDestroyed;
			++m_uiBurstsIssued;
			++m_uiBurstsInPhase;

			if (bNewModel || bNewChunk)
			{
				fprintf(stderr,
				        "[gpu-test] burst %u destroyed %u voxels from model '%s' (%llu) "
				        "in world chunk (%u,%u)%s%s\n",
				        m_uiBurstsIssued, uiDestroyed, candidate.sOwnerName.c_str(),
				        static_cast<unsigned long long>(candidate.uiOwnerId),
				        candidate.v2ChunkIndex.x, candidate.v2ChunkIndex.y,
				        bNewModel ? " [new model]" : "",
				        bNewChunk ? " [new chunk]" : "");
			}

			if (m_uiBurstsInPhase != kBurstsPerPhase)
				return;

			if (m_PhaseDestroyedChunkIds.size() < kMinDestroyedChunksPerPhase ||
			    m_PhaseOwnerIds.size() < kMinDestroyedModelsPerPhase)
			{
				Fail("phase %u coverage too narrow: %zu chunks and %zu models (need %u and %u)",
				     m_uiPhase, m_PhaseDestroyedChunkIds.size(), m_PhaseOwnerIds.size(),
				     kMinDestroyedChunksPerPhase, kMinDestroyedModelsPerPhase);
				return;
			}

			fprintf(stderr,
			        "[gpu-test] world %u phase %u complete: %u productive bursts destroyed %llu voxels "
			        "across %zu chunks / %zu models\n",
			        m_uiWorldIndex, PhaseInWorld(), kBurstsPerPhase,
			        static_cast<unsigned long long>(m_uiPhaseDestroyedVoxels),
			        m_PhaseDestroyedChunkIds.size(), m_PhaseOwnerIds.size());

			++m_uiPhase;
			m_uiBurstsInPhase = 0;
			m_uiResidentStableFrames = 0;
			m_bPhaseReady = false;

			if (m_uiWorldIndex + 1 < kWorldCount &&
			    m_uiPhase == kWorldFirstPhase[m_uiWorldIndex + 1])
			{
				BeginLevelSwitch(world);
				return;
			}

			if (m_uiPhase == kPhaseCount)
			{
				PCommandEngine* pDirect = GetPlatform().GetRenderContext()->GetEngine("Direct");
				m_uiTimelineAtLastBurst = pDirect != nullptr ? pDirect->GetValue() : 0;
				m_bDraining = true;
				m_eExpectedInterval = IntervalKind::Ignore;
				return;
			}

			m_eExpectedInterval = IntervalKind::ChunkTransition;
		}

		void DriveDestruction()
		{
			World* pWorld = GetWorldManager().GetTopWorld();
			if (pWorld == nullptr || !ObserveExpectedWorld(*pWorld))
				return;

			VoxelGrid* pGrid = pWorld->GetPhysics() != nullptr
				? pWorld->GetPhysics()->GetVoxelGrid() : nullptr;
			ChunkSystem* pChunks = pWorld->GetChunkSystem();
			Camera* pCamera = pWorld->GetMainCamera();
			if (pGrid == nullptr || pChunks == nullptr || pCamera == nullptr)
				return;

			/* The map relies on CameraMultiplayer being persistent, but the
			   default Camera that World::Initialize creates is not. A synthetic
			   multi-column jump otherwise lets Chunk::SaveAndDeleteEntities delete
			   the very camera that drives streaming. Pinning the fixture's driver is
			   test setup; no production application path is changed. */
			if (!m_bCameraPinned)
			{
				pCamera->SetPersistent(true);
				m_bCameraPinned = true;
			}

			if (m_bStreamingOnly)
			{
				DriveStreamingOnly(*pGrid, *pChunks);
				return;
			}

			RequestCurrentPhase(*pChunks, *pCamera);

			if (!m_bPhaseReady)
			{
				/* Phase zero can initially describe the requested window before
				   the first gameplay tick has moved its camera. Requiring a run of
				   valid rendered frames proves ChunkSystem has observed the request
				   and that no queued load/unload group is still perturbing it. */
				std::unordered_set<uint32_t> resident;
				if (!ValidateResidentWindow(*pGrid, *pChunks, resident))
				{
					m_uiResidentStableFrames = 0;
					return;
				}

				if (++m_uiResidentStableFrames < kResidentStableFrames)
					return;

				if (!PreparePhase(*pWorld, *pGrid, *pChunks))
					return;
			}

			std::unordered_set<uint32_t> resident;
			if (!ValidateResidentWindow(*pGrid, *pChunks, resident))
			{
				Fail("resident window changed while world %u phase %u was issuing bursts",
				     m_uiWorldIndex, PhaseInWorld());
				return;
			}

			if (!m_bCandidateSearchPending)
			{
				if (++m_uiBurstCadence < kFramesBetweenBursts)
					return;
				m_uiBurstCadence = 0;
			}

			Candidate candidate;
			const CandidateSearchResult search = FindFreshCandidate(*pWorld, *pGrid, candidate);
			if (search == CandidateSearchResult::Pending)
			{
				m_bCandidateSearchPending = true;
				return;
			}

			m_bCandidateSearchPending = false;
			if (search == CandidateSearchResult::Exhausted)
			{
				if (!m_bFinished)
				{
					Fail("phase %u exhausted fresh destructible model voxels after %u/%u bursts",
					     m_uiPhase, m_uiBurstsInPhase, kBurstsPerPhase);
				}
				return;
			}

			IssueBurst(*pWorld, *pGrid, candidate);
		}

		/* One window transition, measured, and nothing else.
		 *
		 * It observes rather than steers, which is the difference between this
		 * mode and the destruction route above it. Two ways of steering were
		 * tried and both are wrong here: SetCameraLoadOffset is recomputed from
		 * the camera in Tick and consumed in FixedTick, so against Beat2's
		 * CameraMultiplayer - which follows a player that walks, dies and
		 * respawns - the effective load position never settles and the window
		 * oscillated across the whole level instead of transitioning once; and
		 * pinning the camera transform from Tick is simply overwritten by the
		 * follow camera's own FixedTick before ChunkSystem reads it.
		 *
		 * So the player walks, through --ui-script forward-on, exactly as a
		 * player would, and this waits for the initial window to settle and then
		 * times the next transition end to end. That is also the flow the
		 * symptom was reported in (plan ledger E12).
		 *
		 * PreparePhase is deliberately not called: hunting destructible model
		 * voxels across eight-million-voxel chunks would be the largest cost in
		 * the frame whose cost is the subject. */
		void DriveStreamingOnly(VoxelGrid& grid, ChunkSystem& chunks)
		{
			const bool bStreaming = chunks.IsStreaming();

			if (!m_bFocusedInitialWindowDone)
			{
				/* The initial window is not the subject - its cost is a world
				   load, which phase 4 owns. Wait for it to go quiet. */
				m_uiResidentStableFrames = bStreaming ? 0 : m_uiResidentStableFrames + 1;

				if (m_uiResidentStableFrames < kResidentStableFrames)
					return;

				m_bFocusedInitialWindowDone = true;
				m_uiResidentStableFrames = 0;
				m_v3StreamingWindowOffset = grid.GetWorldOffset();
				fprintf(stderr,
				        "[gpu-test] initial window settled at (%.0f, %.0f); timing the next "
				        "transition\n",
				        m_v3StreamingWindowOffset.x, m_v3StreamingWindowOffset.z);
				return;
			}

			if (bStreaming)
			{
				m_uiChunkTransitions = 1;
				m_uiResidentStableFrames = 0;
				m_eExpectedInterval = IntervalKind::ChunkTransition;
				return;
			}

			/* Not started yet: keep waiting rather than reporting on an idle
			   run. The --frames cap and the ctest timeout bound this. */
			if (m_uiChunkTransitions == 0)
			{
				m_eExpectedInterval = IntervalKind::Ignore;
				return;
			}

			/* The window stopped moving. Give it the same settle margin before
			   closing the interval - a group finishing does not mean the next
			   one is not about to start. */
			if (++m_uiResidentStableFrames < kResidentStableFrames)
				return;

			m_eExpectedInterval = IntervalKind::Ignore;

			if (grid.GetWorldOffset() == m_v3StreamingWindowOffset)
			{
				Fail("the window reported streaming but did not move "
				     "(world offset still %.1f, %.1f)",
				     m_v3StreamingWindowOffset.x, m_v3StreamingWindowOffset.z);
				return;
			}

			if (m_uiChunkTransitionFrameSamples == 0)
			{
				Fail("focused chunk transition produced no measured frames");
				return;
			}

			if (m_uiChunkTransitionHitchViolations != 0)
			{
				/* The number this whole mode exists to produce, printed on the
				   failing path too - on master it is the baseline. */
				Fail("chunk transition exceeded its frame budget %u time(s) of %llu: "
				     "peak %.2f ms against %.2f ms",
				     m_uiChunkTransitionHitchViolations,
				     static_cast<unsigned long long>(m_uiChunkTransitionFrameSamples),
				     m_fPeakChunkTransitionFrameMs, kMaxStreamingTransitionFrameMs);
				return;
			}

			fprintf(stderr,
			        "[gpu-test] PASS: focused Beat2 chunk transition peak %.2f/%.2f ms "
			        "across %llu frames\n",
			        m_fPeakChunkTransitionFrameMs, kMaxStreamingTransitionFrameMs,
			        static_cast<unsigned long long>(m_uiChunkTransitionFrameSamples));
			m_bPassed = true;
			m_bFinished = true;
			Exit();
		}

		void DrainGPU()
		{
			if (++m_uiDrainFrames < kDrainFrames)
				return;

			PCommandEngine* pDirect = GetPlatform().GetRenderContext()->GetEngine("Direct");
			const uint64_t uiCompleted = pDirect != nullptr ? pDirect->GetCompletedValue() : 0;
			if (pDirect == nullptr || uiCompleted <= m_uiTimelineAtLastBurst ||
			    m_uiMaxCompletedTimeline <= m_uiTimelineStart)
			{
				Fail("direct timeline did not retire post-destruction rendering work (%llu at final burst, %llu complete)",
				     static_cast<unsigned long long>(m_uiTimelineAtLastBurst),
				     static_cast<unsigned long long>(uiCompleted));
				return;
			}

			if (m_DestroyedChunkIds.size() < kMinDestroyedChunks ||
			    m_DestroyedOwnerIds.size() < kMinDestroyedModels)
			{
				Fail("coverage too narrow: %zu chunks and %zu models (need %u and %u)",
				     m_DestroyedChunkIds.size(), m_DestroyedOwnerIds.size(),
				     kMinDestroyedChunks, kMinDestroyedModels);
				return;
			}

			if (m_uiSteadyFrameSamples == 0 || m_uiChunkTransitionFrameSamples == 0 ||
			    m_uiLevelSwitchFrameSamples == 0)
			{
				Fail("CPU hitch gates did not sample every state (steady %llu, chunk %llu, level %llu)",
				     static_cast<unsigned long long>(m_uiSteadyFrameSamples),
				     static_cast<unsigned long long>(m_uiChunkTransitionFrameSamples),
				     static_cast<unsigned long long>(m_uiLevelSwitchFrameSamples));
				return;
			}

			if (m_uiSteadyHitchViolations != 0 || m_uiChunkTransitionHitchViolations != 0 ||
			    m_uiLevelSwitchHitchViolations != 0)
			{
				Fail("CPU hitch limits exceeded: steady %u (peak %.2f/%.2f ms), "
				     "chunk %u (peak %.2f/%.2f ms), level %u (peak %.2f/%.2f ms); "
				     "completed %u bursts / %llu voxels / %u chunk switches / %u level "
				     "switch / %zu chunks / %zu owners; direct timeline %llu -> %llu "
				     "(submitted through %llu)",
				     m_uiSteadyHitchViolations, m_fPeakSteadyFrameMs, kMaxSteadyFrameMs,
				     m_uiChunkTransitionHitchViolations, m_fPeakChunkTransitionFrameMs,
				     kMaxChunkTransitionFrameMs, m_uiLevelSwitchHitchViolations,
				     m_fPeakLevelSwitchFrameMs, kMaxLevelSwitchFrameMs,
				     m_uiBurstsIssued, static_cast<unsigned long long>(m_uiDestroyedVoxels),
				     m_uiChunkTransitions, m_uiLevelSwitches,
				     m_DestroyedChunkIds.size(), m_DestroyedOwnerIds.size(),
				     static_cast<unsigned long long>(m_uiTimelineStart),
				     static_cast<unsigned long long>(uiCompleted),
				     static_cast<unsigned long long>(m_uiMaxSubmittedTimeline));
				return;
			}

			fprintf(stderr,
			        "[gpu-test] PASS: %u productive bursts destroyed %llu voxels across "
			        "%u completed window switches, %u level switch, %zu world chunks and "
			        "%zu model owners; CPU peaks %.2f/%.2f/%.2f ms "
			        "(steady/chunk/level); direct timeline advanced %llu -> %llu "
			        "(submitted through %llu)\n",
			        m_uiBurstsIssued, static_cast<unsigned long long>(m_uiDestroyedVoxels),
			        m_uiChunkTransitions, m_uiLevelSwitches,
			        m_DestroyedChunkIds.size(), m_DestroyedOwnerIds.size(),
			        m_fPeakSteadyFrameMs, m_fPeakChunkTransitionFrameMs,
			        m_fPeakLevelSwitchFrameMs,
			        static_cast<unsigned long long>(m_uiTimelineStart),
			        static_cast<unsigned long long>(uiCompleted),
			        static_cast<unsigned long long>(m_uiMaxSubmittedTimeline));

			m_bPassed = true;
			m_bFinished = true;
			Exit();
		}

		std::vector<ChunkCursor> m_ChunkCursors;
		std::unordered_set<uint32_t> m_PreviousResidentChunkIds;
		std::unordered_set<uint64_t> m_DestroyedChunkIds;
		std::unordered_set<uint32_t> m_PhaseDestroyedChunkIds;
		std::unordered_set<ScopedOwnerId, ScopedOwnerIdHash> m_DestroyedOwnerIds;
		std::unordered_set<uint64_t> m_PhaseOwnerIds;
		std::chrono::steady_clock::time_point m_LastFrameTimestamp;

		uint32_t m_uiPhase = 0;
		uint32_t m_uiBurstsInPhase = 0;
		uint32_t m_uiBurstsIssued = 0;
		uint32_t m_uiBurstCadence = 0;
		uint32_t m_uiChunkTransitions = 0;
		uint32_t m_uiLevelSwitches = 0;
		uint32_t m_uiWorldIndex = 0;
		uint32_t m_uiResidentStableFrames = 0;
		uint32_t m_uiDrainFrames = 0;
		size_t m_uiNextChunkCursor = 0;
		bool m_bCandidateSearchPending = false;
		bool m_bFocusedInitialWindowDone = false;
		Vector3 m_v3StreamingWindowOffset = Vector3(0.f);
		uint64_t m_uiDestroyedVoxels = 0;
		uint64_t m_uiPhaseDestroyedVoxels = 0;

		uint64_t m_uiTimelineStart = 0;
		uint64_t m_uiLastCompletedTimeline = 0;
		uint64_t m_uiMaxSubmittedTimeline = 0;
		uint64_t m_uiMaxCompletedTimeline = 0;
		uint64_t m_uiTimelineAtLastBurst = 0;
		uint32_t m_uiTimelineNoProgressFrames = 0;
		uint64_t m_uiSteadyFrameSamples = 0;
		uint64_t m_uiChunkTransitionFrameSamples = 0;
		uint64_t m_uiLevelSwitchFrameSamples = 0;
		uint32_t m_uiSteadyHitchViolations = 0;
		uint32_t m_uiChunkTransitionHitchViolations = 0;
		uint32_t m_uiLevelSwitchHitchViolations = 0;
		double m_fPeakSteadyFrameMs = 0.0;
		double m_fPeakChunkTransitionFrameMs = 0.0;
		double m_fPeakLevelSwitchFrameMs = 0.0;

		IntervalKind m_eExpectedInterval = IntervalKind::Ignore;
		bool m_bHaveTimelineSample = false;
		bool m_bHaveFrameTimestamp = false;
		bool m_bInitialWorldObserved = false;
		bool m_bAwaitingWorldSwitch = false;
		bool m_bCameraPinned = false;
		bool m_bPhaseReady = false;
		bool m_bDraining = false;
		bool m_bFinished = false;
		bool m_bPassed = true;
		bool m_bStreamingOnly = false;
	};
}

int main(int argc, char* argv[])
{
	if (!LaunchOptions::Get().Parse(argc, argv))
		return 1;

	DestructionSyncStress test;
	test.Run();

	return test.Passed() ? 0 : 1;
}
