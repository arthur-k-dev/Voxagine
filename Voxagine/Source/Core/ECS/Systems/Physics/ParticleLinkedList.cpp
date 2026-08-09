#include "pch.h"
#include "ParticleLinkedList.h"

#include "Core/Platform/Rendering/Objects/Mapper.h"

ParticleLinkedList::ParticleLinkedList(uint32_t uiReserveSize)
{
	m_pFirstAliveParticle = nullptr;
	m_pLastAliveParticle = nullptr;

	/* Add new particles */
	m_Particles.reserve(uiReserveSize);
	for (uint32_t i = 0; i < uiReserveSize; ++i)
		m_Particles.push_back(Particle());

	m_pAvailableParticle = &m_Particles[0];

	/* Each particle points to the next */
	for (uint32_t i = 0; i < uiReserveSize - 1; ++i)
		m_Particles[i].NextAvailable = &m_Particles[i + 1];

	/* The last one terminates the list */
	m_Particles[uiReserveSize - 1].NextAvailable = nullptr;
}

Particle* ParticleLinkedList::SpawnParticle()
{
	if (!m_pAvailableParticle)
		return nullptr;

	Particle* particle = m_pAvailableParticle;
	m_pAvailableParticle = particle->NextAvailable;

	InitParticle(particle);
	
	if (!m_pFirstAliveParticle)
	{
		m_pFirstAliveParticle = particle;
		m_pLastAliveParticle = particle;
	}
	else
	{
		m_pFirstAliveParticle->Prev = particle;
		particle->Next = m_pFirstAliveParticle;
		m_pFirstAliveParticle = particle;
	}

	return particle;
}

void ParticleLinkedList::DestroyParticle(Particle* pParticle)
{
	pParticle->NextAvailable = m_pAvailableParticle;
	m_pAvailableParticle = pParticle;

	if (pParticle->Next && pParticle->Prev)
	{
		pParticle->Prev->Next = pParticle->Next;
		pParticle->Next->Prev = pParticle->Prev;
	}
	else if (pParticle->Next)
	{
		pParticle->Next->Prev = nullptr;
		m_pFirstAliveParticle = pParticle->Next;
	}
	else if (pParticle->Prev)
	{
		pParticle->Prev->Next = nullptr;
		m_pLastAliveParticle = pParticle->Prev;
	}
	else
	{
		m_pFirstAliveParticle = nullptr;
		m_pLastAliveParticle = nullptr;
	}
	
	pParticle->Next = nullptr;
	pParticle->Prev = nullptr;
}

ParticleLinkedList::AuditResult ParticleLinkedList::Audit() const
{
	AuditResult result;
	result.uiPool = m_Particles.size();

	if (m_Particles.empty())
		return result;

	/* One tag per particle rather than a set: the pool is a contiguous vector,
	   so a pointer into it is an index, and the whole audit stays O(n) with no
	   allocation beyond this. Bit 0 = seen alive, bit 1 = seen free. */
	std::vector<uint8_t> seen(m_Particles.size(), 0);

	const Particle* pBase = m_Particles.data();

	auto indexOf = [pBase, this](const Particle* pParticle) -> size_t
	{
		const size_t uiIndex = static_cast<size_t>(pParticle - pBase);
		return uiIndex < m_Particles.size() ? uiIndex : m_Particles.size();
	};

	/* Walked forward from the head. A cycle is bounded by the pool size, which
	   is why the loop counts rather than trusting the terminator. */
	uint64_t uiSteps = 0;

	for (const Particle* p = m_pFirstAliveParticle; p != nullptr; p = p->Next)
	{
		if (++uiSteps > result.uiPool)
		{
			result.bAliveCycle = true;
			break;
		}

		const size_t uiIndex = indexOf(p);

		if (uiIndex == m_Particles.size())
			continue;

		if (seen[uiIndex] & 1)
		{
			++result.uiDuplicated;
			result.bAliveCycle = true;
			break;
		}

		seen[uiIndex] |= 1;
		++result.uiAlive;
	}

	uiSteps = 0;

	for (const Particle* p = m_pAvailableParticle; p != nullptr; p = p->NextAvailable)
	{
		if (++uiSteps > result.uiPool)
		{
			result.bFreeCycle = true;
			break;
		}

		const size_t uiIndex = indexOf(p);

		if (uiIndex == m_Particles.size())
			continue;

		if (seen[uiIndex] & 2)
		{
			++result.uiDuplicated;
			result.bFreeCycle = true;
			break;
		}

		seen[uiIndex] |= 2;
		++result.uiFree;
	}

	for (uint8_t uiTag : seen)
	{
		if (uiTag == 0)
			++result.uiUnaccounted;
		else if (uiTag == 3)
			++result.uiDuplicated;
	}

	return result;
}

void ParticleLinkedList::InitParticle(Particle* pParticle)
{
	pParticle->Live.Velocity = Vector3(0.f);
	pParticle->Live.Position = Vector3(0.f);
	pParticle->Live.GridPosition = Vector3(0.f);
	pParticle->Live.VoxelColor = 0;
	pParticle->Live.BakeOnImpact = true;
	pParticle->Live.UserPointer = nullptr;
	pParticle->Live.Timer = NO_PARTICLE_TIMER;
}
