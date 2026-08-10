#include "SettingsCanvas.h"

#include "Core/ECS/World.h"
#include "Core/ECS/Entity.h"
#include "Core/ECS/Components/Transform.h"
#include "Core/ECS/Components/TextRenderer.h"
#include "Core/ECS/Components/UI/UIButton.h"

#include "Core/Application.h"
#include "Core/Settings.h"
#include "Core/VColors.h"

#include <rttr/registration>
#include "Core/MetaData/PropertyTypeMetaData.h"

RTTR_REGISTRATION
{
	rttr::registration::class_<SettingsCanvas>("Settings Canvas")
		.constructor<World*>()(rttr::policy::ctor::as_raw_ptr)
	;
}

namespace
{
	/* Layout, in the 1280x720 space TextRenderer's ScalesWithScreen maps from -
	   see RenderSystem's text block. Screen-centred, so a row's position is its
	   offset from the middle of the render target rather than from a corner,
	   which is what keeps the list centred on a 20:9 phone and a 16:9 monitor
	   alike. Y counts upward. */
	const float k_fRowSpacing = 42.f;
	const float k_fFirstRowY = 170.f;
	const float k_fLabelX = -260.f;
	const float k_fValueX = 120.f;
	const float k_fTextScale = 1.4f;

	/* Focused rows are the accent colour, the rest are plain, and a row the
	   build cannot offer is dimmed. Three states is what a text list can carry
	   without becoming a legend nobody reads. */
	const VColor k_Normal = VColors::White;
	const VColor k_Focused = VColors::Gold;
	const VColor k_Unavailable = VColors::Gray;

	const char* k_pOff = "Off";
	const char* k_pOn = "On";
}

SettingsCanvas::SettingsCanvas(World* pWorld)
	: Canvas(pWorld)
{
	SetName("Settings Canvas");
}

SettingsCanvas::~SettingsCanvas()
{
}

void SettingsCanvas::Start()
{
	/* Before Canvas::Start, which is what focuses the default component - the
	   rows have to exist and be registered by then or the screen comes up with
	   nothing focused and no way to focus anything. */
	BuildRows();

	Canvas::Start();

	RefreshAll();
}

/* The row list, which is the actual content of this screen. Each row reads and
 * writes Settings directly: every setter there raises RenderQualityChanged, so
 * the renderer resizes what it has to and the next frame is drawn the new way.
 * There is no apply button and deliberately so - a graphics setting you cannot
 * see the effect of while choosing it is a guess.
 *
 * Order is by what it costs, most expensive first, so the first thing a player
 * reaches on a struggling device is the thing most likely to fix it. */
