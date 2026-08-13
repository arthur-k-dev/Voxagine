
#include "pch.h"

#include <rttr/registration.h>

#include "GameManager.h"

#include <Core/ECS/World.h>
#include "Core/ECS/Components/InputHandler.h"
#include "Core/ECS/Components/Transform.h"
#include "Core/ECS/Components/AudioSource.h"
#include "Core/ECS/Components/UI/UIButton.h"
#include "Core/Platform/Input/InputContext.h"

#include "AI/FiniteStateMachine.h"

#include "General/PlayerSlot.h"

#include <cstdlib>

#include "UI/Loadout.h"

#include "Humanoids/Enemies/Monster.h"
#include "Humanoids/Players/Player.h"
#include "UI/HealthUI.h"
#include "UI/ComboUI.h"
#include "UI/ComboSliderUI.h"
#include "UI/ComboIcon.h"

#include "Core/MetaData/PropertyTypeMetaData.h"

// States
#include "Gameplay/States/GM_LoadoutState.h"
#include "UI/States/MenuState.h"

namespace rttr::detail
{
	template<>
	struct template_type_trait<std::array<Player*, 2>> : std::true_type
	{
		static std::vector<::rttr::type> get_template_arguments() { return {}; }
	};

	template<>
	struct template_type_trait<std::array<Entity*, 2>> : std::true_type
	{
		static std::vector<::rttr::type> get_template_arguments() { return {}; }
	};
}

RTTR_REGISTRATION
{
	/*
	 * @brief Example enumeration registration with RTTR
	 * @param Class, Registration name
	 * @param <name, EnumValue>
	 */
	rttr::registration::enumeration<EGameState>("GameState")
	(
		rttr::value("Start",	EGameState::Start),
		rttr::value("Play",		EGameState::Play),
		rttr::value("End",		EGameState::End)
	);

	rttr::registration::class_<GameManager>("GameManager")
		.constructor<World*>()(
			// rttr::policy::ctor::as_object,
			rttr::policy::ctor::as_raw_ptr // ,
			// rttr::policy::ctor::as_std_shared_ptr
		)
		.property("Is Playing", &GameManager::m_bIsPlaying)
		.property("State", &GameManager::GetPlayState, &GameManager::SetPlayState)(RTTR_PUBLIC)
		.property("Players", &GameManager::m_pPlayers)(RTTR_PUBLIC)
		.property("Min Voxel Explosion Range", &GameManager::voxelExplosionRangeMin)(RTTR_PUBLIC)
		.property("Max Voxel Explosion Range", &GameManager::voxelExplosionRangeMax)(RTTR_PUBLIC)
		.property("Bullet Return Speed", &GameManager::m_fBulletReturnSpeed)(RTTR_PUBLIC)
		.property("Health", &GameManager::GetHealth, &GameManager::SetHealth)(RTTR_PUBLIC)
		.property("Max Health", &GameManager::GetMaxHealth, &GameManager::SetMaxHealth)(RTTR_PUBLIC)
		.property("Invincibility Hit Delay", &GameManager::m_fInvincibilityTime)(RTTR_PUBLIC)
		.property("Player End Positions", &GameManager::m_vArrEndEntities)(RTTR_PUBLIC)
		.property("ComboTimer Starting Time", &GameManager::comboTimerStartTime)(RTTR_PUBLIC, RTTR_CATEGORY("ComboSystem"))
		.property("Time Based Bonus 1", &GameManager::timeBasedBonus1)(RTTR_PUBLIC, RTTR_CATEGORY("ComboSystem"))
		.property("Bonus 1 Time held", &GameManager::Bonus1TimeHeld)(RTTR_PUBLIC, RTTR_CATEGORY("ComboSystem"))
		.property("Time Based Bonus 2", &GameManager::timeBasedBonus2)(RTTR_PUBLIC, RTTR_CATEGORY("ComboSystem"))
		.property("Bonus 2 Time held", &GameManager::Bonus2TimeHeld)(RTTR_PUBLIC, RTTR_CATEGORY("ComboSystem"))
		.property("Hitting Geometry Bonus", &GameManager::environmentComboBonus)(RTTR_PUBLIC, RTTR_CATEGORY("ComboSystem"))
		.property("Hitting Enemy Bonus", &GameManager::enemyComboBonus)(RTTR_PUBLIC, RTTR_CATEGORY("ComboSystem"))
		.property("Combo Threshold 1", &GameManager::comboThreshold1)(RTTR_PUBLIC, RTTR_CATEGORY("ComboSystem"))
		.property("Combo Threshold 2", &GameManager::comboThreshold2)(RTTR_PUBLIC, RTTR_CATEGORY("ComboSystem"))
		.property("Combo Threshold 3", &GameManager::comboThreshold3)(RTTR_PUBLIC, RTTR_CATEGORY("ComboSystem"))
		.property("Speed Combo Multiplier 1", &GameManager::speedComboMultiplier1)(RTTR_PUBLIC, RTTR_CATEGORY("ComboSystem"))
		.property("Speed Combo Multiplier 2", &GameManager::speedComboMultiplier2)(RTTR_PUBLIC, RTTR_CATEGORY("ComboSystem"))
		.property("Speed Combo Multiplier 3", &GameManager::speedComboMultiplier3)(RTTR_PUBLIC, RTTR_CATEGORY("ComboSystem"))
		.property("UI buttons", &GameManager::vCurrentButtons)(RTTR_PUBLIC);
}

