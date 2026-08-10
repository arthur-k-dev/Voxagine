#include "pch.h"

#include "Canvas.h"

#include <rttr/registration>
#include "Core/MetaData/PropertyTypeMetaData.h"

#include "Core/ECS/Components/UI/UIButton.h"
#include "Core/ECS/Components/UI/UISlider.h"

#include "Core/ECS/World.h"
#include "Core/Application.h"
#include "Core/Platform/Platform.h"
#include "Core/Platform/Input/Temp/InputContextNew.h"
#include "Core/LaunchOptions.h"

RTTR_REGISTRATION
{
	rttr::registration::class_<Canvas>("Canvas")
		.constructor<World*>()(rttr::policy::ctor::as_raw_ptr)
		.property("Is Navigatable", &Canvas::IsNavigatable, &Canvas::SetNavigatable) (RTTR_PUBLIC)
	;
}

Canvas::Canvas(World * pWorld)
	: Entity(pWorld)
	, m_bNavigatable(true)
	, m_PreviousBindingMap("Default")
{
	UIButton uib(this);
	UISlider uis(this);

	SetName("Canvas");
}

Canvas::~Canvas()
{
	GetWorld()->Resumed -= this;

	OnDisabled();

	GetWorld()->GetApplication()->GetPlatform().GetInputContext()->UnBindAction(m_InputBindings);
}

