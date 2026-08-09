#include "Harness/DestructionRun.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <unordered_set>

#include "Core/Particles/ParticleSimulation.h"
#include "Core/Utils/DeterministicRandom.h"
#include "Core/Voxels/SphericalDestruction.h"
#include "Core/Voxels/VoxelEditBatch.h"

DestructionRun::DestructionRun(const DestructionConfig& config) :
	m_Config(config),
	m_World(config.v3Size, config.v3ChunkSize)
{
	m_Debris.Create(config.uiDebrisCapacity);
	m_Checker.SetVoxelGrid(&m_World.Grid());
}

void DestructionRun::CollectStanding(std::unordered_set<uint64_t>& o_standing) const
{
	o_standing.clear();

	std::vector<uint64_t> island;
	VoxelGrid& grid = const_cast<VoxelWorldHarness&>(m_World).Grid();

	for (uint32_t uiZ = 0; uiZ < m_Config.v3Size.z; ++uiZ)
	for (uint32_t uiY = 1; uiY < m_Config.v3Size.y; ++uiY)
	for (uint32_t uiX = 0; uiX < m_Config.v3Size.x; ++uiX)
	{
		if (!grid.GetCell(uiX, uiY, uiZ).IsActive())
			continue;

		const uint64_t uiHash = IntegrityChecker::PositionToHash(uiX, uiY, uiZ);

		if (o_standing.count(uiHash) != 0)
			continue;

		if (!m_Checker.ClassifyExhaustive(
			Vector3(static_cast<float>(uiX), static_cast<float>(uiY), static_cast<float>(uiZ)), island))
			continue;

		o_standing.insert(island.begin(), island.end());
	}
}

void DestructionRun::ClassifyStanding(uint64_t& o_uiVoxels, uint64_t& o_uiComponents,
                                      std::vector<Vector3>* pSamples,
                                      const std::unordered_set<uint64_t>* pIgnore) const
{
	o_uiVoxels = 0;
	o_uiComponents = 0;

	std::unordered_set<uint64_t> classified;
	std::vector<uint64_t> island;

	VoxelGrid& grid = const_cast<VoxelWorldHarness&>(m_World).Grid();

	for (uint32_t uiZ = 0; uiZ < m_Config.v3Size.z; ++uiZ)
	for (uint32_t uiY = 1; uiY < m_Config.v3Size.y; ++uiY)
	for (uint32_t uiX = 0; uiX < m_Config.v3Size.x; ++uiX)
	{
		if (!grid.GetCell(uiX, uiY, uiZ).IsActive())
			continue;

		const uint64_t uiHash = IntegrityChecker::PositionToHash(uiX, uiY, uiZ);

		if (classified.count(uiHash) != 0)
			continue;

		if (!m_Checker.ClassifyExhaustive(
			Vector3(static_cast<float>(uiX), static_cast<float>(uiY), static_cast<float>(uiZ)), island))
			continue;

		classified.insert(island.begin(), island.end());

		o_uiVoxels += island.size();
		++o_uiComponents;

		/* Only report components that were not already there. */
		if (pSamples != nullptr && pSamples->size() < 8 &&
			(pIgnore == nullptr || pIgnore->count(island.front()) == 0))
		{
			pSamples->push_back(IntegrityChecker::HashToPosition(island.front()));
		}
	}
}