/* **Player discovery by index, retried until it succeeds.**

   This replaced `FindEntitiesOfType<Player>()` indexed positionally behind an
   exact `size() == 1` / `size() == 2` test, which had three failure modes and
   hit all of them under chunk streaming: the vector's order is admission order,
   so which player became P1 varied between runs of the same level; a count of
   zero - the normal state while the players' chunk is still admitting - matched
   neither branch and left both slots null with nothing to try again; and it ran
   only from Awake, once.

   See PlayerSlot for the measurements. */
void GameManager::ResolvePlayers()
{
	const bool bHadBoth = m_pPlayers[0] != nullptr && m_pPlayers[1] != nullptr;

	for (uint32_t uiIndex = 0; uiIndex < m_pPlayers.size(); ++uiIndex)
	{
		if (m_pPlayers[uiIndex] != nullptr)
			continue;

		Player* pPlayer = FindPlayerByIndex(GetWorld(), static_cast<int32_t>(uiIndex));

		if (pPlayer == nullptr)
			continue;

		m_pPlayers[uiIndex] = pPlayer;
		pPlayer->fReturnSpeed = m_fBulletReturnSpeed;

		/* A player is what the resident window is centred on, so it must not be
		   unloaded out from under the level by the chunk it happens to stand in.
		   StartGame meant to do this and set player 0 twice, so player 2 was
		   never pinned - a copy-paste that only shows up as the second player
		   vanishing mid-level. Done here instead, where it applies to whichever
		   players actually attached and does not depend on StartGame having run
		   after they did. */
		pPlayer->SetPersistent(true);

		pPlayer->Destroyed += Event<Entity*>::Subscriber([this, uiIndex](Entity*)
		{
			m_pPlayers[uiIndex] = nullptr;

			/* And break the *other* player's link to this one, which nothing
			   did: SetLinkPlayer hands out a raw Player* and chunk streaming
			   destroys players routinely, so the survivor was left holding a
			   freed pointer that it dereferences on every throw and catch. The
			   cross-link below re-establishes both when the pair is complete
			   again. */
			const uint32_t uiOther = uiIndex == 0 ? 1 : 0;

			if (m_pPlayers[uiOther] != nullptr)
				m_pPlayers[uiOther]->SetLinkPlayer(nullptr);
		}, this);
	}

	/* Cross-link whenever both are present and are not already pointing at each
	 * other. **Idempotent, not edge-triggered**, and that distinction is the
	 * whole defect.
	 *
	 * This used to fire only on the transition into "both present"
	 * (`!bHadBoth && both`), which is a trigger that can be consumed without
	 * doing anything: ResolvePlayers runs from Awake, from StartGame and from
	 * every Tick, so any call that observed both players before the links were
	 * wanted left bHadBoth true forever after, and the pair was never linked.
	 * Measured in a live session - both players resolved, correctly indexed,
	 * with `partners now (nil) / (nil)` and `had both 1`.
	 *
	 * Everything the two-player loop does hangs off this one pointer: the
	 * receiver role, the incoming-bullet list Recall iterates, and the catch. So
	 * the visible symptom was "the bullet never comes back", three layers away
	 * from the missed assignment.
	 *
	 * Two pointer comparisons per tick is not a cost worth an edge trigger. */
	if (m_pPlayers[0] != nullptr && m_pPlayers[1] != nullptr)
	{
		if (m_pPlayers[0]->GetLinkedPlayer() != m_pPlayers[1])
			m_pPlayers[0]->SetLinkPlayer(m_pPlayers[1]);

		if (m_pPlayers[1]->GetLinkedPlayer() != m_pPlayers[0])
			m_pPlayers[1]->SetLinkPlayer(m_pPlayers[0]);
	}


	/* VOXAGINE_GAMEPLAY_DEBUG=1, once per change. Both players report a null
	   partner in a level that has two of them, and this is the only place that
	   pairs them - so the question is which of "found index 0", "found index 1"
	   and "took the cross-link branch" is not happening. */
	static const bool s_bDebug = std::getenv("VOXAGINE_GAMEPLAY_DEBUG") != nullptr;

	if (s_bDebug)
	{
		static const void* s_pLast0 = reinterpret_cast<const void*>(-1);
		static const void* s_pLast1 = reinterpret_cast<const void*>(-1);

		if (s_pLast0 != m_pPlayers[0] || s_pLast1 != m_pPlayers[1])
		{
			s_pLast0 = m_pPlayers[0];
			s_pLast1 = m_pPlayers[1];

			/* Every Player in the world with the index it is actually
			   advertising - because if the pair never completes, the reason is
			   almost certainly that two players are advertising the same index.
			   Player::Awake derives it from the name, and the name arrives by
			   deserialization. */
			for (Player* pAny : GetWorld()->FindEntitiesOfType<Player>())
			{
				if (pAny == nullptr)
					continue;

				fprintf(stderr, "[gamemanager]   candidate %p '%s' index %d destroyed %d\n",
					(void*)pAny, pAny->GetName().c_str(), pAny->GetPlayerIndex(), pAny->IsDestroyed() ? 1 : 0);
			}

			fprintf(stderr, "[gamemanager] players: [0] %p '%s', [1] %p '%s' (had both %d); partners now %p / %p\n",
				(void*)m_pPlayers[0], m_pPlayers[0] ? m_pPlayers[0]->GetName().c_str() : "-",
				(void*)m_pPlayers[1], m_pPlayers[1] ? m_pPlayers[1]->GetName().c_str() : "-",
				bHadBoth ? 1 : 0,
				(void*)(m_pPlayers[0] ? m_pPlayers[0]->GetLinkedPlayer() : nullptr),
				(void*)(m_pPlayers[1] ? m_pPlayers[1]->GetLinkedPlayer() : nullptr));
		}
	}
}

