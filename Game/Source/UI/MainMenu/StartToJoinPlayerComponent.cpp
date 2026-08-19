#include "StartToJoinPlayerComponent.h"

#include "Humanoids/Players/Player.h"
#include "Core/ECS/Components/InputHandler.h"

#include "Core/Application.h"
#include "Core/Platform/Platform.h"

#include "Core/ECS/Entities/UI/Canvas.h"
#include "Core/ECS/Components/VoxAnimator.h"

#include "Core/ECS/Components/AudioSource.h"

#include <rttr/registration>
#include "Core/MetaData/PropertyTypeMetaData.h"
RTTR_REGISTRATION
{
	rttr::registration::class_<StartToJoinPlayerComponent>("StartToJoinPlayerComponent")
		.constructor<Entity*>()(rttr::policy::ctor::as_raw_ptr)
		.property("Press Start To Join Visuals", &StartToJoinPlayerComponent::m_pStartToJoinVisuals)(RTTR_PUBLIC)
		.property("Join Visual Interval", &StartToJoinPlayerComponent::m_fFlickerInterval)(RTTR_PUBLIC)

		.property("Player Joined Visuals", &StartToJoinPlayerComponent::m_pJoinedVisuals)(RTTR_PUBLIC)

		.property("Assigned Player", &StartToJoinPlayerComponent::m_pPlayer)(RTTR_PUBLIC)

		.property("Error Interval", &StartToJoinPlayerComponent::m_fPlayerErrorInterval)(RTTR_PUBLIC)
		.property("Error Switch Times", &StartToJoinPlayerComponent::m_iPlayerErrorSwitchTimes)(RTTR_PUBLIC)

		.property("Default Anim Index", &StartToJoinPlayerComponent::m_iDefaultPlayerAnimIndex)(RTTR_PUBLIC)
		.property("Disabled Anim Index", &StartToJoinPlayerComponent::m_iDisabledPlayerAnimIndex)(RTTR_PUBLIC)
		.property("Error Anim Index", &StartToJoinPlayerComponent::m_iErrorPlayerAnimIndex)(RTTR_PUBLIC)

	;
}

StartToJoinPlayerComponent::StartToJoinPlayerComponent(Entity* m_pEntity)
	: BehaviorScript(m_pEntity)
{
}

StartToJoinPlayerComponent::~StartToJoinPlayerComponent()
{
}

void StartToJoinPlayerComponent::Awake()
{
	// Register all input actions
	InputContextNew* pInputContext = GetWorld()->GetApplication()->GetPlatform().GetInputContext();
	if (pInputContext)
	{
		// Create binding map and register actions to make sure they exist. Inside they check if its already created or registered/
		pInputContext->CreateBindingMap(UI_INPUT_LAYER, false);

		pInputContext->RegisterAction(UI_INPUT_LAYER, "MainMenu_PressToJoin", IKS_PRESSED, IK_GAMEPADOPTION);
		pInputContext->RegisterAction(UI_INPUT_LAYER, "MainMenu_PressToJoin", IKS_PRESSED, IK_MOUSEBUTTONLEFT);

		/* J, because joining had no keyboard binding at all - a keyboard-only
		   player could not get past this screen, and --ui-script is keyboard-only
		   so menu -> level was not scriptable headlessly either
		   (Docs/CHUNK_STREAMING_PLAN.md phase 0 finding 2, phase 8's first item).

		   Deliberately not IK_ENTER or IK_SPACE: Canvas binds both to "Pressed_UI"
		   and the level-select button is live on this same screen, so a shared key
		   would join and click in one press, in an order nothing defines. */
		pInputContext->RegisterAction(UI_INPUT_LAYER, "MainMenu_PressToJoin", IKS_PRESSED, IK_J);
	}
}

