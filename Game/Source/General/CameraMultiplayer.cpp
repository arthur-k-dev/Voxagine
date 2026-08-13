#include "pch.h"
#include "CameraMultiplayer.h"

#include <stdlib.h>
#include <rttr/registration.h>
#include <Core/MetaData/PropertyTypeMetaData.h>
#include <Core/ECS/Entities/Camera.h>
#include <Core/ECS/Components/Transform.h>
#include <Core/Application.h>
#include <Core/ECS/World.h>
#include <Core/Platform/Window/WindowContext.h>
#include <Core/Platform/Platform.h>
#include "Core/ECS/Entity.h"
#include "Core/ECS/Systems/Chunk/ChunkSystem.h"
#include "Humanoids/Players/Player.h"

RTTR_REGISTRATION
{
	rttr::registration::class_<CameraMultiplayer>("CameraMultiplayer")
	.constructor<World*>()(rttr::policy::ctor::as_raw_ptr)
	.property("Target Rotation", &CameraMultiplayer::m_targetRotation)(RTTR_PUBLIC)
	.property("Min Target Distance", &CameraMultiplayer::m_fMinTargetDistance)(RTTR_PUBLIC)
	.property("Max Target Distance", &CameraMultiplayer::m_fMaxTargetDistance)(RTTR_PUBLIC)
	.property("Player 1", &CameraMultiplayer::GetPlayer1, &CameraMultiplayer::SetPlayer1)(RTTR_PUBLIC)
	.property("Player 2", &CameraMultiplayer::GetPlayer2, &CameraMultiplayer::SetPlayer2)(RTTR_PUBLIC)
	.property("Zoom Out Bound", &CameraMultiplayer::m_fZoomOutBound)(RTTR_PUBLIC)
	.property("Zoom In Bound", &CameraMultiplayer::m_fZoomInBound)(RTTR_PUBLIC)
	.property("Movement Smoothing", &CameraMultiplayer::m_fMovementSmoothing)(RTTR_PUBLIC)
	.property("Zoom Speed", &CameraMultiplayer::m_fZoomSpeed)(RTTR_PUBLIC)
	.property("Player Movement Bound", &CameraMultiplayer::m_fPlayerMovementBound)(RTTR_PUBLIC)
	.property("Player Movement Bound Offset", &CameraMultiplayer::m_fPlayerBoundOffset)(RTTR_PUBLIC)
	.property("DEBUG Camera Shake", &CameraMultiplayer::m_fShakeStrength)(RTTR_PUBLIC)
	.property("Max Camera Shake Angle", &CameraMultiplayer::m_fMaxShakeAngle)(RTTR_PUBLIC)
	.property("Camera Shake Falloff", &CameraMultiplayer::m_fShakeFalloff)(RTTR_PUBLIC)
	.property("Chunk Load Offset", &CameraMultiplayer::m_ChunkLoadOffset)(RTTR_PUBLIC);
}

CameraMultiplayer::CameraMultiplayer(World * pWorld) :
	Entity(pWorld),
	m_targetRotation(45, 0, 0),
	m_fMinTargetDistance(175),
	m_fMaxTargetDistance(175),
	m_pPlayer1(nullptr),
	m_pPlayer2(nullptr),
	m_fMovementSmoothing(0.1f),
	m_fZoomOutBound(0.1f),
	m_fZoomInBound(0.2f),
	m_fZoomSpeed(1.5f),
	m_fPlayerMovementBound(0.05f, 0.2f),
	m_fPlayerBoundOffset(0),
	m_fMaxShakeAngle(10.f),
	m_fShakeFalloff(1.f),
	m_ChunkLoadOffset(0, 0, 64),
	m_pMainCamera(nullptr),
	m_updateCamera(true),
	m_targetPosition(0),
	m_fTargetDistance(m_fMinTargetDistance),
	m_player1PrevPosition(-1),
	m_player2PrevPosition(-1),
	m_fShakeStrength(0.f)
{
	SetName("CameraMultiplayerController");

	/* A two-player camera frames players 0 and 1. Levels that assign the links
	   explicitly override the index through PlayerSlot::Adopt. */
	m_Player1Slot.SetIndex(0);
	m_Player2Slot.SetIndex(1);
}

/* The two setters are the *authoring* path - the serialized "Player 1"/"Player 2"
   link and the editor's inspector. They are no longer the mechanism: whatever
   they set, ResolvePlayers re-finds by index if it is ever lost, and it attaches
   the players with no help from them at all if the link never resolves. That is
   the whole point, since measurably it often does not. */