void GameManager::SetPlayerPosition(const Vector3& vPosition, uint32_t uiIndex)
{
	if (uiIndex < m_pPlayers.size() && m_pPlayers[uiIndex]) m_pPlayers[uiIndex]->GetTransform()->SetPosition(vPosition);
}

void GameManager::SetEndPlayerPosition(const Vector3& vPosition, uint32_t uiIndex)
{
	if (uiIndex < m_vArrEndEntities.size() && m_vArrEndEntities[uiIndex]) m_vArrEndEntities[uiIndex]->GetTransform()->SetPosition(vPosition);
}

GameManager::GameManager(World* world) : Entity(world)
{
	SetName("GameManager");

	m_pFiniteStateMachine = new FiniteStateMachine<GameManager>(this);
	AddState({ "LoadOutCustomization", new GM_LoadOutState }, true);
	AddState({ "Playing", new MenuState }); // Dummy state
}

void GameManager::StartGame()
{
	/* Pinning the players moved into ResolvePlayers, which is where they are
	   known - this ran before they had attached and set player 0 twice. */
	ResolvePlayers();

	m_fHealth = m_fMaxHealth;
	m_bIsPlaying = true;
}

void GameManager::Reset()
{
	if (m_pPlayers[0])
		m_pPlayers[0]->Reset();

	if(m_pPlayers[1])
		m_pPlayers[1]->Reset();

	m_bIsPlaying = true;
}