void StartToJoinPlayerComponent::Start()
{
	if (m_pPlayer)
	{
		InputHandler* m_pInputHandler = m_pPlayer->GetComponent<InputHandler>();

		m_pInputHandler->BindAction(UI_INPUT_LAYER, "MainMenu_PressToJoin", IKS_PRESSED, std::bind(&StartToJoinPlayerComponent::TogglePlayerShowing, this));
	}

	if (m_pPlayer)
		m_pPlayerRenderer = m_pPlayer->GetComponent<VoxAnimator>();


	SetJoinVisualActive(m_iCurrentStartToJoinVisual);
	SetPlayerAnimIndex(m_iDisabledPlayerAnimIndex);

	m_pAudioSource = GetOwner()->GetComponent<AudioSource>();
	if (!m_pAudioSource)
		m_pAudioSource = GetOwner()->AddComponent<AudioSource>();

	m_pAudioSource->SetFilePath("Content/SFX/Menu/ButtonError.ogg");
	m_pAudioSource->SetLooping(false);
	m_pAudioSource->Set3DAudio(false);

	/* Player one is joined already, provided its device is actually there.
	   Nobody plays this alone by pressing Start at an empty menu first, and the
	   single-player case is the common one; player two still joins by pressing,
	   and player one can drop out by pressing again - this sets the initial
	   state, it does not pin it.

	   Gated on HasConnectedDevice so the rule stays "a player is a device that
	   is present": a handle with nothing behind it must not arrive in a level
	   as a character nobody can move. Player one holds the keyboard on desktop
	   and the touchscreen on mobile, so in practice this is true; on a build
	   with neither, the menu behaves exactly as it did. */
	if (!PlayerJoined() && m_pPlayer != nullptr)
	{
		const InputHandler* pInputHandler = m_pPlayer->GetComponent<InputHandler>();

		if (pInputHandler != nullptr &&
			pInputHandler->GetPlayerHandle() == k_iFirstPlayerHandle &&
			pInputHandler->HasConnectedDevice())
		{
			TogglePlayerShowing();
		}
	}
}


void StartToJoinPlayerComponent::Tick(float fDeltaTime)
{
	m_fProgressIntervalTime += fDeltaTime;
	if (m_fProgressIntervalTime > m_fFlickerInterval)
	{
		m_fProgressIntervalTime = 0.f;

		// Increment join visual
		m_iCurrentStartToJoinVisual++;
		if (m_iCurrentStartToJoinVisual >= static_cast<int>(m_pStartToJoinVisuals.size()))
			m_iCurrentStartToJoinVisual = 0;
	}

	if (m_bShowErrorAnim && !PlayerJoined())
	{
		m_fPlayerErrorProgress += fDeltaTime;
		if (m_fPlayerErrorProgress > m_fPlayerErrorInterval) 
		{
			m_fPlayerErrorProgress = 0.f;
			m_iPlayerErrorCurrentSwitchTimes++;

			m_bShowErrorVisual = !m_bShowErrorVisual;
			SetPlayerAnimIndex(m_bShowErrorVisual ? m_iErrorPlayerAnimIndex : m_iDisabledPlayerAnimIndex);
			
			if (m_iPlayerErrorCurrentSwitchTimes >= (m_iPlayerErrorSwitchTimes * 2) + 1)
			{
				m_bShowErrorAnim = false;
			}
		}
	}

	if (!PlayerJoined())
	{
		// Update visuals
		SetJoinVisualActive(m_iCurrentStartToJoinVisual);
	}
}

void StartToJoinPlayerComponent::ShowErrorAnim()
{
	if (PlayerJoined())
		return;

	m_pAudioSource->Play();

	m_bShowErrorAnim = true;
	m_bShowErrorVisual = true;
	m_fPlayerErrorProgress = 0.f;
	m_iPlayerErrorCurrentSwitchTimes = 0;
}

void StartToJoinPlayerComponent::TogglePlayerShowing()
{
	m_bPlayerJoined = !m_bPlayerJoined;

	if (PlayerJoined())
	{
		SetJoinVisualActive(-1);
	
		SetPlayerAnimIndex(m_iDefaultPlayerAnimIndex);
	}
	else 
	{
		SetJoinVisualActive(m_iCurrentStartToJoinVisual);

		SetPlayerAnimIndex(m_iDisabledPlayerAnimIndex);
	}
}

void StartToJoinPlayerComponent::SetJoinVisualActive(int index)
{
	for (Entity* entity : m_pJoinedVisuals)
		entity->SetEnabled(index < 0);
	
	for (size_t i = 0; i < m_pStartToJoinVisuals.size(); i++)
		m_pStartToJoinVisuals[i]->SetEnabled(static_cast<int>(i) == index);
}

void StartToJoinPlayerComponent::SetPlayerAnimIndex(unsigned int animIndex)
{
	if (m_pPlayerRenderer)
	{
		uint32_t currentFrame = m_pPlayerRenderer->GetCurrentFrameIndex();
		m_pPlayerRenderer->SetCurrentAnimationWithFrame(animIndex, currentFrame);
	}
}