void Canvas::Awake()
{
	Entity::Awake();

	/* Popping a world that was pushed over this one resumes this one, and that
	   is the only signal a canvas gets that it is in front again. */
	GetWorld()->Resumed += Event<World*>::Subscriber(
		std::bind(&Canvas::OnWorldResumed, this, std::placeholders::_1), this);

	// Register all input actions
	m_pInputContext = GetWorld()->GetApplication()->GetPlatform().GetInputContext();
	if (m_pInputContext)
	{
		// Create binding map and register actions to make sure they exist. Inside they check if its already created or registered/
		m_pInputContext->CreateBindingMap(UI_INPUT_LAYER, false);

		// Previous UI
		m_pInputContext->RegisterAction(UI_INPUT_LAYER, "Previous_UI", IKS_RELEASED, IK_BACK);
		// Next UI
		m_pInputContext->RegisterAction(UI_INPUT_LAYER, "Next_UI", IKS_RELEASED, IK_TAB);

		// UI Up
		m_pInputContext->RegisterAction(UI_INPUT_LAYER, "Up_UI", IKS_RELEASED, IK_UP);
		m_pInputContext->RegisterAction(UI_INPUT_LAYER, "Up_UI", IKS_RELEASED, IK_W);
		m_pInputContext->RegisterAction(UI_INPUT_LAYER, "Up_UI", IKS_RELEASED, IK_GAMEPADLEFTPADUP);
		// UI Down
		m_pInputContext->RegisterAction(UI_INPUT_LAYER, "Down_UI", IKS_RELEASED, IK_DOWN);
		m_pInputContext->RegisterAction(UI_INPUT_LAYER, "Down_UI", IKS_RELEASED, IK_S);
		m_pInputContext->RegisterAction(UI_INPUT_LAYER, "Down_UI", IKS_RELEASED, IK_GAMEPADLEFTPADDOWN);
		// UI Left
		m_pInputContext->RegisterAction(UI_INPUT_LAYER, "Left_UI", IKS_RELEASED, IK_LEFT);
		m_pInputContext->RegisterAction(UI_INPUT_LAYER, "Left_UI", IKS_RELEASED, IK_A);
		m_pInputContext->RegisterAction(UI_INPUT_LAYER, "Left_UI", IKS_RELEASED, IK_GAMEPADLEFTPADLEFT);
		// UI Right
		m_pInputContext->RegisterAction(UI_INPUT_LAYER, "Right_UI", IKS_RELEASED, IK_RIGHT);
		m_pInputContext->RegisterAction(UI_INPUT_LAYER, "Right_UI", IKS_RELEASED, IK_D);
		m_pInputContext->RegisterAction(UI_INPUT_LAYER, "Right_UI", IKS_RELEASED, IK_GAMEPADLEFTPADRIGHT);

		// UI Stick UP DOWN
		m_pInputContext->RegisterAxis(UI_INPUT_LAYER, "Up_Down_UI", IK_GAMEPADRIGHTSTICKAXISY, 1.f);
		m_pInputContext->RegisterAxis(UI_INPUT_LAYER, "Up_Down_UI", IK_GAMEPADLEFTSTICKAXISY, 1.f);
		// UI Stick LEFT RIGHT
		m_pInputContext->RegisterAxis(UI_INPUT_LAYER, "Left_Right_UI", IK_GAMEPADRIGHTSTICKAXISX, 1.f);
		m_pInputContext->RegisterAxis(UI_INPUT_LAYER, "Left_Right_UI", IK_GAMEPADLEFTSTICKAXISX, 1.f);

		// Pressed UI
		m_pInputContext->RegisterAction(UI_INPUT_LAYER, "Pressed_UI", IKS_PRESSED, IK_ENTER);
		m_pInputContext->RegisterAction(UI_INPUT_LAYER, "Pressed_UI", IKS_PRESSED, IK_SPACE);
		m_pInputContext->RegisterAction(UI_INPUT_LAYER, "Pressed_UI", IKS_PRESSED, IK_GAMEPADSELECT);
		m_pInputContext->RegisterAction(UI_INPUT_LAYER, "Pressed_UI", IKS_PRESSED, IK_GAMEPADRIGHTPADDOWN);
		// Pressed UI
		m_pInputContext->RegisterAction(UI_INPUT_LAYER, "Pressed_Repeat_UI", IKS_HELD, IK_ENTER);
		m_pInputContext->RegisterAction(UI_INPUT_LAYER, "Pressed_Repeat_UI", IKS_HELD, IK_SPACE);
		m_pInputContext->RegisterAction(UI_INPUT_LAYER, "Pressed_Repeat_UI", IKS_HELD, IK_GAMEPADSELECT);
		m_pInputContext->RegisterAction(UI_INPUT_LAYER, "Pressed_Repeat_UI", IKS_HELD, IK_GAMEPADRIGHTPADDOWN);
		// Released UI
		m_pInputContext->RegisterAction(UI_INPUT_LAYER, "Released_UI", IKS_RELEASED, IK_ENTER);
		m_pInputContext->RegisterAction(UI_INPUT_LAYER, "Released_UI", IKS_RELEASED, IK_SPACE);
		m_pInputContext->RegisterAction(UI_INPUT_LAYER, "Released_UI", IKS_RELEASED, IK_GAMEPADSELECT);
		m_pInputContext->RegisterAction(UI_INPUT_LAYER, "Released_UI", IKS_RELEASED, IK_GAMEPADRIGHTPADDOWN);


		// Bind callback ation tot the registered actions.
		m_pInputContext->BindAction(UI_INPUT_LAYER, "Previous_UI", IKS_RELEASED, m_InputBindings, BMT_PLAYERCONTROLLERS, std::bind(&Canvas::SetFocusPrevious, this));
		m_pInputContext->BindAction(UI_INPUT_LAYER, "Next_UI", IKS_RELEASED, m_InputBindings, BMT_PLAYERCONTROLLERS, std::bind(&Canvas::SetFocusNext, this));

		m_pInputContext->BindAction(UI_INPUT_LAYER, "Up_UI", IKS_RELEASED, m_InputBindings, BMT_PLAYERCONTROLLERS, std::bind(&Canvas::SetFocusUp, this));
		m_pInputContext->BindAction(UI_INPUT_LAYER, "Down_UI", IKS_RELEASED, m_InputBindings, BMT_PLAYERCONTROLLERS, std::bind(&Canvas::SetFocusDown, this));
		m_pInputContext->BindAction(UI_INPUT_LAYER, "Left_UI", IKS_RELEASED, m_InputBindings, BMT_PLAYERCONTROLLERS, std::bind(&Canvas::SetFocusLeft, this));
		m_pInputContext->BindAction(UI_INPUT_LAYER, "Right_UI", IKS_RELEASED, m_InputBindings, BMT_PLAYERCONTROLLERS, std::bind(&Canvas::SetFocusRight, this));

		m_pInputContext->BindAction(UI_INPUT_LAYER, "Pressed_UI", IKS_PRESSED, m_InputBindings, BMT_PLAYERCONTROLLERS, std::bind(&Canvas::OnPressed, this));
		m_pInputContext->BindAction(UI_INPUT_LAYER, "Pressed_Repeat_UI", IKS_HELD, m_InputBindings, BMT_PLAYERCONTROLLERS, std::bind(&Canvas::OnPressedRepeat, this));
		m_pInputContext->BindAction(UI_INPUT_LAYER, "Released_UI", IKS_RELEASED, m_InputBindings, BMT_PLAYERCONTROLLERS, std::bind(&Canvas::OnReleased, this));
	}
}

