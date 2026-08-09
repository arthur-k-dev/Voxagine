#include "pch.h"

#include <cmath>
#include <chrono>
#include "RenderSystem.h"

#include <algorithm>

#include "Core/ECS/Components/SpriteRenderer.h"
#include "Core/ECS/Components/TextRenderer.h"
#include "Core/ECS/Components/VoxRenderer.h"

#include "Core/ECS/World.h"
#include "Core/ECS/ComponentSystem.h"
#include "Core/ECS/Entities/Camera.h"

#include "Core/ECS/Systems/Physics/PhysicsSystem.h"
#include "Core/ECS/Systems/Chunk/ChunkSystem.h"
#include "Core/Platform/Rendering/RenderAlignment.h"

#include "Core/Platform/Platform.h"
#include "Core/Application.h"

#include "Core/ECS/Systems/Rendering/Buffers/RenderData.h"
#include "Core/ECS/Systems/Rendering/VoxelStamp.h"

#include "Core/Platform/Rendering/RenderContext.h"
#include "Core/Platform/Rendering/FrameProfiler.h"
#include "Core/Settings.h"
#include "Core/GameTimer.h"

// For Font Glyphs
#include <External/imgui/imgui.h>

#if defined(EDITOR) || defined(_DEBUG)
#include "../../Component.h"
#include "../../Components/Transform.h"
#include <Core/Resources/Formats/VoxModel.h>
#endif

#include "../../Components/VoxAnimator.h"
#include "DebugRenderer.h"

RenderSystem::RenderSystem(World* pWorld) :
	ComponentSystem(pWorld),
	m_pRenderContext(m_pWorld->GetApplication()->GetPlatform().GetRenderContext()),
	m_DebugRenderer(m_pRenderContext)
{
	m_pPhysicsSystem = m_pWorld->GetSystem<PhysicsSystem>();
	m_pPhysicsSystem->SetRenderSystem(this);

	m_VoxelBaker.Init(this, m_pPhysicsSystem);

	/* Prepare color data */
	VoxelGrid* pVoxelGrid = m_pPhysicsSystem->GetVoxelGrid();
	pVoxelGrid->GetDimensions(m_v3WorldSize.x, m_v3WorldSize.y, m_v3WorldSize.z);

	m_uiMaxVoxels = m_v3WorldSize.x * m_v3WorldSize.y * m_v3WorldSize.z;

	m_pWorld->Resumed += Event<World*>::Subscriber(std::bind(&RenderSystem::OnWorldResumed, this, std::placeholders::_1), this);

	// Temp for compilation purposes
	Entity entity(pWorld);
	TextRenderer textRenderer(&entity);
}

RenderSystem::~RenderSystem()
{
	for (VoxRenderer* pRenderer : m_VoxRenderers)
	{
		/* Remove old voxels if array is valid */
		if (pRenderer->m_BakeData.Positions)
		{
			delete[] pRenderer->m_BakeData.Positions;
			pRenderer->m_BakeData.Positions = nullptr;
		}
	}

	m_pRenderContext->SetFadeValue(1.f);

	/* Clear the current world's voxels */
	ClearVoxels();

	m_pWorld->Resumed -= this;
}

void RenderSystem::Start()
{
	if (!m_pRenderContext->ResizeWorldBuffer())
		ClearVoxels();

	/* Only from here on is a stamp worth making: everything above wipes the
	   voxel buffer, and the world's entities have already been added by now.
	   See OnComponentAdded. */
	m_bStarted = true;

	SetGroundPlane(m_pWorld->GetGroundTexturePath(), true);

	if (!m_pWorld->GetApplication()->IsInEditor())
		m_pRenderContext->SetFadeValue(0.f);

	m_pRenderContext->m_fFadeTime = 1.f;

	ForceUpdate();
}

bool RenderSystem::CanProcessComponent(Component* pComponent)
{
	return dynamic_cast<VoxRenderer*>(pComponent) || dynamic_cast<VoxAnimator*>(pComponent) || dynamic_cast<SpriteRenderer*>(pComponent) || dynamic_cast<TextRenderer*>(pComponent);
}

void RenderSystem::Tick(float fDeltaTime)
{
}