void CameraMultiplayer::SetPlayer1(Entity* pEntity)
{
	if (pEntity == nullptr) return;

	m_Player1Slot.Adopt(pEntity);
	m_pPlayer1 = pEntity;

	pEntity->Destroyed += Event<Entity*>::Subscriber([this](Entity*) {
		/* Detach, do not give up. The next tick re-resolves by index, so a
		   player destroyed by a chunk unload re-attaches when it comes back. */
		m_Player1Slot.Detach();
		m_pPlayer1 = nullptr;
	}, this);
}

void CameraMultiplayer::SetPlayer2(Entity* pEntity)
{
	if (pEntity == nullptr) return;

	m_Player2Slot.Adopt(pEntity);
	m_pPlayer2 = pEntity;

	pEntity->Destroyed += Event<Entity*>::Subscriber([this](Entity*) {
		m_Player2Slot.Detach();
		m_pPlayer2 = nullptr;
	}, this);
}

void CameraMultiplayer::ResolvePlayers()
{
	if (m_pPlayer1 == nullptr)
	{
		if (Player* pPlayer = m_Player1Slot.Resolve(GetWorld()))
		{
			SetPlayer1(pPlayer);

			/* **Seed the recorded position on attach, not just in Start.**
			   m_player1PrevPosition starts at (-1, -1, -1) and lockPlayersInView
			   writes it *back onto the player* when the player is outside the
			   screen bounds - so a player that attached after Start, which is
			   now the normal case, could be teleported to x = -1 on its first
			   frame off-centre. Start seeded this only because it assumed the
			   players were already here. */
			m_player1PrevPosition = pPlayer->GetTransform()->GetPosition();
		}
	}

	if (m_pPlayer2 == nullptr)
	{
		if (Player* pPlayer = m_Player2Slot.Resolve(GetWorld()))
		{
			SetPlayer2(pPlayer);
			m_player2PrevPosition = pPlayer->GetTransform()->GetPosition();
		}
	}
}

void CameraMultiplayer::Awake()
{
	Entity::Awake();
	SetName("CameraMultiplayerController");
	if (m_fZoomOutBound > m_fZoomInBound)
		m_fZoomOutBound = m_fZoomInBound;
}

void CameraMultiplayer::Start()
{
	Entity::Start();
	m_fPlayerMovementBound = Vector2(0.05f, 0.2f);
	m_fMaxShakeAngle = Vector3(0.9f);
	m_fShakeFalloff = 0.8f;

	/* Start no longer *depends* on the players being here - it runs on the
	   entity's first Tick, which under streaming is routinely before the
	   players' chunk has admitted anything. It takes them if they are available
	   and the per-tick resolution picks them up if they are not. */
	ResolvePlayers();

	if (m_pPlayer1 != nullptr) m_player1PrevPosition = m_pPlayer1->GetTransform()->GetPosition();
	if (m_pPlayer2 != nullptr) m_player2PrevPosition = m_pPlayer2->GetTransform()->GetPosition();

	m_pMainCamera = GetWorld()->GetMainCamera();
	m_pMainCamera->Destroyed += Event<Entity*>::Subscriber([this](Entity* pEntity) 
	{
		m_pMainCamera = nullptr;
	}, this);

	// Center camera position between players
	Vector2 screenPosPlayer1 = Vector2(0);
	Vector2 screenPosPlayer2 = Vector2(0);
	calculatePlayersScreenPos(screenPosPlayer1, screenPosPlayer2);

	// Calculate camera position
	Vector3 lookAtPos = calculateLookAtPos();
	m_fTargetDistance = calculateTargetDistance(screenPosPlayer1, screenPosPlayer2);
	float horzDistance = m_fTargetDistance * cos(DEG2RAD * m_targetRotation.x);
	float vertDistance = m_fTargetDistance * sin(DEG2RAD * m_targetRotation.x);
	m_targetPosition.y = vertDistance;
	m_targetPosition.x = lookAtPos.x - horzDistance * sin(DEG2RAD * m_targetRotation.y);
	m_targetPosition.z = lookAtPos.z - horzDistance * cos(DEG2RAD * m_targetRotation.y);

	m_pMainCamera->GetTransform()->SetPosition(m_targetPosition);
}