void Canvas::Start()
{
	Entity::Start();

	// Call the OnEnabled function when the canvas is enabled by default.
	if (IsEnabled() && IsNavigatable())
		OnEnabled();
}

void Canvas::Tick(float fDeltaTime)
{
	Entity::Tick(fDeltaTime);

	if (m_pInputContext)
	{
		Vector2 m_Value = { 
			m_pInputContext->GetAxisValue(UI_INPUT_LAYER, "Left_Right_UI").Value,
			m_pInputContext->GetAxisValue(UI_INPUT_LAYER, "Up_Down_UI").Value
		};

		if (m_PrevInputValue.x < .5f && m_PrevInputValue.x > -.5f)
		{
			if (m_Value.x < -.5f) {
				SetFocusLeft(); 
			}
			else if (m_Value.x > .5f) {
				SetFocusRight();
			}
		}

		if (m_PrevInputValue.y < .5f && m_PrevInputValue.y > -.5f)
		{
			if (m_Value.y < -.5f) {
				SetFocusDown();
			}
			else if (m_Value.y > .5f) {
				SetFocusUp();
			}
		}

		m_PrevInputValue = std::move(m_Value);
	}
}

/* See the header. The world stack is the authority on which menu is in front,
   because nothing else is: the canvas underneath is still enabled, still
   navigatable and still bound to the same input map. */
void Canvas::OnWorldResumed(World*)
{
	TakeControl();
}

void Canvas::TakeControl()
{
	if (!IsInteractive())
		return;

	/* --- The input map ------------------------------------------------------
	   This is the half that actually broke, and the mechanism is worth keeping.

	   OnEnabled remembers whichever map happened to be active when this canvas
	   was enabled, and OnDisabled hands that exact string back. For a single
	   canvas over gameplay that is right. For a canvas pushed over *another
	   canvas* it is only right if the map active at that moment was the one the
	   canvas underneath wants - and on the main menu it is not: players join and
	   move their characters there, so the active map is the gameplay one, and
	   the settings screen dutifully saved it and restored it on the way out.
	   The main menu came back with the stick moving a character and the pause
	   button opening the pause menu, over a menu that no longer navigated.

	   Restoring a remembered string cannot be made correct - the canvas that is
	   in front is the only thing that knows what the map should be, so it says
	   so itself rather than trusting what the one leaving put back. */
	InputContextNew* pInput = GetWorld()->GetApplication()->GetPlatform().GetInputContext();

	if (pInput != nullptr)
	{
		/* Re-record first: what is active now is what this canvas should hand
		   back when it is the one going away. */
		const InputBindingMapInformation* pActive = pInput->GetActiveBindingMap(BindingMapType::BMT_PLAYERCONTROLLERS);

		if (pActive != nullptr && pActive->Name != UI_INPUT_LAYER)
			m_PreviousBindingMap = pActive->Name;

		pInput->SetActiveBindingMap(UI_INPUT_LAYER, BindingMapType::BMT_PLAYERCONTROLLERS);
	}

	TraceUI("take-control", nullptr);

	/* --- The focus ----------------------------------------------------------
	   A canvas focuses its default component once, from Start, and being pushed
	   over does not disable it - so nothing re-runs OnEnabled on the way back
	   and whatever state the focus was left in is what it comes back with.

	   ChangeFocus refuses a null argument, so the fallback is picked here. */
	UIComponent* pTarget = m_pFocusedComp != nullptr ? m_pFocusedComp : GetDefaultFocus();

	if (pTarget == nullptr)
		return;

	/* Straight to OnFocus rather than through ChangeFocus: re-focusing what is
	   already focused would otherwise fire OnFocusLost on it first, which plays
	   the navigation sound and reads as a phantom keypress on returning to a
	   menu. */
	m_pFocusedComp = pTarget;
	m_pFocusedComp->OnFocus();
}