void GameManager::SetPlayState(EGameState state)
{
	if (m_EGameState == state)
		return;

	m_EGameState = state;
	switch (m_EGameState)
	{
	case EGameState::Start:
		SetState("LoadOutCustomization");
		break;
	case EGameState::Play:
		SetState("Playing");
		break;
	case EGameState::End:
		break;
	}
}

void GameManager::Awake()
{
	ResolvePlayers();

	const auto children = GetChildren();
	if (!children.empty())
	{
		for (auto pChild : children)
		{
			if (pChild->HasTag("End"))
			{
				if (!m_vArrEndEntities[0])
					m_vArrEndEntities[0] = pChild;
				else if (!m_vArrEndEntities[1])
					m_vArrEndEntities[1] = pChild;
			}
		}

		for (auto& pEntity : m_vArrEndEntities)
		{
			if (!pEntity)
			{
				pEntity = GetWorld()->SpawnEntity<Entity>(GetTransform()->GetPosition(), GetTransform()->GetRotation(), Vector3(1.0f));
				pEntity->AddTag("End");
				pEntity->SetParent(this);
			}
		}
	}
	else
	{
		uint32_t counter = 0;
		for (auto& pEntity : m_vArrEndEntities)
		{
			if (!pEntity)
			{
				pEntity = GetWorld()->SpawnEntity<Entity>(GetTransform()->GetPosition(), GetTransform()->GetRotation(), Vector3(1.0f));
				pEntity->AddTag("End");
				pEntity->SetName("EndEntity " + std::to_string(counter++));
				pEntity->SetParent(this);
			}
		}
	}

	if (m_pFiniteStateMachine->GetCurrentState()) m_pFiniteStateMachine->GetCurrentState()->Awake(this);
}

void GameManager::Start()
{
	Entity::Start();

	SetPersistent(true);

	m_pHealthUI = dynamic_cast<HealthUI*>(GetWorld()->FindEntity("HealthUI"));
	m_pComboUI = dynamic_cast<ComboUI*>(GetWorld()->FindEntity("ComboUI"));
	m_pComboSlider = dynamic_cast<ComboSliderUI*>(GetWorld()->FindEntity("ComboSliderUI"));
	m_pComboIcon = dynamic_cast<ComboIcon*>(GetWorld()->FindEntity("ComboIcon"));

	AddComponent<AudioSource>();
	/* TODO grab it for the load out state = */ AddComponent<LoadOut>();

	switch (m_EGameState)
	{
	case EGameState::Start:
		SetState("LoadOutCustomization");
		break;
	case EGameState::Play:
		SetState("Playing");
		break;
	case EGameState::End:
		break;
	}

	m_pInputHandler = AddComponent<InputHandler>();
	if (!m_pInputHandler)
		m_pInputHandler = GetComponent<InputHandler>();

	StartGame();
}

void GameManager::Tick(float fDeltaTime)
{
	Entity::Tick(fDeltaTime);

	ResolvePlayers();

	if (m_fInvincibilityTimer > 0)
		m_fInvincibilityTimer -= fDeltaTime;

	if(m_pFiniteStateMachine)
	{
		m_pFiniteStateMachine->Tick(fDeltaTime);
	}

	if (currentComboTimer > 0)
	{
		currentComboTimer = currentComboTimer - fDeltaTime;
	}
}

