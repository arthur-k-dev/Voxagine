#include "pch.h"

#include "Core/Particles/ParticleCore.h"

void ParticleCore::Create(uint32_t uiCapacity)
{
	m_uiCapacity = uiCapacity;
	m_uiCount = 0;

	Position.assign(uiCapacity, Vector3(0.f));
	Velocity.assign(uiCapacity, Vector3(0.f));
	Color.assign(uiCapacity, 0u);
	Timer.assign(uiCapacity, -1.f);
	BakeOnImpact.assign(uiCapacity, 1u);

	m_SlotToDense.assign(uiCapacity, UINT32_MAX);
	m_DenseToSlot.assign(uiCapacity, UINT32_MAX);

	/* Generations survive a Clear but not a Create - a resize is a different
	   pool, and no handle from the old one should be interpretable. */
	m_SlotGeneration.assign(uiCapacity, 1u);

	m_FreeSlots.clear();
	m_FreeSlots.reserve(uiCapacity);

	/* Descending, so the first spawns take slot 0 upward. Nothing depends on
	   it; it just makes a dump of the pool readable. */
	for (uint32_t i = uiCapacity; i > 0; --i)
		m_FreeSlots.push_back(i - 1);
}

void ParticleCore::Clear()
{
	/* Bump every live particle's generation before dropping it, so handles
	   taken before the clear read as dead rather than as whoever spawns next. */
	for (uint32_t uiDense = 0; uiDense < m_uiCount; ++uiDense)
	{
		const uint32_t uiSlot = m_DenseToSlot[uiDense];

		if (uiSlot >= m_uiCapacity)
			continue;

		++m_SlotGeneration[uiSlot];
		m_SlotToDense[uiSlot] = UINT32_MAX;
		m_FreeSlots.push_back(uiSlot);
	}

	m_uiCount = 0;
}

ParticleHandle ParticleCore::Spawn(const ParticleSpawn& spawn)
{
	ParticleHandle handle;

	if (m_uiCount >= m_uiCapacity || m_FreeSlots.empty())
		return handle;

	const uint32_t uiSlot = m_FreeSlots.back();
	m_FreeSlots.pop_back();

	const uint32_t uiDense = m_uiCount++;

	m_SlotToDense[uiSlot] = uiDense;
	m_DenseToSlot[uiDense] = uiSlot;

	Position[uiDense] = spawn.v3Position;
	Velocity[uiDense] = spawn.v3Velocity;
	Color[uiDense] = spawn.uiColor;
	Timer[uiDense] = spawn.fTimer;
	BakeOnImpact[uiDense] = spawn.bBakeOnImpact ? 1u : 0u;

	handle.uiSlot = uiSlot;
	handle.uiGeneration = m_SlotGeneration[uiSlot];

	return handle;
}

uint32_t ParticleCore::Resolve(const ParticleHandle& handle) const
{
	if (handle.uiSlot >= m_uiCapacity)
		return UINT32_MAX;

	if (m_SlotGeneration[handle.uiSlot] != handle.uiGeneration)
		return UINT32_MAX;

	const uint32_t uiDense = m_SlotToDense[handle.uiSlot];

	return uiDense < m_uiCount ? uiDense : UINT32_MAX;
}

bool ParticleCore::IsAlive(const ParticleHandle& handle) const
{
	return Resolve(handle) != UINT32_MAX;
}

void ParticleCore::Retire(uint32_t uiIndex)
{
	if (uiIndex >= m_uiCount)
		return;

	const uint32_t uiSlot = m_DenseToSlot[uiIndex];

	if (uiSlot < m_uiCapacity)
	{
		/* Bumped before the slot goes back on the free list, so any handle to
		   this particle is dead from here on rather than aliasing the next
		   particle to take the slot. That is P2's double-free and P1's
		   render-after-retire made impossible rather than guarded against. */
		++m_SlotGeneration[uiSlot];
		m_SlotToDense[uiSlot] = UINT32_MAX;
		m_FreeSlots.push_back(uiSlot);
	}

	const uint32_t uiLast = m_uiCount - 1;

	if (uiIndex != uiLast)
	{
		Position[uiIndex] = Position[uiLast];
		Velocity[uiIndex] = Velocity[uiLast];
		Color[uiIndex] = Color[uiLast];
		Timer[uiIndex] = Timer[uiLast];
		BakeOnImpact[uiIndex] = BakeOnImpact[uiLast];

		const uint32_t uiMovedSlot = m_DenseToSlot[uiLast];

		m_DenseToSlot[uiIndex] = uiMovedSlot;

		if (uiMovedSlot < m_uiCapacity)
			m_SlotToDense[uiMovedSlot] = uiIndex;
	}

	m_DenseToSlot[uiLast] = UINT32_MAX;
	--m_uiCount;
}

ParticleCore::AuditResult ParticleCore::Audit() const
{
	AuditResult result;

	result.uiCount = m_uiCount;
	result.uiCapacity = m_uiCapacity;
	result.uiFreeListSize = static_cast<uint32_t>(m_FreeSlots.size());

	std::vector<uint8_t> seen(m_uiCapacity, 0);

	for (uint32_t uiDense = 0; uiDense < m_uiCount; ++uiDense)
	{
		const uint32_t uiSlot = m_DenseToSlot[uiDense];

		if (uiSlot >= m_uiCapacity || m_SlotToDense[uiSlot] != uiDense)
		{
			++result.uiBrokenMappings;
			continue;
		}

		if (seen[uiSlot])
			++result.uiDuplicateSlots;

		seen[uiSlot] = 1;
	}

	for (uint32_t uiSlot : m_FreeSlots)
	{
		if (uiSlot >= m_uiCapacity || seen[uiSlot])
			++result.uiDuplicateSlots;
	}

	return result;
}
