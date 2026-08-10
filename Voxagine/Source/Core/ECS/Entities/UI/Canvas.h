#pragma once

#include "Core/ECS/Entity.h"
#include "Core/Math.h"

#include <rttr/type>

#include <vector>

class Platform;
class UIComponent;
class InputContextNew;

#define UI_INPUT_LAYER "UIInput"

/*!
 * Canvas entity, used as a container for all UI entities and components in the world.
 * 
 */
class Canvas : public Entity
{
	friend class Player;
public:
	Canvas(World* pWorld);
	virtual ~Canvas();

	virtual void Awake() override;
	virtual void Start() override;

	virtual void Tick(float) override;

	void RegisterUIComponent(UIComponent*);
	void RemoveUIComponent(UIComponent*);

	virtual void OnEnabled() override;
	virtual void OnDisabled() override;

	virtual void SetNavigatable(bool);
	bool IsNavigatable() const;

protected:

	/* Whether this canvas should act on input at all.
	 *
	 * Being enabled and navigatable is not enough, and that is the part worth
	 * knowing: a canvas binds its callbacks straight into the input context, so
	 * they keep firing no matter which world is on top. Every menu here uses the
	 * same UI_INPUT_LAYER map, so when one menu world is *pushed* over another -
	 * the pause menu over the game, the graphics settings over the pause menu -
	 * the canvas underneath goes on receiving every press.
	 *
	 * That is not a cosmetic problem. Navigating the settings screen was also
	 * walking the pause menu's focus around behind it, and a confirm was
	 * delivered to both: the press that changed a setting was also pressing
	 * whatever the pause menu happened to have focused, which is how "MAIN MENU"
	 * and "QUIT GAME" end up one keypress away from a settings row.
	 *
	 * World::Pause does not help - it shelves the job queue, it does not disable
	 * entities - so the test has to be which world is on top. */
	bool IsInteractive() const;

	/* Re-assert focus after this canvas's world comes back to the top of the
	   stack - a menu that was pushed over has been popped.
	 *
	 * A canvas focuses its default component once, from Start, and never again:
	 * being pushed over does not disable it, so nothing re-runs OnEnabled when
	 * it comes back. Whatever state the focus was left in is the state it comes
	 * back with, and if anything cleared it in between - or if the component
	 * that was focused belonged to the world that has just been destroyed -
	 * there is nothing selected and no key that will select anything, because
	 * every navigation call starts from m_pFocusedComp.
	 *
	 * So: re-focus the previous component if it is still there, and fall back to
	 * the default if it is not. Both re-fire OnFocus, which is what a row drawn
	 * from a TextRenderer needs in order to look selected again - a sprite
	 * button kept its focused child enabled and never showed the problem. */
	void TakeControl();

private:
	void OnWorldResumed(World*);
	void TraceUI(const char* pWhat, const char* pDetail) const;

protected:


	virtual void SetFocusPrevious();
	virtual void SetFocusNext();

	virtual void SetFocusUp();
	virtual void SetFocusDown();
	virtual void SetFocusLeft();
	virtual void SetFocusRight();


	void ChangeFocus(UIComponent*);
	UIComponent* GetDefaultFocus();


	virtual void OnPressed();
	virtual void OnPressedRepeat();
	virtual void OnReleased();

protected:

	std::vector<uint64_t> m_InputBindings;

private:

	// All registered UI entities
	std::vector<UIComponent*> m_UIComponents;

	bool m_bNavigatable;

	UIComponent* m_pFocusedComp = nullptr;
	UIComponent* m_pPressedComp = nullptr;

	std::string m_PreviousBindingMap;

	// Platform pointer used for data in the UIComponents.
	Platform* m_Platform = nullptr;
	InputContextNew* m_pInputContext = nullptr;

	Vector2 m_PrevInputValue;

	RTTR_ENABLE(Entity);
};