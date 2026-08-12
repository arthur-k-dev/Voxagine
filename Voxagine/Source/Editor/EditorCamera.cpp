#include "pch.h"
#include "EditorCamera.h"

#include "Editor/Editor.h"
#include "Core/Application.h"
#include "Core/ECS/Systems/Physics/HitResult.h"

#include "Core/Platform/Input/SDL/SDLEventInput.h"
#include "Core/Platform/Input/Temp/InputContextNew.h"
#include "Core/ECS/World.h"
#include <Core/Platform/Platform.h>
#include "Core/ECS/Entity.h"
#include "Core/Settings.h"

#include "Editor/EditorWorld.h"
#include "Core/ECS/Systems/Physics/VoxelGrid.h"
#include "Core/ECS/Systems/Chunk/ChunkSystem.h"

/* For the render resolution, which the pan needs to convert pixels of finger
   movement into world units at the focus distance. */
#include "Core/Platform/Rendering/RenderContext.h"

#include <cmath>

EditorCamera::EditorCamera(World * pWorld, Editor* pEditor)
	: Camera(pWorld)
	, m_pEditor(pEditor)
{
}

EditorCamera::~EditorCamera()
{
	GetWorld()->GetApplication()->GetPlatform().GetInputContext()->UnBindAction(m_uilBindIDMouseAction);
}

void EditorCamera::Awake()
{
	Camera::Awake();

	SetName("Editor Camera");

	m_pInputContext = GetWorld()->GetApplication()->GetPlatform().GetInputContext();

	UVector2 chunkWorldSize = m_pEditor->GetEditorWorld()->GetChunkSystem()->GetWorldSize();
	UVector3 worldSize;
	GetWorld()->GetVoxelGrid()->GetDimensions(worldSize.x, worldSize.y, worldSize.z);

	m_fMaxWorldDistance = static_cast<float>(sqrt(chunkWorldSize.x * chunkWorldSize.x + worldSize.y * worldSize.y + chunkWorldSize.y * chunkWorldSize.y));
	std::vector<uint64_t> m_ActionBindings;

	m_pInputContext->BindAction(EDITOR_INPUT_LAYER_NAME, "Mouse_Action", IKS_RELEASED, m_ActionBindings, BMT_GLOBAL, [this]() {
		if (IsEnabled())
		{
			if (!GetEditor()->IsMouseHoveringEditorWindows() && !GetEditor()->IsModifyingSelectedEntityTransform())
			{
				IVector2 MousePosition = GetWorld()->GetApplication()->GetPlatform().GetInputContext()->GetMousePosition();
				Vector3 MouseDir = ScreenToWorld(Vector2(static_cast<float>(MousePosition.x), static_cast<float>(MousePosition.y)));

				HitResult Result;
				if (GetWorld()->RayCast(GetTransform()->GetPosition(), MouseDir, Result, 400.f))
					GetEditor()->SetSelectedEntity(Result.HitEntity);
			}
		}
	});

	m_uilBindIDMouseAction = m_ActionBindings[0];

	ApplyPositionalCorrection();
	Recalculate();
}

void EditorCamera::PreTick()
{
	Camera::PreTick();
}

void EditorCamera::PostTick(float fDelta)
{
	Camera::PostTick(fDelta);
	
	m_AccumulateMouseDelta += m_pInputContext->GetMousePositionDelta();
	m_fAccumulateScrollDelta += m_pInputContext->GetMouseWheelDelta();

	/* Touch gestures accumulate on the same schedule as the mouse, for the
	   same reason: this runs once per frame and PostFixedTick may run any
	   number of times, so a gesture read there directly would be applied twice
	   in one frame or not at all. See SDLEventInput.h for the mapping. */
	const SDLEventInput::Gesture& gesture = SDLEventInput::Get().GetGesture();

	/* Routed by finger count here rather than in PostFixedTick, because the
	   count can change between the two and a drag accumulated as an orbit must
	   not be spent as a pan. */
	if (gesture.Fingers == 2)
		m_AccumulateOrbitDelta += gesture.Drag;
	else if (gesture.Fingers == 3)
		m_AccumulatePanDelta += gesture.Drag;

	/* Multiplicative, because it is a ratio: two frames that each spread the
	   fingers by 10% are a 21% spread, not 20%. */
	m_fAccumulatePinchScale *= gesture.PinchScale;
}

