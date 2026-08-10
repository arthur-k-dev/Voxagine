#pragma once

#include "Core/ECS/Components/BehaviorScript.h"

#include <string>

class UIButton;

/* Pushes the graphics settings world when its button is confirmed, and that is
 * all it does.
 *
 * **Push, not switch.** WorldSwitch - what every other menu button here uses -
 * calls World::OpenWorld, which *replaces* the world. That is right for going
 * from the main menu to level select and wrong for a settings screen, which has
 * to come back to whichever menu opened it: from the main menu that would be
 * recoverable, but the pause menu is itself pushed on top of a live game world,
 * and replacing it would throw the game away. `OpenWorld(path, false)` pushes,
 * exactly as Player.cpp opens the pause menu, and SettingsCanvas::Leave pops.
 *
 * There is no fade. WorldSwitch fades because it is tearing down a world and
 * loading a level; this puts a menu over a menu, and a fade to black between two
 * screens of the same menu reads as a load rather than as a step.
 */
class SettingsScreenButton : public BehaviorScript
{
public:
	SettingsScreenButton(Entity* pEntity);
	virtual ~SettingsScreenButton();

	virtual void Start() override;

	UIButton* m_pButton = nullptr;

private:
	std::string m_sSettingsWorld = "Content/Worlds/Menus/Settings_Menu.wld";

	RTTR_ENABLE(BehaviorScript);
	RTTR_REGISTRATION_FRIEND
};