bool Canvas::IsInteractive() const
{
	if (!IsEnabled() || !IsNavigatable())
		return false;

	World* pWorld = GetWorld();

	if (pWorld == nullptr)
		return false;

	return pWorld->GetApplication()->GetWorldManager().GetTopWorld() == pWorld;
}

void Canvas::RegisterUIComponent(UIComponent* pUIComponent)
{
	std::vector<UIComponent*>::iterator position = std::find(m_UIComponents.begin(), m_UIComponents.end(), pUIComponent);
	if (position == m_UIComponents.end()) // Only add if is not already in list
		m_UIComponents.push_back(pUIComponent);
}

void Canvas::RemoveUIComponent(UIComponent* pUIComponent)
{
	std::vector<UIComponent*>::iterator position = std::find(m_UIComponents.begin(), m_UIComponents.end(), pUIComponent);
	if (position != m_UIComponents.end()) // Remove the only one that should be in the list.
		m_UIComponents.erase(position);
}


void Canvas::OnEnabled()
{
	Entity::OnEnabled();

	if (!IsNavigatable())
		return;

	m_pPressedComp = nullptr;
	ChangeFocus(GetDefaultFocus());
	
	// Change the binding map to the UI layer.
	m_PreviousBindingMap = GetWorld()->GetApplication()->GetPlatform().GetInputContext()->GetActiveBindingMap(BindingMapType::BMT_PLAYERCONTROLLERS)->Name;
	GetWorld()->GetApplication()->GetPlatform().GetInputContext()->SetActiveBindingMap(UI_INPUT_LAYER, BindingMapType::BMT_PLAYERCONTROLLERS);
}
void Canvas::OnDisabled()
{
	Entity::OnDisabled();

	if (!IsNavigatable())
		return;

	if (m_PreviousBindingMap != "")
	{
		// Change the binding map to the previous layer.
		GetWorld()->GetApplication()->GetPlatform().GetInputContext()->SetActiveBindingMap(m_PreviousBindingMap, BindingMapType::BMT_PLAYERCONTROLLERS);
		m_PreviousBindingMap = "";
	}
}

void Canvas::SetNavigatable(bool bNavigatable)
{
	m_bNavigatable = bNavigatable;

	if (m_bNavigatable)
	{
		if (m_pFocusedComp)
		{
			ChangeFocus(m_pFocusedComp);
		}
		else
		{
			ChangeFocus(GetDefaultFocus());
		}
	}
	else
	{
		ChangeFocus(nullptr);
	}
}

bool Canvas::IsNavigatable() const
{
	return m_bNavigatable;
}


void Canvas::SetFocusPrevious()
{
	if (!IsInteractive())
		return;

	if (m_pFocusedComp)
	{
		UIComponent* nextFocusComp = m_pFocusedComp->GetPreviousComponent();
		while (nextFocusComp && !nextFocusComp->GetIsFocusable())
			nextFocusComp = nextFocusComp->GetPreviousComponent();

		ChangeFocus(nextFocusComp);
	}
}

void Canvas::SetFocusNext()
{
	if (!IsInteractive())
		return;

	if (m_pFocusedComp)
	{
		UIComponent* nextFocusComp = m_pFocusedComp->GetNextComponent();
		while (nextFocusComp && !nextFocusComp->GetIsFocusable())
			nextFocusComp = nextFocusComp->GetNextComponent();

		ChangeFocus(nextFocusComp);
	}
}

void Canvas::SetFocusUp()
{
	if (!IsInteractive())
		return;

	if (m_pFocusedComp)
	{
		UIComponent* nextFocusComp = m_pFocusedComp->GetUpComponent();
		while (nextFocusComp && !nextFocusComp->GetIsFocusable())
			nextFocusComp = nextFocusComp->GetUpComponent();

		ChangeFocus(nextFocusComp);
	}
}