void SettingsCanvas::BuildRows()
{
	Settings& settings = GetWorld()->GetApplication()->GetSettings();
	Settings* pSettings = &settings;

	{
		Row row;
		row.Label = "SHADOWS";

		/* Cheapest first, which is not the order the enum is in - SHQ_RAY was
		   appended so that a value already written to PlayerPrefs keeps meaning
		   what it meant. The row maps between the two rather than the enum being
		   renumbered to suit a menu. */
		static const ShadowQuality k_Order[] = { SHQ_OFF, SHQ_RAY, SHQ_HARD, SHQ_SOFT };

		row.Values = { k_pOff, "Sharp", "Hard", "Soft" };

		row.Get = [pSettings]()
		{
			const ShadowQuality current = pSettings->GetShadowQuality();

			for (int i = 0; i < 4; ++i)
			{
				if (k_Order[i] == current)
					return i;
			}

			return 0;
		};

		row.Set = [pSettings](int i) { pSettings->SetShadowQuality(k_Order[i]); };

		/* Settings::ShadowsEnabled decides whether the Sun Shadow pass and the
		   shadow-reading voxel shader were built at all, once, at startup.
		   Without them there is no map for any quality to read, so this row has
		   nothing to offer rather than three choices that all mean off. */
		row.IsAvailable = [pSettings]() { return pSettings->IsShadowEnabled(); };

		m_Rows.push_back(std::move(row));
	}

	{
		/* Separate from SHADOWS because it is a separate question: that row
		   decides how the map is filtered, this one decides how much the map can
		   resolve. It is also the only lever on the Sun Shadow pass's cost, which
		   is exactly this number squared. */
		Row row;
		row.Label = "SHADOW DETAIL";
		row.Values = { "Low", "Medium", "High" };

		static const uint32_t k_uiResolutions[] = { 256u, 512u, 1024u };

		row.Get = [pSettings]()
		{
			const uint32_t uiCurrent = pSettings->GetSunShadowResolution();

			int iBest = 0;

			for (int i = 0; i < 3; ++i)
			{
				if (k_uiResolutions[i] <= uiCurrent)
					iBest = i;
			}

			return iBest;
		};

		row.Set = [pSettings](int i) { pSettings->SetSunShadowResolution(k_uiResolutions[i]); };

		row.IsAvailable = [pSettings]() { return pSettings->NeedsSunShadowMap(); };

		m_Rows.push_back(std::move(row));
	}

	{
		/* Only meaningful in Sharp mode - the map-based modes amortise a texel's
		   march over every pixel that lands in it and need no leash. */
		Row row;
		row.Label = "SHADOW DISTANCE";
		row.Values = { "32", "64", "128", "256", "Unlimited" };

		static const float k_fDistances[] = { 32.f, 64.f, 128.f, 256.f, 0.f };

		row.Get = [pSettings]()
		{
			const float fCurrent = pSettings->GetShadowRayDistance();

			if (fCurrent <= 0.f)
				return 4;

			int iBest = 0;

			for (int i = 0; i < 4; ++i)
			{
				if (k_fDistances[i] <= fCurrent)
					iBest = i;
			}

			return iBest;
		};

		row.Set = [pSettings](int i) { pSettings->SetShadowRayDistance(k_fDistances[i]); };

		row.IsAvailable = [pSettings]() { return pSettings->GetShadowQuality() == SHQ_RAY; };

		m_Rows.push_back(std::move(row));
	}

	{
		Row row;
		row.Label = "AMBIENT OCCLUSION";
		row.Values = { k_pOff, "Simple", "Cone" };
		row.Get = [pSettings]() { return static_cast<int>(pSettings->GetAmbientQuality()); };
		row.Set = [pSettings](int i) { pSettings->SetAmbientQuality(static_cast<AmbientQuality>(i)); };

		m_Rows.push_back(std::move(row));
	}

	{
		Row row;
		row.Label = "BOUNCE LIGHT";
		row.Values = { k_pOff, k_pOn };
		row.Get = [pSettings]() { return pSettings->IsBounceLightEnabled() ? 1 : 0; };
		row.Set = [pSettings](int i) { pSettings->SetBounceLight(i != 0); };

		m_Rows.push_back(std::move(row));
	}

	{
		Row row;
		row.Label = "REFLECTIONS";
		row.Values = { k_pOff, k_pOn };
		row.Get = [pSettings]() { return pSettings->IsReflectionEnabled() ? 1 : 0; };
		row.Set = [pSettings](int i) { pSettings->SetReflections(i != 0); };

		m_Rows.push_back(std::move(row));
	}

	{
		Row row;
		row.Label = "ANTI-ALIASING";
		row.Values = { k_pOff, k_pOn };
		row.Get = [pSettings]() { return pSettings->IsFXAAEnabled() ? 1 : 0; };
		row.Set = [pSettings](int i) { pSettings->SetFXAA(i != 0); };

		m_Rows.push_back(std::move(row));
	}

	{
		/* Not a continuous slider. Every step reallocates the Voxel and Particle
		   targets, which idles the device, so a value a stick can scrub through
		   would rebuild attachments tens of times a second. Four steps also
		   means the row reads as a choice rather than as a number to optimise. */
		Row row;
		row.Label = "RESOLUTION";
		/* Below 50% is available but is not any platform's default - see
		   Settings::ApplyPlatformRenderDefaults, which asks for 0.5. The two low
		   steps are there for a device that cannot hold a frame rate any other
		   way, and the point of a settings screen is that the person holding the
		   device gets to decide that rather than the build.

		   They are more usable than they were: post processing point-samples the
		   upscale now, so a low resolution reads as bigger voxels rather than as
		   a blurrier image, which is the right failure mode for this art. */
		row.Values = { "25%", "35%", "40%", "50%", "65%", "80%", "100%" };

		static const float k_fScales[] = { 0.25f, 0.35f, 0.4f, 0.5f, 0.65f, 0.8f, 1.0f };

		row.Get = [pSettings]()
		{
			const float fScale = pSettings->GetResolutionScale();

			int iBest = 0;
			float fBestDistance = 1e9f;

			for (int i = 0; i < static_cast<int>(sizeof(k_fScales) / sizeof(k_fScales[0])); ++i)
			{
				const float fDistance = std::abs(k_fScales[i] - fScale);

				if (fDistance < fBestDistance)
				{
					fBestDistance = fDistance;
					iBest = i;
				}
			}

			return iBest;
		};

		row.Set = [pSettings](int i) { pSettings->SetResolutionScale(k_fScales[i]); };

		m_Rows.push_back(std::move(row));
	}

	{
		Row row;
		row.Label = "V-SYNC";
		row.Values = { k_pOff, k_pOn };
		row.Get = [pSettings]() { return pSettings->IsVSyncEnabled() ? 1 : 0; };
		row.Set = [pSettings](int i) { pSettings->SetVSync(i != 0); };

		m_Rows.push_back(std::move(row));
	}

	{
		Row row;
		row.Label = "BACK";
		row.Activate = [this]() { Leave(); };

		m_Rows.push_back(std::move(row));
	}

	for (size_t i = 0; i < m_Rows.size(); ++i)
		SpawnRow(i);

	/* Vertical navigation, wrapping at both ends: the list is short enough that
	   holding down past the last row and arriving back at the first reads as
	   convenience rather than as a lost position. */
	for (size_t i = 0; i < m_Rows.size(); ++i)
	{
		UIButton* pButton = m_Rows[i].pButton;

		if (pButton == nullptr)
			continue;

		const size_t uiUp = (i + m_Rows.size() - 1) % m_Rows.size();
		const size_t uiDown = (i + 1) % m_Rows.size();

		pButton->SetUpComponent(m_Rows[uiUp].pButton);
		pButton->SetDownComponent(m_Rows[uiDown].pButton);

		/* Tab/shoulder navigation follows the same order, so the two ways of
		   moving through a menu do not disagree. */
		pButton->SetPreviousComponent(m_Rows[uiUp].pButton);
		pButton->SetNextComponent(m_Rows[uiDown].pButton);
	}

	if (!m_Rows.empty() && m_Rows[0].pButton != nullptr)
		m_Rows[0].pButton->SetDefaultFocus(true);
}

