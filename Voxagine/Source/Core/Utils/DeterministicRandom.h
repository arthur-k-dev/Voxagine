#pragma once

#include <cstdint>

/* A small, explicitly-seeded PRNG for anything the destruction gauntlet has to
 * reproduce (DESTRUCTION_PLAN.md phase 0).
 *
 * glm::linearRand draws from a global engine that srand/rand and every other
 * caller shares, so two runs of the same script diverge on the first unrelated
 * call anywhere in the process. That makes a state hash worthless. This is
 * xorshift64*: one uint64 of state, no allocation, no global, and the same
 * sequence on every platform and compiler - which the standard distributions
 * explicitly do not promise.
 *
 * It is not a replacement for gameplay randomness in general; it is for the
 * paths a deterministic replay has to pin down. Seeding it identically and
 * feeding it the same call sequence is the whole contract.
 */
class DeterministicRandom
{
public:
	explicit DeterministicRandom(uint64_t uiSeed = k_uiDefaultSeed) { Seed(uiSeed); }

	/* Zero is the one state xorshift cannot leave, so it is folded away. */
	void Seed(uint64_t uiSeed) { m_uiState = uiSeed ? uiSeed : k_uiDefaultSeed; }

	uint64_t Next()
	{
		m_uiState ^= m_uiState >> 12;
		m_uiState ^= m_uiState << 25;
		m_uiState ^= m_uiState >> 27;

		return m_uiState * 2685821657736338717ull;
	}

	/* [0, 1). 53 bits of mantissa, taken from the high end where xorshift64* is
	   strongest. */
	double NextUnit() { return static_cast<double>(Next() >> 11) * (1.0 / 9007199254740992.0); }

	float Range(float fMin, float fMax)
	{
		return fMin + static_cast<float>(NextUnit()) * (fMax - fMin);
	}

private:
	static const uint64_t k_uiDefaultSeed = 11400714819323198485ull;

	uint64_t m_uiState = k_uiDefaultSeed;
};
