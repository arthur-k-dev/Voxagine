#include "pch.h"

#include <algorithm>

#include "External/imgui/imgui.h"
#include "RenderContext.h"

#include "Core/Application.h"
#include "Core/Platform/Platform.h"
#include "Core/Settings.h"

#include "Core/Platform/Window/WindowContext.h"

#include "Core/ECS/World.h"
#include "Core/ECS/WorldManager.h"
#include "Core/ECS/Systems/Physics/PhysicsSystem.h"
#include "Core/ECS/Systems/Chunk/ChunkSystem.h"
#include "Core/ECS/Systems/Chunk/FarFieldBaker.h"

#include "Core/Platform/Rendering/FrameProfiler.h"
#include "RenderDefines.h"
#include "Core/Platform/Rendering/Managers/TextureManagerInc.h"
#include "Core/Platform/Rendering/CommandEngineInc.h"
#include "Core/Platform/Rendering/RenderContextInc.h"
#include "Core/Platform/Rendering/RenderPassInc.h"

/* For m_uiBindlessCapacity, the size of the descriptor array the packing below
   has to fit inside. It lives with the binding table because that is what
   declares the array, and the number is a Metal hardware limit rather than a
   renderer preference - the header says which one. */
#include "Core/Platform/Rendering/Vulkan/VKPassBindings.h"

/* Object */
#include "Core/Platform/Rendering/Objects/Shader.h"
#include "Core/Platform/Rendering/Objects/View.h"
#include "Core/Platform/Rendering/Objects/Buffer.h"
#include "Core/Platform/Rendering/Objects/Sampler.h"
#include "Core/Platform/Rendering/Objects/Mapper.h"

/* Passes */
#include "Core/Platform/Rendering/Passes/ParticlePass.h"
#include "Core/Platform/Rendering/Passes/DebugPass.h"
#include "Core/Platform/Rendering/Passes/PostProcessingPass.h"
#include "Core/Platform/Rendering/Passes/UIPass.h"
#include "Core/Platform/Rendering/Passes/SunShadowPass.h"
#include "Core/Platform/Rendering/Passes/VoxelPass.h"
#include "Core/Platform/Rendering/Passes/VoxelBakePass.h"
#include "Core/Platform/Rendering/Passes/VoxelModelPass.h"
#include "Core/Platform/Rendering/Passes/SunShadowModelPass.h"
#include "Core/Platform/Rendering/Passes/SunShadowCombinePass.h"

#include "Core/ECS/Components/VoxRenderer.h"
#include "Core/ECS/Systems/Rendering/ModelMeshStore.h"
#include "Core/Platform/Rendering/ModelMeshUpload.h"
#include "External/optick/optick.h"

#include "Editor/imgui/Contexts/ImContext.h"
#include "Editor/imgui/Contexts/VKImContext.h"

RenderContext::RenderContext(Platform* pPlatform)
{
	m_pPlatform = pPlatform;
}

void RenderContext::SetFadeValue(float fValue)
{
	m_fFader = fValue;
	m_bFaderUpdated = true;
}

RenderContext::~RenderContext()
{
	/* Settings is an Application member and outlives every render context, so
	   a subscription left behind here is a call into freed memory the next time
	   somebody changes a setting. Only reachable if a context is destroyed
	   without the application following it, which the editor's play mode is
	   close enough to that it is not worth relying on the order. */
	if (m_pPlatform != nullptr && m_pPlatform->GetApplication() != nullptr)
	{
		Settings& settings = m_pPlatform->GetApplication()->GetSettings();

		settings.RenderQualityChanged -= this;
		settings.FullscreenChanged -= this;
	}
}

void RenderContext::Initialize()
{
	Settings& settings = m_pPlatform->GetApplication()->GetSettings();
	settings.FullscreenChanged += Event<bool>::Subscriber(std::bind(&RenderContext::OnFullscreenChanged, this, std::placeholders::_1), this);

	/* The settings menu writes straight into Settings and does not know a
	   renderer exists; this is what turns a changed value into resized
	   attachments. See ApplyRenderSettings for what actually needs it. */
	settings.RenderQualityChanged += Event<>::Subscriber(std::bind(&RenderContext::ApplyRenderSettings, this), this);

	m_bIsFullscreen = settings.IsFullscreen();

	const UVector2 initialSize = m_bIsFullscreen
		? m_v2ScreenResolution
		: UVector2(m_pPlatform->GetWindowContext()->GetSize().x, m_pPlatform->GetWindowContext()->GetSize().y);

	m_v2RenderResolution = ConstrainToAspectRatio(initialSize.x, initialSize.y);

	m_pSettings = &m_pPlatform->GetApplication()->GetSettings();

	// Unit debug sphere
#if defined(EDITOR) || defined(_DEBUG)
	m_UnitDebugSphere.reserve(static_cast<size_t>(m_iSphereLineCount * 2.f));

	// Compute our step around each circle
	float twoPi = glm::pi<float>() * 2.f;
	float step = twoPi / m_iSphereResolution;

	// Create the loop on the XY plane first
	for (float a = 0.f; a < twoPi; a += step)
	{
		m_UnitDebugSphere.push_back(Vector3(std::cos(a), std::sin(a), 0.f));
		m_UnitDebugSphere.push_back(Vector3(std::cos(a + step), std::sin(a + step), 0.f));
	}

	// Next on the XZ plane
	for (float a = 0.f; a < twoPi; a += step)
	{
		m_UnitDebugSphere.push_back(Vector3(std::cos(a), 0.f, std::sin(a)));
		m_UnitDebugSphere.push_back(Vector3(std::cos(a + step), 0.f, std::sin(a + step)));
	}

	// Finally on the YZ plane
	for (float a = 0.f; a < twoPi; a += step)
	{
		m_UnitDebugSphere.push_back(Vector3(0.f, std::cos(a), std::sin(a)));
		m_UnitDebugSphere.push_back(Vector3(0.f, std::cos(a + step), std::sin(a + step)));
	}
#endif
}

PRenderContext* RenderContext::Get()
{
	return reinterpret_cast<PRenderContext*>(this);
}

TextureReadData* RenderContext::ReadTexture(const std::string& texturePath)
{
	return m_pTextureManager->ReadTexture(texturePath);
}

void RenderContext::WaitForGPU()
{
	for (auto& it : m_pCommandEngines)
	{
		it.second->WaitForGPU();
	}
}

void RenderContext::WaitForVoxelReaders()
{
	/* Retire only the engine that reads the voxel-side buffers, rather than
	   every engine WaitForGPU drains. Everything that host-writes a buffer a
	   previous VDirect submission may still be fetching from needs this and
	   nothing wider. */
	const auto iter = m_pCommandEngines.find("VDirect");
	if (iter != m_pCommandEngines.end() && iter->second != nullptr)
		iter->second->WaitForGPU();
}

void RenderContext::Submit(const RenderData& renderData)
{
	m_RenderList.push_back(renderData);
}

void RenderContext::Submit(const DebugLine& renderData)
{
#if defined(EDITOR) || defined(_DEBUG)
	Vector4 color = Vector4(renderData.m_Color.inst.Colors.r, renderData.m_Color.inst.Colors.g, renderData.m_Color.inst.Colors.b, renderData.m_Color.inst.Colors.a) * 0.00392156863f;
	m_DebugDrawLines.push_back({ Vector4(renderData.m_Start, 1.f), color });
	m_DebugDrawLines.push_back({ Vector4(renderData.m_End, 1.f), color });
#endif
}

void RenderContext::Submit(const DebugSphere& renderData)
{
#if defined(EDITOR) || defined(_DEBUG)
	Vector4 color = Vector4(renderData.m_Color.inst.Colors.r, renderData.m_Color.inst.Colors.g, renderData.m_Color.inst.Colors.b, renderData.m_Color.inst.Colors.a) * 0.00392156863f;

	// Create the loop on the XY plane first
	for (size_t i = 0; i < m_UnitDebugSphere.size(); ++i)
	{
		m_DebugDrawLines.push_back({ Vector4(renderData.m_Center + renderData.m_fRadius * m_UnitDebugSphere[i], 1.f), color });
	}
#endif
}

void RenderContext::Submit(const DebugBox& renderData)
{
#if defined(EDITOR) || defined(_DEBUG)
	Vector4 color = Vector4(renderData.m_Color.inst.Colors.r, renderData.m_Color.inst.Colors.g, renderData.m_Color.inst.Colors.b, renderData.m_Color.inst.Colors.a) * 0.00392156863f;

	/* FRONT FACE */
	m_DebugDrawLines.push_back({ Vector4(renderData.m_Center + Vector3(-renderData.m_Extents.x, renderData.m_Extents.y, -renderData.m_Extents.z), 1.f), color });
	m_DebugDrawLines.push_back({ Vector4(renderData.m_Center + Vector3(renderData.m_Extents.x, renderData.m_Extents.y, -renderData.m_Extents.z), 1.f), color });

	m_DebugDrawLines.push_back({ Vector4(renderData.m_Center + Vector3(renderData.m_Extents.x, renderData.m_Extents.y, -renderData.m_Extents.z), 1.f), color });
	m_DebugDrawLines.push_back({ Vector4(renderData.m_Center + Vector3(renderData.m_Extents.x, -renderData.m_Extents.y, -renderData.m_Extents.z), 1.f), color });

	m_DebugDrawLines.push_back({ Vector4(renderData.m_Center + Vector3(renderData.m_Extents.x, -renderData.m_Extents.y, -renderData.m_Extents.z), 1.f), color });
	m_DebugDrawLines.push_back({ Vector4(renderData.m_Center + Vector3(-renderData.m_Extents.x, -renderData.m_Extents.y, -renderData.m_Extents.z), 1.f), color });

	m_DebugDrawLines.push_back({ Vector4(renderData.m_Center + Vector3(-renderData.m_Extents.x, -renderData.m_Extents.y, -renderData.m_Extents.z), 1.f), color });
	m_DebugDrawLines.push_back({ Vector4(renderData.m_Center + Vector3(-renderData.m_Extents.x, renderData.m_Extents.y, -renderData.m_Extents.z), 1.f), color });

	/* BACK FACE */
	m_DebugDrawLines.push_back({ Vector4(renderData.m_Center + Vector3(-renderData.m_Extents.x, renderData.m_Extents.y, renderData.m_Extents.z), 1.f), color });
	m_DebugDrawLines.push_back({ Vector4(renderData.m_Center + Vector3(renderData.m_Extents.x, renderData.m_Extents.y, renderData.m_Extents.z), 1.f), color });

	m_DebugDrawLines.push_back({ Vector4(renderData.m_Center + Vector3(renderData.m_Extents.x, renderData.m_Extents.y, renderData.m_Extents.z), 1.f), color });
	m_DebugDrawLines.push_back({ Vector4(renderData.m_Center + Vector3(renderData.m_Extents.x, -renderData.m_Extents.y, renderData.m_Extents.z), 1.f), color });

	m_DebugDrawLines.push_back({ Vector4(renderData.m_Center + Vector3(renderData.m_Extents.x, -renderData.m_Extents.y, renderData.m_Extents.z), 1.f), color });
	m_DebugDrawLines.push_back({ Vector4(renderData.m_Center + Vector3(-renderData.m_Extents.x, -renderData.m_Extents.y, renderData.m_Extents.z), 1.f), color });

	m_DebugDrawLines.push_back({ Vector4(renderData.m_Center + Vector3(-renderData.m_Extents.x, -renderData.m_Extents.y, renderData.m_Extents.z), 1.f), color });
	m_DebugDrawLines.push_back({ Vector4(renderData.m_Center + Vector3(-renderData.m_Extents.x, renderData.m_Extents.y, renderData.m_Extents.z), 1.f), color });

	/* TOP */
	m_DebugDrawLines.push_back({ Vector4(renderData.m_Center + Vector3(-renderData.m_Extents.x, renderData.m_Extents.y, -renderData.m_Extents.z), 1.f), color });
	m_DebugDrawLines.push_back({ Vector4(renderData.m_Center + Vector3(-renderData.m_Extents.x, renderData.m_Extents.y, renderData.m_Extents.z), 1.f), color });

	m_DebugDrawLines.push_back({ Vector4(renderData.m_Center + Vector3(renderData.m_Extents.x, renderData.m_Extents.y, -renderData.m_Extents.z), 1.f), color });
	m_DebugDrawLines.push_back({ Vector4(renderData.m_Center + Vector3(renderData.m_Extents.x, renderData.m_Extents.y, renderData.m_Extents.z), 1.f), color });

	/* BOTTOM */
	m_DebugDrawLines.push_back({ Vector4(renderData.m_Center + Vector3(-renderData.m_Extents.x, -renderData.m_Extents.y, -renderData.m_Extents.z), 1.f), color });
	m_DebugDrawLines.push_back({ Vector4(renderData.m_Center + Vector3(-renderData.m_Extents.x, -renderData.m_Extents.y, renderData.m_Extents.z), 1.f), color });

	m_DebugDrawLines.push_back({ Vector4(renderData.m_Center + Vector3(renderData.m_Extents.x, -renderData.m_Extents.y, -renderData.m_Extents.z), 1.f), color });
	m_DebugDrawLines.push_back({ Vector4(renderData.m_Center + Vector3(renderData.m_Extents.x, -renderData.m_Extents.y, renderData.m_Extents.z), 1.f), color });
#endif
}