void RenderSystem::PostTick(float fDeltaTime)
{
	/* Submit voxel data */
	m_pRenderContext->Submit(RenderData(
		nullptr,
		m_v3WorldSize.x * m_v3WorldSize.y * m_v3WorldSize.z * sizeof(uint32_t),
		{ sizeof(uint32_t) },
		RenderDataType::VOXEL
	));

	static const bool s_bCoverageAudit = std::getenv("VOXAGINE_COVERAGE_AUDIT") != nullptr;

	if (s_bCoverageAudit)
		m_AuditProxies.clear();

	for (VoxRenderer* pRenderer : m_VoxRenderers) {
		CheckRendererChange(pRenderer);

		const VoxFrame* pFrame = pRenderer->GetFrame();
		if (pFrame == nullptr || !pRenderer->IsEnabled()) continue;

		Box bounds = pRenderer->GetBounds();
		Vector3 offset = Vector3(0.f, 0.f, 0.f);
		if (pFrame->GetModel()->GetFrameCount() > 1) {//Animation

			//Get animation relative-to-absolute offset
			const VoxFrame* tFrame = pFrame->GetModel()->GetFrame(0);
			Vector3 offsetCen = -(tFrame->GetFitSizeOffset() - pFrame->GetFitSizeOffset()) * 0.5f
				+ ((pFrame->GetFitSizeOffset() + pFrame->GetFittedSize()) - (tFrame->GetFitSizeOffset() + tFrame->GetFittedSize())) * 0.5f;
			offsetCen.y *= -1.f;

			//Transform the offset based on rotation and scale
			offset = pRenderer->GetTransform()->GetMatrix() * Vector4(offsetCen, 1.f);
			offset -= pRenderer->GetTransform()->GetPosition();
		}

		StructuredVoxelBuffer buffer;

		/* The proxy has to contain what VoxelBaker stamps, so it is derived from
		   the stamp rather than from the transform - see
		   ComputeStampedGridBounds. The transform-derived box below it is what
		   shipped, and it is short wherever the two quantizations disagree: the
		   stamp rotates by a multiple of the renderer's angle limit and is
		   placed by two floors, neither of which the matrix knows about. A voxel
		   outside every proxy is simply not rasterized, which reads on screen as
		   part of a model missing from some angles and present from others.

		   Both are computed here and the proxy is their union: the stamp box is
		   the geometry that exists, and keeping the old box in the union means
		   no view that renders today can lose anything. */
		const VoxelGrid& grid = m_pPhysicsSystem->m_VoxelGrid;

		Vector3 v3ProxyMin = grid.WorldToGrid(pRenderer->GetTransform()->GetPosition() - offset, true) - (bounds.GetSize() * 0.5f + Vector3(1.f));
		Vector3 v3ProxyMax = grid.WorldToGrid(pRenderer->GetTransform()->GetPosition() - offset, true) + (bounds.GetSize() * 0.5f + Vector3(1.f));

		VoxelStampTransform stamp;
		Vector3 v3StampMin(0.f);
		Vector3 v3StampMax(0.f);

		if (ComputeVoxelStampTransform(pRenderer, grid.GetWorldOffset(), 1.f / static_cast<float>(grid.GetVoxelSize()), stamp) &&
			ComputeStampedGridBounds(pRenderer, stamp, v3StampMin, v3StampMax))
		{
			/* A voxel at index v occupies [v, v + 1), so the far face is one
			   voxel past the last index, and a voxel of slack either side keeps
			   the ray's entry point off the geometry itself. */
			AuditProxyBounds(pRenderer, v3ProxyMin, v3ProxyMax, v3StampMin, v3StampMax);

			v3ProxyMin = glm::min(v3ProxyMin, v3StampMin - Vector3(1.f));
			v3ProxyMax = glm::max(v3ProxyMax, v3StampMax + Vector3(2.f));
		}

		buffer.Position = (v3ProxyMin + v3ProxyMax) * 0.5f;
		buffer.Extents = (v3ProxyMax - v3ProxyMin) * 0.5f;
		buffer.MapperID = pRenderer->GetFrame()->GetMapperID();

		/* VoxelBaker::Occupy already refuses a non-finite stamp position; this
		   is the same check on the other consumer of the same transform, and
		   it is the one that could hang the machine. The proxy AABB is what
		   the marcher starts from, and the marcher has had no step cap since
		   RENDERING_PLAN.md phase 2 - so a NaN or absurd box is not a wrong
		   pixel, it is a frame that never finishes, reported only as the
		   [stall] line in RenderContext with nothing naming the cause.
		   Dropping the model loses one entity from the image and says which. */
		if (!(std::isfinite(buffer.Position.x + buffer.Position.y + buffer.Position.z) &&
		      std::isfinite(buffer.Extents.x + buffer.Extents.y + buffer.Extents.z)))
		{
			static bool s_bWarned = false;

			if (!s_bWarned)
			{
				s_bWarned = true;
				fprintf(stderr, "[render] non-finite voxel proxy from '%s': pos(%.2f %.2f %.2f) extents(%.2f %.2f %.2f) - model dropped\n",
				        pRenderer->GetOwner()->GetName().c_str(),
				        buffer.Position.x, buffer.Position.y, buffer.Position.z,
				        buffer.Extents.x, buffer.Extents.y, buffer.Extents.z);
			}

			continue;
		}

		m_pRenderContext->Submit(buffer);

		if (s_bCoverageAudit)
		{
			Box proxy;
			proxy.Min = Vector3(buffer.Position) - Vector3(buffer.Extents);
			proxy.Max = Vector3(buffer.Position) + Vector3(buffer.Extents);

			m_AuditProxies.push_back(proxy);
		}

#if defined(EDITOR) || defined(_DEBUG)
		if (!pRenderer->DrawBoundsEnabled())
			continue;

		DebugBox box;
		box.m_Center = pRenderer->GetTransform()->GetPosition()-offset;
		box.m_Extents = bounds.GetSize() * 0.5f;
		box.m_Color = VColors::LightSkyBlue;

		m_DebugRenderer.AddBox(
			box
		);
#endif
	}

	SubmitLooseVoxelProxies(s_bCoverageAudit);

	if (s_bCoverageAudit)
		AuditProxyCoverage(fDeltaTime);

	/* VOXAGINE_INTEGRITY_AUDIT=<seconds>: the connectivity oracle. Separate
	   from the sync audit because they cost very differently - this one walks
	   cached CPU voxels, that one reads the window back over PCIe - and a
	   destruction session wants this one often and that one rarely. */
	{
		static const double s_fInterval =
			std::getenv("VOXAGINE_INTEGRITY_AUDIT") ? atof(std::getenv("VOXAGINE_INTEGRITY_AUDIT")) : 0.0;
		static double s_fElapsed = 0.0;

		if (s_fInterval > 0.0 && m_pPhysicsSystem != nullptr)
		{
			s_fElapsed += fDeltaTime;

			if (s_fElapsed >= s_fInterval)
			{
				s_fElapsed = 0.0;
				m_pPhysicsSystem->AuditIntegrity();
			}
		}
	}

	/* VOXAGINE_SYNC_AUDIT=<seconds>: repeated rather than one-shot, because the
	   disagreement it looks for is produced by a *write path* and so does not
	   exist at rest - see DESTRUCTION_PLAN.md's verification reference.

	   In PostTick, off the wall-clock delta, and not in Render off the fixed
	   timer where the other two audits live. Render is only reached on a fixed
	   step, so a run whose fixed timer never advances - which is what the
	   editor does while a world is loading - accumulates nothing and the audit
	   silently never fires. It looks identical to "everything agrees". */
	{
		static const double s_fInterval =
			std::getenv("VOXAGINE_SYNC_AUDIT") ? atof(std::getenv("VOXAGINE_SYNC_AUDIT")) : 0.0;
		static double s_fElapsed = 0.0;

		if (s_fInterval > 0.0 && m_pPhysicsSystem != nullptr)
		{
			/* Says once that it is armed. Without it, "the variable was not
			   picked up" and "everything agrees" produce the same empty log. */
			static bool s_bAnnounced = false;

			if (!s_bAnnounced)
			{
				s_bAnnounced = true;
				fprintf(stderr, "[sync-audit] armed, every %.1f s\n", s_fInterval);
			}

			s_fElapsed += fDeltaTime;

			if (s_fElapsed >= s_fInterval)
			{
				s_fElapsed = 0.0;
				AuditRepresentationSync();
			}
		}
	}

#ifndef _ORBIS
	/* Render text data */
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.f, 0.f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0.f, 0.f));

	uint32_t uiTextRendererCount = 0;

	for (TextRenderer* pTextRenderer : m_TextRenderers)
	{
		if (!pTextRenderer->IsEnabled() || pTextRenderer->GetText().empty())
			continue;

		Vector3 position = pTextRenderer->GetTransform()->GetPosition();

		float scale = pTextRenderer->GetScale();
		Vector2 screenScale = Vector2(1.f, 1.f);

		if (pTextRenderer->ScalesWithScreen())
		{
			Vector2 renderRes = m_pRenderContext->GetRenderResolution();
			screenScale = Vector2(renderRes.x / 1280.f, renderRes.y / 720.f);
			scale *= std::min(screenScale.x, screenScale.y);
		}

		Vector2 v2Alignment = GetNormRenderAlignment(pTextRenderer->GetAlignment());
		Vector2 v2ScreenAlignment = GetNormRenderAlignment(pTextRenderer->GetScreenAlignment());

		ImVec2 alignment = ImVec2(v2Alignment.x, v2Alignment.y);
		ImVec2 screenAlignment = ImVec2(v2ScreenAlignment.x, v2ScreenAlignment.y);

		ImGui::SetNextWindowBgAlpha(0.f);
		
		ImGui::Begin(("Text Renderer " + std::to_string(uiTextRendererCount)).c_str(), NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs);
		uiTextRendererCount++;

		ImGui::PushStyleColor(
			ImGuiCol_Text,
			ImVec4(
				pTextRenderer->GetColor().inst.Colors.r / 255.f,
				pTextRenderer->GetColor().inst.Colors.g / 255.f,
				pTextRenderer->GetColor().inst.Colors.b / 255.f,
				pTextRenderer->GetColor().inst.Colors.a / 255.f * m_pRenderContext->GetFadeValue()
			)
		);

		ImGui::SetWindowFontScale(scale);

		ImVec2 windowPosition = ImVec2(
			screenAlignment.x * ImGui::GetIO().DisplaySize.x + position.x * screenScale.x - ImGui::GetWindowWidth() * alignment.x,
			screenAlignment.y * ImGui::GetIO().DisplaySize.y - position.y * screenScale.y - ImGui::GetWindowHeight() * alignment.y
		);

		if (pTextRenderer->IsWrapping())
		{
			ImGui::PushTextWrapPos(ImGui::GetIO().DisplaySize.x - windowPosition.x);
			ImGui::TextWrapped(pTextRenderer->GetText().c_str());
			ImGui::PopTextWrapPos();
		}
		else
		{
			/* Unformatted: the text is world data and a '%' in it would otherwise
			   be read as a conversion against arguments that were never passed. */
			ImGui::TextUnformatted(pTextRenderer->GetText().c_str());
		}

		ImGui::SetWindowPos(windowPosition);

		ImGui::PopStyleColor();
		ImGui::End();
	}

	ImGui::PopStyleVar();
	ImGui::PopStyleVar();
	ImGui::PopStyleVar();
	ImGui::PopStyleVar();
	ImGui::PopStyleVar();
#endif

	// Sort AABBs
	m_pRenderContext->SortAABBs();

	/* Ground plane SDF.

	   Raised so the box's underside sits on *top* of the chunk ground plane
	   rather than inside it. The ground is the voxel layer at integer y = 0,
	   which occupies [0, 1], and this proxy used to span [0, 10] - so a ray
	   entering through one of its side faces within that first unit began the
	   march already inside an occupied voxel. VoxelRenderer.ps.hlsl takes the
	   normal of a hit on the very first sample from the face the ray crossed to
	   get in, which for a side face is horizontal: a piece of floor shaded as a
	   wall, with GetShineLine's vertical-wall branch putting a lit rim along the
	   top of it. Perspective stretches that one-voxel strip along the window's
	   boundary into a receding line of dark wedges under a bright line.

	   Starting at y = 1 leaves no way in below the ground's top face. A
	   descending ray still crosses into the y = 0 layer during the march and
	   gets the up normal it should, and one entering through the underside is
	   travelling upward and correctly hits nothing. */
	DebugBox box;
	box.m_Extents = Vector3((float)m_v3WorldSize.x, 5.0f, (float)m_v3WorldSize.z) * 0.5f;
	box.m_Center = m_pPhysicsSystem->m_VoxelGrid.GridToWorld(
		box.m_Extents + Vector3(0.f, R_GROUND_PLANE_HEIGHT, 0.f));
	box.m_Color = VColors::LightSkyBlue;

	StructuredVoxelBuffer buffer;
	buffer.Position = m_pPhysicsSystem->m_VoxelGrid.WorldToGrid(box.m_Center, true);
	buffer.Extents = box.m_Extents;
	buffer.MapperID = 0;

	m_pRenderContext->Submit(buffer);

