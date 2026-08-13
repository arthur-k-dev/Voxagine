#include "pch.h"
#include "PlayerSlot.h"

#include <Core/ECS/World.h>

#include "Humanoids/Players/Player.h"

Player* FindPlayerByIndex(World* pWorld, int32_t iIndex)
{
	if (pWorld == nullptr || iIndex < 0)
		return nullptr;

	/* By type rather than by tag: `Player::Awake` adds the "Player" tag, but a
	   root that has been staged and admitted has not necessarily run Awake yet,
	   and the index is available from the deserialized property either way. */
	for (Player* pPlayer : pWorld->FindEntitiesOfType<Player>())
	{
		if (pPlayer == nullptr || pPlayer->IsDestroyed())
			continue;

		if (pPlayer->GetPlayerIndex() == iIndex)
			return pPlayer;
	}

	return nullptr;
}

void PlayerSlot::Adopt(Entity* pEntity)
{
	Player* pPlayer = dynamic_cast<Player*>(pEntity);

	if (pPlayer == nullptr)
		return;

	m_pPlayer = pPlayer;

	/* Take the index from what was authored, so re-resolution later finds this
	   same player rather than whatever the slot was configured to want. */
	if (pPlayer->GetPlayerIndex() >= 0)
		m_iIndex = pPlayer->GetPlayerIndex();
}

Player* PlayerSlot::Resolve(World* pWorld)
{
	/* Steady state: one null test. */
	if (m_pPlayer != nullptr)
		return m_pPlayer;

	m_pPlayer = FindPlayerByIndex(pWorld, m_iIndex);

	return m_pPlayer;
}