void RenderContext::Submit(const SpriteData& renderData)
{
	m_SpriteList.push_back(renderData);
}

void RenderContext::PackBindlessTextures()
{
	/* Sentinel for "this texture has no slot yet this frame". UINT32_MAX is
	   also what TextureManager returns for a failed acquire, and the two
	   meanings do not collide: a sprite carrying it never had a texture, so it
	   correctly fails to find a slot below. */
	static constexpr uint32_t k_uiUnpackedTexture = UINT32_MAX;

	/* A sanity ceiling on the ID space, not on how many textures may live.
	   This project ships 133 of them and IDs are recycled, so four figures is
	   already far past anything real. */
	static constexpr uint32_t k_uiMaxTextureID = 65536;

	m_BindlessTextureIDs.clear();
	m_PackedSpriteList = m_SpriteList;

	for (SpriteData& sprite : m_PackedSpriteList)
	{
		const uint32_t uiTextureID = sprite.TextureID;

		/* Catches both UINT32_MAX, which is what TextureManager returns when a
		   texture was never created, and any other implausible value. The
		   scratch lookup below is indexed by texture ID, so a wild one would
		   size it by that number rather than by the live set; IDs are dense
		   and recycled (VKTextureManager::AcquireID), so anything up here is a
		   stale or corrupt reference. It used to be unreachable only because
		   the ID space itself was capped at 96.

		   Slot 0 is a real, uploaded texture - the descriptor writer
		   guarantees every slot is - so this draws the wrong image rather than
		   sampling a hole, which on MoltenVK is a GPU address fault and not a
		   black pixel. */
		if (uiTextureID >= k_uiMaxTextureID)
		{
			sprite.TextureID = 0;
			continue;
		}

		if (uiTextureID >= m_BindlessSlotForTexture.size())
			m_BindlessSlotForTexture.resize(uiTextureID + 1, k_uiUnpackedTexture);

		uint32_t& uiSlot = m_BindlessSlotForTexture[uiTextureID];

		if (uiSlot == k_uiUnpackedTexture)
		{
			if (m_BindlessTextureIDs.size() >= VKPassBinding::m_uiBindlessCapacity)
			{
				/* Reachable only by drawing more than 96 *distinct* textures
				   in one frame, which is a different and much rarer thing than
				   the ID-indexed scheme's "more than 96 textures loaded". If
				   this ever fires the fix is to split the pass or batch by
				   texture, not to raise the capacity - 96 is the hardware
				   ceiling, not a preference. */
				if (!m_bWarnedBindlessWorkingSet)
				{
					fprintf(stderr,
						"[render] more than %u distinct textures in one frame; "
						"the rest will draw with the wrong image\n",
						VKPassBinding::m_uiBindlessCapacity);
					m_bWarnedBindlessWorkingSet = true;
				}

				sprite.TextureID = 0;
				continue;
			}

			uiSlot = static_cast<uint32_t>(m_BindlessTextureIDs.size());
			m_BindlessTextureIDs.push_back(uiTextureID);
		}

		sprite.TextureID = uiSlot;
	}

	/* Only the entries actually touched, so this costs the working set rather
	   than the whole ID space. The scratch vector has to come out of here
	   clean because nothing else resets it. */
	for (uint32_t uiTextureID : m_BindlessTextureIDs)
		m_BindlessSlotForTexture[uiTextureID] = k_uiUnpackedTexture;

	/* An early warning for the one thing that can still overflow. The failure
	   it precedes is not a crash or a validation error - textures simply
	   sample the wrong image, which on a phone looks like a content bug and
	   was diagnosed as one for a long time. Reporting the high-water mark as
	   it approaches the ceiling is what turns that into a build-time signal
	   instead of a device-only mystery.
	 *
	 * Reported once per new peak above the threshold, so a level that sits at
	 * 40 says nothing at all and one creeping towards 96 says so a few times
	 * and then stops. */
	static constexpr size_t k_uiReportWorkingSetAbove =
		(VKPassBinding::m_uiBindlessCapacity * 3) / 4;

	if (m_BindlessTextureIDs.size() > m_uiPeakBindlessWorkingSet)
	{
		m_uiPeakBindlessWorkingSet = m_BindlessTextureIDs.size();

		if (m_uiPeakBindlessWorkingSet > k_uiReportWorkingSetAbove)
		{
			fprintf(stderr, "[render] bindless texture working set at %zu of %u\n",
				m_uiPeakBindlessWorkingSet, VKPassBinding::m_uiBindlessCapacity);
		}
	}
}

void RenderContext::Submit(StructuredVoxelBuffer& renderData)
{
	renderData.Distance = glm::distance(Vector3(renderData.Position), Vector3(m_CameraData.m_WorldPos));
	m_AABBList.push_back(renderData);
}

uint32_t RenderContext::SubmitModelInstance(const ModelInstanceData& instance)
{
	m_ModelInstances.push_back(instance);
	return static_cast<uint32_t>(m_ModelInstances.size() - 1);
}

void RenderContext::SubmitModelQuads(uint32_t uiInstanceIndex, uint32_t uiFirstQuad, uint32_t uiQuadCount)
{
	m_ModelQuadInstances.reserve(m_ModelQuadInstances.size() + uiQuadCount);

	for (uint32_t i = 0; i < uiQuadCount; i++)
		m_ModelQuadInstances.push_back({ uiInstanceIndex, uiFirstQuad + i });
}

void RenderContext::SyncModelMeshStore()
{
	const ModelMeshStore& store = ModelMeshStore::Get();
	const uint32_t uiCurrentQuads = store.GetTotalQuadCount();

	if (uiCurrentQuads == m_uiModelMeshUploadedQuads)
		return;

	/* Which range to copy is decided by ModelMeshUpload::PlanUpload, which is
	   pure and unit-tested against a simulated allocator - the whole of the
	   defect this guards against was that arithmetic, not any Vulkan call. Its
	   header carries the reasoning; the two rules are that a reallocation
	   invalidates everything already uploaded, and that growth is geometric so
	   reallocations stay rare. */
	const uint32_t uiRequiredWords = uiCurrentQuads * ModelMeshUpload::k_uiWordsPerQuad;

	const ModelMeshUpload::Plan plan = ModelMeshUpload::PlanUpload(
		uiCurrentQuads, m_uiModelMeshUploadedQuads, m_uiModelMeshCapacityWords);

	if (uiCurrentQuads < m_uiModelMeshUploadedQuads)
	{
		fprintf(stderr,
			"[render] the model mesh store shrank (%u -> %u quads); re-uploading it whole\n",
			m_uiModelMeshUploadedQuads, uiCurrentQuads);
	}

	if (plan.bReallocate)
	{
		if (m_uiModelMeshCapacityWords != 0)
			WaitForGPU();

		m_pModelMeshMapper->Resize(plan.uiNewCapacityWords, sizeof(uint32_t));
		m_uiModelMeshCapacityWords = plan.uiNewCapacityWords;
	}

	/* Quad words are immutable once meshed, and an older command buffer cannot
	   index into an appended range because its instance list was built against
	   m_uiModelMeshUploadedQuads - but the allocation is shared, and
	   HOST_COHERENT memory does not order a host write against a fetch already
	   in flight. Retire that reader first. */
	WaitForVoxelReaders();

	std::memcpy(
		m_pModelMeshMapper->GetData() + plan.uiFirstWord,
		store.GetQuads().data() + plan.uiFirstWord,
		static_cast<size_t>(plan.uiWordCount) * sizeof(uint32_t));

	m_uiModelMeshUploadedQuads = uiCurrentQuads;

	/* VOXAGINE_MODEL_MESH_AUDIT=1: the whole uploaded range must equal the
	   store. This is the check that would have caught the append-into-a-fresh-
	   allocation bug above the moment it was written, and the failure it looks
	   for is invisible in any single frame - a model that stopped being drawn
	   looks like content, not like corruption.
	 *
	 * Env-gated because it reads the mapping back, and the mapping is device-
	 * local host-visible, so every word is a PCIe read of VRAM. */
	static const bool s_bAuditMeshStore = std::getenv("VOXAGINE_MODEL_MESH_AUDIT") != nullptr;

	if (s_bAuditMeshStore)
	{
		const uint32_t* pUploaded = m_pModelMeshMapper->GetData();
		const uint32_t* pSource = store.GetQuads().data();

		for (uint32_t i = 0; i < uiRequiredWords; ++i)
		{
			if (pUploaded[i] == pSource[i])
				continue;

			fprintf(stderr,
				"[mesh-audit] word %u of %u disagrees (uploaded 0x%08x, store 0x%08x) "
				"- quad %u, so every model meshed below that is drawing garbage\n",
				i, uiRequiredWords, pUploaded[i], pSource[i], i / 3u);
			break;
		}
	}
}

void RenderContext::SortAABBs()
{
	//Vector3 invRayDirection = Vector3::Transform(Vector3(0.0f, 0.0f, 1.0f), m_CameraData.m_ModelView);
	//CLEANUP

	std::sort(m_AABBList.begin(), m_AABBList.end(),
		[&](const StructuredVoxelBuffer& a, const StructuredVoxelBuffer& b) -> bool
	{
		return a.Distance < b.Distance;
	});
}

void RenderContext::EnableDebugLines(bool bEnabled)
{
	m_bDebugEnabled = bEnabled;
	m_bDebugCleared = false;
}

bool RenderContext::ResizeWorldBuffer()
{
	/* Called from the world load and unload paths, where the top world can be
	   absent or half-constructed - a sprite-only menu world has no physics
	   system at all. Every one of these was an unchecked dereference. */
	Application* pApplication = m_pPlatform->GetApplication();
	World* pWorld = pApplication->GetWorldManager().GetTopWorld();
	if (pWorld == nullptr)
		return false;

	PhysicsSystem* pPhysics = pWorld->GetSystem<PhysicsSystem>();
	if (pPhysics == nullptr)
		return false;

	VoxelGrid* pGrid = pPhysics->GetVoxelGrid();
	if (pGrid == nullptr)
		return false;

	UVector3 uWorldSize;
	pGrid->GetDimensions(
		uWorldSize.x,
		uWorldSize.y,
		uWorldSize.z
	);

	// Has changed?
	bool bChanged =  m_pVoxelMapper->Resize(uWorldSize.x * uWorldSize.y * uWorldSize.z, sizeof(uint32_t));
	m_pVoxelData = m_pVoxelMapper->GetData();

	/* Same dimensions is the common case: every world load and every resume
	   asks, and every world in this game has the same 3x3 window. Both callers
	   answer a false return with ClearVoxels, which zeroes the mapping and calls
	   ClearAll - so re-zeroing the five levels here and then flushing all
	   10.8 M mirror cells is a full pyramid rebuild that is discarded a line
	   later. The reallocating path is unchanged. */
	const bool bGridChanged = m_BrickGrid.GetWorldSize() != uWorldSize;

	if (bGridChanged)
	{
		/* The brick grid describes this window, so it follows the same resize.
		   Order matters: the grid drops its mirror pointers first, the mapper is
		   then free to reallocate underneath it, and Flush repopulates. */
		m_BrickGrid.Resize(uWorldSize);
		m_pBrickMapper->Resize(m_BrickGrid.GetBrickCount(), sizeof(uint32_t));
		m_BrickGrid.SetBuffers(m_pBrickMapper->GetData(), m_pBrickMapper->GetBackBufferData());

		/* Same order again for the coverage texture and its staging: the grid is
		   holding pointers into a mapper that is about to be reallocated, so it
		   drops them (in Resize, above), the mapper resizes, and Flush repopulates
		   both mirrors. The texture's mip 0 is the pyramid's finest level, so its
		   extent is that level's grid rather than the window's. */
		if (m_BrickGrid.GetFineCellCount() > 0)
		{
			m_pPyramidStaging->Resize(m_BrickGrid.GetFineCellCount(), sizeof(uint32_t));

			m_BrickGrid.SetDensityBuffers(
				m_pPyramidStaging->GetData(),
				m_pPyramidStaging->GetBackBufferData());

			m_pPyramidView->Resize(m_BrickGrid.GetFineGridSize());
		}

		m_BrickGrid.Flush();
	}

	/* Whatever the bakers stamped is gone. See GetVoxelGeneration. */
	if (bChanged || bGridChanged)
		++m_uiVoxelGeneration;

	return bChanged;
}

uint32_t RenderContext::ValidateBrickGrid()
{
	return m_BrickGrid.Validate(false, m_pVoxelData);
}