void Canvas::SetFocusDown()
{
	if (!IsInteractive())
		return;

	if (m_pFocusedComp)
	{
		UIComponent* nextFocusComp = m_pFocusedComp->GetDownComponent();
		while (nextFocusComp && !nextFocusComp->GetIsFocusable())
			nextFocusComp = nextFocusComp->GetDownComponent();

		ChangeFocus(nextFocusComp);
	}
}

void Canvas::SetFocusLeft()
{
	if (!IsInteractive())
		return;

	if (m_pFocusedComp)
	{
		UIComponent* nextFocusComp = m_pFocusedComp->GetLeftComponent();
		while (nextFocusComp && !nextFocusComp->GetIsFocusable())
			nextFocusComp = nextFocusComp->GetLeftComponent();

		ChangeFocus(nextFocusComp);
	}
}

void Canvas::SetFocusRight()
{
	if (!IsInteractive())
		return;

	if (m_pFocusedComp)
	{
		UIComponent* nextFocusComp = m_pFocusedComp->GetRightComponent();
		while (nextFocusComp && !nextFocusComp->GetIsFocusable())
			nextFocusComp = nextFocusComp->GetRightComponent();

		ChangeFocus(nextFocusComp);
	}
}

/* --ui-script traces these, because "the menu stopped responding" is three
   different states that look identical in a screenshot: no focused component,
   a focused component on a canvas that is no longer in front, or the wrong
   binding map entirely. See LaunchOptions::GetUIScript. */
void Canvas::TraceUI(const char* pWhat, const char* pDetail) const
{
	if (!LaunchOptions::Get().HasUIScript())
		return;

	const InputContextNew* pInput = m_pInputContext;

	const InputBindingMapInformation* pActive = pInput != nullptr
		? pInput->GetActiveBindingMap(BindingMapType::BMT_PLAYERCONTROLLERS)
		: nullptr;

	printf("[ui]   %-14s canvas='%s' %s map='%s'\n",
	       pWhat,
	       GetName().c_str(),
	       pDetail != nullptr ? pDetail : "",
	       pActive != nullptr ? pActive->Name.c_str() : "<none>");
}

void Canvas::ChangeFocus(UIComponent * pNextFocusComponent)
{
	if (pNextFocusComponent)
	{
		if(m_pFocusedComp)
			m_pFocusedComp->OnFocusLost();

		m_pFocusedComp = pNextFocusComponent;

		if (m_pFocusedComp)
		{
			m_pFocusedComp->OnFocus();
			if (m_pPressedComp)
				m_pFocusedComp->OnPressed();

			TraceUI("focus", m_pFocusedComp->GetOwner()->GetName().c_str());
		}
	}
	else
	{
		m_pPressedComp = nullptr;
	}
}

UIComponent * Canvas::GetDefaultFocus()
{
	// Check which UIComponent has default focus and focus is.
	for (auto& pUIComp : m_UIComponents)
	{
		if (pUIComp->GetDefaultFocus())
		{
			return pUIComp;
		}
	}

	if (m_UIComponents.size() > 0)
		return m_UIComponents[0];
	
	return nullptr;
}




void Canvas::OnPressed()
{
	if (!IsInteractive())
		return;

	if (m_pFocusedComp)
	{
		m_pFocusedComp->OnPressed();
		m_pPressedComp = m_pFocusedComp;
	}
}

void Canvas::OnPressedRepeat()
{
	if (!IsInteractive())
		return;

	if (m_pFocusedComp)
		m_pFocusedComp->OnPressedRepeat();
}

void Canvas::OnReleased()
{
	if (!IsInteractive())
		return;

	if(m_pFocusedComp)
		m_pFocusedComp->OnReleased();

	// Only clicked event if this is the same ui component we pressed at the start.
	if (m_pPressedComp && m_pPressedComp == m_pFocusedComp)
		m_pPressedComp->OnClicked();

	m_pPressedComp = nullptr;
}