#if defined(EDITOR) || defined(_DEBUG)
	m_DebugRenderer.AddBox(
		box
	);
#endif
}

void RenderSystem::FixedTick(const GameTimer& fixedTimer)
{
	// Fade scene
	if (m_bFaded)
	{
		if (m_pRenderContext->m_fFader > 0.f)
		{
			m_pRenderContext->SetFadeValue(
				std::max(
					0.f,
					m_pRenderContext->m_fFader - static_cast<float>(fixedTimer.GetElapsedSeconds()) / m_pRenderContext->m_fFadeTime
				)
			);
		}
	}
	else
	{
		if (m_pRenderContext->m_fFader < 1.f)
		{
			m_pRenderContext->SetFadeValue(
				std::min(
					1.f,
					m_pRenderContext->m_fFader + static_cast<float>(fixedTimer.GetElapsedSeconds()) / m_pRenderContext->m_fFadeTime
				)
			);
		}
	}

	/* Update animators on a fixed timestep */
	for (auto& pAnimator : m_VoxAnimators) {
		if (!pAnimator->IsEnabled())
			continue;

		pAnimator->Tick(static_cast<float>(fixedTimer.GetElapsedSeconds()));
	}
}

void RenderSystem::PostFixedTick(const GameTimer& fixedTimer)
{
	m_pRenderContext->FixedClear();

	/* Submit sprite data */
	SpriteData spriteData;

	/* Sort sprite renders by layer, so the rendering is updated in the editor
	   when changing the layer.

	   Descending, which is back to front: UIRenderer.vs.hlsl maps a higher
	   render layer to a *greater* depth, and the UI pass depth-tests
	   LESS_OR_EQUAL with depth writes on. Ascending submitted the nearest
	   sprite first, so anything behind it was depth-rejected rather than
	   blended under it - the splash screen's opaque background (layer 8) lost
	   every pixel its logos (layer -1) had already covered. Translucent
	   geometry has to be drawn back to front regardless; this is that. */
	std::map<int, std::vector<SpriteRenderer*>, std::greater<int>> LayeredSpriteRenderers;
	for (SpriteRenderer* pSpriteRenderer : m_SpriteRenderers)
	{
		if (pSpriteRenderer)
			LayeredSpriteRenderers[pSpriteRenderer->GetRenderLayer()].push_back(pSpriteRenderer);
	}

	for (std::pair<int, std::vector<SpriteRenderer*>> LayeredSpriteRenderer : LayeredSpriteRenderers)
	{
		for (SpriteRenderer* pSpriteRenderer : LayeredSpriteRenderer.second)
		{
			if (
				!pSpriteRenderer ||
				!pSpriteRenderer->IsEnabled() ||
				!pSpriteRenderer->m_pTextureReference ||
				!pSpriteRenderer->m_pTextureReference->IsLoaded() ||
				!pSpriteRenderer->m_pTextureReference->TextureView
			)
				continue;

			if (!pSpriteRenderer->IsScreenSpace() && pSpriteRenderer->IsBillboard())
			{
				pSpriteRenderer->GetTransform()->SetRotation(m_pWorld->GetMainCamera()->GetTransform()->GetRotation());
			}

			Vector2 scale = pSpriteRenderer->GetScale();
			float minScale = std::min(scale.x, scale.y);

			spriteData.Model = pSpriteRenderer->GetTransform()->GetMatrix();
			spriteData.Model *= glm::scale(Vector3(minScale, minScale, 1.f));

			spriteData.Model[3][0] = pSpriteRenderer->GetTransform()->GetPosition().x * scale.x;
			spriteData.Model[3][1] = pSpriteRenderer->GetTransform()->GetPosition().y * scale.y;

			spriteData.TextureID = pSpriteRenderer->m_pTextureReference->GetID();

			VColor color = pSpriteRenderer->GetColor();

			spriteData.Color = Vector4(
				color.inst.Colors.r / 255.0,
				color.inst.Colors.g / 255.0,
				color.inst.Colors.b / 255.0,
				color.inst.Colors.a / 255.0
			);

			spriteData.Size = pSpriteRenderer->m_pTextureReference->TextureView->GetInfo().m_Size;

			spriteData.Alignment = pSpriteRenderer->GetAlignment();
			spriteData.ScreenAlignment = pSpriteRenderer->GetScreenAlignment();

			spriteData.IsScreen = pSpriteRenderer->IsScreenSpace();

			spriteData.Layer = pSpriteRenderer->GetRenderLayer();

			spriteData.TextureRepeat = pSpriteRenderer->GetTilingAmount();

			spriteData.cullStart = pSpriteRenderer->GetCullingStart();
			spriteData.cullEnd = pSpriteRenderer->GetCullingEnd();

			m_pRenderContext->Submit(spriteData);
		}
	}
}

void RenderSystem::Render(const GameTimer& fixedTimer)
{
	/* VOXAGINE_VOXEL_AUDIT=<seconds> runs the scan below once, that many
	   seconds in. It is the acceptance test for RENDERING_PLAN.md phase 4d and
	   is kept for it; off unless the variable is set. */
	{
		static const double s_fAuditAfter =
			std::getenv("VOXAGINE_VOXEL_AUDIT") ? atof(std::getenv("VOXAGINE_VOXEL_AUDIT")) : 0.0;
		static double s_fElapsed = 0.0;
		static bool s_bDone = false;

		if (s_fAuditAfter > 0.0 && !s_bDone)
		{
			s_fElapsed += fixedTimer.GetElapsedSeconds();

			if (s_fElapsed >= s_fAuditAfter)
			{
				s_bDone = true;
				AuditVoxelRepresentation();
			}
		}
	}

	/* Get voxel data on fixed timestep */
	bool bShouldUpdateVoxelWorld = m_bForcedUpdate;
	Camera* pCamera = m_pWorld->GetMainCamera();

	m_bShouldUpdateVoxelWorld = bShouldUpdateVoxelWorld || pCamera->IsUpdated();

	m_VoxelBaker.Bake();

	m_bForcedUpdate = false;

	/* DESTRUCTION_PLAN.md P16: Render runs once per rendered frame and only on
	   one that actually ran a fixed tick (Application.cpp gates the call on
	   bFixedStep) - which is exactly the swap this back-buffered mapper needs.
	   Swapping on a frame with no fixed tick would present the frame-before-
	   last's particles, a visible jump; not swapping here would let the next
	   fixed tick's writes race the frame still being drawn from this one. */
	if (Mapper* pParticleMapper = m_pRenderContext->GetParticleMapper())
		pParticleMapper->SwapBuffer();
}

void RenderSystem::OnWorldResumed(World* pWorld)
{
	/* Clear voxel world, make planes and force update */

	if (!m_pRenderContext->ResizeWorldBuffer())
		ClearVoxels();

	if (!m_pWorld->GetApplication()->IsInEditor())
		m_pRenderContext->SetFadeValue(0.f);

	m_pRenderContext->m_fFadeTime = 1.f;

	

	//ForceUpdate();
}