void GameManager::OnDrawGizmos(float)
{
	DebugRenderer* pDebugRenderer = GetWorld()->GetDebugRenderer();
	if (pDebugRenderer)
	{
		pDebugRenderer->AddCenteredSphere((m_vArrEndEntities[0] ? m_vArrEndEntities[0]->GetTransform()->GetPosition() : Vector3(0.0f)), Vector3(15.0f), VColors::YellowGreen);
		pDebugRenderer->AddCenteredSphere((m_vArrEndEntities[1] ? m_vArrEndEntities[1]->GetTransform()->GetPosition() : Vector3(0.0f)), Vector3(15.0f), VColors::YellowGreen);
	}
}

void GameManager::ResetComboTimer()
{
	currentComboTimer = comboTimerStartTime;
}

int GameManager::GetComboStreak()
{
	if (m_pComboUI)
	{
		m_pComboUI->SetComboUI(std::to_string(comboStreak));
	}
	return comboStreak;
}

float GameManager::GetComboTimer()
{
	return currentComboTimer;
}

int GameManager::GetSharedPlayerHealth()
{
	return sharedPlayerHealth;
}

bool GameManager::CanBeDamaged()
{
	return m_fInvincibilityTimer <= 0.f;
}

void GameManager::SharedPlayerHealthTakeDamage(int damage)
{
	sharedPlayerHealth = sharedPlayerHealth - damage;
	if (m_pHealthUI)
	{
		const float percentage = static_cast<float>(sharedPlayerHealth) / m_fMaxHealth;
		m_pHealthUI->SetHealthCullingEnd(percentage);
	}

	m_fInvincibilityTimer = m_fInvincibilityTime;
}

void GameManager::AddComboStreak(int comboNumber)
{
	comboStreak = comboStreak + comboNumber;
	if (m_pComboUI)
	{
		m_pComboUI->SetComboUI(std::to_string(comboStreak));
	}
}

void GameManager::AddToOnComboOnCatch(int comboNumber)
{
	comboOnCatch = comboOnCatch + comboNumber;
		if (Utils::InRangeExcluded(0, comboStreak, comboThreshold1) || comboStreak == 0)
		{
			if (m_pComboSlider)
			{
				m_pComboSlider->SetComboSlider(comboOnCatch, comboThreshold1);
			}
			if (m_pComboIcon)
			{
				m_pComboIcon->SetComboIconImage(0);
			}
		}
		if (Utils::InRangeExcluded(comboThreshold1, comboStreak, comboThreshold2))
		{
			if (m_pComboSlider)
			{
				m_pComboSlider->SetComboSlider(comboOnCatch, comboThreshold2);
			}
			if (m_pComboIcon)
			{
				m_pComboIcon->SetComboIconImage(1);
			}
		}
		if (Utils::InRangeExcluded(comboThreshold2, comboStreak, comboThreshold3))
		{
			if (m_pComboSlider)
			{
				m_pComboSlider->SetComboSlider(comboOnCatch, comboThreshold3);
			}
			if (m_pComboIcon)
			{
				m_pComboIcon->SetComboIconImage(2);
			}
		}
		if (comboStreak>comboThreshold3)
		{
			if (m_pComboSlider)
			{
				m_pComboSlider->SetComboSlider(comboOnCatch, comboThreshold3);
			}
			if (m_pComboIcon)
			{
				m_pComboIcon->SetComboIconImage(3);
			}
		}
}

void GameManager::ResetComboStreak()
{
	comboStreak = 0;
	if (m_pComboUI)
	{
		m_pComboUI->SetComboUI(std::to_string(comboStreak));
	}
	if (m_pComboSlider)
	{
		m_pComboSlider->SetComboSlider(comboStreak, comboStreak);
	}
	if (m_pComboIcon)
	{
		m_pComboIcon->SetComboIconImage(0);
	}
}
