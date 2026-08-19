#pragma once

#include "Core/ECS/Component.h"

#include <rttr/type>

enum class EUIState {
	DEFAULT,
	HOVERED,
	PRESSED,
	DISABLED
};

class Canvas;

class UIComponent : public Component
{
	friend class Canvas;

public:

	UIComponent(Entity* pOwner);
	virtual ~UIComponent();

	virtual void Awake() override;
	virtual void Start() override;

	void RegisterToCanvas();
	Canvas* GetCanvas() { return m_Canvas; };

	virtual void SetIsFocusable(bool);
	bool GetIsFocusable() const;

	void SetDefaultFocus(bool);
	bool GetDefaultFocus() const;

	/* Whether the canvas currently has focus on this component.
	 *
	 * It exists because the focus events are not enough on their own: a canvas
	 * focuses its default component from Canvas::Start, and a component whose
	 * own Start runs later has already missed that first OnFocus. Anything that
	 * draws focus rather than swapping pre-authored objects - a text row, say -
	 * has to be able to ask. */
	bool IsFocused() const { return m_Focused; }

	void SetPreviousComponent(UIComponent*);
	UIComponent* GetPreviousComponent() const;
	void SetNextComponent(UIComponent*);
	UIComponent* GetNextComponent() const;
	void SetUpComponent(UIComponent*);
	UIComponent* GetUpComponent() const;
	void SetDownComponent(UIComponent*);
	UIComponent* GetDownComponent() const;
	void SetLeftComponent(UIComponent*);
	UIComponent* GetLeftComponent() const;
	void SetRightComponent(UIComponent*);
	UIComponent* GetRightComponent() const;
	
protected:

	bool m_IsFocusable = true;
	bool m_DefaultFocus = false;

	bool m_Focused = false;

	virtual void OnFocus() { m_Focused = true; };
	virtual void OnFocusLost() { m_Focused = false; };
	virtual void OnPressed() {};
	virtual void OnPressedRepeat() {};
	virtual void OnReleased() {};
	virtual void OnClicked() {};

	Canvas* m_Canvas = nullptr;

	UIComponent* m_PreviousComponent = nullptr;
	UIComponent* m_NextComponent = nullptr;
	UIComponent* m_UpComponent = nullptr;
	UIComponent* m_DownComponent = nullptr;
	UIComponent* m_LeftComponent = nullptr;
	UIComponent* m_RightComponent = nullptr;

	bool m_bAwoken = false
		, m_bStarted = false;
	
	RTTR_ENABLE(Component)
};