void RenderSystem::OnComponentAdded(Component* pComponent)
{
	if (VoxRenderer* pRenderer = dynamic_cast<VoxRenderer*>(pComponent))
	{
		pRenderer->GetOwner()->StaticPropertyChanged -= this;
		pRenderer->GetOwner()->StaticPropertyChanged += Event<Entity*, bool>::Subscriber([this](Entity* pEntity, bool isStatic)
		{
			VoxRenderer* pRenderer = pEntity->GetComponent<VoxRenderer>();
			if (pRenderer)
			{
				OnComponentDestroyed(pRenderer);
				OnComponentAdded(pRenderer);
				pRenderer->RequestUpdate();
			}
		}, this);

		// Set render data and occupy space in world
		Vector3 pos = pRenderer->GetTransform()->GetPosition();
		pos.x = floor(pos.x);
		pos.y = floor(pos.y);
		pos.z = floor(pos.z);

		VoxRenderer::BakeData bakeData;

		bakeData.LastLocation = pos;
		bakeData.LastRotation = pRenderer->GetTransform()->GetRotation();
		bakeData.LastScale = pRenderer->GetTransform()->GetScale();
		bakeData.WorldOffset = m_pPhysicsSystem->m_VoxelGrid.GetWorldOffset();
		bakeData.IsEnabled = pRenderer->IsEnabled();

		m_VoxRenderers.push_back(pRenderer);
		pRenderer->m_BakeData = bakeData;

		/* Two reasons not to stamp here.

		   Before Start(), because Start() wipes the voxel buffer and every
		   entity in the world has been added by then: the stamp was thrown
		   away a moment later and the forced first bake redid all of it. On
		   Valley_Path_To_Castle_Beat1 that was 3.3 M voxels written, discarded
		   and written again.

		   When disabled, because the bake would only clear it again - and the
		   bake can no longer be relied on to do that, since it now skips a
		   renderer whose recorded stamp is still current. */
		if (m_bStarted && !pRenderer->IsChunkInstanceLoaded() && pRenderer->IsEnabled())
		{
			/* Reported separately because it is *not* inside VoxelBaker::Bake,
			   and that gap is misleading: this is where a world load's stamping
			   actually happens - 587 renderers and 3.1 M voxels in 399.5 ms on
			   Valley_Path_To_Castle_Beat1 - while the Bake timer next to it
			   reads microseconds. Quoting Bake alone makes the load look free
			   when two thirds of the cost simply never passed through it.
			   RENDERING_PLAN.md phase 4c. */
			const bool bProfiling = FrameProfiler::Get().IsEnabled();
			const std::chrono::steady_clock::time_point start =
				bProfiling ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();

			uint32_t* voxels = m_VoxelBaker.Occupy(pRenderer, &pRenderer->m_BakeData);
			pRenderer->m_BakeData.Positions = voxels;

			if (bProfiling)
			{
				FrameProfiler::Get().Report("CPU VoxelBaker::Occupy (added)",
					std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
			}
		}
	}
	else if (VoxAnimator* pAnimator = dynamic_cast<VoxAnimator*>(pComponent))
	{
		m_VoxAnimators.push_back(pAnimator);
	}
	else if (TextRenderer* pText = dynamic_cast<TextRenderer*>(pComponent))
	{
		m_TextRenderers.push_back(pText);
		pText->m_pRenderSystem = this;
	}
	else if (SpriteRenderer* pSprite = dynamic_cast<SpriteRenderer*>(pComponent))
	{
		m_SpriteRenderers.push_back(pSprite);
		pSprite->m_pRenderSystem = this;
	}
}

void RenderSystem::OnComponentDestroyed(Component* pComponent)
{
	if (VoxRenderer* pVoxRenderer = dynamic_cast <VoxRenderer*>(pComponent))
	{
		auto iter = std::find(m_VoxRenderers.begin(), m_VoxRenderers.end(), pVoxRenderer);

		if (iter != m_VoxRenderers.end())
		{
			/* Remove old voxels if array is valid */
			m_VoxelBaker.Clear(*iter);
			m_VoxRenderers.erase(iter);
		}
	}
	else if (VoxAnimator* pVoxAnimator = dynamic_cast <VoxAnimator*>(pComponent))
	{
		m_VoxAnimators.erase(std::remove(m_VoxAnimators.begin(), m_VoxAnimators.end(), pVoxAnimator), m_VoxAnimators.end());
	}
	else if (TextRenderer* pText = dynamic_cast<TextRenderer*>(pComponent))
	{
		m_TextRenderers.erase(std::remove(m_TextRenderers.begin(), m_TextRenderers.end(), pText), m_TextRenderers.end());
	}
	else if (SpriteRenderer* pSprite = dynamic_cast<SpriteRenderer*>(pComponent))
	{
		m_SpriteRenderers.erase(std::remove(m_SpriteRenderers.begin(), m_SpriteRenderers.end(), pSprite), m_SpriteRenderers.end());
	}
}

uint32_t RenderSystem::GetVoxel(int32_t x, int32_t y, int32_t z) const
{
	/* Per axis, not against the flat count: an x past the row width would
	   otherwise fold into the next row and read a real but wrong voxel. */
	if (x < 0 || y < 0 || z < 0)
		return 0;

	if (static_cast<uint32_t>(x) >= m_v3WorldSize.x ||
		static_cast<uint32_t>(y) >= m_v3WorldSize.y ||
		static_cast<uint32_t>(z) >= m_v3WorldSize.z)
		return 0;

	return m_pRenderContext->GetVoxel(static_cast<uint32_t>(x + y * m_v3WorldSize.x + z * m_v3WorldSize.x * m_v3WorldSize.y));
}

uint32_t RenderSystem::GetVoxel(uint32_t uiVolumeId) const
{
	return m_pRenderContext->GetVoxel(uiVolumeId);
}

void RenderSystem::ClearVoxels()
{
	m_pRenderContext->ClearVoxels();
}

bool RenderSystem::IsFaded() const
{
	return m_pRenderContext->m_fFader <= 0.f;
}

bool RenderSystem::IsFading() const
{
	return m_pRenderContext->m_fFader > 0.f && m_pRenderContext->m_fFader < 1.f;
}

/* RenderSystem's own flag, which is read - by Render, to decide whether the
   bake re-examines every renderer. Not to be confused with the RenderContext
   flag of the same name that used to sit beside it: that one was written from
   seven places and read from none, and is gone (ledger M1). */
void RenderSystem::ForceUpdate()
{
	m_bForcedUpdate = true;
}

void RenderSystem::ForceCameraDataUpdate()
{
	m_pRenderContext->ForceCameraDataUpdate();
}

void RenderSystem::SetGroundPlane(const std::string& texturePath, bool bForce)
{
	VoxelGrid* pVoxelGrid = m_pPhysicsSystem->GetVoxelGrid();
	TextureReadData* pTextureData = nullptr;
	
	if (!texturePath.empty())
		pTextureData = m_pWorld->GetApplication()->GetPlatform().GetRenderContext()->ReadTexture(texturePath);

	uint32_t id = 0;
	uint32_t color = VColor(static_cast<unsigned char>(50), 50, 50, 255).inst.Color;

	bool bHasData = pTextureData && pTextureData->m_Data && pTextureData->m_Dimensions.x > 0 && pTextureData->m_Dimensions.y > 0;

	for (uint32_t z = 0; z < m_v3WorldSize.z; ++z)
	{
		for (uint32_t x = 0; x < m_v3WorldSize.x; ++x)
		{
			if (bHasData)
			{
				id = x % pTextureData->m_Dimensions.x + ((pTextureData->m_Dimensions.y - 1 - z) * pTextureData->m_Dimensions.x) % (pTextureData->m_Dimensions.x * (pTextureData->m_Dimensions.y));
				color = pTextureData->m_Data[id];
			}

			/* Tagged rather than passed through - RENDERING_PLAN.md 7.4. The
			   voxel word's top byte is a tag now, not an opacity, and a ground
			   texel's 255 sets every reserved bit in it including
			   VOXEL_EMISSIVE_TAG. That reads as a floor made of light, which is
			   exactly how this second copy of Chunk::UpdateGroundPlane's loop
			   was found: fixing that one left the ground still glowing. */
			const uint32_t uiTagged = (color & 0x00FFFFFFu) | VoxelStateTag(RS_DEFAULT, false);

			ModifyVoxel(
				x, 0, z,
				uiTagged
			);

			const VoxelCell cell = pVoxelGrid->GetCell(x, 0, z);
			cell.SetColor(uiTagged);
			cell.ClearOwner();
		}
	}

	delete pTextureData;
}

/* The acceptance test for RENDERING_PLAN.md phase 4d, kept now that the phase
 * has landed because it is the only check on this representation that does not
 * depend on the camera.
 *
 * Before the phase it asked whether Voxel::Active and Voxel::UserPointer could
 * be dropped. Now that they are gone it reports what replaced them: how much of
 * the resident grid is occupied, how many voxels carry a static owner slot and
 * how many of those slots name an entity that no longer exists, and how many
 * carry a transient particle claim - including the owner-set-but-inactive
 * combination, which is debris in flight and is the state that blocks a static
 * re-bake over that voxel.
 *
 * Reads 75 M voxels out of ordinary cached memory, so it costs a second or two
 * and freezes the frame - on demand only, like the brick and far-field
 * validators. Run it *during destruction* as well as at rest: a static scene
 * has no particle claims at all.
 */
void RenderSystem::AuditVoxelRepresentation()
{
	VoxelGrid& grid = m_pPhysicsSystem->m_VoxelGrid;
	const UVector3 dims = grid.GetDimensions();

	uint64_t uiActive = 0;
	uint64_t uiOwners = 0;
	uint64_t uiDeadOwners = 0;
	uint64_t uiOwnerNotActive = 0;
	uint64_t uiReservedSlots = 0;

	std::unordered_map<uint64_t, bool> aliveCache;
	std::unordered_map<uint16_t, uint64_t> slotCounts;

	for (uint32_t z = 0; z < dims.z; ++z)
	for (uint32_t y = 0; y < dims.y; ++y)
	for (uint32_t x = 0; x < dims.x; ++x)
	{
		const VoxelCell cell = grid.GetCell(x, y, z);

		if (!cell)
			continue;

		const bool bActive = cell.IsActive();

		if (bActive)
			++uiActive;

		const uint16_t uiSlot = cell.GetSlot();

		if (uiSlot == VoxelOwnerVolume::k_uiNoOwnerSlot)
			continue;

		++uiOwners;
		++slotCounts[uiSlot];

		if (!bActive)
			++uiOwnerNotActive;

		/* Nothing writes this since phase 3 deleted particle claims, so it
		   should read zero. A non-zero count means chunk data older than that
		   is still resident, which is fine, or that something started handing
		   the reserved slot out, which is not. */
		if (uiSlot == VoxelOwnerVolume::k_uiReservedSlot)
		{
			++uiReservedSlots;
			continue;
		}

		const uint64_t uiEntityID = grid.ResolveOwnerSlot(uiSlot);

		std::unordered_map<uint64_t, bool>::iterator it = aliveCache.find(uiEntityID);

		if (it == aliveCache.end())
			it = aliveCache.emplace(uiEntityID, m_pWorld->FindEntity(uiEntityID) != nullptr).first;

		if (!it->second)
			++uiDeadOwners;
	}

	fprintf(stderr, "[voxel-audit] %llu active of %u (%zu B per CPU voxel + %zu B of owner slot)\n",
	        (unsigned long long)uiActive, dims.x * dims.y * dims.z,
	        sizeof(Voxel), sizeof(uint16_t));

	fprintf(stderr, "[voxel-audit] owners: %llu set, %llu naming a dead entity, %llu on an inactive voxel, %llu on the reserved slot (%zu distinct slots of %zu allocated)\n",
	        (unsigned long long)uiOwners, (unsigned long long)uiDeadOwners,
	        (unsigned long long)uiOwnerNotActive, (unsigned long long)uiReservedSlots,
	        slotCounts.size(), grid.GetOwnerSlotCount());

	/* The other half of the representation is the RLE the chunk system encodes
	   into on unload, and reaching that for real means walking far enough for a
	   chunk to leave the window. Round-tripping each resident chunk in place
	   exercises the same codec here. */
	ChunkSystem* pChunkSystem = m_pWorld->GetChunkSystem();

	if (pChunkSystem)
	{
		uint64_t uiDiverged = 0;
		uint32_t uiChunks = 0;

		for (const std::pair<const uint32_t, Chunk*>& entry : pChunkSystem->GetChunks())
		{
			if (!entry.second || !entry.second->IsLoaded())
				continue;

			++uiChunks;
			uiDiverged += entry.second->VerifyVoxelCodecRoundTrip();
		}

		fprintf(stderr, "[voxel-audit] codec round trip over %u loaded chunks: %llu diverging voxels\n",
		        uiChunks, (unsigned long long)uiDiverged);
	}
}

/* The representation-sync audit: rule 3 made checkable.
 *
 * A voxel exists in four places - the CPU colour in the chunk, the word in the
 * mapped GPU buffer, one bit in the occupancy bitmap and one unit of a brick
 * count - and nothing in the type system says a write has to touch all four.
 * ValidateBrickGrid already compares the last three against the mapping; what
 * it cannot see is the mapping disagreeing with the CPU voxel, which is the
 * failure a write path that updates one and not the other produces, and the one
 * that reads on screen as geometry that is there in physics and absent in the
 * image (or the reverse).
 *
 * Reads the whole window back out of the mapping, which is ReBAR host-visible
 * memory. It stages it into ordinary memory first with one bulk copy, and that
 * is not an optimization detail - it is the difference between an audit and a
 * hang. Two passes want the words (the brick validator and the colour compare
 * below), and read scattered from an uncached mapping each one costs on the
 * order of a hundred nanoseconds a voxel: 75 M voxels twice is minutes, which
 * on a short interval means the process never leaves this function. Staged, the
 * PCIe traffic is one sequential 300 MB read and both passes then walk cache.
 *
 * Still on demand, and it still freezes the frame for as long as it takes -
 * expected, not a hang. On this machine's 768x128x768 window the staging read
 * alone is **19 seconds** (302 MB at about 15 MB/s, which is what an uncached
 * PCIe read of VRAM costs and is consistent with the ~500 ns per voxel read
 * measured in RENDERING_PLAN.md phase 4b). memcpy does not help; the memory
 * type does. So give it an interval of a minute or more, and expect the report
 * to describe the world as it was when the copy started.
 */
void RenderSystem::AuditRepresentationSync()
{
	VoxelGrid& grid = m_pPhysicsSystem->m_VoxelGrid;
	const UVector3 dims = grid.GetDimensions();
	const uint32_t* pMapped = m_pRenderContext->GetVoxelData();

	/* Says so rather than returning quietly. An audit that produces no output
	   when it cannot run reads exactly like one that ran and found nothing,
	   which is the worst possible failure mode for a check whose whole value is
	   its silence. */
	if (pMapped == nullptr || dims.x == 0 || dims.y == 0 || dims.z == 0)
	{
		fprintf(stderr, "[sync-audit] skipped: no voxel window yet (%ux%ux%u, mapping %s)\n",
		        dims.x, dims.y, dims.z, pMapped ? "present" : "absent");
		return;
	}

	const uint32_t uiWordCount = m_pRenderContext->GetVoxelDataSize();

	const std::chrono::high_resolution_clock::time_point stageStart =
		std::chrono::high_resolution_clock::now();

	std::vector<uint32_t> staged(pMapped, pMapped + uiWordCount);

	const std::chrono::duration<double, std::milli> stageSpan =
		std::chrono::high_resolution_clock::now() - stageStart;

	const uint32_t* pWords = staged.data();

	const uint32_t uiBrickDisagreements =
		m_pRenderContext->GetBrickGrid().Validate(false, pWords);

	uint64_t uiColourDisagreements = 0;
	uint64_t uiMissingFromGPU = 0;
	uint64_t uiMissingFromCPU = 0;
	uint32_t uiFirstBad = UINT32_MAX;

	for (uint32_t z = 0; z < dims.z; ++z)
	for (uint32_t y = 0; y < dims.y; ++y)
	for (uint32_t x = 0; x < dims.x; ++x)
	{
		const uint32_t uiID = x + y * dims.x + z * dims.x * dims.y;

		if (uiID >= uiWordCount)
			continue;

		const VoxelCell cell = grid.GetCell(x, y, z);

		if (!cell)
			continue;

		const uint32_t uiCPU = cell.GetColor();
		const uint32_t uiGPU = pWords[uiID];

		if (uiCPU == uiGPU)
			continue;

		++uiColourDisagreements;

		/* Split by direction, because the two are not equally interesting.

		   Occupied only on the CPU is a defect every time: geometry physics
		   can see and the image cannot, which is a write that reached the
		   chunk and not the mapping.

		   Occupied only on the GPU has a *legitimate* source and is expected
		   to be non-zero - a dynamic VoxRenderer's voxels live in the render
		   buffer and nowhere else, because VoxelBaker::Occupy only touches the
		   chunk and the physics grid for static renderers (see CLAUDE.md,
		   "Dynamic renderers are invisible to the physics grid"). So the
		   number to watch here is its size and its trend, not whether it is
		   zero: it should track roughly the voxel count of the dynamic
		   renderers on screen, and it should come back down when they leave. */
		if ((uiCPU >> 24) != 0 && (uiGPU >> 24) == 0)
			++uiMissingFromGPU;
		else if ((uiCPU >> 24) == 0 && (uiGPU >> 24) != 0)
			++uiMissingFromCPU;

		if (uiFirstBad == UINT32_MAX)
			uiFirstBad = uiID;
	}

	fprintf(stderr, "[sync-audit] %llu of %u voxels disagree between the CPU chunk and the mapping "
	                "(%llu occupied only on the CPU - always a defect; %llu only on the GPU - "
	                "expected, dynamic renderers stamp the mapping alone; first at %u); "
	                "%u brick/bitmap disagreements; staged %u words in %.1f ms\n",
	        (unsigned long long)uiColourDisagreements, dims.x * dims.y * dims.z,
	        (unsigned long long)uiMissingFromGPU, (unsigned long long)uiMissingFromCPU,
	        uiFirstBad, uiBrickDisagreements, uiWordCount, stageSpan.count());

	if (m_pPhysicsSystem)
		m_pPhysicsSystem->AuditParticlePool();
}

void RenderSystem::EnableDebugLines(bool bEnabled)
{
	m_pRenderContext->EnableDebugLines(bEnabled);
}

void RenderSystem::SetFadeTime(float fFadeTime)
{
	m_pRenderContext->m_fFadeTime = fFadeTime <= 0.0f ? 1.0f : fFadeTime;
}

VoxelEditTarget RenderSystem::MakeEditTarget()
{
	VoxelEditTarget target;

	target.pGrid = &m_pPhysicsSystem->m_VoxelGrid;
	target.pBricks = &m_pRenderContext->GetBrickGrid();
	target.pWords = m_pRenderContext->GetVoxelData();
	target.uiWordCount = m_pRenderContext->GetVoxelDataSize();
	target.v3WindowSize = m_v3WorldSize;
	target.pLooseVoxels = this;

	return target;
}

void RenderSystem::AddLooseVoxel(const Vector3& v3GridPosition)
{
	/* Level space: grid space plus the window's own offset. */
	const Vector3 v3Level = v3GridPosition + m_pPhysicsSystem->m_VoxelGrid.GetWorldOffset();

	/* Ledger L4. This used to drop out-of-range positions silently, which is
	   the same as saying "this voxel gets no proxy and nobody will ever know".
	   Dropping is still the right answer - a clamped cell key names a different
	   place, so the proxy would be submitted somewhere the voxel is not - but
	   it says so now. */
	auto reject = [&v3Level](const char* pReason)
	{
		static bool s_bWarned = false;

		if (!s_bWarned)
		{
			s_bWarned = true;
			fprintf(stderr, "[loose] dropping a voxel at level (%.1f %.1f %.1f): %s - it will only be drawn "
			                "when another model's proxy happens to cover it\n",
			        v3Level.x, v3Level.y, v3Level.z, pReason);
		}
	};

	if (v3Level.x < 0.f || v3Level.y < 0.f || v3Level.z < 0.f)
	{
		reject("negative level coordinate");
		return;
	}

	const UVector3 v3Voxel(
		static_cast<uint32_t>(v3Level.x),
		static_cast<uint32_t>(v3Level.y),
		static_cast<uint32_t>(v3Level.z));

	const UVector3 v3Cell(
		v3Voxel.x >> k_uiLooseCellShift,
		v3Voxel.y >> k_uiLooseCellShift,
		v3Voxel.z >> k_uiLooseCellShift);

	/* Ten bits an axis is a level of 32768 voxels a side at this cell size,
	   which is 21 times the largest one here. */
	if (v3Cell.x > 1023 || v3Cell.y > 1023 || v3Cell.z > 1023)
	{
		reject("outside the ten-bit cell grid");
		return;
	}

	const uint32_t uiKey = v3Cell.x | (v3Cell.y << 10) | (v3Cell.z << 20);

	/* Five bits an axis inside the cell. */
	const uint16_t uiOffset = static_cast<uint16_t>(
		(v3Voxel.x & (k_uiLooseCellSize - 1)) |
		((v3Voxel.y & (k_uiLooseCellSize - 1)) << 5) |
		((v3Voxel.z & (k_uiLooseCellSize - 1)) << 10));

	std::unordered_map<uint32_t, LooseVoxelCell>::iterator it = m_LooseVoxelCells.find(uiKey);

	if (it == m_LooseVoxelCells.end())
	{
		if (m_LooseVoxelCells.size() >= k_uiMaxLooseCells)
			EvictFarthestLooseCell();

		LooseVoxelCell cell;
		cell.Bounds.Min = v3Level;
		cell.Bounds.Max = v3Level;
		cell.Offsets.push_back(uiOffset);

		m_LooseVoxelCells.emplace(uiKey, std::move(cell));
		m_bLooseCellKeysDirty = true;

		return;
	}

	it->second.Bounds.Min = glm::min(it->second.Bounds.Min, v3Level);
	it->second.Bounds.Max = glm::max(it->second.Bounds.Max, v3Level);

	/* Sorted insert, so registering the same voxel twice - which debris landing
	   in the same spot does routinely - costs a search rather than a duplicate
	   the validator would then have to test twice. */
	std::vector<uint16_t>& offsets = it->second.Offsets;
	std::vector<uint16_t>::iterator at = std::lower_bound(offsets.begin(), offsets.end(), uiOffset);

	if (at == offsets.end() || *at != uiOffset)
		offsets.insert(at, uiOffset);
}

/* The cell whose box is farthest from the window, dropped when the registry
   hits its cap. See k_uiMaxLooseCells: this loses a proxy, not the debris. */
void RenderSystem::EvictFarthestLooseCell()
{
	if (m_LooseVoxelCells.empty())
		return;

	const Vector3 v3Offset = m_pPhysicsSystem->m_VoxelGrid.GetWorldOffset();
	const Vector3 v3WindowCentre = v3Offset + Vector3(m_v3WorldSize) * 0.5f;

	uint32_t uiWorstKey = 0;
	float fWorstDistance = -1.f;

	for (const std::pair<const uint32_t, LooseVoxelCell>& cell : m_LooseVoxelCells)
	{
		const Vector3 v3Centre = (cell.second.Bounds.Min + cell.second.Bounds.Max) * 0.5f;
		const float fDistance = glm::distance2(v3Centre, v3WindowCentre);

		if (fDistance > fWorstDistance)
		{
			fWorstDistance = fDistance;
			uiWorstKey = cell.first;
		}
	}

	static bool s_bWarned = false;

	if (!s_bWarned)
	{
		s_bWarned = true;
		fprintf(stderr, "[loose] registry hit %zu cells; dropping the farthest from the window. "
		                "Debris there stays in the world but loses its proxy.\n", m_LooseVoxelCells.size());
	}

	m_LooseVoxelCells.erase(uiWorstKey);
	m_bLooseCellKeysDirty = true;
}

/* Judges one cell against the voxels it actually recorded, dropping the ones
   that are no longer occupied and re-tightening the box around what is left.
   Returns false when nothing of the cell survives.

   This is ledger L1's fix and the whole reason a cell stores voxels rather than
   a box. The old test asked the brick grid whether *any* 8^3 block overlapping
   the cell held anything, which static geometry satisfies - so a cell over a
   wall never retired, and its box re-tightened onto the wall's bricks, growing
   a proxy that covered geometry the wall's own renderer already covered. */
bool RenderSystem::ValidateLooseCell(uint32_t uiKey, LooseVoxelCell& cell, const Vector3& v3Offset)
{
	VoxelBrickGrid& brickGrid = m_pRenderContext->GetBrickGrid();

	const UVector3 v3Cell(uiKey & 1023u, (uiKey >> 10) & 1023u, (uiKey >> 20) & 1023u);

	const UVector3 v3CellBase(
		v3Cell.x << k_uiLooseCellShift,
		v3Cell.y << k_uiLooseCellShift,
		v3Cell.z << k_uiLooseCellShift);

	Vector3 v3Min(0.f);
	Vector3 v3Max(0.f);
	bool bAny = false;

	size_t uiKept = 0;

	for (size_t i = 0; i < cell.Offsets.size(); ++i)
	{
		const uint16_t uiOffset = cell.Offsets[i];

		const Vector3 v3Level(
			static_cast<float>(v3CellBase.x + (uiOffset & 31u)),
			static_cast<float>(v3CellBase.y + ((uiOffset >> 5) & 31u)),
			static_cast<float>(v3CellBase.z + ((uiOffset >> 10) & 31u)));

		const Vector3 v3Grid = v3Level - v3Offset;

		/* Only voxels inside the window can be judged - outside it they are not
		   in the buffer at all, and erasing on that would delete debris that is
		   merely far away. Those are kept untested. */
		const bool bInside =
			v3Grid.x >= 0.f && v3Grid.y >= 0.f && v3Grid.z >= 0.f &&
			v3Grid.x < static_cast<float>(m_v3WorldSize.x) &&
			v3Grid.y < static_cast<float>(m_v3WorldSize.y) &&
			v3Grid.z < static_cast<float>(m_v3WorldSize.z);

		if (bInside)
		{
			const uint32_t uiVoxelID =
				static_cast<uint32_t>(v3Grid.x) +
				static_cast<uint32_t>(v3Grid.y) * m_v3WorldSize.x +
				static_cast<uint32_t>(v3Grid.z) * m_v3WorldSize.x * m_v3WorldSize.y;

			if (!brickGrid.IsOccupied(uiVoxelID))
				continue;
		}

		cell.Offsets[uiKept++] = uiOffset;

		v3Min = bAny ? glm::min(v3Min, v3Level) : v3Level;
		v3Max = bAny ? glm::max(v3Max, v3Level) : v3Level;
		bAny = true;
	}

	cell.Offsets.resize(uiKept);

	if (!bAny)
		return false;

	cell.Bounds.Min = v3Min;
	cell.Bounds.Max = v3Max;

	return true;
}

/* A proxy for each cell of loose voxels the resident window can see.
 *
 * Boxed per cell rather than one box for all of them because debris ends up
 * scattered across a level: a single box around all of it would enclose most of
 * the window, and a proxy is not only what gets rasterized, it is where the
 * ray starts - a box that big would hand every pixel it covers a march from its
 * own front face. Cells keep each box near the voxels that need it.
 */
void RenderSystem::SubmitLooseVoxelProxies(bool bAudit)
{
	if (m_LooseVoxelCells.empty())
		return;

	ScopedFrameTimer timer("CPU RenderSystem::SubmitLooseVoxelProxies");

	const Vector3 v3Offset = m_pPhysicsSystem->m_VoxelGrid.GetWorldOffset();
	const Vector3 v3WindowMax = Vector3(m_v3WorldSize) - Vector3(1.f);

	/* Iterated through a stable key vector rather than through the map's own
	   order, which changes on every rehash and made the round-robin cursor skip
	   cells arbitrarily (ledger L3). */
	if (m_bLooseCellKeysDirty)
	{
		m_LooseCellKeys.clear();
		m_LooseCellKeys.reserve(m_LooseVoxelCells.size());

		for (const std::pair<const uint32_t, LooseVoxelCell>& cell : m_LooseVoxelCells)
			m_LooseCellKeys.push_back(cell.first);

		std::sort(m_LooseCellKeys.begin(), m_LooseCellKeys.end());

		m_bLooseCellKeysDirty = false;
	}

	if (m_LooseValidateCursor >= m_LooseCellKeys.size())
		m_LooseValidateCursor = 0;

	const size_t uiValidateFrom = m_LooseValidateCursor;
	const size_t uiValidateTo = uiValidateFrom + k_uiLooseValidatePerFrame;

	m_LooseValidateCursor += k_uiLooseValidatePerFrame;

	std::vector<uint32_t> retired;

	for (size_t uiIndex = 0; uiIndex < m_LooseCellKeys.size(); ++uiIndex)
	{
		const uint32_t uiKey = m_LooseCellKeys[uiIndex];

		std::unordered_map<uint32_t, LooseVoxelCell>::iterator it = m_LooseVoxelCells.find(uiKey);

		if (it == m_LooseVoxelCells.end())
			continue;

		LooseVoxelCell& cell = it->second;

		if (uiIndex >= uiValidateFrom && uiIndex < uiValidateTo)
		{
			if (!ValidateLooseCell(uiKey, cell, v3Offset))
			{
				retired.push_back(uiKey);
				continue;
			}
		}

		/* Level space back to the window's grid space. */
		Vector3 v3Min = cell.Bounds.Min - v3Offset;
		Vector3 v3Max = cell.Bounds.Max - v3Offset;

		if (v3Max.x < 0.f || v3Max.y < 0.f || v3Max.z < 0.f ||
			v3Min.x > v3WindowMax.x || v3Min.y > v3WindowMax.y || v3Min.z > v3WindowMax.z)
			continue;

		/* Same one-voxel slack the renderer proxies carry: a voxel at index v
		   occupies [v, v + 1), and the ray wants to enter off the geometry. */
		v3Min = glm::max(v3Min - Vector3(1.f), Vector3(0.f));
		v3Max = glm::min(v3Max + Vector3(2.f), Vector3(m_v3WorldSize));

		StructuredVoxelBuffer buffer;
		buffer.Position = (v3Min + v3Max) * 0.5f;
		buffer.Extents = (v3Max - v3Min) * 0.5f;
		buffer.MapperID = 0;

		m_pRenderContext->Submit(buffer);

		if (bAudit)
		{
			Box proxy;
			proxy.Min = v3Min;
			proxy.Max = v3Max;

			m_AuditProxies.push_back(proxy);
		}
	}

	for (uint32_t uiKey : retired)
		m_LooseVoxelCells.erase(uiKey);

	if (!retired.empty())
		m_bLooseCellKeysDirty = true;
}

/* Bounds of the occupied bricks inside a window-space box, or false if none of
   them holds anything. Brick granularity is deliberate: it is what the grid
   counts, it is 512x fewer lookups than asking per voxel, and a box rounded out
   to whole bricks still contains every voxel it needs to. */
bool RenderSystem::FindOccupiedBrickBounds(
	VoxelBrickGrid& brickGrid, const UVector3& v3BrickGrid,
	const Vector3& v3Min, const Vector3& v3Max,
	Vector3& o_v3Min, Vector3& o_v3Max) const
{
	const uint32_t uiShift = VoxelBrickGrid::k_uiBrickShift;
	const uint32_t uiSize = VoxelBrickGrid::k_uiBrickSize;

	const UVector3 v3First(
		static_cast<uint32_t>(glm::max(v3Min.x, 0.f)) >> uiShift,
		static_cast<uint32_t>(glm::max(v3Min.y, 0.f)) >> uiShift,
		static_cast<uint32_t>(glm::max(v3Min.z, 0.f)) >> uiShift);

	const UVector3 v3Last(
		glm::min(static_cast<uint32_t>(glm::max(v3Max.x, 0.f)) >> uiShift, v3BrickGrid.x - 1),
		glm::min(static_cast<uint32_t>(glm::max(v3Max.y, 0.f)) >> uiShift, v3BrickGrid.y - 1),
		glm::min(static_cast<uint32_t>(glm::max(v3Max.z, 0.f)) >> uiShift, v3BrickGrid.z - 1));

	bool bFound = false;

	for (uint32_t z = v3First.z; z <= v3Last.z; ++z)
	{
		for (uint32_t y = v3First.y; y <= v3Last.y; ++y)
		{
			for (uint32_t x = v3First.x; x <= v3Last.x; ++x)
			{
				const uint32_t uiBrick = x + y * v3BrickGrid.x + z * v3BrickGrid.x * v3BrickGrid.y;

				if (brickGrid.GetCount(false, uiBrick) == 0)
					continue;

				const Vector3 v3BrickMin(
					static_cast<float>(x << uiShift),
					static_cast<float>(y << uiShift),
					static_cast<float>(z << uiShift));

				const Vector3 v3BrickMax = v3BrickMin + Vector3(static_cast<float>(uiSize - 1));

				o_v3Min = bFound ? glm::min(o_v3Min, v3BrickMin) : v3BrickMin;
				o_v3Max = bFound ? glm::max(o_v3Max, v3BrickMax) : v3BrickMax;

				bFound = true;
			}
		}
	}

	return bFound;
}

/* Occupied bricks of the resident window that no AABB proxy contains.
 *
 * The voxel pass rasterizes proxy cubes and nothing else, so a voxel outside
 * every proxy is only ever drawn when some *other* model's box happens to cover
 * the pixel and the ray from that box's face runs into it. Whether one does
 * changes with the camera, which is what makes an uncovered voxel flicker
 * rather than simply disappear.
 *
 * Every voxel a VoxRenderer stamps is covered by construction now (see
 * PostTick). What is not is everything written into the window by something
 * that is not a renderer, and the brick grid is the cheapest place to see it:
 * it already knows which 8^3 blocks hold something, in ordinary cached memory.
 *
 * The y = 0 brick row is reported separately because the ground layer lives
 * there and is deliberately uncovered - PostProcessing composites it
 * analytically as an endless plane rather than marching it from a proxy.
 */
void RenderSystem::AuditProxyCoverage(float fDeltaTime)
{
	static const double s_fInterval =
		std::getenv("VOXAGINE_COVERAGE_AUDIT") ? atof(std::getenv("VOXAGINE_COVERAGE_AUDIT")) : 0.0;

	if (s_fInterval <= 0.0)
		return;

	static double s_fElapsed = 0.0;

	s_fElapsed += fDeltaTime;

	if (s_fElapsed < s_fInterval)
		return;

	s_fElapsed = 0.0;

	VoxelBrickGrid& brickGrid = m_pRenderContext->GetBrickGrid();

	const UVector3 v3Grid = brickGrid.GetGridSize();
	const uint32_t uiBricks = brickGrid.GetBrickCount();

	if (uiBricks == 0)
		return;

	std::vector<uint8_t> covered(uiBricks, 0);

	const uint32_t uiShift = VoxelBrickGrid::k_uiBrickShift;

	for (const Box& proxy : m_AuditProxies)
	{
		const IVector3 v3Min = glm::max(IVector3(glm::floor(proxy.Min)), IVector3(0));
		const IVector3 v3Max = glm::min(IVector3(glm::floor(proxy.Max)),
			IVector3(m_v3WorldSize) - IVector3(1));

		if (v3Min.x > v3Max.x || v3Min.y > v3Max.y || v3Min.z > v3Max.z)
			continue;

		for (int32_t z = v3Min.z >> uiShift; z <= (v3Max.z >> uiShift); ++z)
			for (int32_t y = v3Min.y >> uiShift; y <= (v3Max.y >> uiShift); ++y)
				for (int32_t x = v3Min.x >> uiShift; x <= (v3Max.x >> uiShift); ++x)
					covered[x + y * v3Grid.x + z * v3Grid.x * v3Grid.y] = 1;
	}

	uint32_t uiOccupied = 0;
	uint32_t uiUncovered = 0;
	uint32_t uiUncoveredAboveGround = 0;
	uint64_t uiUncoveredVoxels = 0;
	UVector3 v3Sample(0, 0, 0);

	for (uint32_t z = 0; z < v3Grid.z; ++z)
	{
		for (uint32_t y = 0; y < v3Grid.y; ++y)
		{
			for (uint32_t x = 0; x < v3Grid.x; ++x)
			{
				const uint32_t uiBrick = x + y * v3Grid.x + z * v3Grid.x * v3Grid.y;
				const uint32_t uiCount = brickGrid.GetCount(false, uiBrick);

				if (uiCount == 0)
					continue;

				++uiOccupied;

				if (covered[uiBrick])
					continue;

				++uiUncovered;
				uiUncoveredVoxels += uiCount;

				if (y > 0)
				{
					if (uiUncoveredAboveGround == 0)
						v3Sample = UVector3(x << uiShift, y << uiShift, z << uiShift);

					++uiUncoveredAboveGround;
				}
			}
		}
	}

	fprintf(stderr, "[coverage] %u of %u occupied bricks outside every proxy (%u above the ground row, %llu voxels total), %zu proxies; first uncovered at (%u %u %u)\n",
		uiUncovered, uiOccupied, uiUncoveredAboveGround,
		static_cast<unsigned long long>(uiUncoveredVoxels),
		m_AuditProxies.size(),
		v3Sample.x, v3Sample.y, v3Sample.z);
}

/* How far the box that shipped falls short of the voxels that exist, measured
 * rather than argued: the union in PostTick makes the shortfall invisible on
 * screen, and this is what says whether it was there.
 *
 * Reports the running worst offender every 600 renderers that fail, plus a
 * running rate, because the interesting cases are rotation- and
 * scale-dependent and a single frame's worst is not representative.
 */
void RenderSystem::AuditProxyBounds(VoxRenderer* pRenderer,
	const Vector3& v3ProxyMin, const Vector3& v3ProxyMax,
	const Vector3& v3StampMin, const Vector3& v3StampMax)
{
	static const bool s_bEnabled = std::getenv("VOXAGINE_BOUNDS_AUDIT") != nullptr;

	if (!s_bEnabled)
		return;

	static uint64_t s_uiChecked = 0;
	static uint64_t s_uiShort = 0;
	static float s_fWorst = 0.f;

	++s_uiChecked;

	if ((s_uiChecked % 200000) == 0)
		fprintf(stderr, "[bounds] %llu of %llu proxy submissions short of the stamp (%.1f%%), worst %.1f voxels\n",
			static_cast<unsigned long long>(s_uiShort),
			static_cast<unsigned long long>(s_uiChecked),
			100.0 * static_cast<double>(s_uiShort) / static_cast<double>(s_uiChecked),
			s_fWorst);

	const Vector3 v3Under = glm::max(v3ProxyMin - v3StampMin, v3StampMax + Vector3(1.f) - v3ProxyMax);
	const float fWorst = glm::max(v3Under.x, glm::max(v3Under.y, v3Under.z));

	if (fWorst <= 0.f)
		return;

	++s_uiShort;

	if (fWorst <= s_fWorst)
		return;

	s_fWorst = fWorst;

	fprintf(stderr, "[bounds] '%s' (%s): proxy short by %.1f voxels - proxy [%.0f %.0f %.0f]..[%.0f %.0f %.0f] stamp [%.0f %.0f %.0f]..[%.0f %.0f %.0f] (%llu of %llu submissions short)\n",
		pRenderer->GetOwner()->GetName().c_str(),
		pRenderer->GetModelFilePath().c_str(),
		fWorst,
		v3ProxyMin.x, v3ProxyMin.y, v3ProxyMin.z,
		v3ProxyMax.x, v3ProxyMax.y, v3ProxyMax.z,
		v3StampMin.x, v3StampMin.y, v3StampMin.z,
		v3StampMax.x, v3StampMax.y, v3StampMax.z,
		static_cast<unsigned long long>(s_uiShort),
		static_cast<unsigned long long>(s_uiChecked));
}

void RenderSystem::CheckRendererChange(VoxRenderer* pRenderer)
{
	if (pRenderer->IsEnabled() != pRenderer->m_BakeData.IsEnabled)
	{
		pRenderer->m_BakeData.Updated = true;
		pRenderer->m_BakeData.IsEnabled = pRenderer->IsEnabled();
		return;
	}
	
	if (pRenderer->m_BakeData.Updated)
		return;

	if (pRenderer->IsFrameChanged())
	{
		pRenderer->ResetFrameChanged();
		pRenderer->m_BakeData.Updated = true;
		return;
	}

	Transform* pTransform = pRenderer->GetTransform();
	Vector3 scale = pTransform->GetScale();

	if (
		scale.x != pRenderer->m_BakeData.LastScale.x ||
		scale.y != pRenderer->m_BakeData.LastScale.y ||
		scale.z != pRenderer->m_BakeData.LastScale.z
		)
	{
		pRenderer->m_BakeData.Updated = true;
		return;
	}

	Vector3 position = pTransform->GetPosition();
	//Vector3 position = m_pPhysicsSystem->m_VoxelGrid.WorldToGrid(pTransform->GetPosition());

	/* glm::distance is for vectors; on scalars this is just the absolute
	   difference. floor() also returned a double here, widening the compare. */
	if (
		std::fabs(pRenderer->m_BakeData.LastLocation.x - std::floor(position.x)) >= 1.0f ||
		std::fabs(pRenderer->m_BakeData.LastLocation.y - std::floor(position.y)) >= 1.0f ||
		std::fabs(pRenderer->m_BakeData.LastLocation.z - std::floor(position.z)) >= 1.0f
	)
	{
		pRenderer->m_BakeData.Updated = true;
		return;
	}

	Quaternion rotation = pTransform->GetRotation();

	if (
		pRenderer->m_BakeData.LastRotation.x != rotation.x ||
		pRenderer->m_BakeData.LastRotation.y != rotation.y ||
		pRenderer->m_BakeData.LastRotation.z != rotation.z ||
		pRenderer->m_BakeData.LastRotation.w != rotation.w
	)
	{
		pRenderer->m_BakeData.Updated = true;
	}
}