uint32_t RenderContext::ValidateVoxelPyramid()
{
	if (m_pPyramidView == nullptr || m_pPyramidView->GetNative() == nullptr ||
		m_pPyramidStaging == nullptr || m_pPyramidStaging->GetData() == nullptr)
	{
		fprintf(stderr, "[pyramid] no coverage texture to validate\n");
		return 0;
	}

	const UVector3 v3Base = m_pPyramidView->GetInfo().m_Size;
	const uint32_t uiMips = m_pPyramidView->GetNative()->GetMipLevels();

	std::vector<UVector3> levelSize;
	std::vector<size_t> levelOffset;
	size_t uiTotal = 0;

	for (uint32_t uiLevel = 0; uiLevel < uiMips; ++uiLevel)
	{
		const UVector3 v3Size(
			std::max(v3Base.x >> uiLevel, 1u),
			std::max(v3Base.y >> uiLevel, 1u),
			std::max(v3Base.z >> uiLevel, 1u));

		levelSize.push_back(v3Size);
		levelOffset.push_back(uiTotal);

		uiTotal += static_cast<size_t>(v3Size.x) * v3Size.y * v3Size.z;
	}

	Mapper::Info readbackDesc;
	readbackDesc.m_Name = "Voxel Pyramid Readback";
	readbackDesc.m_ColorFormat = E_UNKNOWN;
	readbackDesc.m_GPUAccessType = E_READ_ONLY;

	std::unique_ptr<Mapper> pReadback = std::make_unique<Mapper>(Get(), readbackDesc, false);
	pReadback->Resize(static_cast<uint32_t>(uiTotal), sizeof(uint32_t));

	/* The uploads are recorded on VDirect and read back here on Texture, and
	   two submissions to the same queue carry no dependency on each other. A
	   readback that overtakes the frame that filled the texture reports the
	   part of the upload that had not landed yet, which reads exactly like a
	   missed dirty region. */
	m_pCommandEngines["VDirect"]->WaitForGPU();

	PCommandEngine* pEngine = m_pCommandEngines["Texture"]->Get();

	if (pReadback->GetNative() == nullptr ||
		!pEngine->ReadbackImageMips(m_pPyramidView->GetNative(), pReadback->GetNative(), sizeof(uint32_t)))
	{
		fprintf(stderr, "[pyramid] readback of the coverage texture failed\n");
		return 0;
	}

	const uint32_t* pRead = pReadback->GetData();
	const uint32_t* pMirror = m_pPyramidStaging->GetData();

	uint32_t uiMismatches = 0;

	/* Mip 0 against the mirror the CPU maintains, exactly. This is the check
	   that matters: ValidateBrickGrid proves the mirror against the voxel
	   buffer, and nothing else proves that what the GPU holds is the mirror.
	   A disagreement here is a dirty region that was never uploaded - stale
	   ambient occlusion, which is invisible in a code review and reads as art
	   in the image. */
	{
		const size_t uiFine = static_cast<size_t>(levelSize[0].x) * levelSize[0].y * levelSize[0].z;

		uint32_t uiBad = 0;
		size_t uiFirst = 0;

		for (size_t i = 0; i < uiFine; ++i)
		{
			if (pRead[i] == pMirror[i])
				continue;

			if (uiBad == 0)
				uiFirst = i;

			++uiBad;
		}

		if (uiBad > 0)
		{
			fprintf(stderr, "[pyramid] mip 0 disagrees with the staging mirror for %u of %zu texels, "
			                "first at %zu (texture %08X, mirror %08X)\n",
			        uiBad, uiFine, uiFirst, pRead[uiFirst], pMirror[uiFirst]);
		}

		uiMismatches += uiBad;
	}

	/* Every coarser mip against the average of its eight children. The blit
	   that produced them is a linear resample, which *is* that average when
	   all three axes halve exactly and is something else when one of them does
	   not - so an odd axis is reported as unchecked rather than as wrong. */
	uint32_t uiSkipped = 0;

	for (uint32_t uiLevel = 1; uiLevel < uiMips; ++uiLevel)
	{
		const UVector3& v3Size = levelSize[uiLevel];
		const UVector3& v3Child = levelSize[uiLevel - 1];

		if (v3Child.x != v3Size.x * 2 || v3Child.y != v3Size.y * 2 || v3Child.z != v3Size.z * 2)
		{
			++uiSkipped;
			continue;
		}

		const uint32_t* pLevel = pRead + levelOffset[uiLevel];
		const uint32_t* pBelow = pRead + levelOffset[uiLevel - 1];

		uint32_t uiBad = 0;

		for (uint32_t uiZ = 0; uiZ < v3Size.z; ++uiZ)
		for (uint32_t uiY = 0; uiY < v3Size.y; ++uiY)
		for (uint32_t uiX = 0; uiX < v3Size.x; ++uiX)
		{
			/* Per channel: the radiance the cones gather is in RGB and the
			   occlusion in A, and a blit that got one right and the other wrong
			   is exactly the kind of thing this exists to catch. */
			uint32_t uiSum[4] = {};

			for (uint32_t uiChildZ = 0; uiChildZ < 2; ++uiChildZ)
			for (uint32_t uiChildY = 0; uiChildY < 2; ++uiChildY)
			for (uint32_t uiChildX = 0; uiChildX < 2; ++uiChildX)
			{
				const uint32_t uiChild = pBelow[(uiX * 2 + uiChildX)
					+ (uiY * 2 + uiChildY) * v3Child.x
					+ (uiZ * 2 + uiChildZ) * v3Child.x * v3Child.y];

				for (uint32_t uiChannel = 0; uiChannel < 4; ++uiChannel)
					uiSum[uiChannel] += (uiChild >> (uiChannel * 8)) & 0xFFu;
			}

			const uint32_t uiActual = pLevel[uiX + uiY * v3Size.x + uiZ * v3Size.x * v3Size.y];

			for (uint32_t uiChannel = 0; uiChannel < 4; ++uiChannel)
			{
				const int32_t iExpected = static_cast<int32_t>((uiSum[uiChannel] + 4) / 8);
				const int32_t iChannel = static_cast<int32_t>((uiActual >> (uiChannel * 8)) & 0xFFu);

				/* Two, not zero: the hardware filter rounds its own way and the
				   reference above rounds to nearest. A stale level is off by far
				   more than the rounding of an eight-way mean. */
				if (std::abs(iChannel - iExpected) > 2)
				{
					++uiBad;
					break;
				}
			}
		}

		if (uiBad > 0)
		{
			fprintf(stderr, "[pyramid] mip %u (%ux%ux%u) disagrees with the average of its children "
			                "for %u cells\n", uiLevel, v3Size.x, v3Size.y, v3Size.z, uiBad);
		}

		uiMismatches += uiBad;
	}

	fprintf(stderr, "[pyramid] validated %zu texels over %u mips of %ux%ux%u: %u disagree%s\n",
	        uiTotal, uiMips, v3Base.x, v3Base.y, v3Base.z, uiMismatches,
	        uiSkipped > 0 ? " (levels with an odd axis are resampled rather than averaged and are not checked)" : "");

	return uiMismatches;
}

void RenderContext::UploadVoxelPyramid(PCommandEngine* pEngine)
{
	if (pEngine == nullptr || m_pPyramidView == nullptr || m_pPyramidStaging == nullptr)
		return;

	/* Nothing samples the pyramid unless a cone is traced through it, and the
	   only three that do are cone AO, the diffuse bounce and the environment
	   specular. With all three off this was still copying its dirty boxes into
	   a 3D texture every frame for no reader: measured on a Galaxy S23 at
	   exactly those settings, `Voxel Pyramid Upload` was 0.97 ms of a 19.1 ms
	   frame - 5% of the budget spent on an image nothing looked at.
	 *
	 * The mirror and the dirty set are still maintained on the CPU, which is
	 * what makes this safe to skip rather than merely cheap: VoxelBrickGrid
	 * keeps accumulating dirty regions while this is not draining them, so
	 * turning AO back on mid-frame uploads everything outstanding on the next
	 * pass rather than showing a stale texture. TakeDensityRegions' "the whole
	 * thing is wrong" path covers the case where that backlog grows past the
	 * point of being worth issuing box by box.
	 *
	 * Deliberately not gated on the *pass* existing - the voxel shader always
	 * declares the texture and the pass always binds it, because those are
	 * startup decisions and this is a per-frame one. An unread texture holding
	 * stale densities is fine; a descriptor pointing at nothing is not. */
	const Settings& settings = m_pPlatform->GetApplication()->GetSettings();

	const bool bPyramidRead =
		settings.GetAmbientQuality() >= AMQ_CONE ||
		settings.IsBounceLightEnabled() ||
		settings.IsReflectionEnabled();

	if (!bPyramidRead)
		return;

	if (m_pPyramidView->GetNative() == nullptr || m_pPyramidStaging->GetNative() == nullptr)
		return;

	/* True means the texture holds the wrong buffer's densities entirely - a
	   resize, a clear, a window slide, or more dirty boxes than are worth
	   issuing separately. See VoxelBrickGrid::TakeDensityRegions. */
	if (m_BrickGrid.TakeDensityRegions(m_PyramidRegions))
	{
		const UVector3& v3Fine = m_BrickGrid.GetFineGridSize();

		ImageRegion whole;
		whole.m_uiWidth = std::max(v3Fine.x, 1u);
		whole.m_uiHeight = std::max(v3Fine.y, 1u);
		whole.m_uiDepth = std::max(v3Fine.z, 1u);

		m_PyramidRegions.assign(1, whole);
	}

	if (m_PyramidRegions.empty())
		return;

	pEngine->UploadImageRegions(
		m_pPyramidView->GetNative(), m_pPyramidStaging->GetNative(),
		m_PyramidRegions.data(), static_cast<uint32_t>(m_PyramidRegions.size()),
		sizeof(uint32_t));
}

uint32_t RenderContext::ValidateFarField()
{
	return FarFieldBaker::Validate(
		m_pPlatform->GetApplication()->GetWorldManager().GetTopWorld(),
		m_FarField
	);
}

UVector3 RenderContext::GetFarFieldShaderGridSize() const
{
	return (m_bFarFieldEnabled && m_FarField.IsBuilt())
		? m_FarField.GetGridSize()
		: UVector3(0, 0, 0);
}

void RenderContext::BuildFarField(World* pWorld)
{
	ChunkSystem* pChunkSystem = pWorld != nullptr ? pWorld->GetChunkSystem() : nullptr;
	PhysicsSystem* pPhysics = pWorld != nullptr ? pWorld->GetSystem<PhysicsSystem>() : nullptr;

	if (pChunkSystem == nullptr || pPhysics == nullptr)
		return;

	/* The level is the chunk grid; the window is at most 3x3 of it. Height is
	   not chunked, so the window's Y is the level's Y. */
	const UVector2 v2LevelXZ = pChunkSystem->GetWorldSize();
	const UVector3 v3WindowSize = pPhysics->GetVoxelGrid()->GetDimensions();
	const UVector3 v3LevelSize(v2LevelXZ.x, v3WindowSize.y, v2LevelXZ.y);

	/* A level the window already covers - a 1x1 chunk grid, which every menu
	   world is - has no far field to draw. Building one would cost the memory
	   and the marching for a volume every ray is masked out of anyway. */
	if (v3LevelSize.x <= v3WindowSize.x && v3LevelSize.z <= v3WindowSize.z)
	{
		m_FarField.Resize(UVector3(0, 0, 0));
		return;
	}

	m_FarField.Resize(v3LevelSize);

	FarFieldBaker::Build(pWorld, m_FarField);

	PublishFarField();
}

void RenderContext::BeginFarFieldBuild(World* pWorld)
{
	ChunkSystem* pChunkSystem = pWorld != nullptr ? pWorld->GetChunkSystem() : nullptr;
	PhysicsSystem* pPhysics = pWorld != nullptr ? pWorld->GetSystem<PhysicsSystem>() : nullptr;

	if (pChunkSystem == nullptr || pPhysics == nullptr)
		return;

	const UVector2 v2LevelXZ = pChunkSystem->GetWorldSize();
	const UVector3 v3WindowSize = pPhysics->GetVoxelGrid()->GetDimensions();
	const UVector3 v3LevelSize(v2LevelXZ.x, v3WindowSize.y, v2LevelXZ.y);

	if (v3LevelSize.x <= v3WindowSize.x && v3LevelSize.z <= v3WindowSize.z)
	{
		CancelFarFieldBuild();
		m_FarField.Resize(UVector3(0, 0, 0));
		return;
	}

	m_FarField.Resize(v3LevelSize);

	FarFieldBaker::Begin(pWorld, m_FarField, m_FarFieldBuild);
}

bool RenderContext::ContinueFarFieldBuild(StreamingBudget::Scope& budget)
{
	if (!m_FarFieldBuild.bActive)
		return true;

	if (!FarFieldBaker::Continue(m_FarFieldBuild, budget))
		return false;

	PublishFarField();

	return true;
}

void RenderContext::CancelFarFieldBuild()
{
	FarFieldBaker::Cancel(m_FarFieldBuild);
}

void RenderContext::PublishFarField()
{
	if (!m_FarField.IsBuilt())
		return;

	/* Same order the window's brick grid is resized in: the grid drops its
	   mirror first, the mapper reallocates, then the mirrors are re-supplied.
	   Only the front buffer exists here - the volume never swaps. */
	m_FarFieldBricks.Resize(m_FarField.GetGridSize());

	m_pFarFieldMapper->Resize(m_FarField.GetCellCount(), sizeof(uint32_t));
	m_pFarFieldBrickMapper->Resize(m_FarFieldBricks.GetBrickCount(), sizeof(uint32_t));

	m_FarFieldBricks.SetBuffers(m_pFarFieldBrickMapper->GetData(), nullptr);

	m_FarField.Flush(m_pFarFieldMapper->GetData(), m_FarFieldBricks);

	/* The volume never changes again, so its pyramid is built once here rather
	   than from the frame loop. */
	m_FarFieldBricks.FlushDirty();

	ForceCameraDataUpdate();
}

