#pragma once

#include "Core/ECS/Entities/UI/Canvas.h"

#include <functional>
#include <string>
#include <vector>

class TextRenderer;
class UIButton;

/* The graphics settings screen - a list of rows, each a named setting and its
 * current value, navigated the way every other menu in this game is: Up/Down to
 * move, confirm to act. Left/Right change the focused row's value, which is the
 * one thing no other menu here does and the reason this is a Canvas subclass
 * rather than a component - Canvas::SetFocusLeft/Right are the virtuals that
 * decide what a horizontal press means, and for a row of options it should mean
 * "next value", not "next row".
 *
 * **It is built in code, out of TextRenderer, and that is a deliberate choice
 * with a real cost.** Every other menu here is authored in the editor as sprite
 * art - three PNGs per button, for its normal, focused and pressed states - and
 * there is no settings art in this project. Text rows need none, read correctly
 * at any resolution, and can name a setting that did not exist when the art was
 * drawn. What they do not do is look like the rest of the game. If art appears
 * later, the row list below is the part worth keeping and the presentation is
 * the part to replace.
 *
 * **Touch does not drive this**, exactly as it does not drive any menu here.
 * There is no hit-testing from a screen point to a UI component anywhere in the
 * engine; the touch controller reports gamepad keys, so a stick and a button
 * work and a tap on a row does not. Docs/MOBILE_PORT_LOG.md has the whole
 * argument and scopes the fix to the touch-native UI initiative rather than to
 * this screen.
 */
class SettingsCanvas : public Canvas
{
public:
	SettingsCanvas(World* pWorld);
	virtual ~SettingsCanvas();

	virtual void Start() override;

protected:
	/* Horizontal input changes the focused row's value rather than moving
	   focus. Nothing on this screen is laid out horizontally, so there is no
	   navigation being taken away. */
	virtual void SetFocusLeft() override;
	virtual void SetFocusRight() override;

private:
	/* One setting. Values are the labels a player reads, and Get/Set index into
	   them - so a row knows nothing about what its setting means, and adding one
	   is adding an entry rather than a case in three switches.

	   A row with no values is an action (the Back row): confirming it runs
	   Activate and Left/Right do nothing. */
	struct Row
	{
		std::string Label;
		std::vector<std::string> Values;

		std::function<int()> Get;
		std::function<void(int)> Set;

		std::function<void()> Activate;

		/* Left/Right and confirm all refuse when this returns false, and the
		   value renders as "Unavailable". For a setting the build has taken
		   away rather than one the player turned off - shadows, when the sun
		   shadow pass was never created. */
		std::function<bool()> IsAvailable;

		TextRenderer* pText = nullptr;
		UIButton* pButton = nullptr;
	};

	void BuildRows();
	void SpawnRow(size_t index);
	void RefreshRow(size_t index);
	void RefreshAll();

	void CycleFocusedRow(int iDelta);

	void Leave();

	std::vector<Row> m_Rows;

	/* Which row the focus is on. Canvas keeps its own focused component and
	   keeps it private, so this tracks it from each button's focus event -
	   which is also the only thing that needs to know, since every row is
	   identical apart from its index. */
	size_t m_uiFocusedRow = 0;

	bool m_bLeaving = false;

	RTTR_ENABLE(Canvas);
};