void CameraMultiplayer::PreTick()
{
	Entity::PreTick();

	ResolvePlayers();

	// Get player positions
	Vector3 player1Position = Vector3(-1);
	Vector3 player2Position = Vector3(-1);
	if (m_pPlayer1 != nullptr) player1Position = m_pPlayer1->GetTransform()->GetPosition();
	if (m_pPlayer2 != nullptr) player2Position = m_pPlayer2->GetTransform()->GetPosition();

	// Calculate screen positions
	Vector2 screenPosPlayer1 = Vector2(0);
	Vector2 screenPosPlayer2 = Vector2(0);
	calculatePlayersScreenPos(screenPosPlayer1, screenPosPlayer2);

	// Get screen bounds
	const UVector2& windowsize = GetWorld()->GetApplication()->GetPlatform().GetWindowContext()->GetSize();
	const Vector2 movementMinBound = m_fPlayerMovementBound  * Vector2(windowsize);
	const Vector2 movementMaxBound = Vector2(windowsize) - m_fPlayerMovementBound  * Vector2(windowsize);

	// If the player is on screen, recrod it's position
	if (screenPosPlayer1.x + m_fPlayerBoundOffset.x >= movementMinBound.x && 
		screenPosPlayer1.x + m_fPlayerBoundOffset.x <= movementMaxBound.x) 
		m_player1PrevPosition.x = player1Position.x;
	if (screenPosPlayer1.y + m_fPlayerBoundOffset.y >= movementMinBound.y && 
		screenPosPlayer1.y + m_fPlayerBoundOffset.y <= movementMaxBound.y) 
		m_player1PrevPosition.z = player1Position.z;

	if (screenPosPlayer2.x + m_fPlayerBoundOffset.x >= movementMinBound.x && 
		screenPosPlayer2.x + m_fPlayerBoundOffset.x <= movementMaxBound.x) 
		m_player2PrevPosition.x = player2Position.x;
	if (screenPosPlayer2.y + m_fPlayerBoundOffset.y >= movementMinBound.y &&
		screenPosPlayer2.y + m_fPlayerBoundOffset.y <= movementMaxBound.y) 
		m_player2PrevPosition.z = player2Position.z;
}

void CameraMultiplayer::PostFixedTick(const GameTimer& timer)
{
	Entity::PostFixedTick(timer);

	ResolvePlayers();

	/* Re-acquire rather than give up for good. m_pMainCamera is captured once
	   in Start(), so anything that runs Start() before the world has a main
	   camera - or that destroys the one it found - disables this controller
	   permanently and silently. Belt to World::Initialize's braces: that stops
	   the camera being destroyed, this stops a single unlucky ordering being
	   fatal. */
	if (m_pMainCamera == nullptr)
	{
		m_pMainCamera = GetWorld()->GetMainCamera();

		if (m_pMainCamera == nullptr)
			return;

		m_pMainCamera->Destroyed += Event<Entity*>::Subscriber([this](Entity*)
		{
			m_pMainCamera = nullptr;
		}, this);
	}
	Vector3 currentRotation = m_pMainCamera->GetTransform()->GetEulerAngles();
	Vector3 currentPosition = m_pMainCamera->GetTransform()->GetPosition();

	// Update chunk camera load offset
	Vector3 cameraLoadOffset(0.f);
	calculateChunkLoadOffset(cameraLoadOffset);
	GetWorld()->GetChunkSystem()->SetCameraLoadOffset(cameraLoadOffset);

	// Update camera shake
	if (m_fShakeStrength > 0.f)
		m_fShakeStrength = std::max(0.f, m_fShakeStrength - m_fShakeFalloff * (float)timer.GetElapsedSeconds());

	// Center camera position between players
	Vector2 screenPosPlayer1 = Vector2(0);
	Vector2 screenPosPlayer2 = Vector2(0);
	calculatePlayersScreenPos(screenPosPlayer1, screenPosPlayer2);

	// Lock players in view
	lockPlayersInView(screenPosPlayer1, screenPosPlayer2);

	// Calculate camera position
	Vector3 lookAtPos = calculateLookAtPos();
	m_fTargetDistance = calculateTargetDistance(screenPosPlayer1, screenPosPlayer2);
	float horzDistance = m_fTargetDistance * cos(DEG2RAD * m_targetRotation.x);
	float vertDistance = m_fTargetDistance * sin(DEG2RAD * m_targetRotation.x);
	m_targetPosition.y = vertDistance;
	m_targetPosition.x = lookAtPos.x - horzDistance * sin(DEG2RAD * m_targetRotation.y);
	m_targetPosition.z = lookAtPos.z - horzDistance * cos(DEG2RAD * m_targetRotation.y);

	// Update camera only if needed
	if (
		glm::distance(m_targetPosition, currentPosition) > 5.f ||
		m_targetRotation != currentRotation || m_fShakeStrength != 0.f
	) m_updateCamera = true;

	if (m_updateCamera)
	{
		float xShake = m_fMaxShakeAngle.x * m_fShakeStrength * m_fShakeStrength * (-1.f + (float)rand() / (RAND_MAX / 2.f));
		float yShake = m_fMaxShakeAngle.y * m_fShakeStrength * m_fShakeStrength * (-1.f + (float)rand() / (RAND_MAX / 2.f));
		float zShake = m_fMaxShakeAngle.z * m_fShakeStrength * m_fShakeStrength * (-1.f + (float)rand() / (RAND_MAX / 2.f));
		m_pMainCamera->GetTransform()->SetEulerAngles(m_targetRotation + Vector3(xShake, yShake, zShake));

		// Interpolate between the current and target position based on smoothing and distance
		float smoothFactor = std::max(0.f, m_fMovementSmoothing * glm::distance(m_targetPosition, currentPosition));
		Vector3 smoothedPosition = (m_targetPosition - currentPosition) * smoothFactor * (float)timer.GetElapsedSeconds();
		m_pMainCamera->GetTransform()->SetPosition(currentPosition + smoothedPosition);

		m_pMainCamera->Recalculate();
		m_updateCamera = false;
	}
}