uint32_t RenderContext::GetVoxel(uint32_t uiID) const
{
	/* Same guard the two write paths already carry. Callers arrive here with
	   ids derived from world positions and from baked position data, neither of
	   which is clamped on the way in, and an empty voxel reads as zero anyway -
	   so out of range answers "nothing here" rather than reading past the map. */
	if (uiID >= GetVoxelDataSize())
		return 0;

	return m_pVoxelData[uiID];
}

void RenderContext::ClearVoxels()
{
	memset(m_pVoxelMapper->GetData(), 0, m_pVoxelMapper->GetInfo().m_uiElementCount * m_pVoxelMapper->GetInfo().m_uiElementSize);
	memset(m_pVoxelMapper->GetBackBufferData(), 0, m_pVoxelMapper->GetInfo().m_uiElementCount * m_pVoxelMapper->GetInfo().m_uiElementSize);

	m_BrickGrid.ClearAll();

	++m_uiVoxelGeneration;
}

void RenderContext::Clear()
{
	OPTICK_CATEGORY("Rendercontext", Optick::Category::Rendering);
	OPTICK_EVENT();
	m_RenderList.clear();
	m_AABBList.clear();

	/* DYNAMIC_MODELS_PLAN.md phase 2 - cleared alongside the AABB list they
	   are submitted next to, in RenderSystem::PostTick. */
	m_ModelInstances.clear();
	m_ModelQuadInstances.clear();

#if defined(EDITOR) || defined(_DEBUG)
	m_DebugDrawLines.clear();
#endif
}

void RenderContext::FixedClear()
{
	m_SpriteList.clear();
}

