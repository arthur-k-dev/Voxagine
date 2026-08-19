#include "TextMenuButton.h"

#include "Core/ECS/Components/TextRenderer.h"
#include "Core/ECS/Components/UI/UIButton.h"
#include "Core/ECS/Entity.h"
#include "Core/ECS/World.h"

#include <rttr/registration>
#include "Core/MetaData/PropertyTypeMetaData.h"

RTTR_REGISTRATION
{
	rttr::registration::class_<TextMenuButton>("Text Menu Button")
		.constructor<Entity*>()(rttr::policy::ctor::as_raw_ptr)
		.property("Button", &TextMenuButton::m_pButton) (RTTR_PUBLIC)
		.property("Label", &TextMenuButton::m_Label) (RTTR_PUBLIC)
		.property("Normal Color", &TextMenuButton::m_NormalColor) (RTTR_PUBLIC)
		.property("Focused Color", &TextMenuButton::m_FocusedColor) (RTTR_PUBLIC)
		.property("Disabled Color", &TextMenuButton::m_DisabledColor) (RTTR_PUBLIC)
	;
}

TextMenuButton::TextMenuButton(Entity* pEntity)
	: BehaviorScript(pEntity)
{
}

TextMenuButton::~TextMenuButton()
{
}

void TextMenuButton::Start()
{
	BehaviorScript::Start();

	/* The button is normally the one on this entity; the property exists for
	   the case where it is not, and matches what WorldSwitch does. */
	if (m_pButton == nullptr)
		m_pButton = GetOwner()->GetComponent<UIButton>();

	if (m_pButton == nullptr)
		return;

	m_pButton->m_FocusEvent += Event<UIButton*>::Subscriber([this](UIButton*)
	{
		m_bFocused = true;
		Apply();
	}, this);

	m_pButton->m_LostFocusEvent += Event<UIButton*>::Subscriber([this](UIButton*)
	{
		m_bFocused = false;
		Apply();
	}, this);

	/* Read the current state rather than assuming unfocused. Canvas::Start
	   focuses its default component, and component Start order does not
	   guarantee this runs first - so on the pause menu, whose default focus is
	   RESUME, subscribing alone left every row the same colour and the menu came
	   up with nothing selected. A sprite button never noticed because
	   UIButton::SetState had already enabled the right child.

	   There is deliberately no third "pressed" tint: a press here is a frame or
	   two between a focus and a world switch, and a flash of another colour
	   reads as a glitch rather than as feedback. */
	m_bFocused = m_pButton->IsFocused();

	Apply();
}

void TextMenuButton::SetLabel(const std::string& label)
{
	m_Label = label;
	Apply();
}

void TextMenuButton::Apply()
{
	TextRenderer* pText = GetOwner()->GetComponent<TextRenderer>();

	if (pText == nullptr)
		return;

	if (!m_Label.empty())
		pText->SetText(m_Label);

	const bool bFocusable = m_pButton == nullptr || m_pButton->GetIsFocusable();

	pText->SetColor(!bFocusable
		? m_DisabledColor
		: (m_bFocused ? m_FocusedColor : m_NormalColor));
}