/* One row: an entity parented to this canvas carrying the label, plus a second
 * carrying the value. Two TextRenderers rather than one string, because they are
 * at fixed columns and a single centred string would shuffle sideways every time
 * a value changed length.
 *
 * The UIButton has no Normal/Focused/Pressed objects. It is here for the focus
 * machinery only - Canvas navigates UIComponents, and this is the one it
 * navigates - and UIButton::SetState null-checks all four, so a button with no
 * art is a supported thing rather than something being got away with.
 */
void SettingsCanvas::SpawnRow(size_t index)
{
	Row& row = m_Rows[index];

	const float fY = k_fFirstRowY - static_cast<float>(index) * k_fRowSpacing;

	Entity* pLabel = GetWorld()->SpawnEntity<Entity>(
		Vector3(k_fLabelX, fY, 0.f), Vector3(0.f), Vector3(1.f));

	pLabel->SetName("Setting " + row.Label);
	pLabel->SetParent(this);

	TextRenderer* pLabelText = pLabel->AddComponent<TextRenderer>();
	pLabelText->SetText(row.Label);
	pLabelText->SetScaleWithScreen(true);
	pLabelText->SetScale(k_fTextScale);
	pLabelText->SetWrapping(false);
	pLabelText->SetAlignment(RA_LEFTCENTER);
	pLabelText->SetScreenAlignment(RA_CENTERED);

	/* The value column only exists for rows that have one; BACK is a label and
	   an action. */
	if (!row.Values.empty())
	{
		Entity* pValue = GetWorld()->SpawnEntity<Entity>(
			Vector3(k_fValueX, fY, 0.f), Vector3(0.f), Vector3(1.f));

		pValue->SetName("Value " + row.Label);
		pValue->SetParent(this);

		row.pText = pValue->AddComponent<TextRenderer>();
		row.pText->SetScaleWithScreen(true);
		row.pText->SetScale(k_fTextScale);
		row.pText->SetWrapping(false);
		row.pText->SetAlignment(RA_LEFTCENTER);
		row.pText->SetScreenAlignment(RA_CENTERED);
	}

	row.pButton = pLabel->AddComponent<UIButton>();

	/* Runtime-added components do not necessarily get the Awake that registers
	   them, and the registration walks up to the owning Canvas - which is why
	   SetParent happens above rather than after. Calling it directly is
	   idempotent: RegisterUIComponent refuses a duplicate. */
	row.pButton->RegisterToCanvas();

	row.pButton->m_FocusEvent += Event<UIButton*>::Subscriber([this, index](UIButton*)
	{
		m_uiFocusedRow = index;
		RefreshAll();
	}, this);

	row.pButton->m_LostFocusEvent += Event<UIButton*>::Subscriber([this, index](UIButton*)
	{
		RefreshRow(index);
	}, this);

	/* Confirm cycles a value row forward, which is what makes this usable with a
	   pad that has a stick and one button - the same gesture that picks an item
	   in every other menu here changes a setting, and Left is how you go back a
	   step. On an action row it runs the action. */
	row.pButton->m_ClickedEvent += Event<UIButton*>::Subscriber([this, index](UIButton*)
	{
		Row& clicked = m_Rows[index];

		if (clicked.Activate)
		{
			clicked.Activate();
			return;
		}

		m_uiFocusedRow = index;
		CycleFocusedRow(1);
	}, this);
}