void EditorCamera::PostFixedTick(const GameTimer& timer)
{
	Entity::PostFixedTick(timer);

	GetWorld()->GetChunkSystem()->SetCameraLoadOffset(Vector3(0.f));

	if (m_bFrameLock || GetEditor()->IsModifyingSelectedEntityTransform())
	{
		m_bFrameLock = false;
		Recalculate();
		m_AccumulateMouseDelta = Vector2(0.f);
		m_fAccumulateScrollDelta = 0.f;
		m_AccumulateOrbitDelta = Vector2(0.f);
		m_AccumulatePanDelta = Vector2(0.f);
		m_fAccumulatePinchScale = 1.f;
		return;
	}

	const bool bRightClicked = m_pInputContext->IsMouseButtonDownRight();

	if (!m_bRightClickAction && bRightClicked)
	{
		m_bRightClickActionValid = (!GetEditor()->IsMouseHoveringEditorWindows()) ? true : false;
		m_bRightClickAction = true;
	}
	else if (!bRightClicked)
	{
		m_bRightClickAction = false;
		m_bRightClickActionValid = false;
	}

	if (m_pInputContext->GetActiveBindingMap()->Name == EDITOR_INPUT_LAYER_NAME)
	{
		float deltaTime = static_cast<float>(timer.GetElapsedSeconds());

		/* Get Input Axis values*/
		Vector3 CamerTranslation = Vector3(0.f);

		CamerTranslation.x = m_pInputContext->GetAxisValue("EditorCamera_Left", BMT_GLOBAL).Value + m_pInputContext->GetAxisValue("EditorCamera_Right", BMT_GLOBAL).Value;
		CamerTranslation.z = m_pInputContext->GetAxisValue("EditorCamera_Forward", BMT_GLOBAL).Value + m_pInputContext->GetAxisValue("EditorCamera_Backward", BMT_GLOBAL).Value;

		/* Get input */
		const float fMouseWheelDelta = m_fAccumulateScrollDelta;
		m_fAccumulateScrollDelta = 0.f;

		Vector2 mouseDelta = m_AccumulateMouseDelta;
		m_AccumulateMouseDelta = Vector2(0.f);

		/* Touch, drained here whether or not it is used, so a gesture made
		   over an editor window does not queue up and fire the moment the
		   cursor leaves it. */
		const Vector2 orbitDelta = m_AccumulateOrbitDelta;
		const Vector2 panDelta = m_AccumulatePanDelta;
		const float fPinchScale = m_fAccumulatePinchScale;

		m_AccumulateOrbitDelta = Vector2(0.f);
		m_AccumulatePanDelta = Vector2(0.f);
		m_fAccumulatePinchScale = 1.f;

		/* Same gate the mouse paths use below: gestures over the panels belong
		   to the panels. A two-finger drag there is a scroll (SDLImPlatform),
		   not an orbit. */
		const bool bTouchAllowed =
			!GetEditor()->IsMouseHoveringEditorWindows() &&
			!GetEditor()->IsModifyingSelectedEntityTransform();

		const bool bTouching = bTouchAllowed &&
			(glm::length(orbitDelta) != 0.f || glm::length(panDelta) != 0.f ||
			 fPinchScale != 1.f);

		const bool bShouldUpdate = fMouseWheelDelta != 0 || bTouching || ((bRightClicked) && (glm::length(mouseDelta) != 0 || glm::length(CamerTranslation) != 0));

		if (!bShouldUpdate) {
			Recalculate();
			return;
		}

		if (bTouching)
			ApplyTouchGesture(orbitDelta, panDelta, fPinchScale);

		/* Translate position on mouse scroll */
		if (fMouseWheelDelta != 0 && !GetEditor()->IsMouseHoveringEditorWindows() && !GetEditor()->IsModifyingSelectedEntityTransform())
			/* std::abs, not abs: the unqualified name resolves to the C
			   `int abs(int)` unless something has pulled the float overloads
			   into the global namespace, and this argument is a float. That
			   made the divisor 0 for any scroll delta under 1 - a division by
			   zero producing an infinite camera translation. */
			GetTransform()->Translate(GetTransform()->GetForward() * fMouseWheelDelta / std::abs(fMouseWheelDelta) * deltaTime * m_fScrollSpeed);

		/* Correct rotational speed */
		Vector2 rotation = mouseDelta;
		rotation *= m_fRotationSpeed;

		rotation = glm::clamp(rotation, -Vector2(1.f) * m_fMaxRotationSpeed, Vector2(1.f) * m_fMaxRotationSpeed);
		rotation *= deltaTime;

		if (glm::length(CamerTranslation) != 0 && bRightClicked && m_bRightClickActionValid)
		{
			CamerTranslation = glm::normalize(CamerTranslation);
			Vector3 RotationForward = glm::rotate(GetTransform()->GetRotation(), CamerTranslation);
			//Vector3::Transform(CamerTranslation, GetTransform()->GetRotation());

			GetTransform()->Translate(RotationForward * m_fTranslationSpeed * deltaTime);
		}

		if (bRightClicked && m_bRightClickActionValid)
		{
			GetTransform()->Rotate(Vector3(0, rotation.x, 0));
			GetTransform()->LocalRotate(Vector3(rotation.y, 0, 0));
		}

		ApplyPositionalCorrection();
		Recalculate();
	}
}

