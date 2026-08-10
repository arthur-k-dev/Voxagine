#pragma once

namespace EditorLayout
{
	/* Logical sizes authored for a 1x display. Editor panels convert these to
	   framebuffer pixels with Editor::GetUiScale(). */
	constexpr float SidePanelWidth = 290.0f;
	constexpr float ConsoleHeight = 250.0f;
	constexpr float MenuBarHeight = 27.0f;
	constexpr float MenuBarBackgroundHeight = 26.0f;
	constexpr float ToolbarButtonSize = 26.0f;
}