void SettingsCanvas::RefreshRow(size_t index)
{
	if (index >= m_Rows.size())
		return;

	Row& row = m_Rows[index];

	const bool bAvailable = !row.IsAvailable || row.IsAvailable();
	const bool bFocused = (index == m_uiFocusedRow);

	const VColor color = !bAvailable
		? k_Unavailable
		: (bFocused ? k_Focused : k_Normal);

	if (row.pText != nullptr)
	{
		if (!bAvailable)
		{
			row.pText->SetText("Unavailable");
		}
		else
		{
			const int iValue = row.Get ? row.Get() : 0;

			/* Clamped rather than trusted. Settings can hold a value this row
			   has no label for - a PlayerPrefs file written by a build with more
			   options in it, or a hand-edited one - and an unclamped index here
			   is an out-of-bounds read on player data. */
			const size_t uiValue = static_cast<size_t>(
				std::max(0, std::min(iValue, static_cast<int>(row.Values.size()) - 1)));

			/* The arrows say the row is horizontally adjustable, which nothing
			   else on screen would. */
			row.pText->SetText("< " + row.Values[uiValue] + " >");
		}

		row.pText->SetColor(color);
	}

	/* The label carries the focus colour too, so a row reads as selected across
	   its whole width rather than only where its value happens to be. */
	if (row.pButton != nullptr)
	{
		TextRenderer* pLabelText = row.pButton->GetOwner()->GetComponent<TextRenderer>();

		if (pLabelText != nullptr)
			pLabelText->SetColor(color);
	}
}

void SettingsCanvas::RefreshAll()
{
	for (size_t i = 0; i < m_Rows.size(); ++i)
		RefreshRow(i);
}

void SettingsCanvas::SetFocusLeft()
{
	CycleFocusedRow(-1);
}

void SettingsCanvas::SetFocusRight()
{
	CycleFocusedRow(1);
}

/* Move the focused row's value, wrapping. Every setter it reaches raises
 * Settings::RenderQualityChanged, and the renderer answers that by reallocating
 * targets - so this is a call that can idle the GPU, and it is deliberately
 * driven only by a discrete key or a stick crossing its threshold rather than by
 * anything continuous. */
void SettingsCanvas::CycleFocusedRow(int iDelta)
{
	/* Same test the base uses for navigation - see Canvas::IsInteractive. This
	   screen is itself pushed over another menu, so it will one day be the one
	   underneath. */
	if (!IsInteractive() || m_bLeaving)
		return;

	if (m_uiFocusedRow >= m_Rows.size())
		return;

	Row& row = m_Rows[m_uiFocusedRow];

	if (row.Values.empty() || !row.Get || !row.Set)
		return;

	if (row.IsAvailable && !row.IsAvailable())
		return;

	const int iCount = static_cast<int>(row.Values.size());
	const int iCurrent = std::max(0, std::min(row.Get(), iCount - 1));

	row.Set(((iCurrent + iDelta) % iCount + iCount) % iCount);

	RefreshRow(m_uiFocusedRow);
}

/* Write what the player chose and pop back to whichever menu pushed this world.
 *
 * Saving here rather than on every change is the point: a player holding Right
 * through four values should not write a file four times, and there is exactly
 * one way off this screen. The settings themselves are already live - they took
 * effect the moment they changed - so this persists a decision that has already
 * been made rather than applying one. */
void SettingsCanvas::Leave()
{
	if (m_bLeaving)
		return;

	m_bLeaving = true;

	GetWorld()->GetApplication()->SaveRenderSettings();
	GetWorld()->GetApplication()->GetWorldManager().PopWorld();
}