float EditorCamera::GetFocusDistance() const
{
	/* How far away the thing being looked at is - which is what a gesture has
	 * to be measured against, because a fixed world-units-per-pixel is exactly
	 * the wrong behaviour at two different scales. Up close it flies past
	 * everything; far out it barely moves, and the camera feels stuck.
	 *
	 * Where the view ray meets the ground is a good enough answer and costs
	 * nothing: this is a voxel world sitting on a plane, and an editor camera
	 * that is clamped above it (ApplyPositionalCorrection). A raycast into the
	 * scene would be more accurate and would also make the zoom speed jump
	 * whenever the ray happened to cross an edge.
	 *
	 * Looking level or upwards there is no intersection in front, so the
	 * camera's own height stands in - it is the only scale available and it is
	 * the right order of magnitude. */
	const Vector3 position = GetTransform()->GetPosition();
	const Vector3 forward = GetTransform()->GetForward();

	float fDistance = position.y;

	if (forward.y < -k_fMinDownwardComponent)
		fDistance = position.y / -forward.y;

	return glm::clamp(fDistance, k_fMinFocusDistance, k_fMaxFocusDistance);
}

void EditorCamera::ApplyTouchGesture(const Vector2& orbitDelta, const Vector2& panDelta,
                                     float fPinchScale)
{
	/* The touch equivalents of what the mouse can do to this camera, and
	 * deliberately not routed through synthetic mouse state to get there.
	 * Faking a held right button would also fake it for the picking raycast,
	 * the gizmo and every ImGui context menu.
	 *
	 * Direct manipulation rather than rates: unlike the mouse paths above,
	 * none of this is multiplied by deltaTime. The world moves with the
	 * fingers and stops when they stop, which is what makes a gesture feel
	 * attached to the screen rather than driven by it. */
	const float fFocusDistance = GetFocusDistance();

	if (orbitDelta != Vector2(0.f))
	{
		/* Degrees per pixel, and deliberately not scaled by distance: turning
		   your head is the same gesture wherever you are standing. */
		const Vector2 rotation = orbitDelta * m_fTouchOrbitSpeed;

		GetTransform()->Rotate(Vector3(0, rotation.x, 0));
		GetTransform()->LocalRotate(Vector3(rotation.y, 0, 0));
	}

	if (panDelta != Vector2(0.f))
	{
		/* One-to-one with the fingers: the point under them should stay under
		   them. A pixel covers (2 * distance * tan(fov/2) / height) world
		   units at the focus distance, which is the exact conversion and the
		   reason this needs the field of view rather than a tuned constant.
		 *
		 * Negated on both axes so the scene follows the fingers: dragging left
		   moves the camera right, which reads as pushing the world left. */
		const float fHalfFov = glm::radians(GetFieldOfView()) * 0.5f;
		const float fViewportHeight =
			static_cast<float>(GetWorld()->GetApplication()->GetPlatform()
				.GetRenderContext()->GetRenderResolution().y);

		const float fWorldPerPixel = fViewportHeight > 0.f
			? (2.f * fFocusDistance * std::tan(fHalfFov)) / fViewportHeight
			: 0.f;

		const Vector3 movement =
			GetTransform()->GetRight() * (-panDelta.x * fWorldPerPixel) +
			GetTransform()->GetUp() * (panDelta.y * fWorldPerPixel);

		GetTransform()->Translate(movement);
	}

	if (fPinchScale != 1.f)
	{
		/* Spreading the fingers by a factor brings the focus point that much
		 * closer - pinch as a ratio, which is what it is everywhere else and
		 * what makes it correct at every distance for free. Doubling the
		 * finger separation halves the distance to what you are looking at,
		 * whether that was two metres or two hundred.
		 *
		 * This is the part a pixels-per-unit zoom cannot do: it has one speed,
		 * so it is either useless far out or uncontrollable up close. */
		const float fClamped = glm::clamp(fPinchScale, k_fMinPinchStep, k_fMaxPinchStep);
		const float fMovement = (fFocusDistance - fFocusDistance / fClamped) * m_fTouchZoomSpeed;

		GetTransform()->Translate(GetTransform()->GetForward() * fMovement);
	}

	ApplyPositionalCorrection();
}

void EditorCamera::LockThisFrame(bool bLock)
{
	m_bFrameLock = true;
}

Editor * EditorCamera::GetEditor()
{
	return m_pEditor;
}

void EditorCamera::ApplyPositionalCorrection()
{
	/* Clamp values */
	Vector3 position = GetTransform()->GetPosition();

	UVector3 worldSize;
	UVector2 chunkWorldSize = m_pEditor->GetEditorWorld()->GetChunkSystem()->GetWorldSize();
	m_pEditor->GetEditorWorld()->GetVoxelGrid()->GetDimensions(worldSize.x, worldSize.y, worldSize.z);
	worldSize.x = chunkWorldSize.x;
	worldSize.y = chunkWorldSize.y;

	Vector3 worldCenter = Vector3(
		worldSize.x * 0.5f,
		worldSize.y * 0.5f,
		worldSize.z * 0.5f
	);

	float distance = glm::distance(
		position,
		worldCenter
	);

	/* Make sure the camera doesn't get too far from the world */
	if (distance > m_fMaxWorldDistance)
	{
		float delta = distance - m_fMaxWorldDistance;

		Vector3 direction = glm::normalize(worldCenter - position);
		GetTransform()->Translate(direction * delta);
	}

	/* Make sure the camera position doesn't get below the world */
	position = GetTransform()->GetPosition();

	if (position.y < 10.0f) {
		position.y = 10.0f;

		GetTransform()->SetPosition(position);
	}
}