bool RenderContext::Present()
{
	OPTICK_CATEGORY("Rendercontext", Optick::Category::Rendering);
	OPTICK_EVENT();

#ifndef _ORBIS
	// Render ImGui
	ImGui::Render();
#endif

	/* Rebuild the coverage pyramid over whatever was written this frame, before
	   anything is submitted (RENDERING_PLAN.md 7.1b). Here rather than at each
	   write site because the work per cell is the same whether one voxel of it
	   changed or thirty thousand, and a burst is where both extremes live -
	   maintaining it per voxel measured 3.9x the write cost of the bricks
	   alone. Main thread; chunk streaming marks the back buffer from job
	   threads and the marks are atomic. */
	/* Before the flush, and only while nothing is outstanding: at this point
	   the texture holds everything the last upload carried and the mirror
	   holds everything the last flush wrote, which is the one moment in the
	   frame when the two are supposed to be identical. RENDERING_PLAN.md 7.1b
	   route B; VOXAGINE_SYNC_AUDIT's counterpart for the coverage texture, and
	   run it *during destruction* for the same reason - a missed dirty region
	   is produced by a write and does not exist at rest. */
	{
		static const float s_fPyramidAuditInterval = []
		{
			const char* pValue = std::getenv("VOXAGINE_PYRAMID_AUDIT");

			return pValue != nullptr ? static_cast<float>(atof(pValue)) : 0.f;
		}();

		if (s_fPyramidAuditInterval > 0.f && !m_BrickGrid.HasPendingDensityRegions())
		{
			m_fPyramidAuditTimer += static_cast<float>(m_pPlatform->GetApplication()->GetTimer().GetElapsedSeconds());

			if (m_fPyramidAuditTimer >= s_fPyramidAuditInterval)
			{
				m_fPyramidAuditTimer = 0.f;
				ValidateVoxelPyramid();
			}
		}
	}

	m_BrickGrid.FlushDirty();

	// Hold timer that counts drawn frames
	float fDeltaTime = static_cast<float>(m_pPlatform->GetApplication()->GetTimer().GetElapsedSeconds());
	m_fFrameTimer += fDeltaTime;

	FrameProfiler::Get().Tick(fDeltaTime);

	/* Worst and 99th-percentile frame of the second, alongside the average.
	   An average cannot tell a stall from a latency problem - 200 fps with one
	   40 ms frame in it reads exactly the same as 200 fps that are all 5 ms,
	   and the two have nothing in common. The percentile is what separates "it
	   hitches now and then" from "every frame is late".
	   Sampled into a fixed ring so this costs a store per frame and no
	   allocation; the profiler is off in Release and this line is not. */
	if (m_uiFrameSamples < k_uiMaxFrameSamples)
		m_fFrameSamples[m_uiFrameSamples++] = fDeltaTime * 1000.f;

	if (m_fFrameTimer >= 1.0f)
	{
		m_fFrameTimer = std::fmod(m_fFrameTimer, 1.0f);

		m_uiFPS = m_uiDrawnFrames;
		m_uiDrawnFrames = 0;

		float fWorst = 0.f;
		float fP99 = 0.f;

		if (m_uiFrameSamples > 0)
		{
			std::sort(m_fFrameSamples, m_fFrameSamples + m_uiFrameSamples);

			fWorst = m_fFrameSamples[m_uiFrameSamples - 1];
			fP99 = m_fFrameSamples[(m_uiFrameSamples * 99) / 100];
		}

		fprintf(stderr, "[fps] %u  (frame ms: p99 %.2f, worst %.2f, over %u samples)\n",
		        m_uiFPS, fP99, fWorst, m_uiFrameSamples);

		m_uiFrameSamples = 0;
	}
	
#if defined(_DEBUG) || defined(EDITOR)
	Buffer* pDebugBuffer = m_mBuffers["Debug Lines"].get();
	PRenderPass* pDebugPass = m_pRenderPasses["Debug Renderer"].get();

	// Draw once with cleared debug line list
	if (!m_bDebugEnabled && !m_bDebugCleared)
	{
		m_DebugDrawLines.clear();
	}
#endif

	Settings& settings = GetPlatform()->GetApplication()->GetSettings();

	// Present
	Buffer* pCameraBuffer = m_mBuffers["Camera Data"].get();
	Buffer* pAABBBuffer = m_mBuffers["AABB Data"].get();

	Buffer* pSpriteBuffer = m_mBuffers["Sprite Data"].get();

	/* DYNAMIC_MODELS_PLAN.md phase 2. */
	Buffer* pModelInstanceBuffer = m_mBuffers["Model Instance Data"].get();
	Buffer* pModelQuadInstanceBuffer = m_mBuffers["Model Quad Instance Data"].get();

	PRenderPass* pParticlePass = m_pRenderPasses["Particles"].get();
	PRenderPass* pSunShadowPass = m_pRenderPasses["Sun Shadow"].get();
	PRenderPass* pSunShadowModelPass = m_pRenderPasses["Sun Shadow Models"].get();
	PRenderPass* pSunShadowCombinePass = m_pRenderPasses["Sun Shadow Combine"].get();
	PRenderPass* pVoxelModelPass = m_pRenderPasses["Voxel Models"].get();
	PRenderPass* pVoxelPass = m_pRenderPasses["Voxel"].get();
	PRenderPass* pUIPass = m_pRenderPasses["UI Renderer"].get();
	PRenderPass* pPostProcessingPass = m_pRenderPasses["Post Processing"].get();

	PCommandEngine* pVDirectEngine = m_pCommandEngines["VDirect"].get();

	PCommandEngine* pDirectEngine = m_pCommandEngines["Direct"].get();

	const bool bIsCompleted = pVDirectEngine->GetCompletedValue() >= pVDirectEngine->GetValue();

	if (bIsCompleted && !m_bIsDrawTextureCopied)
	{
		// Reset command allocators
		if (pDirectEngine->GetValue() > 0)
			pDirectEngine->AdvanceFrame();

		/* Copy target texture to to-be-drawn texture */
		pDirectEngine->Reset();
		pDirectEngine->Start();

		View* pSource = pVoxelPass->GetTargetView();
		pVoxelPass->ToggleBackBuffer();
		View* pTarget = pVoxelPass->GetTargetView();
		pVoxelPass->ToggleBackBuffer();

		// Transition
		pDirectEngine->QueueBarrier(pTarget->GetNative(), E_STATE_COPY_DEST);

		pDirectEngine->QueueBarrier(pSource->GetNative(), E_STATE_COPY_SOURCE);

		pDirectEngine->ApplyBarriers();

		pDirectEngine->CopyResource(pTarget->GetNative(), pSource->GetNative());

		// Transition
		pDirectEngine->QueueBarrier(pTarget->GetNative(), E_STATE_PIXEL_SHADER_RESOURCE);

		pDirectEngine->QueueBarrier(pSource->GetNative(), E_STATE_PIXEL_SHADER_RESOURCE);

		pDirectEngine->ApplyBarriers();

		pDirectEngine->Execute();
		pDirectEngine->AdvanceFrame();

		m_bIsDrawTextureCopied = true;
		m_uiDrawnFrames++;
	}

	// Upload buffers
	if (bIsCompleted) 
	{
		m_uiMissedFrames = 0;
		m_bIsDrawTextureCopied = false;

		Application* pApplication = m_pPlatform->GetApplication();
		World* pWorld = pApplication->GetWorldManager().GetTopWorld();

		/* An editor build loads no world at startup - VoxApp only does that
		   under !EDITOR - so there is nothing to read particles from until one
		   is opened. This was an unconditional dereference. */
		PhysicsSystem* pPhysics = pWorld != nullptr ? pWorld->GetSystem<PhysicsSystem>() : nullptr;

		m_uiParticleCount = pPhysics != nullptr ? pPhysics->m_uiActiveParticleCount : 0;

		// Camera buffer
		{
			UVector2 v2Size = GetRenderResolution();
			v2Size.x *= m_fRenderScale;
			v2Size.y *= m_fRenderScale;

			Vector4 v4LightDirection = glm::normalize(Vector4(-0.4f, -0.8f, 0.6f, 0.0f));

			/* An editor build loads no world at startup, so there is no physics
			   system to take the grid from until one is opened. A zero world
			   size makes the marcher's bounds test fail immediately, which is
			   what "no world" should look like. */
			VoxelGrid* pGrid = pPhysics != nullptr ? pPhysics->GetVoxelGrid() : nullptr;

			UVector3 uWorldSize(0, 0, 0);

			if (pGrid != nullptr)
			{
				pGrid->GetDimensions(
					uWorldSize.x,
					uWorldSize.y,
					uWorldSize.z
				);
			}

			pCameraBuffer->Clear();

			pCameraBuffer->AddConstantData(m_CameraData.m_MVP);
			pCameraBuffer->AddConstantData(m_CameraData.m_ModelView);

			pCameraBuffer->AddConstantData(m_CameraData.m_WorldPos);
			pCameraBuffer->AddConstantData(m_CameraData.m_CameraOffset);

			pCameraBuffer->AddConstantData(Vector4(
				static_cast<float>(v2Size.x), static_cast<float>(v2Size.y),
				m_CameraData.m_bIsOrthographic ? 0 : m_CameraData.m_fProjectionValue, m_CameraData.m_fAspectRatio
			));

			pCameraBuffer->AddConstantData(v4LightDirection);
			pCameraBuffer->AddConstantData(UVector4(uWorldSize, 1.0));

			pCameraBuffer->AddConstantData(settings.GetResolutionScale());
			pCameraBuffer->AddConstantData(m_fFader);

			pCameraBuffer->AddConstantData(m_uiParticleCount);
			pCameraBuffer->AddConstantData(static_cast<uint32_t>(GetAABBList().size()));

			/* Far-field cell grid (RENDERING_PLAN.md phase 4), or zero when
			   there is none - a level the window already covers, a build that
			   found nothing, or the runtime toggle off. FarField.hlsl tests
			   this before touching either far-field mapper, which is what
			   keeps the descriptors valid at one dummy element until a world
			   with a real far field is loaded. */
			pCameraBuffer->AddConstantData(UVector4(GetFarFieldShaderGridSize(), 1));

			/* The camera of the *previous* upload. Post processing samples a
			   copy of the voxel target taken at the top of this frame, so the
			   image it composites was rendered with that camera rather than the
			   one being uploaded now, and anything reconstructing a world-space
			   ray there has to match it - see CameraData.hlsl's sceneInvMvp.
			   Written after the current values and before they are latched, so
			   the buffer carries both. */
			pCameraBuffer->AddConstantData(m_PreviousSceneCamera.m_InvMVP);
			pCameraBuffer->AddConstantData(m_PreviousSceneCamera.m_WorldPos);
			pCameraBuffer->AddConstantData(m_PreviousSceneCamera.m_Offset);

			m_PreviousSceneCamera.m_InvMVP = glm::inverse(m_CameraData.m_MVP);
			m_PreviousSceneCamera.m_WorldPos = m_CameraData.m_WorldPos;
			m_PreviousSceneCamera.m_Offset = m_CameraData.m_CameraOffset;

			/* Light-space frame for the sun shadow map - RENDERING_PLAN.md
			   7.1a, and CameraData.hlsl documents the mapping. Computed here
			   rather than in a shader because both inputs are effectively
			   constant: lightDirection is a fixed engine value and uWorldSize
			   only moves when the voxel grid is resized.

			   The basis is built off world up rather than off an arbitrary
			   orthogonal vector, so the map's V axis stays roughly vertical.
			   That matters for nothing mathematically and for a lot when
			   reading the map in a debug view. */
			{
				const Vector3 v3Light(v4LightDirection);

				Vector3 v3Tangent = glm::cross(v3Light, Vector3(0.f, 1.f, 0.f));

				/* Degenerate only for a light pointing straight down, which
				   this one does not - but it is a fixed constant today and a
				   setting tomorrow. */
				v3Tangent = glm::dot(v3Tangent, v3Tangent) > 1e-6f
					? glm::normalize(v3Tangent)
					: Vector3(1.f, 0.f, 0.f);

				const Vector3 v3Bitangent = glm::normalize(glm::cross(v3Light, v3Tangent));

				/* The window's projection onto the basis. A box's support along
				   an axis is the sum of its extents weighted by |axis|, so the
				   eight corners never have to be enumerated - and the box's
				   near corner in window space is the origin, so the minimum is
				   just the negative part of each weighted extent. */
				const Vector3 v3Extent(
					static_cast<float>(uWorldSize.x),
					static_cast<float>(uWorldSize.y),
					static_cast<float>(uWorldSize.z));

				auto AxisRange = [&v3Extent](const Vector3& v3Axis, float& fMin, float& fMax)
				{
					fMin = 0.f;
					fMax = 0.f;

					for (int i = 0; i < 3; ++i)
					{
						const float fTerm = v3Axis[i] * v3Extent[i];

						if (fTerm < 0.f)
							fMin += fTerm;
						else
							fMax += fTerm;
					}
				};

				float fMinU, fMaxU, fMinV, fMaxV, fMinW, fMaxW;

				AxisRange(v3Tangent, fMinU, fMaxU);
				AxisRange(v3Bitangent, fMinV, fMaxV);
				AxisRange(v3Light, fMinW, fMaxW);

				const float fSizeU = std::max(fMaxU - fMinU, 1.f);
				const float fSizeV = std::max(fMaxV - fMinV, 1.f);

				pCameraBuffer->AddConstantData(Vector4(v3Tangent, 0.f));
				pCameraBuffer->AddConstantData(Vector4(v3Bitangent, 0.f));
				pCameraBuffer->AddConstantData(Vector4(fMinU, fMinV, fSizeU, fSizeV));

				/* World units per shadow texel on each axis, which is what a
				   PCSS filter radius has to be converted through, plus the
				   plane the march starts from.

				   The resolution is the setting's, not a constant: the map is
				   half size at SHQ_HARD, so a texel covers twice the world and
				   a filter radius quoted in texels means twice as much. Reading
				   a fixed 1024 here would put every shadow edge at half the
				   width it should be the moment the quality dropped. */
				const float fShadowResolution =
					static_cast<float>(settings.GetSunShadowResolution());

				pCameraBuffer->AddConstantData(Vector4(
					fMinW,
					fSizeU / fShadowResolution,
					fSizeV / fShadowResolution,
					fMaxW - fMinW));
			}

			/* Render quality - CameraData.hlsl's renderQuality, and Settings is
			   the authority on every component. Uploaded unconditionally rather
			   than on a change flag: the buffer is rewritten field by field
			   whenever anything in it moves, so a conditional append would
			   shift every field after it on the frames it skipped. */
			{
				uint32_t uiFlags = 0;

				if (settings.IsBounceLightEnabled())
					uiFlags |= 1u;

				if (settings.IsReflectionEnabled())
					uiFlags |= 2u;

				if (settings.IsFXAAEnabled())
					uiFlags |= 4u;

				pCameraBuffer->AddConstantData(Vector4(
					static_cast<float>(settings.GetShadowQuality()),
					static_cast<float>(settings.GetAmbientQuality()),
					static_cast<float>(uiFlags),
					static_cast<float>(settings.GetSunShadowResolution())));

				/* CameraData.hlsl's renderTuning. */
				pCameraBuffer->AddConstantData(Vector4(
					settings.GetShadowRayDistance(), 0.f, 0.f, 0.f));
			}

			pCameraBuffer->Allocate();

			//Sets the forced data update to false
			m_bCameraDataUpdated = false;
			m_bFaderUpdated = false;
		}

		// AABB buffer. Uploaded every frame: the list is rebuilt each frame and
		// entity AABBs move whether or not any voxel did, so gating the upload
		// on a "the world changed" flag rendered from a stale list. It is ~32
		// bytes per drawn model, so the upload is not worth guarding - which is
		// the reason that flag ended up with no readers at all (ledger M1).
		{
			pAABBBuffer->Clear();
			pAABBBuffer->AddStructuredData(
				m_AABBList.data(),
				sizeof(StructuredVoxelBuffer),
				m_AABBList.size(),
				false
			);
			pAABBBuffer->Allocate();
		}

		/* Model instance / quad-instance buffers - DYNAMIC_MODELS_PLAN.md
		   phase 2, same "rebuild every frame, not worth guarding" reasoning as
		   the AABB buffer just above. The mesh quad store itself is synced
		   separately (SyncModelMeshStore) since it only needs to grow when a
		   genuinely new model frame is first meshed, not every frame. */
		{
			/* Both indices in a ModelQuadInstance are consumed directly by the
			   vertex shader with no bounds check available to it. One stale
			   reference is an arbitrary transform applied to an arbitrary quad,
			   which expands six vertices across the whole render target - and it
			   presents as a frame the driver kills (CLAUDE.md, "the freeze is a
			   GPU timeout") rather than as a validation error. Validate at the
			   CPU/GPU boundary; drop only the corrupt references.

			   This is a backstop and should not fire. A non-finite transform is
			   caught and *repaired* where the entity is known, in
			   RenderSystem::PostTick - because dropping an instance here makes
			   a character silently vanish, which is worse than one drawn
			   unrotated and much harder to trace to a NaN. Anything that
			   reaches this test bypassed that guard, so the report says so.

			   The finiteness test is a sum because a NaN in any component
			   poisons it, and one comparison is cheaper than eleven. */
			const uint32_t uiMeshQuads = ModelMeshStore::Get().GetTotalQuadCount();
			std::vector<uint8_t> validInstances(m_ModelInstances.size(), 1u);

			for (size_t i = 0; i < m_ModelInstances.size(); ++i)
			{
				const ModelInstanceData& instance = m_ModelInstances[i];
				const float fFinite =
					instance.Rotation.x + instance.Rotation.y + instance.Rotation.z + instance.Rotation.w +
					instance.Scale.x + instance.Scale.y + instance.Scale.z +
					instance.WorldOrigin.x + instance.WorldOrigin.y + instance.WorldOrigin.z;

				validInstances[i] = std::isfinite(fFinite) ? 1u : 0u;
			}

			const size_t uiBeforeQuadRefs = m_ModelQuadInstances.size();
			m_ModelQuadInstances.erase(
				std::remove_if(m_ModelQuadInstances.begin(), m_ModelQuadInstances.end(),
					[&](const ModelQuadInstance& ref)
					{
						return ref.InstanceIndex >= validInstances.size() ||
							validInstances[ref.InstanceIndex] == 0u ||
							ref.QuadIndex >= uiMeshQuads;
					}),
				m_ModelQuadInstances.end());

			/* Once. Whatever gets past the submission guard gets past it every
			   frame until it is fixed, and this is a per-frame path (rule R9).
			   A model *is* missing from the image when this fires, so it says
			   that rather than reporting a count and leaving the reader to
			   wonder what the visible effect was. */
			if (m_ModelQuadInstances.size() != uiBeforeQuadRefs && !m_bWarnedInvalidModelQuads)
			{
				fprintf(stderr,
					"[render] dropped %zu model quad references that are not safe to "
					"draw (%zu instances, %u stored quads) - a model is missing from "
					"the image, and it bypassed RenderSystem's transform repair\n",
					uiBeforeQuadRefs - m_ModelQuadInstances.size(),
					m_ModelInstances.size(), uiMeshQuads);
				m_bWarnedInvalidModelQuads = true;
			}

			/* Buffer::Allocate resets and reuses its upload page whenever the
			   exact byte count changes, so a frame that changes the instance or
			   quad count overwrites the allocation the preceding VDirect
			   submission is still reading. Animation changes it. Frames of a
			   stable size keep the normal pipelined path. */
			const uint32_t uiNextInstanceBytes =
				static_cast<uint32_t>(m_ModelInstances.size() * sizeof(ModelInstanceData));
			const uint32_t uiNextQuadBytes =
				static_cast<uint32_t>(m_ModelQuadInstances.size() * sizeof(ModelQuadInstance));

			if (pModelInstanceBuffer->GetTotalSize() != uiNextInstanceBytes ||
				pModelQuadInstanceBuffer->GetTotalSize() != uiNextQuadBytes)
			{
				WaitForVoxelReaders();
			}

			pModelInstanceBuffer->Clear();
			pModelInstanceBuffer->AddStructuredData(
				m_ModelInstances.data(), sizeof(ModelInstanceData), m_ModelInstances.size(), false);
			pModelInstanceBuffer->Allocate();

			pModelQuadInstanceBuffer->Clear();
			pModelQuadInstanceBuffer->AddStructuredData(
				m_ModelQuadInstances.data(), sizeof(ModelQuadInstance), m_ModelQuadInstances.size(), false);
			pModelQuadInstanceBuffer->Allocate();

			SyncModelMeshStore();
		}

		if (pVDirectEngine->GetValue() > 0)
		{
			pVDirectEngine->AdvanceFrame();
		}

		pVDirectEngine->Reset();
		pVDirectEngine->Start();

#if defined(VOXAGINE_IOS)
		/* Metal reports a GPU address fault only for the complete Vulkan queue
		   submission, which otherwise hides which render stage supplied the bad
		   resource. Keep the producer/consumer stages as separate submissions on
		   iOS. Besides making the failing stage explicit in the device log, this
		   gives MoltenVK a command-buffer boundary for dependencies that D3D12
		   previously resolved within one command list. */
		auto SubmitVDirectStage = [pVDirectEngine](const char* pName)
		{
			fprintf(stderr, "[ios-gpu] submitting VDirect stage '%s'\n", pName);
			pVDirectEngine->ApplyBarriers();
			pVDirectEngine->Execute();
			pVDirectEngine->WaitForGPU();

			if (pVDirectEngine->GetCompletedValue() < pVDirectEngine->GetValue())
			{
				fprintf(stderr, "[ios-gpu] VDirect stage '%s' failed\n", pName);
				return false;
			}

			fprintf(stderr, "[ios-gpu] completed VDirect stage '%s'\n", pName);
			pVDirectEngine->Reset();
			pVDirectEngine->Start();
			return true;
		};
#endif

		/* Before any pass opens: the voxel pass samples the coverage texture,
		   and a copy cannot be recorded inside a render pass instance. */
		UploadVoxelPyramid(pVDirectEngine);
#if defined(VOXAGINE_IOS)
		if (!SubmitVDirectStage("voxel pyramid upload"))
			return false;
#endif

		/* One render pass instance per pass: dynamic rendering cannot nest
		   them, so the DX12-style interleaved Begin order would silently skip
		   every pass after the first. The voxel pass samples the particle
		   targets, so particles draw first. */
		pVDirectEngine->Begin(pParticlePass);
		pVDirectEngine->Draw(pParticlePass);
		pVDirectEngine->End(pParticlePass);
#if defined(VOXAGINE_IOS)
		if (!SubmitVDirectStage("particles"))
			return false;
#endif

		/* The voxel pass samples this one's target at t3, so it has to be
		   complete first - same ordering constraint as the particle targets
		   above, and the same reason there is one render pass instance each.

		   Skipped entirely at SHQ_OFF and SHQ_RAY, which is the whole point of
		   both modes:
		   this pass is one full march of the resident window per texel and
		   costs the same whether or not anything reads it. The target is left
		   holding whatever it last wrote rather than being cleared to "lit" -
		   the lookup returns 1.0 without touching it, so its contents cannot be
		   observed, and clearing a megapixel target every frame to say nothing
		   would be paying a fraction of the cost this mode exists to avoid. */
		if (pSunShadowPass != nullptr && settings.NeedsSunShadowMap())
		{
			pVDirectEngine->Begin(pSunShadowPass);
			pVDirectEngine->Draw(pSunShadowPass);
			pVDirectEngine->End(pSunShadowPass);
#if defined(VOXAGINE_IOS)
			if (!SubmitVDirectStage("sun shadow"))
				return false;
#endif

			/* DYNAMIC_MODELS_PLAN.md phase 4: dynamic renderers' own
			   light-space depth, then the combine pass that min()s it against
			   pSunShadowPass's target above - each has to be complete before
			   the next reads it, same one-render-pass-instance-per-pass rule
			   as everything else in this sequence. */
			pVDirectEngine->Begin(pSunShadowModelPass);
			pVDirectEngine->Draw(pSunShadowModelPass);
			pVDirectEngine->End(pSunShadowModelPass);
#if defined(VOXAGINE_IOS)
			if (!SubmitVDirectStage("sun shadow models"))
				return false;
#endif

			pVDirectEngine->Begin(pSunShadowCombinePass);
			pVDirectEngine->Draw(pSunShadowCombinePass);
			pVDirectEngine->End(pSunShadowCombinePass);
#if defined(VOXAGINE_IOS)
			if (!SubmitVDirectStage("sun shadow combine"))
				return false;
#endif
		}

		/* Dynamic models - DYNAMIC_MODELS_PLAN.md phase 2. The voxel pass
		   samples this one's colour and depth targets (t3/t4), same ordering
		   constraint as particles and the sun shadow map above. */
		pVDirectEngine->Begin(pVoxelModelPass);
		pVDirectEngine->Draw(pVoxelModelPass);
		pVDirectEngine->End(pVoxelModelPass);
#if defined(VOXAGINE_IOS)
		if (!SubmitVDirectStage("voxel models"))
			return false;
#endif

		pVDirectEngine->Begin(pVoxelPass);
		pVDirectEngine->Draw(pVoxelPass);
		pVDirectEngine->End(pVoxelPass);

#if defined(VOXAGINE_IOS)
		if (!SubmitVDirectStage("voxel world"))
			return false;
#else
		pVDirectEngine->ApplyBarriers();

		pVDirectEngine->Execute();
#endif
	}

	// Texture data
	{
		/* Rewrites each sprite's TextureID from a TextureManager ID to a slot
		   in this frame's bindless working set, and records which texture goes
		   in which slot for VKRenderPass::WriteDescriptors to bind. Uploading
		   m_SpriteList directly here is what made the array's size depend on
		   the highest live ID; see m_BindlessTextureIDs. */
		PackBindlessTextures();

		pSpriteBuffer->Clear();
		pSpriteBuffer->AddStructuredData(m_PackedSpriteList.data(), sizeof(SpriteData), m_PackedSpriteList.size(), false);
		pSpriteBuffer->Allocate();
	}

#if defined(_DEBUG) || defined(EDITOR)
	// Debug line buffer
	if (m_bDebugEnabled || !m_bDebugCleared)
	{
		pDebugBuffer->Clear();
		pDebugBuffer->AddStructuredData(
			m_DebugDrawLines.data(),
			sizeof(DebugDrawLine),
			m_DebugDrawLines.size(),
			false
		);
		pDebugBuffer->Allocate();
	}
#endif

	if (pDirectEngine->GetCompletedValue() < pDirectEngine->GetValue())
	{
		/* The GPU has not retired the frame we submitted, so there is nothing
		   to do but come back next tick. Sustained, that is indistinguishable
		   from a freeze: the window stops updating while the main loop keeps
		   spinning, and no validation error is produced because nothing
		   illegal happened. Report it once with the numbers that say whether
		   the work is merely enormous or genuinely stuck. */
		++m_uiStalledFrames;

		if (m_uiStalledFrames == 600)
		{
			fprintf(stderr, "[stall] GPU has not completed for 600 frames: "
			                "direct %llu/%llu, vdirect %llu/%llu, voxel instances %u, aabbs %zu\n",
			        static_cast<unsigned long long>(pDirectEngine->GetCompletedValue()),
			        static_cast<unsigned long long>(pDirectEngine->GetValue()),
			        static_cast<unsigned long long>(pVDirectEngine->GetCompletedValue()),
			        static_cast<unsigned long long>(pVDirectEngine->GetValue()),
			        pVoxelPass != nullptr ? pVoxelPass->GetData().m_uiInstanceCount : 0,
			        m_AABBList.size());
		}

		return false;
	}

	m_uiStalledFrames = 0;

	// Reset command allocators
	if (pDirectEngine->GetValue() > 0)
		pDirectEngine->AdvanceFrame();

	pDirectEngine->Reset();

	// Direct Engine List 1
	{
		// pDirectEngine->Wait(pCopyEngine, 1);
		pDirectEngine->Start();

#if defined(VOXAGINE_IOS)
		auto SubmitDirectStage = [pDirectEngine](const char* pName)
		{
			fprintf(stderr, "[ios-gpu] submitting Direct stage '%s'\n", pName);
			pDirectEngine->ApplyBarriers();
			pDirectEngine->Execute();
			pDirectEngine->WaitForGPU();

			if (pDirectEngine->GetCompletedValue() < pDirectEngine->GetValue())
			{
				fprintf(stderr, "[ios-gpu] Direct stage '%s' failed\n", pName);
				return false;
			}

			fprintf(stderr, "[ios-gpu] completed Direct stage '%s'\n", pName);
			pDirectEngine->Reset();
			pDirectEngine->Start();
			return true;
		};
#endif

		/* One render pass instance per pass (see the VDirect block above).
		   Post processing samples the UI and debug targets, so both close
		   before it begins. */
		pDirectEngine->Begin(pUIPass);
		pDirectEngine->Draw(pUIPass);
		pDirectEngine->End(pUIPass);
#if defined(VOXAGINE_IOS)
		if (!SubmitDirectStage("UI"))
			return false;
#endif

#if defined(_DEBUG) || defined(EDITOR)
		if (m_bDebugEnabled || !m_bDebugCleared)
		{
			pDirectEngine->Begin(pDebugPass);
			pDirectEngine->Draw(pDebugPass);
			pDirectEngine->End(pDebugPass);
		}
#endif

		pDirectEngine->Begin(pPostProcessingPass);
		pDirectEngine->Draw(pPostProcessingPass);

		/* ImContext::Draw takes only the draw data; the Vulkan context reads
		   the command buffer off the engine it was constructed with, so the
		   backend command list no longer has to be threaded through here.
		   It records into the post processing pass's instance.

		   This used to be compiled out on iOS, to isolate a MoltenVK GPU address
		   fault in this submission. It is not optional work: TextRenderer draws
		   every string in the game through ImGui, so skipping it is why no text
		   appeared on device at all - and the editor is ImGui end to end. The
		   faults it was hiding have since been traced to the voxel buffers'
		   image format and the AABB stride, both fixed. */
		m_pPlatform->GetImguiSystem().GetContext()->Draw(ImGui::GetDrawData());

		pDirectEngine->End(pPostProcessingPass);
		pDirectEngine->ApplyBarriers();

		/* Execute command list */
#if defined(VOXAGINE_IOS)
		if (!SubmitDirectStage("post processing"))
			return false;
#else
		pDirectEngine->Execute(); // 1
#endif
	}

	if (!m_bDebugEnabled && !m_bDebugCleared)
		m_bDebugCleared = true;

	return true;
}