void CameraMultiplayer::AddCameraShake(float strength)
{
	m_fShakeStrength += strength;
}

void CameraMultiplayer::SetCameraShake(float strength)
{
	m_fShakeStrength = strength;
}

float CameraMultiplayer::GetCameraShake() const
{
	return m_fShakeStrength;
}

void CameraMultiplayer::calculatePlayersScreenPos(Vector2& screenPosPlayer1, Vector2& screenPosPlayer2) const
{
	const UVector2& windowsize = GetWorld()->GetApplication()->GetPlatform().GetWindowContext()->GetSize();

	screenPosPlayer1 = Vector2(windowsize) / 2.f;
	if (m_pPlayer1 != nullptr)
	{
		Transform* transform = m_pPlayer1->GetTransform();
		if (transform != nullptr && m_pMainCamera != nullptr)
			screenPosPlayer1 = m_pMainCamera->WorldToScreenPoint(transform->GetPosition());
	}

	screenPosPlayer2 = Vector2(windowsize) / 2.f;
	if (m_pPlayer2 != nullptr)
	{
		Transform* transform = m_pPlayer2->GetTransform();
		if (transform != nullptr && m_pMainCamera != nullptr)
			screenPosPlayer2 = m_pMainCamera->WorldToScreenPoint(transform->GetPosition());
	}
}

Vector3 CameraMultiplayer::calculateLookAtPos() const
{
	// Center camera position between players
	Vector3 lookAtPos = Vector3(0);

	if (m_pPlayer1 != nullptr)
		lookAtPos += m_pPlayer1->GetTransform()->GetPosition();

	if (m_pPlayer2 != nullptr)
		lookAtPos += m_pPlayer2->GetTransform()->GetPosition();

	if (m_pPlayer1 != nullptr && m_pPlayer2 != nullptr)
		lookAtPos /= 2.f;

	return lookAtPos;
}

float CameraMultiplayer::calculateTargetDistance(const Vector2& screenPosPlayer1, const Vector2& screenPosPlayer2) const
{
	const UVector2& windowsize = GetWorld()->GetApplication()->GetPlatform().GetWindowContext()->GetSize();
	float distance = m_fTargetDistance;

	// Zoom out
	const Vector2 zoomOutMinBound = m_fZoomOutBound * Vector2(windowsize);
	const Vector2 zoomOutMaxBound = Vector2(windowsize) - m_fZoomOutBound * Vector2(windowsize);
	if (screenPosPlayer1.x < zoomOutMinBound.x || screenPosPlayer1.y < zoomOutMinBound.y ||
		screenPosPlayer1.x > zoomOutMaxBound.x || screenPosPlayer1.y > zoomOutMaxBound.y ||
		screenPosPlayer2.x < zoomOutMinBound.x || screenPosPlayer2.y < zoomOutMinBound.y ||
		screenPosPlayer2.x > zoomOutMaxBound.x || screenPosPlayer2.y > zoomOutMaxBound.y)
	{
		distance = std::min(distance + m_fZoomSpeed, m_fMaxTargetDistance);
	}

	// Zoom in
	const Vector2 zoomInMinBound = m_fZoomInBound * Vector2(windowsize);
	const Vector2 zoomInMaxBound = Vector2(windowsize) - m_fZoomInBound * Vector2(windowsize);
	if (screenPosPlayer1.x > zoomInMinBound.x && screenPosPlayer1.y > zoomInMinBound.y &&
		screenPosPlayer1.x < zoomInMaxBound.x && screenPosPlayer1.y < zoomInMaxBound.y &&
		screenPosPlayer2.x > zoomInMinBound.x && screenPosPlayer2.y > zoomInMinBound.y &&
		screenPosPlayer2.x < zoomInMaxBound.x && screenPosPlayer2.y < zoomInMaxBound.y)
	{
		distance = std::max(distance - m_fZoomSpeed, m_fMinTargetDistance);
	}

	return distance;
}

