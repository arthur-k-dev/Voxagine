#pragma once

#include "Core/ECS/Components/BehaviorScript.h"

#include "Core/VColors.h"

class UIButton;
class TextRenderer;

/* Gives a `UIButton` its look, out of a `TextRenderer` rather than out of three
 * authored sprites.
 *
 * **This replaces the sprite buttons rather than supplementing them.** Every
 * menu in this game used to be built from a PNG per state - `Resume_Default`,
 * `Resume_Focussed`, `Resume_Pressed` - which means four entities and three
 * image files per button, a new set of art for every new option, and a label
 * that cannot change at runtime. It also meant a button *without* art focused
 * silently, because `UIButton::SetState` null-checks all four objects: the
 * first text row added to the pause menu made the highlight disappear when the
 * focus reached it, which read as the menu having stopped responding.
 *
 * One component, three colours and a string is the whole of it. A new menu
 * option costs an entity.
 *
 * The colours are properties so a screen can tint its own list, and the
 * defaults are the pair `SettingsCanvas` already uses - white normal, gold
 * focused - so every menu agrees on what selected looks like.
 */
class TextMenuButton : public BehaviorScript
{
public:
	TextMenuButton(Entity* pEntity);
	virtual ~TextMenuButton();

	virtual void Start() override;

	/* Set from outside for a row whose label changes - SettingsCanvas builds
	   its own rows in code and does not use this, but a level-select entry
	   would. */
	void SetLabel(const std::string& label);

	UIButton* m_pButton = nullptr;

private:
	void Apply();

	/* Written into the TextRenderer at Start, so the label lives on the button
	   rather than on a separate text object nobody can find. Empty leaves
	   whatever the TextRenderer already had, which is how a row that sets its
	   own text keeps it. */
	std::string m_Label;

	VColor m_NormalColor = VColors::White;
	VColor m_FocusedColor = VColors::Gold;
	VColor m_DisabledColor = VColors::Gray;

	bool m_bFocused = false;

	RTTR_ENABLE(BehaviorScript);
	RTTR_REGISTRATION_FRIEND
};