void RenderContext::InitializeRenderLoop()
{
	Settings& settings = m_pPlatform->GetApplication()->GetSettings();

	VoxelPass* pVoxelPass = nullptr;

	UIPass* pUIPass = nullptr;
	PostProcessingPass* pPostProcessingPass = nullptr;

	Buffer* pCameraBuffer = nullptr;
	Buffer* pAABBBuffer = nullptr;

	Mapper* pParticleMapper = nullptr;
	Buffer* pSpriteBuffer = nullptr;

	Buffer* pModelInstanceBuffer = nullptr;
	Buffer* pModelQuadInstanceBuffer = nullptr;

	Sampler* pLinearSampler = nullptr;
	Sampler* pPointSampler = nullptr;
	Sampler* pPyramidSampler = nullptr;

#if defined(_DEBUG) || defined(EDITOR)
	DebugPass* pDebugPass = nullptr;
	Buffer* pLineBuffer = nullptr;
#endif

	// Camera buffer
	{
		Buffer::Info camBufInfo;
		camBufInfo.m_Name = "Camera Data";
		camBufInfo.m_Type = Buffer::E_CONSTANT;

		m_mBuffers.emplace(camBufInfo.m_Name, std::make_unique<Buffer>(Get(), camBufInfo));
		pCameraBuffer = m_mBuffers[camBufInfo.m_Name].get();
	}

	// Depth buffer
	{
		Buffer::Info aabbBufInfo;
		aabbBufInfo.m_Name = "AABB Data";
		aabbBufInfo.m_Type = Buffer::E_STRUCTURED;

		m_mBuffers.emplace(aabbBufInfo.m_Name, std::make_unique<Buffer>(Get(), aabbBufInfo));
		pAABBBuffer = m_mBuffers[aabbBufInfo.m_Name].get();
	}

	// Bake command buffer
	{
		Buffer::Info bakeCmdBufInfo;
		bakeCmdBufInfo.m_Name = "Bake Command Data";
		bakeCmdBufInfo.m_Type = Buffer::E_CONSTANT;

		m_mBuffers.emplace(bakeCmdBufInfo.m_Name, std::make_unique<Buffer>(Get(), bakeCmdBufInfo));
	}

	// Sprite buffer
	{
		Buffer::Info spriteBufInfo;
		spriteBufInfo.m_Name = "Sprite Data";
		spriteBufInfo.m_Type = Buffer::E_STRUCTURED;

		m_mBuffers.emplace(spriteBufInfo.m_Name, std::make_unique<Buffer>(Get(), spriteBufInfo));
		pSpriteBuffer = m_mBuffers[spriteBufInfo.m_Name].get();
	}

	/* Model instance / quad-instance buffers - DYNAMIC_MODELS_PLAN.md phase 2.
	   Rebuilt every frame from m_ModelInstances/m_ModelQuadInstances, same
	   pattern as the AABB buffer just above (RenderContext::Present, "AABB
	   buffer" comment): the lists are rebuilt whether or not anything moved,
	   so gating the upload on a dirty flag is not worth it at this size. */
	{
		Buffer::Info modelInstanceBufInfo;
		modelInstanceBufInfo.m_Name = "Model Instance Data";
		modelInstanceBufInfo.m_Type = Buffer::E_STRUCTURED;

		m_mBuffers.emplace(modelInstanceBufInfo.m_Name, std::make_unique<Buffer>(Get(), modelInstanceBufInfo));
		pModelInstanceBuffer = m_mBuffers[modelInstanceBufInfo.m_Name].get();
	}

	{
		Buffer::Info modelQuadInstanceBufInfo;
		modelQuadInstanceBufInfo.m_Name = "Model Quad Instance Data";
		modelQuadInstanceBufInfo.m_Type = Buffer::E_STRUCTURED;

		m_mBuffers.emplace(modelQuadInstanceBufInfo.m_Name, std::make_unique<Buffer>(Get(), modelQuadInstanceBufInfo));
		pModelQuadInstanceBuffer = m_mBuffers[modelQuadInstanceBufInfo.m_Name].get();
	}

#if defined(_DEBUG) || defined(EDITOR)
	// Debug line buffer
	{
		Buffer::Info lineBufInfo;
		lineBufInfo.m_Name = "Debug Lines";
		lineBufInfo.m_Type = Buffer::E_STRUCTURED;

		m_mBuffers.emplace(lineBufInfo.m_Name, std::make_unique<Buffer>(Get(), lineBufInfo));
		pLineBuffer = m_mBuffers[lineBufInfo.m_Name].get();
	}
#endif

	// Samplers
	{
		// Linear sampler
		Sampler::Info linearSamplerDesc;
		linearSamplerDesc.m_FilterMode = E_LINEAR;
		m_pSamplers.push_back(std::make_unique<Sampler>(Get(), linearSamplerDesc));
		pLinearSampler = m_pSamplers.back().get();

		// Point sampler
		Sampler::Info pointSamplerDesc;
		pointSamplerDesc.m_FilterMode = E_POINT;
		m_pSamplers.push_back(std::make_unique<Sampler>(Get(), pointSamplerDesc));
		pPointSampler = m_pSamplers.back().get();
	}

	/* Model mesh quad mapper - DYNAMIC_MODELS_PLAN.md phase 2, the GPU mirror
	   of ModelMeshStore's CPU quad list. E_UNKNOWN colour format so it binds
	   as a plain structured buffer rather than an image (VKPassBindings.cpp's
	   AddMappers, same test the brick mapper's own comment already explains);
	   E_READ_ONLY because only the CPU ever writes it - SyncModelMeshStore in
	   Present. One element and no back buffer: nothing is resident until the
	   first dynamic model is meshed, and unlike the voxel/brick mappers this
	   is never read back mid-frame by anything the GPU wrote, so there is
	   nothing to double-buffer against. */
	{
		Mapper::Info modelMeshMapperDesc;
		modelMeshMapperDesc.m_Name = "Model Mesh Quads";
		modelMeshMapperDesc.m_ColorFormat = E_UNKNOWN;
		modelMeshMapperDesc.m_GPUAccessType = E_READ_ONLY;
		modelMeshMapperDesc.m_uiElementCount = 1;
		modelMeshMapperDesc.m_uiElementSize = sizeof(uint32_t);

		m_pMappers.push_back(std::make_unique<Mapper>(Get(), modelMeshMapperDesc, false));
		m_pModelMeshMapper = m_pMappers.back().get();
	}

	// 3D voxel mapper
	{
		Mapper::Info voxelMapperDesc;
		voxelMapperDesc.m_Name = "Voxel Data Mapper";
		voxelMapperDesc.m_ColorFormat = E_R8G8B8A8_UNORM;
		voxelMapperDesc.m_GPUAccessType = E_READ_WRITE;

		voxelMapperDesc.m_bHasBackBuffer = true;

		m_pMappers.push_back(std::make_unique<Mapper>(Get(), voxelMapperDesc, false));
		m_pVoxelMapper = m_pMappers.back().get();
		m_pVoxelMapper->BufferSwapped += Event<uint32_t*&>::Subscriber([this](uint32_t*& newData)
		{
			m_pVoxelData = newData;

			/* The brick grid and its mapper describe the voxel window, so they
			   flip with it or they describe the wrong one. Doing it from here
			   rather than at the ChunkSystem call site is what guarantees the
			   three stay in lockstep. */
			m_pBrickMapper->SwapBuffer();
			m_pPyramidStaging->SwapBuffer();

			m_BrickGrid.Swap();
			m_BrickGrid.SetBuffers(m_pBrickMapper->GetData(), m_pBrickMapper->GetBackBufferData());

			m_BrickGrid.SetDensityBuffers(
				m_pPyramidStaging->GetData(),
				m_pPyramidStaging->GetBackBufferData());
		}, this);
	}

	/* Occupancy brick mapper (RENDERING_PLAN.md phase 2)
	 *
	 * No colour format, so it binds as a plain storage buffer rather than a
	 * texel buffer - the shader reads raw counts, not texels. Read-write only
	 * so that it lands in the u register range: the t range in front of it is
	 * already occupied by the AABB buffer and the particle textures, and
	 * taking a t register would renumber all of them. Nothing writes to it
	 * from the GPU.
	 *
	 * Back-buffered for the same reason the voxel mapper is - see the
	 * BufferSwapped subscriber above. */
	{
		Mapper::Info brickMapperDesc;
		brickMapperDesc.m_Name = "Voxel Brick Mapper";
		brickMapperDesc.m_ColorFormat = E_UNKNOWN;
		brickMapperDesc.m_GPUAccessType = E_READ_WRITE;

		brickMapperDesc.m_bHasBackBuffer = true;

		m_pMappers.push_back(std::make_unique<Mapper>(Get(), brickMapperDesc, false));
		m_pBrickMapper = m_pMappers.back().get();
	}

	/* Coverage and radiance pyramid texture (RENDERING_PLAN.md 7.1b route B,
	 * and 7.3)
	 *
	 * Mip L is pyramid level L. Alpha is the occupied fraction of the cell,
	 * which is all an occlusion term needs and quantizes exactly at the finest
	 * level (a 2^3 cell holds at most eight voxels); RGB is that cell's albedo
	 * in linear light, premultiplied by the fraction, which is what a bounce
	 * cone gathers. One fetch answers both, which is why 7.3 widened this
	 * texture rather than adding a second one beside it.
	 *
	 * One element and 1x1x1 until a world is loaded, so the descriptor and the
	 * image layout are valid from the first frame. ResizeWorldBuffer gives both
	 * their real size. */
	{
		Mapper::Info pyramidStagingDesc;
		pyramidStagingDesc.m_Name = "Voxel Pyramid Staging";
		pyramidStagingDesc.m_ColorFormat = E_UNKNOWN;
		pyramidStagingDesc.m_GPUAccessType = E_READ_ONLY;
		pyramidStagingDesc.m_bHasBackBuffer = true;

		m_pMappers.push_back(std::make_unique<Mapper>(Get(), pyramidStagingDesc, false));
		m_pPyramidStaging = m_pMappers.back().get();
		m_pPyramidStaging->Resize(1, sizeof(uint32_t));

		View::Info pyramidDesc;
		pyramidDesc.m_Name = "Voxel Pyramid";
		pyramidDesc.m_DimensionType = E_TEXTURE_3D;
		pyramidDesc.m_ColorFormat = E_R8G8B8A8_UNORM;
		pyramidDesc.m_Size = UVector3(1, 1, 1);
		pyramidDesc.m_Type = View::E_SHADER_RESOURCE_VIEW;
		pyramidDesc.m_State = E_STATE_PIXEL_SHADER_RESOURCE;
		pyramidDesc.m_uiMipLevels = VoxelBrickGrid::k_uiPyramidLevels;
		pyramidDesc.m_bIsAttachment = false;

		m_pViews.push_back(std::make_unique<View>(Get(), pyramidDesc));
		m_pPyramidView = m_pViews.back().get();

		/* Linear within a level, point across them, and transparent-black
		   outside the window. Each of the three is load-bearing:

		   - linear is the whole reason the pyramid moved into a texture; a
		     point sample of a 2-voxel lattice reports whichever way a block
		     happens to fall in it, which is the artefact route A's hand-written
		     filter existed to remove.
		   - point across levels keeps a cone step at one fetch. Blending two
		     mips is smoother and costs twice what route B is buying.
		   - the border is what makes a cone that leaves the window read as open
		     sky rather than as a smear of whatever was at the edge. */
		Sampler::Info pyramidSamplerDesc;
		pyramidSamplerDesc.m_FilterMode = E_LINEAR;
		pyramidSamplerDesc.m_MipFilterMode = E_POINT;
		pyramidSamplerDesc.m_WrapMode = E_BORDER;

		m_pSamplers.push_back(std::make_unique<Sampler>(Get(), pyramidSamplerDesc));
		pPyramidSampler = m_pSamplers.back().get();
	}

	/* Far-field LOD volume (RENDERING_PLAN.md phase 4)
	 *
	 * The whole level at a quarter resolution, so that a ray leaving the 3x3
	 * detail window has something to hit. Static after a build: no back buffer,
	 * because unlike the window it does not slide.
	 *
	 * Colour format matches the voxel mapper's, so the shader reads cells the
	 * same way it reads voxels. Read-write for the same reason the brick mapper
	 * is - it is the u register range that is free, and taking a t would
	 * renumber the textures that are already there. Nothing writes to either
	 * from the GPU. */
	{
		Mapper::Info farFieldDesc;
		farFieldDesc.m_Name = "Far Field Mapper";
		farFieldDesc.m_ColorFormat = E_R8G8B8A8_UNORM;
		farFieldDesc.m_GPUAccessType = E_READ_WRITE;

		m_pMappers.push_back(std::make_unique<Mapper>(Get(), farFieldDesc, false));
		m_pFarFieldMapper = m_pMappers.back().get();

		Mapper::Info farFieldBrickDesc;
		farFieldBrickDesc.m_Name = "Far Field Brick Mapper";
		farFieldBrickDesc.m_ColorFormat = E_UNKNOWN;
		farFieldBrickDesc.m_GPUAccessType = E_READ_WRITE;

		m_pMappers.push_back(std::make_unique<Mapper>(Get(), farFieldBrickDesc, false));
		m_pFarFieldBrickMapper = m_pMappers.back().get();

		/* One element each so the descriptors are valid before a world has been
		   loaded. GetFarFieldShaderGridSize reports (0,0,0) until a build
		   succeeds, and the shader skips the far field entirely on that. */
		m_pFarFieldMapper->Resize(1, sizeof(uint32_t));
		m_pFarFieldBrickMapper->Resize(1, sizeof(uint32_t));
	}

	// Particle Pass
	{
		// Vertex shader
		Shader::Info vertexShader;
		vertexShader.m_FilePath = "Engine/Assets/Shaders/Particles.vs";
		vertexShader.m_Type = Shader::E_VERTEX;

		m_pShaders.push_back(std::make_unique<Shader>(Get(), vertexShader));
		Shader* pVertexShader = m_pShaders.back().get();

		// Pixel shader
		Shader::Info pixelShader;
		pixelShader.m_FilePath = "Engine/Assets/Shaders/Particles.ps";
		pixelShader.m_Type = Shader::E_PIXEL;

		m_pShaders.push_back(std::make_unique<Shader>(Get(), pixelShader));
		Shader* pPixelShader = m_pShaders.back().get();

		// Particle Mapper
		Mapper::Info mapperInfo;
		mapperInfo.m_Name = "Particle Mapper";
		mapperInfo.m_ColorFormat = E_UNKNOWN;

		/* DESTRUCTION_PLAN.md P16: double-buffered like the voxel and brick
		   mappers, for the same reason - PhysicsSystem writes GPU records every
		   fixed tick with no fence against the frame this pass is reading.
		   Unlike those two, the swap fires once per rendered frame rather than
		   on a rare event, and only when a fixed tick actually ran - see
		   RenderSystem::Render. */
		mapperInfo.m_bHasBackBuffer = true;

		m_pMappers.push_back(std::make_unique<Mapper>(Get(), mapperInfo, false));
		pParticleMapper = m_pMappers.back().get();
		m_pParticleMapper = pParticleMapper;

		// Create screen render target from data
		m_pParticlePass = new ParticlePass(Get(), pVertexShader, pPixelShader, pCameraBuffer, pParticleMapper, pPointSampler);
		m_pRenderPasses.emplace(m_pParticlePass->GetData().m_Name, std::unique_ptr<ParticlePass>(m_pParticlePass));
	}

	/* Sun Shadow Pass. Before the voxel pass, which samples its target -
	   RENDERING_PLAN.md 7.1a. Only built when shadows are on; the ShadowLess
	   voxel shader declares no t3, so the pass would be a target nothing
	   reads. */
	SunShadowPass* pSunShadowPass = nullptr;

	/* DYNAMIC_MODELS_PLAN.md phase 4: dynamic renderers' own light-space
	   depth, and the pass that combines it with the world map above into one
	   texture shaped like pSunShadowPass's own - see SunShadowModel.vs.hlsl's
	   header for why a separate combine pass rather than either producer
	   writing into the other's target. VoxelPass below binds the *combined*
	   result as its shadow map, not pSunShadowPass's raw target. */
	SunShadowModelPass* pSunShadowModelPass = nullptr;
	SunShadowCombinePass* pSunShadowCombinePass = nullptr;

	if (settings.IsShadowEnabled())
	{
		Shader::Info vertexShader;
		vertexShader.m_FilePath = "Engine/Assets/Shaders/ScreenQuad.vs";
		vertexShader.m_Type = Shader::E_VERTEX;

		m_pShaders.push_back(std::make_unique<Shader>(Get(), vertexShader));
		Shader* pVertexShader = m_pShaders.back().get();

		Shader::Info pixelShader;
		pixelShader.m_FilePath = "Engine/Assets/Shaders/SunShadow.ps";
		pixelShader.m_Type = Shader::E_PIXEL;

		m_pShaders.push_back(std::make_unique<Shader>(Get(), pixelShader));
		Shader* pPixelShader = m_pShaders.back().get();

		pSunShadowPass = new SunShadowPass(
			Get(), pVertexShader, pPixelShader, pPointSampler,
			pCameraBuffer, m_pVoxelMapper, m_pBrickMapper);

		m_pRenderPasses.emplace(pSunShadowPass->GetData().m_Name, std::unique_ptr<SunShadowPass>(pSunShadowPass));

		{
			Shader::Info modelVertexShader;
			modelVertexShader.m_FilePath = "Engine/Assets/Shaders/SunShadowModel.vs";
			modelVertexShader.m_Type = Shader::E_VERTEX;

			m_pShaders.push_back(std::make_unique<Shader>(Get(), modelVertexShader));
			Shader* pModelVertexShader = m_pShaders.back().get();

			Shader::Info modelPixelShader;
			modelPixelShader.m_FilePath = "Engine/Assets/Shaders/SunShadowModel.ps";
			modelPixelShader.m_Type = Shader::E_PIXEL;

			m_pShaders.push_back(std::make_unique<Shader>(Get(), modelPixelShader));
			Shader* pModelPixelShader = m_pShaders.back().get();

			pSunShadowModelPass = new SunShadowModelPass(
				Get(), pModelVertexShader, pModelPixelShader,
				m_pModelMeshMapper, pCameraBuffer, pModelInstanceBuffer, pModelQuadInstanceBuffer);

			m_pRenderPasses.emplace(pSunShadowModelPass->GetData().m_Name, std::unique_ptr<SunShadowModelPass>(pSunShadowModelPass));
		}

		{
			Shader::Info combineVertexShader;
			combineVertexShader.m_FilePath = "Engine/Assets/Shaders/ScreenQuad.vs";
			combineVertexShader.m_Type = Shader::E_VERTEX;

			m_pShaders.push_back(std::make_unique<Shader>(Get(), combineVertexShader));
			Shader* pCombineVertexShader = m_pShaders.back().get();

			Shader::Info combinePixelShader;
			combinePixelShader.m_FilePath = "Engine/Assets/Shaders/SunShadowCombine.ps";
			combinePixelShader.m_Type = Shader::E_PIXEL;

			m_pShaders.push_back(std::make_unique<Shader>(Get(), combinePixelShader));
			Shader* pCombinePixelShader = m_pShaders.back().get();

			pSunShadowCombinePass = new SunShadowCombinePass(
				Get(), pCombineVertexShader, pCombinePixelShader, pPointSampler, pCameraBuffer,
				pSunShadowPass->GetTargetView(), pSunShadowModelPass->GetTargetView());

			m_pRenderPasses.emplace(pSunShadowCombinePass->GetData().m_Name, std::unique_ptr<SunShadowCombinePass>(pSunShadowCombinePass));
		}
	}

	/* Voxel Models pass - DYNAMIC_MODELS_PLAN.md phase 2, self-shadowing added
	   as a phase 4 follow-up. Before the voxel pass, which composites its
	   target the same way it already composites particles' - see
	   VoxelRenderer.ps.hlsl's composite branch and this file's per-frame
	   instance ordering comment ("the voxel pass samples the particle
	   targets, so particles draw first"). Built unconditionally, unlike the
	   sun shadow passes: dynamic renderers exist regardless of ShadowQuality.
	   Selects between two pixel shader variants exactly like the world voxel
	   pass does (rule 8) - the ShadowLess one when there is no combined
	   shadow map to sample. */
	{
		Shader::Info vertexShader;
		vertexShader.m_FilePath = "Engine/Assets/Shaders/VoxelModel.vs";
		vertexShader.m_Type = Shader::E_VERTEX;

		m_pShaders.push_back(std::make_unique<Shader>(Get(), vertexShader));
		Shader* pVertexShader = m_pShaders.back().get();

		Shader::Info pixelShader;
		pixelShader.m_FilePath = settings.IsShadowEnabled()
			? "Engine/Assets/Shaders/VoxelModel.ps"
			: "Engine/Assets/Shaders/VoxelModel.ShadowLess.ps";
		pixelShader.m_Type = Shader::E_PIXEL;

		m_pShaders.push_back(std::make_unique<Shader>(Get(), pixelShader));
		Shader* pPixelShader = m_pShaders.back().get();

		m_pVoxelModelPass = new VoxelModelPass(
			Get(), pVertexShader, pPixelShader, pPointSampler, pPyramidSampler,
			m_pModelMeshMapper, pCameraBuffer, pModelInstanceBuffer, pModelQuadInstanceBuffer,
			m_pPyramidView,
			pSunShadowCombinePass != nullptr ? pSunShadowCombinePass->GetTargetView() : nullptr);

		m_pRenderPasses.emplace(m_pVoxelModelPass->GetData().m_Name, std::unique_ptr<VoxelModelPass>(m_pVoxelModelPass));
	}

	// Voxel Pass
	{
		// Vertex shader
		Shader::Info vertexShader;
		vertexShader.m_FilePath = "Engine/Assets/Shaders/VoxelRenderer.vs";
		vertexShader.m_Type = Shader::E_VERTEX;

		m_pShaders.push_back(std::make_unique<Shader>(Get(), vertexShader));
		Shader* pVertexShader = m_pShaders.back().get();

		// Pixel shader
		Shader::Info pixelShader;
		pixelShader.m_FilePath = settings.IsShadowEnabled() ? "Engine/Assets/Shaders/VoxelRenderer.ps" : "Engine/Assets/Shaders/VoxelRenderer.ShadowLess.ps";
		pixelShader.m_Type = Shader::E_PIXEL;

		m_pShaders.push_back(std::make_unique<Shader>(Get(), pixelShader));
		Shader* pPixelShader = m_pShaders.back().get();

		// Create screen render target from data
		/* The shadow map goes in as the fifth texture (t5) - see
		   VoxelRenderer.ps.hlsl. Null under ShadowLess, where that variant
		   declares no t5 and VoxelPass skips the binding.
		   DYNAMIC_MODELS_PLAN.md phase 4: this is pSunShadowCombinePass's
		   target, not pSunShadowPass's own raw one - the combined map already
		   folds in what dynamic renderers cast. */
		pVoxelPass = new VoxelPass(Get(), pVertexShader, pPixelShader, pPointSampler, pPyramidSampler, m_pVoxelMapper, m_pBrickMapper, pCameraBuffer, pAABBBuffer,
			m_pParticlePass->GetTargetView(0), m_pParticlePass->GetTargetView(1),
			m_pVoxelModelPass->GetTargetView(0), m_pVoxelModelPass->GetTargetView(1),
			pSunShadowCombinePass != nullptr ? pSunShadowCombinePass->GetTargetView() : nullptr, m_pPyramidView);
		m_pRenderPasses.emplace(pVoxelPass->GetData().m_Name, std::unique_ptr<VoxelPass>(pVoxelPass));
	}

#if defined(_DEBUG) || defined(EDITOR)
	// Debug
	{
		// Vertex shader
		Shader::Info vertexShader;
		vertexShader.m_FilePath = "Engine/Assets/Shaders/Debug.vs";
		vertexShader.m_Type = Shader::E_VERTEX;

		m_pShaders.push_back(std::make_unique<Shader>(Get(), vertexShader));
		Shader* pVertexShader = m_pShaders.back().get();

		// Pixel shader
		Shader::Info pixelShader;
		pixelShader.m_FilePath = "Engine/Assets/Shaders/Debug.ps";
		pixelShader.m_Type = Shader::E_PIXEL;

		m_pShaders.push_back(std::make_unique<Shader>(Get(), pixelShader));
		Shader* pPixelShader = m_pShaders.back().get();

		// Create screen render target from data
		pDebugPass = new DebugPass(Get(), pVertexShader, pPixelShader, pCameraBuffer, pLineBuffer);
		m_pRenderPasses.emplace(pDebugPass->GetData().m_Name, std::unique_ptr<DebugPass>(pDebugPass));
	}
#endif

	// UI
	{
		// Vertex shader
		Shader::Info vertexShader;
		vertexShader.m_FilePath = "Engine/Assets/Shaders/UIRenderer.vs";
		vertexShader.m_Type = Shader::E_VERTEX;

		m_pShaders.push_back(std::make_unique<Shader>(Get(), vertexShader));
		Shader* pVertexShader = m_pShaders.back().get();

		// Pixel shader
		Shader::Info pixelShader;
		pixelShader.m_FilePath = "Engine/Assets/Shaders/UIRenderer.ps";
		pixelShader.m_Type = Shader::E_PIXEL;

		m_pShaders.push_back(std::make_unique<Shader>(Get(), pixelShader));
		Shader* pPixelShader = m_pShaders.back().get();

		// Create screen render target from data
		pUIPass = new UIPass(Get(), pVertexShader, pPixelShader, pLinearSampler, pCameraBuffer, pSpriteBuffer);
		m_pRenderPasses.emplace(pUIPass->GetData().m_Name, std::unique_ptr<UIPass>(pUIPass));
	}

	// Post Processing
	{
		// Vertex shader
		Shader::Info vertexShader;
		vertexShader.m_FilePath = "Engine/Assets/Shaders/ScreenQuad.vs";
		vertexShader.m_Type = Shader::E_VERTEX;

		m_pShaders.push_back(std::make_unique<Shader>(Get(), vertexShader));
		Shader* pVertexShader = m_pShaders.back().get();

		// Pixel shader
		Shader::Info pixelShader;
#if defined(_DEBUG) || defined(EDITOR)
		pixelShader.m_FilePath = "Engine/Assets/Shaders/PostProcessing.Debug.ps";
#else
		pixelShader.m_FilePath = "Engine/Assets/Shaders/PostProcessing.ps";
#endif
		pixelShader.m_Type = Shader::E_PIXEL;

		m_pShaders.push_back(std::make_unique<Shader>(Get(), pixelShader));
		Shader* pPixelShader = m_pShaders.back().get();

		// To 1, needed for GetTargetView
		pVoxelPass->ToggleBackBuffer();

		// Create screen render target from data
		pPostProcessingPass = new PostProcessingPass(
			Get(),
			pVertexShader,
			pPixelShader,
			pLinearSampler,
			pPointSampler,
			pCameraBuffer,
			m_pVoxelMapper,
			m_pFarFieldMapper,
			m_pFarFieldBrickMapper,
#if defined(_DEBUG) || defined(EDITOR)
			{ pVoxelPass->GetTargetView(), pUIPass->GetTargetView(), pDebugPass->GetTargetView() }
#else
			{ pVoxelPass->GetTargetView(), pUIPass->GetTargetView() }
#endif
		);

		// To 0
		pVoxelPass->ToggleBackBuffer();

		m_pRenderPasses.emplace(pPostProcessingPass->GetData().m_Name, std::unique_ptr<PostProcessingPass>(pPostProcessingPass));
	}
}