void CameraMultiplayer::lockPlayersInView(const Vector2& screenPosPlayer1, const Vector2& screenPosPlayer2)
{
	const UVector2& windowsize = GetWorld()->GetApplication()->GetPlatform().GetWindowContext()->GetSize();

	// Lock players in view
	if (m_fTargetDistance == m_fMaxTargetDistance)
	{
		const Vector2 movementMinBound = m_fPlayerMovementBound  * Vector2(windowsize);
		const Vector2 movementMaxBound = Vector2(windowsize) - m_fPlayerMovementBound  * Vector2(windowsize);
		if (m_pPlayer1 != nullptr)
		{
			Vector3 playerPosition = m_pPlayer1->GetTransform()->GetPosition();
			if (screenPosPlayer1.x + m_fPlayerBoundOffset.x < movementMinBound.x && m_player1PrevPosition.x > playerPosition.x)
				playerPosition.x = m_player1PrevPosition.x;
			if (screenPosPlayer1.x + m_fPlayerBoundOffset.x > movementMaxBound.x && m_player1PrevPosition.x < playerPosition.x)
				playerPosition.x = m_player1PrevPosition.x;
			if (screenPosPlayer1.y + m_fPlayerBoundOffset.y < movementMinBound.y && m_player1PrevPosition.z < playerPosition.z)
				playerPosition.z = m_player1PrevPosition.z;
			if (screenPosPlayer1.y + m_fPlayerBoundOffset.y > movementMaxBound.y && m_player1PrevPosition.z > playerPosition.z)
				playerPosition.z = m_player1PrevPosition.z;

			m_pPlayer1->GetTransform()->SetPosition(playerPosition);
		}
		if (m_pPlayer2 != nullptr)
		{
			Vector3 playerPosition = m_pPlayer2->GetTransform()->GetPosition();
			if (screenPosPlayer2.x + m_fPlayerBoundOffset.x < movementMinBound.x && m_player2PrevPosition.x > playerPosition.x) 
				playerPosition.x = m_player2PrevPosition.x;
			if (screenPosPlayer2.x + m_fPlayerBoundOffset.x > movementMaxBound.x && m_player2PrevPosition.x < playerPosition.x)
				playerPosition.x = m_player2PrevPosition.x;
			if (screenPosPlayer2.y + m_fPlayerBoundOffset.y < movementMinBound.y && m_player2PrevPosition.z < playerPosition.z) 
				playerPosition.z = m_player2PrevPosition.z;
			if (screenPosPlayer2.y + m_fPlayerBoundOffset.y > movementMaxBound.y && m_player2PrevPosition.z > playerPosition.z)  
				playerPosition.z = m_player2PrevPosition.z;
			m_pPlayer2->GetTransform()->SetPosition(playerPosition);
		}
	}
}

void CameraMultiplayer::calculateChunkLoadOffset(Vector3& loadOffset)
{
	if (m_pPlayer1 != nullptr && m_pPlayer2 != nullptr)
	{
		Vector3 diff = m_pPlayer2->GetTransform()->GetPosition() - m_pPlayer1->GetTransform()->GetPosition();
		diff *= 0.5f;
		loadOffset = (m_pPlayer1->GetTransform()->GetPosition() + diff);
	}
	else if (m_pPlayer1 != nullptr || m_pPlayer2 != nullptr)
	{
		loadOffset = m_pPlayer1 != nullptr ? m_pPlayer1->GetTransform()->GetPosition() : m_pPlayer2->GetTransform()->GetPosition();
	}

	loadOffset -= m_pMainCamera->GetTransform()->GetPosition();
	loadOffset += m_ChunkLoadOffset;
	loadOffset.y = 0.f;
}