DestructionResult DestructionRun::Run(const std::vector<Burst>& bursts)
{
	DestructionResult result;

	result.uiBuilt = m_World.CountOccupied();

	/* Every protected voxel, recorded before anything happens. The invariant is
	   checked against this rather than by trusting the predicate to have been
	   asked. */
	std::vector<uint64_t> protectedVoxels;

	for (uint32_t uiZ = 0; uiZ < m_Config.v3Size.z; ++uiZ)
	for (uint32_t uiY = 0; uiY < m_Config.v3Size.y; ++uiY)
	for (uint32_t uiX = 0; uiX < m_Config.v3Size.x; ++uiX)
	{
		const VoxelCell cell = m_World.Grid().GetCell(uiX, uiY, uiZ);

		if (cell.IsActive() && cell.GetSlot() == k_uiProtectedSlot)
			protectedVoxels.push_back(IntegrityChecker::PositionToHash(uiX, uiY, uiZ));
	}

	result.uiProtectedAtStart = protectedVoxels.size();

	std::unordered_set<uint64_t> standingAtStart;

	if (m_Config.bClassifyStanding)
	{
		CollectStanding(standingAtStart);
		ClassifyStanding(result.uiStandingAtStart, result.uiStandingComponentsAtStart);
	}

	DeterministicRandom random(m_Config.uiSeed);

	std::deque<std::vector<uint64_t>> pendingIslands;
	std::unordered_set<uint64_t> pendingIslandVoxels;
	size_t uiIslandCursor = 0;
	size_t uiNextBurst = 0;

	const ParticleSimulation::Settings simSettings;
	const Vector3 v3WorldOffset = m_World.Grid().GetWorldOffset();

	const uint32_t uiQuarters = 4;
	result.integrityQuarterMs.assign(uiQuarters, 0.0);

	for (uint32_t uiTick = 0; uiTick < m_Config.uiTicks; ++uiTick)
	{
		const uint32_t uiQuarter = std::min(uiQuarters - 1,
			uiTick * uiQuarters / std::max(1u, m_Config.uiTicks));

		std::vector<uint64_t> seeds;

		while (uiNextBurst < bursts.size() && bursts[uiNextBurst].uiTick == uiTick)
		{
			const Burst& burst = bursts[uiNextBurst];
			const Stopwatch watch;

			VoxelEditBatch batch(m_World.MakeEditTarget());

			/* The counter advances per destroyed voxel whether or not a
			   particle was spawned, so density stays even across the sphere
			   rather than clustering wherever the loop happened to start. */
			uint32_t uiSpawnCounter = 0;

			const SphericalDestruction::Result burstResult = SphericalDestruction::Apply(
				batch, m_World.Grid(), burst.v3Center, burst.fRadius,
				[](uint16_t uiSlot) { return uiSlot != k_uiProtectedSlot; },
				[&](const Vector3& v3Position, uint32_t uiColor)
				{
					const bool bSpawn = (uiSpawnCounter % m_Config.uiSpawnEveryNth) == 0;
					++uiSpawnCounter;

					if (!bSpawn)
						return;

					Vector3 v3Away = v3Position - burst.v3Center;
					const float fLengthSquared = glm::length2(v3Away);

					v3Away = fLengthSquared > 0.f
						? v3Away / std::sqrt(fLengthSquared)
						: Vector3(0.f, 1.f, 0.f);

					ParticleSpawn spawn;
					spawn.v3Position = v3Position;
					spawn.v3Velocity = v3Away * random.Range(20.f, 60.f);
					spawn.uiColor = uiColor;
					spawn.bBakeOnImpact = true;

					m_Debris.Spawn(spawn);
				},
				&seeds);

			result.uiDestroyed += burstResult.uiDestroyed;

			if (m_Config.bMeasure)
				result.burstMs.Add(watch.Milliseconds());

			++uiNextBurst;
		}

		SphericalDestruction::FilterSeeds(m_World.Grid(), seeds);

		if (!seeds.empty())
			m_Checker.EnqueueBulk(seeds);

		/* Direct injections, high above the level so they fall a long way and
		   the count stays where it was put for many ticks. */
		for (const Injection& injection : m_Config.injections)
		{
			if (injection.uiTick != uiTick)
				continue;

			for (uint32_t i = 0; i < injection.uiCount; ++i)
			{
				ParticleSpawn spawn;
				spawn.v3Position = Vector3(
					random.Range(1.f, static_cast<float>(m_Config.v3Size.x) - 2.f),
					random.Range(static_cast<float>(m_Config.v3Size.y) * 0.6f,
					             static_cast<float>(m_Config.v3Size.y) - 2.f),
					random.Range(1.f, static_cast<float>(m_Config.v3Size.z) - 2.f));
				spawn.v3Velocity = Vector3(
					random.Range(-4.f, 4.f), random.Range(-2.f, 2.f), random.Range(-4.f, 4.f));
				spawn.uiColor = 0xFFC0C0C0u;
				spawn.bBakeOnImpact = false;

				m_Debris.Spawn(spawn);
			}
		}

		/* Debris, at the fixed step the engine runs at. */
		if (m_Debris.GetCount() > 0)
		{
			const uint32_t uiAliveBefore = m_Debris.GetCount();
			const Stopwatch watch;

			VoxelEditBatch batch(m_World.MakeEditTarget());

			uint32_t uiIndex = 0;

			while (uiIndex < m_Debris.GetCount())
			{
				const ParticleSimulation::Outcome outcome = ParticleSimulation::Step(
					m_Debris, uiIndex, 1.f / 60.f, m_World.Grid(), m_World.Bricks(), m_Config.v3Size,
					v3WorldOffset, simSettings);

				/* Not onto something that is already falling - the engine's
				   rule, driven here rather than reimplemented. */
				if (outcome.bRetire)
				{
					const uint64_t uiSupport = IntegrityChecker::PositionToHash(
						static_cast<uint32_t>(outcome.iImpactX),
						static_cast<uint32_t>(outcome.iImpactY),
						static_cast<uint32_t>(outcome.iImpactZ));

					if (pendingIslandVoxels.count(uiSupport) != 0 ||
						m_Checker.IsClassifying(uiSupport))
					{
						++uiIndex;
						continue;
					}
				}

				if (outcome.bBake)
				{
					batch.Set(
						Vector3(static_cast<float>(outcome.iBakeX),
						        static_cast<float>(outcome.iBakeY),
						        static_cast<float>(outcome.iBakeZ)),
						m_Debris.Color[uiIndex], VoxelOwnerVolume::k_uiNoOwnerSlot);

					++result.uiBaked;
				}

				if (outcome.bRetire)
				{
					m_Debris.Retire(uiIndex);
					continue;
				}

				++uiIndex;
			}

			if (m_Config.bMeasure)
			{
				const double fMilliseconds = watch.Milliseconds();

				result.particleMs.Add(fMilliseconds);
				result.simSamples.emplace_back(uiAliveBefore, fMilliseconds);
			}

			result.uiPeakAlive = std::max(result.uiPeakAlive, uiAliveBefore);
		}

		/* Integrity. */
		{
			const Stopwatch watch;

			std::vector<std::vector<uint64_t>> islands;
			m_Checker.Process(m_Config.uiVisitBudget, islands);

			if (m_Config.bMeasure)
			{
				const double fMilliseconds = watch.Milliseconds();

				result.integrityMs.Add(fMilliseconds);
				result.integrityQuarterMs[uiQuarter] += fMilliseconds;
			}

			result.uiIslandsFound += islands.size();

			for (std::vector<uint64_t>& island : islands)
			{
				pendingIslandVoxels.insert(island.begin(), island.end());
				pendingIslands.push_back(std::move(island));
			}
		}

		if (pendingIslands.empty())
			continue;

		/* Conversion, with the same destructibility test the engine applies -
		   which island conversion did not have until a non-destructible
		   building collapsed in play. */
		const Stopwatch convertWatch;

		VoxelEditBatch batch(m_World.MakeEditTarget());
		uint32_t uiBudget = m_Config.uiConvertBudget;

		while (uiBudget > 0 && !pendingIslands.empty())
		{
			std::vector<uint64_t>& island = pendingIslands.front();

			for (; uiIslandCursor < island.size() && uiBudget > 0; ++uiIslandCursor)
			{
				--uiBudget;

				pendingIslandVoxels.erase(island[uiIslandCursor]);

				const Vector3 v3Position = IntegrityChecker::HashToPosition(island[uiIslandCursor]);
				const VoxelCell cell = m_World.Grid().GetCell(
					static_cast<uint32_t>(v3Position.x),
					static_cast<uint32_t>(v3Position.y),
					static_cast<uint32_t>(v3Position.z));

				if (!cell || !cell.IsActive())
					continue;

				if (cell.GetSlot() == k_uiProtectedSlot)
					continue;

				/* An island falls as a shape, so it is one particle per voxel
				   regardless of the explosion density above. Thinning it took
				   three quarters of a falling roof away. */
				ParticleSpawn spawn;
				spawn.v3Position = v3Position;
				spawn.uiColor = cell.GetColor();
				spawn.bBakeOnImpact = true;

				m_Debris.Spawn(spawn);

				batch.Clear(v3Position);

				++result.uiConverted;
			}

			if (uiIslandCursor >= island.size())
			{
				pendingIslands.pop_front();
				uiIslandCursor = 0;
			}
		}

		if (m_Config.bMeasure)
			result.convertMs.Add(convertWatch.Milliseconds());
	}

	result.uiRepresentation = m_World.Validate();
	result.uiHash = m_World.Hash();
	result.bPoolSound = m_Debris.Audit().IsSound();
	result.uiRemaining = m_World.CountOccupied();
	result.checker = m_Checker.GetStats();

	for (uint64_t uiHash : protectedVoxels)
	{
		const Vector3 v3 = IntegrityChecker::HashToPosition(uiHash);

		if (!m_World.Grid().GetCell(
			static_cast<uint32_t>(v3.x), static_cast<uint32_t>(v3.y), static_cast<uint32_t>(v3.z)).IsActive())
			++result.uiProtectedCleared;
	}

	if (m_Config.bClassifyStanding)
	{
		ClassifyStanding(result.uiStanding, result.uiStandingComponents,
		                 &result.standingSamples, &standingAtStart);
	}

	return result;
}