void RenderContext::LoadTexture(TextureReference* pTextureReference)
{
	if (!pTextureReference || pTextureReference->TextureView)
		return;

	/* A backend can fail during device/swapchain creation (most commonly when
	   MoltenVK is unavailable or rejects a required feature).  The context
	   object still exists in that case, but its managers/engines do not.  World
	   deserialization happens immediately after Platform::Initialize and must
	   not turn that recoverable initialization failure into a null dereference.
	*/
	if (!m_pTextureManager)
	{
		fprintf(stderr, "[render] texture load skipped: texture manager is not initialized\n");
		return;
	}

	auto commandEngine = m_pCommandEngines.find("Texture");
	if (commandEngine == m_pCommandEngines.end() || !commandEngine->second)
	{
		fprintf(stderr, "[render] texture load skipped: Texture command engine is not initialized\n");
		return;
	}

	m_pTextureManager->LoadTexture(commandEngine->second->Get(), pTextureReference);
}

void RenderContext::DestroyTexture(const TextureReference* pTextureRef)
{
	WaitForGPU();
	m_pTextureManager->DestroyTexture(pTextureRef);
}

void RenderContext::OnFullscreenChanged(bool bFullscreen)
{
	m_bIsFullscreen = bFullscreen;
	FullscreenChanged(bFullscreen);

	if (bFullscreen)
	{
		OnResize(m_v2ScreenResolution.x, m_v2ScreenResolution.y);
	}
	else
	{
		/* SDL owns the window on every desktop platform now, so there is no
		   Win32 branch here any more: GetHandle() is an SDL_Window*, and the
		   old code cast it to an HWND for GetClientRect. */
		OnResize(m_v2RenderResolution.x, m_v2RenderResolution.y);
	}
}

