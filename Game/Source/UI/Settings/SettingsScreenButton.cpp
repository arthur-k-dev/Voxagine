#include "SettingsScreenButton.h"

#include "Core/ECS/Components/UI/UIButton.h"
#include "Core/ECS/Entity.h"
#include "Core/ECS/World.h"

#include <rttr/registration>
#include "Core/MetaData/PropertyTypeMetaData.h"

RTTR_REGISTRATION
{
	rttr::registration::class_<SettingsScreenButton>("Settings Screen Button")
		.constructor<Entity*>()(rttr::policy::ctor::as_raw_ptr)
		.property("Button", &SettingsScreenButton::m_pButton) (RTTR_PUBLIC)
		.property("Settings World", &SettingsScreenButton::m_sSettingsWorld) (RTTR_PUBLIC, RTTR_RESOURCE("wld"))
	;
}

SettingsScreenButton::SettingsScreenButton(Entity* pEntity)
	: BehaviorScript(pEntity)
{
}

SettingsScreenButton::~SettingsScreenButton()
{
}

void SettingsScreenButton::Start()
{
	BehaviorScript::Start();

	/* Same fallback WorldSwitch uses: the button is usually the one on this
	   entity, and making that the default is one fewer reference to wire up in
	   a world file by hand. */
	if (m_pButton == nullptr)
		m_pButton = GetOwner()->GetComponent<UIButton>();

	if (m_pButton == nullptr)
		return;

	m_pButton->m_ClickedEvent += Event<UIButton*>::Subscriber([this](UIButton*)
	{
		GetWorld()->OpenWorld(m_sSettingsWorld, false);
	}, this);
}