UVector2 RenderContext::ConstrainToAspectRatio(uint32_t uiWidth, uint32_t uiHeight) const
{
	const float fLocked =
		m_pPlatform->GetApplication()->GetSettings().GetLockedAspectRatio();

	if (fLocked <= 0.f || uiWidth == 0 || uiHeight == 0)
		return UVector2(uiWidth, uiHeight);

	/* Largest box of the locked ratio that fits the window; the leftover is
	   the letterbox the swapchain blit leaves black. */
	const float fWindow = static_cast<float>(uiWidth) / static_cast<float>(uiHeight);

	if (fWindow > fLocked)
		uiWidth = static_cast<uint32_t>(uiHeight * fLocked);
	else
		uiHeight = static_cast<uint32_t>(uiWidth / fLocked);

	return UVector2(std::max(uiWidth, 1u), std::max(uiHeight, 1u));
}

Vector2 RenderContext::WindowToRenderNormalized(const Vector2& v2WindowPoint) const
{
	const UVector2 windowSize = m_pPlatform->GetWindowContext()->GetSize();

	/* m_v2RenderResolution is the constrained size in window pixels; the
	   present blit centres it. */
	const float fBarX = (static_cast<float>(windowSize.x) - static_cast<float>(m_v2RenderResolution.x)) * 0.5f;
	const float fBarY = (static_cast<float>(windowSize.y) - static_cast<float>(m_v2RenderResolution.y)) * 0.5f;

	if (m_v2RenderResolution.x == 0 || m_v2RenderResolution.y == 0)
		return Vector2(0.f);

	return Vector2(
		(v2WindowPoint.x - fBarX) / static_cast<float>(m_v2RenderResolution.x),
		(v2WindowPoint.y - fBarY) / static_cast<float>(m_v2RenderResolution.y));
}

bool RenderContext::OnResize(uint32_t uiWidth, uint32_t uiHeight)
{
	const UVector2 constrained = ConstrainToAspectRatio(uiWidth, uiHeight);
	uiWidth = constrained.x;
	uiHeight = constrained.y;

	if (m_v2RenderResolution.x == uiWidth && m_v2RenderResolution.y == uiHeight)
		return false;

	UVector2 oldResolution = m_v2RenderResolution;
	m_v2RenderResolution = UVector2(uiWidth, uiHeight);

	IVector2 resolutionDelta;
	resolutionDelta.x = static_cast<int32_t>(m_v2RenderResolution.x * m_fRenderScale) - static_cast<int32_t>(oldResolution.x * m_fRenderScale);
	resolutionDelta.y = static_cast<int32_t>(m_v2RenderResolution.y * m_fRenderScale) - static_cast<int32_t>(oldResolution.y * m_fRenderScale);

	/* Deliberately not logged here. This size is what the engine *asked* for -
	   SetFullscreen calls OnResize with the screen resolution - and a tiling
	   compositor is free to ignore it, which Hyprland does until Alt+Up. A log
	   line here reads as ground truth and is not: it reported 2732x1536 for a
	   run whose passes were all rasterizing at 1365x767. VKRenderPass logs the
	   size each pass actually creates its attachments at, which is the number a
	   GPU timing is comparable against. */
	SizeChanged(
		static_cast<uint32_t>(uiWidth * m_fRenderScale),
		static_cast<uint32_t>(uiHeight * m_fRenderScale),
		resolutionDelta
	);

	return true;
}
