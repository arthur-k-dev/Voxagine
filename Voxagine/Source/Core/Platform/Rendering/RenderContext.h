#pragma once

#include "Core/ECS/Systems/Rendering/Buffers/RenderData.h"

#include "Core/Event.h"

#include "Core/ECS/Systems/Rendering/Buffers/RenderBuffer.h"
#include "Core/ECS/Systems/Rendering/Buffers/Structures/StructuredVoxelBuffer.h"
#include "Core/ECS/Systems/Rendering/Buffers/Structures/ModelInstanceData.h"

#include "Core/Platform/Rendering/RenderDefines.h"
#include "Core/Platform/Rendering/Managers/ModelManagerInc.h"
#include "Core/Platform/Rendering/Managers/TextureManagerInc.h"
#include "Core/Platform/Rendering/CommandEngineInc.h"
#include "Core/Platform/Rendering/RenderPassInc.h"
#include "Core/Platform/Rendering/ComputePassInc.h"

#include "Core/Math.h"
#include "Core/VColors.h"

#include <stdint.h>
#include <cstddef>
#include <string>
#include <memory>
#include <unordered_map>

#include "Core/Platform/Rendering/Objects/Buffer.h"
#include "Core/Platform/Rendering/Objects/Sampler.h"
#include "Core/Platform/Rendering/Objects/Shader.h"
#include "Core/Platform/Rendering/Objects/View.h"
#include "Core/Platform/Rendering/Objects/Mapper.h"

#include "Core/Resources/Formats/TextureReference.h"
#include "Core/Resources/Formats/ShaderReference.h"

#include "Core/Platform/Rendering/RenderAlignment.h"
#include "Core/Platform/Rendering/VoxelBrickGrid.h"
#include "Core/Platform/Rendering/FarFieldVolume.h"
#include "Core/ECS/Systems/Chunk/FarFieldBaker.h"
#include "Core/Voxels/VoxelWindow.h"

class Platform;
class WindowContext;
class RenderPass;

class ParticlePass;
class VoxelModelPass;

class Settings;
class Camera;

class Particle;
class Mapper;

class VoxRenderer;

struct CameraRenderData {
	CameraRenderData() = default;
	CameraRenderData(
		const Matrix4& mvp,
		const Matrix4& modelView,
		const Matrix4& view,
		const Matrix4& projection,

		float fProjectionValue,
		float fAspectRatio,
		bool bIsOrthographic,
		bool bIsUpdated,

		Vector4 worldPos,
		Vector4 cameraOffset
	) {
		/* Used for debug and depth rendering */
		m_MVP = mvp;

		m_ModelView = modelView;
		m_View = view;
		m_Projection = projection;

		m_fProjectionValue = fProjectionValue;
		m_fAspectRatio = fAspectRatio;
		m_bIsOrthographic = bIsOrthographic;
		m_bIsUpdated = bIsUpdated;

		m_WorldPos = worldPos;
		m_CameraOffset = cameraOffset;
	}

	/* Used for debug and depth rendering */
	Matrix4 m_MVP;

	/* Used for voxel rendering */
	Matrix4 m_ModelView;

	Matrix4 m_View;
	Matrix4 m_Projection;

	float m_fProjectionValue;
	float m_fAspectRatio;
	bool m_bIsOrthographic;
	bool m_bIsUpdated;

	Vector4 m_WorldPos;
	Vector4 m_CameraOffset;
};

struct DebugLine {
	Vector3 m_Start;
	Vector3 m_End;
	VColor m_Color = VColors::Green;
};

struct DebugSphere {
	Vector3 m_Center;
	float m_fRadius;
	VColor m_Color = VColors::Green;
};

struct DebugBox {
	Vector3 m_Center;
	Vector3 m_Extents;
	VColor m_Color = VColors::Green;
};

struct SpriteData {
	Matrix4 Model;
	uint32_t TextureID;

	Vector4 Color;

	Vector2 Offset;
	Vector2 Size;

	uint32_t Alignment;
	uint32_t ScreenAlignment;

	uint32_t IsScreen;

	int32_t Layer;

	Vector2 TextureRepeat;

	Vector2 cullStart;
	Vector2 cullEnd;

	uint32_t padding;
};

// GPU structured-buffer ABI. UIRenderer.vs.hlsl deliberately spells vector
// members as scalars so every backend uses these packed offsets and stride.
static_assert(sizeof(SpriteData) == 144, "SpriteData GPU stride changed");
static_assert(offsetof(SpriteData, TextureID) == 64, "SpriteData TextureID offset changed");
static_assert(offsetof(SpriteData, Color) == 68, "SpriteData Color offset changed");
static_assert(offsetof(SpriteData, Offset) == 84, "SpriteData Offset offset changed");
static_assert(offsetof(SpriteData, Size) == 92, "SpriteData Size offset changed");
static_assert(offsetof(SpriteData, Alignment) == 100, "SpriteData Alignment offset changed");
static_assert(offsetof(SpriteData, ScreenAlignment) == 104, "SpriteData ScreenAlignment offset changed");
static_assert(offsetof(SpriteData, IsScreen) == 108, "SpriteData IsScreen offset changed");
static_assert(offsetof(SpriteData, Layer) == 112, "SpriteData Layer offset changed");
static_assert(offsetof(SpriteData, TextureRepeat) == 116, "SpriteData TextureRepeat offset changed");
static_assert(offsetof(SpriteData, cullStart) == 124, "SpriteData cullStart offset changed");
static_assert(offsetof(SpriteData, cullEnd) == 132, "SpriteData cullEnd offset changed");
static_assert(offsetof(SpriteData, padding) == 140, "SpriteData padding offset changed");

struct ParticleMapperData {
	ParticleMapperData(Mapper* pMapper, uint32_t uiCount) : m_pMapper(pMapper), m_uiCount(uiCount) {}
	Mapper* m_pMapper = nullptr;
	uint32_t m_uiCount;
};

struct TextureReadData
{
	/* Defined in TextureManager.cpp, next to the decoder that allocates
	   m_Data - the free has to match stbi's allocator, not operator new[]. */
	~TextureReadData();

	uint32_t* m_Data = nullptr;
	UVector2 m_Dimensions = UVector2(0, 0);
};

/* IVoxelWindow is the seam chunk streaming reaches the resident window through
   - see Core/Voxels/VoxelWindow.h. It carries no state and adds no cost here;
   what it buys is a ChunkSystem that can be driven with no device behind it. */
class RenderContext : public IVoxelWindow
{
public:
	friend class TextureReference;
	friend class ShaderReference;
	friend class PhysicsSystem;
	friend class ParticlePass;
	friend class DebugPass;
	friend class RenderSystem;
	friend class Editor;

	// Maximum queued frames on the GPU
	static const uint32_t m_uiFrameCount = 2;

	/* The side of the square sun shadow map is Settings::GetSunShadowResolution
	   now, not a constant here - RENDERING_PLAN.md 7.1a, and Settings.h's
	   ShadowQuality for the two sizes and why there are two.

	   The sizing argument that used to live here, kept because it is what
	   picked those numbers: the 768x128x768 window projects to roughly
	   1065 x 877 world units perpendicular to the light, so 1024 is close to one
	   texel per voxel and 512 is one per two. A depth map records the nearest
	   blocker per texel, so a coarser map makes a one-voxel post cast a shadow
	   wider than itself rather than losing it - erring thick, which is the right
	   direction here (phase 6.2 could not make thin occluders read at all).

	   Its cost is one brick-DDA march per texel and is *independent of screen
	   resolution*, which is the whole reason the sun stopped being a per-pixel
	   ray: at 3840x2160 a single shadow ray per pixel measures ~9.45 ms, and
	   the entire lighting budget is 3 ms. It is also why the map's size is the
	   only thing that moves its cost, and so why it is a setting. */

	virtual ~RenderContext();

	static void Report();

	virtual void Initialize();
	virtual void Deinitialize() {};

	/* The render loop owns the voxel mapper consumed by world and physics
	   startup. A backend can create a window yet fail before that loop exists;
	   callers must be able to stop cleanly instead of dereferencing it. */
	bool IsReady() const { return m_pVoxelMapper != nullptr; }
	/* A backend-specific explanation for a failed startup. Kept on the common
	   interface so Application can report it without knowing Vulkan/Metal/DX. */
	virtual std::string GetStartupError() const { return {}; }

	PRenderContext* Get();

	virtual TextureReadData* ReadTexture(const std::string& texturePath);
	virtual void DestroyShader(const ShaderReference* pTextureReference) = 0;

	virtual void WaitForGPU();

	/* Drain only the VDirect engine - the one that reads the voxel-side
	   buffers. Host writes to a buffer a submission may still be fetching from
	   need this; a full WaitForGPU is a much wider stall for the same effect. */
	void WaitForVoxelReaders();

	/* Submit data to the draw list */
	virtual void Submit(const RenderData& renderData);

	virtual void Submit(const DebugLine& renderData);
	virtual void Submit(const DebugSphere& renderData);
	virtual void Submit(const DebugBox& renderData);

	virtual void Submit(const SpriteData& renderData);

	/* This frame's bindless texture working set: packed slot -> TextureManager
	   ID. The descriptor writer fills slot N from entry N; a sprite's
	   TextureID as uploaded is the slot, not the ID. See m_BindlessTextureIDs
	   for why the two are no longer the same thing. */
	const std::vector<uint32_t>& GetBindlessTextureIDs() const { return m_BindlessTextureIDs; }

	virtual void Submit(StructuredVoxelBuffer& renderData);

	/* DYNAMIC_MODELS_PLAN.md phase 2. One dynamic (non-static) VoxRenderer's
	   continuous world transform this frame - see ModelInstanceData.h for why
	   it is not VoxelStamp.h's quantized VoxelStampTransform. Returns the
	   index SubmitModelQuads needs to reference it; both lists are cleared
	   once a frame the same way m_AABBList already is. */
	uint32_t SubmitModelInstance(const ModelInstanceData& instance);

	/* Expands ModelMeshStore's [uiFirstQuad, uiFirstQuad + uiQuadCount) into
	   this frame's quad-instance list, each entry naming uiInstanceIndex - the
	   return value of the SubmitModelInstance call for the same renderer. */
	void SubmitModelQuads(uint32_t uiInstanceIndex, uint32_t uiFirstQuad, uint32_t uiQuadCount);

	void SortAABBs();

	void EnableDebugLines(bool bEnabled);

	/* Window size reduced to the locked aspect ratio from Settings; equal to
	   the input when nothing is locked. */
	UVector2 ConstrainToAspectRatio(uint32_t uiWidth, uint32_t uiHeight) const;

	/* Window pixel to 0..1 across the presented image. A locked aspect ratio
	   centres a smaller render target in the window, so the two spaces differ
	   by the black bar; identity when nothing is locked. */
	Vector2 WindowToRenderNormalized(const Vector2& v2WindowPoint) const;

	bool ResizeWorldBuffer();
	inline bool ModifyVoxel(uint32_t uiID, uint32_t uiColor, bool bOverwrite = true)
	{
		/* The callers derive this ID from float world positions, so a bad
		   transform reaches here as an index rather than as a crash at the
		   source. Writing outside the mapped voxel buffer corrupts whatever
		   the allocator put next to it. */
		if (uiID >= GetVoxelDataSize())
			return false;

		/* The old *occupancy* comes from the brick grid's CPU-side bitmap, not
		   from the mapping. The mapping prefers ReBAR, so reading a voxel back
		   is a PCIe read of VRAM, and the bake path performs millions of them:
		   this read alone was 5.3 seconds of a world load and 74 ms of a chunk
		   load, measured. See VoxelBrickGrid.

		   That drops the old redundant-write guard (uiOldColor != uiColor),
		   which needed the colour rather than the occupancy. Writing the same
		   value twice is a streaming store into write-combined memory and
		   costs less than learning it was unnecessary would. */
		const bool bWasOccupied = m_BrickGrid.IsOccupied(uiID);

		/* "Do not overwrite" means do not overwrite anything solid. The old
		   form tested the whole word against zero; occupancy is alpha > 0
		   (rule 3), and nothing writes a colour with a zero alpha byte, so the
		   two agree on every value the engine produces. */
		if (!bOverwrite && bWasOccupied)
			return false;

		m_pVoxelData[uiID] = uiColor;
		m_BrickGrid.SetVoxel(uiID, uiColor);

		return true;
	}

	inline void ModifyVoxelFast(uint32_t uiID, uint32_t uiColor)
	{
		if (uiID >= GetVoxelDataSize())
			return;

		/* Write-only with respect to the mapping, as the name promises: the
		   old occupancy the brick count needs comes from the bitmap, in
		   ordinary cached memory. See ModifyVoxel above. */
		m_pVoxelData[uiID] = uiColor;
		m_BrickGrid.SetVoxel(uiID, uiColor);
	}

	uint32_t GetVoxelDataSize() const { return m_pVoxelMapper->GetInfo().m_uiElementCount; }

	/* Bumped every time the voxel buffer stops holding what was stamped into
	   it - a clear or a resize. A baker records it alongside the positions it
	   wrote, and comparing it is how VoxelBaker::Bake tells "the world was
	   reset under me, re-stamp" apart from "something asked for an update but
	   my voxels are still there". Without it a forced update has to assume the
	   worst and re-stamp every renderer, which at a world load is a clear and
	   an occupy that exactly cancel. */
	uint32_t GetVoxelGeneration() const { return m_uiVoxelGeneration; }

	uint32_t GetVoxel(uint32_t uiID) const;
	uint32_t* GetVoxelData() { return m_pVoxelMapper->GetData(); }
	uint32_t* GetVoxelBackData() { return m_pVoxelMapper->GetBackBufferData(); }
	const uint32_t* GetVoxelData() const { return m_pVoxelMapper->GetData(); }
	Mapper* GetVoxelMapper() const { return m_pVoxelMapper; }
	void ClearVoxels();

	/* --- IVoxelWindow --------------------------------------------------------
	   Forwarders, deliberately: the names above are what the rest of the engine
	   has always called these and renaming 60 call sites to land a test seam
	   would be the tail wagging the dog. */
	uint32_t* GetFrontData() override { return GetVoxelData(); }
	uint32_t* GetBackData() override { return GetVoxelBackData(); }
	uint32_t GetWordCount() const override { return GetVoxelDataSize(); }
	void WaitForReaders() override { WaitForVoxelReaders(); }
	/* The brick grid, the brick mapper and the pyramid staging buffer all flip
	   with the voxel mapper already - see the BufferSwapped subscriber in
	   Initialize, which exists so that the four cannot get out of lockstep from
	   a call site. So this really is just the one call. */
	/* **A swap bumps the voxel generation, and leaving that out cost a level
	   8% of its geometry.** The generation means exactly one thing - "the
	   buffer no longer holds what you stamped" (see GetVoxelGeneration) - and
	   reversing the two buffers is the purest possible instance of it: every
	   renderer stamped into the old front buffer now has its voxels in the
	   buffer nothing draws.

	   It went unnoticed because on a window *slide* the world offset changes in
	   the same transaction, and VoxelBaker::Bake's bBakeCurrent test fails on
	   the offset before it ever looks at the generation. The initial window is
	   the one commit that swaps with the offset *unchanged* - so the chunk the
	   camera starts in, whose renderers JsonSerializer::DeserializeWorld
	   admitted and the first PreTick stamped, kept bBakeCurrent true forever and
	   was never re-stamped. Measured on Fishing_Village_Beat1: 2,706,535 active
	   voxels before chunk streaming phase 4, 2,493,640 after.

	   This costs nothing on a slide, where every renderer is re-examined
	   already. */
	void Swap() override
	{
		m_pVoxelMapper->SwapBuffer();
		++m_uiVoxelGeneration;
	}

	/* Coarse occupancy over the same window, for the marcher's outer walk.
	   Kept current by ModifyVoxel/ModifyVoxelFast above and, in bulk, by
	   ChunkSystem::RenderChunk. See VoxelBrickGrid. */
	VoxelBrickGrid& GetBrickGrid() override { return m_BrickGrid; }
	Mapper* GetBrickMapper() const { return m_pBrickMapper; }

	/* Double-buffered per DESTRUCTION_PLAN.md P16: PhysicsSystem writes each
	   fixed tick's records into the back buffer, and RenderSystem::Render
	   swaps it in only on a frame that actually ran a fixed tick - otherwise
	   the CPU write races the frame the GPU is still drawing from. */
	Mapper* GetParticleMapper() const { return m_pParticleMapper; }

	/* Recomputes the whole brick grid from the voxel buffer and logs anything
	   that disagrees. On demand only - it reads the entire window back out of
	   uncached memory. */
	uint32_t ValidateBrickGrid();

	/* Pushes whatever of the coverage pyramid's finest level the texture does
	   not yet hold, and rebuilds the rest of its mip chain. Once a frame, on
	   the engine that records the voxel pass and before it opens - the pass
	   samples the result. See m_pPyramidView. */
	void UploadVoxelPyramid(PCommandEngine* pEngine);

	/* DYNAMIC_MODELS_PLAN.md phase 2. Grows m_pModelMeshMapper to match
	   ModelMeshStore's current quad count and re-uploads it, but only when
	   that count has grown since the last call - the common case once a
	   level's dynamic models have all been seen once. Called from Present
	   inside the same "GPU is not still reading last frame's data" guard the
	   AABB buffer's own per-frame rebuild already relies on (RenderContext.cpp,
	   "AABB buffer" comment) - a plain CPU write into mapped memory, so unlike
	   UploadVoxelPyramid it needs no command engine. */
	void SyncModelMeshStore();

	/* Reads the coverage texture back and checks it against the mirror the CPU
	   maintains, plus every coarser mip against the average of its children.
	   The failure it exists for is a dirty region that was never uploaded:
	   ValidateBrickGrid proves the mirror, and nothing else proves that what
	   the GPU holds is the mirror. Blocking, and it allocates the whole chain
	   again to read into - a menu item, not a frame check. */
	uint32_t ValidateVoxelPyramid();

	/* Cross-checks the far field's placement against the resident window, which
	   holds the same geometry at full resolution. See FarFieldVolume::Validate.
	   On demand only, for the same reason. */
	uint32_t ValidateFarField();

	/* Far-field LOD volume (RENDERING_PLAN.md phase 4): the whole level at a
	   quarter resolution, so a ray that leaves the 3x3 detail window has
	   something to hit. See FarFieldVolume. */
	FarFieldVolume& GetFarField() { return m_FarField; }

	/* Rebuilds the volume for pWorld's level and pushes it to the GPU. Sizes
	   the mappers, so it must run before anything samples them. */
	void BuildFarField(class World* pWorld);

	/* The same build in budgeted slices - CHUNK_STREAMING_PLAN.md phase 4.
	   `BuildFarField` is 447 ms for Beat2 and it ran inside World::Initialize,
	   off the frame loop, where a loading screen cannot animate over it.
	   ChunkSystem::Tick drives these; the volume reports itself unbuilt (and so
	   is never sampled) until Continue returns true. */
	void BeginFarFieldBuild(class World* pWorld);
	bool ContinueFarFieldBuild(StreamingBudget::Scope& budget);
	void CancelFarFieldBuild();
	bool IsFarFieldBuilding() const { return m_FarFieldBuild.bActive; }

private:
	/* Everything after the stamping: resize the brick grid, grow the two
	   mappers, flush the volume into them and build its pyramid once. Shared by
	   the one-shot and the incremental build so the two cannot diverge on the
	   order, which matters - the grid drops its mirror before the mapper
	   reallocates and is re-supplied after. */
	void PublishFarField();

public:

	/* The cell grid the shader marches, or (0,0,0) when there is no far field -
	   which is what a level whose window already covers it reports, and what
	   the toggle below reports when off. */
	UVector3 GetFarFieldShaderGridSize() const;

	/* Runtime toggle: what the far field costs depends entirely on how much
	   sky is on screen, so an A/B is only meaningful without moving the camera
	   between measurements. */
	bool IsFarFieldEnabled() const { return m_bFarFieldEnabled; }
	void SetFarFieldEnabled(bool bEnabled) { m_bFarFieldEnabled = bEnabled; ForceCameraDataUpdate(); }

	/* Present pacing, serialized as Settings::EnableVSync.
	 *
	 * On when the displayed frames should each carry an equal slice of world
	 * time, off for the lowest latency. Neither is free: mailbox at 200 fps on
	 * a 60 Hz display advances the world 15 ms, then 20, then 15 between shown
	 * frames, which reads as skipping even though every frame is on time.
	 * Runtime-toggleable for the same reason the far field is - which of the
	 * two is worse is a judgement, and judging it means switching without
	 * anything else changing. */
	virtual bool IsVSyncEnabled() const { return false; }
	virtual void SetVSyncEnabled(bool bEnabled) { (void)bEnabled; }

	/* Clear the screen */
	virtual void Clear();
	virtual void FixedClear();

	/* Builds m_PackedSpriteList and m_BindlessTextureIDs from m_SpriteList.
	   Call once per frame, immediately before the sprite buffer is uploaded -
	   the upload and the descriptor write both depend on the result and must
	   see the same one. */
	void PackBindlessTextures();

	/* Present all the gathered data to the screen */
	virtual bool Present();

	/* Write a named pass's render target to `path` as a binary PPM, for
	   --screenshot (LaunchOptions.h). Stalls the GPU and allocates a staging
	   buffer the size of the target, so it is a debugging facility and not
	   something to call per frame.

	   It exists so that a rendering change can be *looked at* without putting a
	   window on the developer's display - and so an intermediate target can be
	   inspected directly rather than through a debug shader that has to be
	   written, compiled and then removed again. */
	virtual void CaptureTarget(const std::string& passName, const std::string& path) { (void)passName; (void)path; }

	Platform* GetPlatform() { return m_pPlatform; }
	UVector2 GetRenderResolution() const { return UVector2(m_v2RenderResolution.x * m_fRenderScale, m_v2RenderResolution.y * m_fRenderScale); }
	UVector2 GetScreenResolution() const { return m_v2ScreenResolution; }

	float GetRenderScale() const { return m_fRenderScale; }

	CameraRenderData& GetCameraData() { return m_CameraData; }
	void SetCameraData(CameraRenderData cameraData) { m_CameraData = cameraData; }

	const std::vector<StructuredVoxelBuffer>& GetAABBList() const { return m_AABBList; }

	uint32_t GetFrameIndex() const { return m_uiFrameIndex; }
	uint32_t GetMissedFrames() const { return m_uiMissedFrames; }
	uint32_t GetDrawnFrames() const { return m_uiDrawnFrames; }

	uint32_t GetFPS() const { return m_uiFPS; }

	void ResetFrameCount() { m_uiDrawnFrames = 0; }

	PCommandEngine* GetEngine(const std::string& sName) { return m_pCommandEngines[sName].get(); }

	PRenderPass* GetRenderPass(const std::string& sName)
	{
		auto found = m_pRenderPasses.find(sName);
		return found != m_pRenderPasses.end() ? found->second.get() : nullptr;
	}

	PTextureManager* GetTextureManager() const { return m_pTextureManager.get(); }
	PModelManager* GetModelManager() const { return m_pModelManager.get(); }

	void ForceCameraDataUpdate() { m_bCameraDataUpdated = true; };

	void SetFadeValue(float fValue);
	float GetFadeValue() const { return m_fFader; }

	/* Resizes the context, buffers and window */
	virtual bool OnResize(uint32_t uiWidth, uint32_t uiHeight);

	/* Re-applies the render settings to things a per-frame uniform cannot
	   carry. Subscribed to Settings::RenderQualityChanged in Initialize, so
	   nothing has to remember to call it.

	   Almost every render setting reaches the shaders through the camera
	   constant buffer and needs nothing here - see CameraData.hlsl's
	   renderQuality. Exactly two are sizes of GPU images and so cannot:
	   ShadowQuality decides how large the sun shadow map is, and
	   ResolutionScale decides how large the Voxel and Particle targets are.
	   Both mean reallocating attachments, which means idling the device, which
	   is why this is an event on a settings change rather than a per-frame
	   comparison.

	   The base does nothing: a context with no passes has nothing to resize. */
	virtual void ApplyRenderSettings() {}

	/* The app is about to lose (or has just regained) the window/surface -
	   Android's onPause/onResume. Desktop never calls these; the default does
	   nothing, which is correct there. VKRenderContext is the real
	   implementation - see its header for why a resize is not enough. */
	virtual void SuspendForBackground() {}
	virtual bool ResumeFromBackground() { return true; }

	Event<bool> FullscreenChanged;
	Event<uint32_t, uint32_t, IVector2> SizeChanged;

protected:
	RenderContext(Platform* pPlatform);

	struct DebugDrawLine {
		Vector4 m_Position;
		Vector4 m_Color = Vector4(0.f, 1.f, 0.f, 1.f);
	};

	void InitializeRenderLoop();

	/* Textures */
	virtual void LoadTexture(TextureReference* pTextureReference);
	virtual void DestroyTexture(const TextureReference* pTextureRef);

	/* Shaders */
	virtual void LoadShader(ShaderReference* pTextureReference) = 0;

	/* Events */
	void OnFullscreenChanged(bool bFullscreen = false);

	uint32_t m_uiFrameIndex = 0;

	Platform* m_pPlatform;
	Settings* m_pSettings = nullptr;

	std::unique_ptr<PTextureManager> m_pTextureManager = nullptr;
	std::unique_ptr<PModelManager> m_pModelManager = nullptr;

	// Render Targets
	std::unordered_map<std::string, std::unique_ptr<PRenderPass>> m_pRenderPasses;

	// Compute passes
	std::unordered_map<std::string, std::unique_ptr<PComputePass>> m_pComputePasses;

	// Command Engines
	std::unordered_map<std::string, std::unique_ptr<PCommandEngine>> m_pCommandEngines;

	// Resources
	std::vector<std::unique_ptr<View>> m_pViews;
	std::vector<std::unique_ptr<Shader>> m_pShaders;
	std::vector<std::unique_ptr<Sampler>> m_pSamplers;
	std::vector<std::unique_ptr<Mapper>> m_pMappers;

	std::unordered_map<std::string, std::unique_ptr<Buffer>> m_mBuffers;
	
	ParticlePass* m_pParticlePass = nullptr;
	uint32_t m_uiParticleCount = 0;
	Mapper* m_pParticleMapper = nullptr;

	Mapper* m_pVoxelMapper = nullptr;
	uint32_t* m_pVoxelData = nullptr;

	Mapper* m_pBrickMapper = nullptr;
	VoxelBrickGrid m_BrickGrid;

	/* The coverage pyramid's GPU form (RENDERING_PLAN.md 7.1b route B): a 3D
	   R8 texture whose mip chain is the pyramid, so an AO cone step is one
	   SampleLevel instead of the eight hand-written fetches route A measured
	   at 4.6x the rest of the cone.

	   The staging mapper is the pyramid's finest level as unorm bytes, written
	   by VoxelBrickGrid::FlushDirty and copied box by box into mip 0; the rest
	   of the chain is blitted from it on the GPU. Back-buffered in lockstep
	   with the voxel and brick mappers, for the same reason they are. */
	View* m_pPyramidView = nullptr;
	Mapper* m_pPyramidStaging = nullptr;
	std::vector<ImageRegion> m_PyramidRegions;
	float m_fPyramidAuditTimer = 0.f;

	/* Far field. Its own brick grid, over its own cell grid rather than the
	   window's voxels - the two never interact, and keeping them separate is
	   what lets the far-field marcher be the same two-level walk. */
	Mapper* m_pFarFieldMapper = nullptr;
	Mapper* m_pFarFieldBrickMapper = nullptr;
	FarFieldVolume m_FarField;
	FarFieldBaker::Progress m_FarFieldBuild;
	VoxelBrickGrid m_FarFieldBricks;

	bool m_bFaderUpdated = false;

	// Frontend resources
	std::vector<StructuredVoxelBuffer> m_AABBList;

	/* DYNAMIC_MODELS_PLAN.md phase 2. This frame's dynamic-renderer transforms
	   and the quads that reference them - filled by SubmitModelInstance/
	   SubmitModelQuads, cleared once a frame the same place m_AABBList is,
	   uploaded to pModelInstanceBuffer/pModelQuadInstanceBuffer in Present the
	   same way m_AABBList is uploaded to the AABB buffer. */
	std::vector<ModelInstanceData> m_ModelInstances;
	std::vector<ModelQuadInstance> m_ModelQuadInstances;

	/* The shared greedy-mesh quad store (ModelMeshStore, VoxelMesher.h) mirrored
	   to the GPU. Grows across a session as new model frames are first meshed,
	   never shrinks - SyncModelMeshStore in Present appends the new range only
	   when ModelMeshStore's own count has grown since the last sync, which is
	   rare after the first few seconds of a level.

	   The capacity is tracked separately from the used length because the
	   allocation is grown geometrically: Mapper::Resize destroys the VkBuffer
	   in-flight submissions are reading from, so it has to be rare and drained,
	   not once per newly meshed animation frame. */
	Mapper* m_pModelMeshMapper = nullptr;
	uint32_t m_uiModelMeshUploadedQuads = 0;
	uint32_t m_uiModelMeshCapacityWords = 0;

	VoxelModelPass* m_pVoxelModelPass = nullptr;

	std::vector<RenderData> m_RenderList;
	std::vector<SpriteData> m_SpriteList;

	/* The bindless texture array, packed by what this frame actually draws.
	 *
	 * The array used to be indexed by TextureManager ID directly, which made
	 * its required size the *highest live ID* rather than the number of
	 * textures a frame references. Those are very different numbers: this
	 * project ships 133 PNGs and a level keeps well over a hundred of them
	 * resident, while a frame of UI samples a few dozen. The array cannot
	 * simply be grown to fit - 96 is a hard limit on A12Z hardware, where
	 * MoltenVK binds the set through a Metal indirect argument buffer and
	 * Metal refuses one with more than 96 textures in it (see
	 * VKPassBinding::m_uiBindlessCapacity for the exact error). So every
	 * texture whose ID landed past 96 sampled whatever occupied the last slot,
	 * which is why in-game text garbled while the main menu - far fewer
	 * textures loaded - looked correct.
	 *
	 * Packing decouples the two. IDs stay whatever TextureManager hands out
	 * and can climb as far as the content needs; the descriptor index a sprite
	 * carries is a slot in this frame's working set, assigned in first-seen
	 * order. Overflow now needs more than 96 *distinct textures in one frame*,
	 * which the content does not do.
	 *
	 * Both halves of the frame read this: PackBindlessTextures rewrites the
	 * sprite copy that is uploaded, and VKRenderPass::WriteDescriptors fills
	 * slot N with m_BindlessTextureIDs[N]. They have to agree, which is why
	 * the packing happens in Present immediately before the sprite upload and
	 * not, say, at submission time. */
	std::vector<uint32_t> m_BindlessTextureIDs;

	/* The sprite list as uploaded, with TextureID replaced by its packed slot.
	 *
	 * A copy rather than a rewrite of m_SpriteList, because the sprite list is
	 * rebuilt on the *fixed* tick and Present runs on the render tick: a frame
	 * where no fixed tick happened would otherwise re-pack IDs that were
	 * already slots, and bind a completely different set of textures. */
	std::vector<SpriteData> m_PackedSpriteList;

	/* Scratch for the packing: TextureManager ID -> slot this frame, or
	   k_uiUnpackedTexture. Kept as a member only to avoid reallocating it
	   every frame; it is reset before it is read again. */
	std::vector<uint32_t> m_BindlessSlotForTexture;

	/* Most distinct textures any one frame has referenced this session. Only
	   interesting as it approaches m_uiBindlessCapacity, which is what it is
	   reported for. */
	size_t m_uiPeakBindlessWorkingSet = 0;

	bool m_bWarnedBindlessWorkingSet = false;

	// Present drops non-finite model instances; said once, not once a frame.
	bool m_bWarnedInvalidModelQuads = false;

#if defined(EDITOR) || defined(_DEBUG)
	static const int m_iSphereResolution = 30;
	static const int m_iSphereLineCount = (m_iSphereResolution + 1) * 3;

	std::vector<DebugDrawLine> m_DebugDrawLines;
	std::vector<Vector3> m_UnitDebugSphere;
#endif

	float m_fFader = 0.f;
	float m_fFadeTime = 1.f;

	float m_fFrameTimer = 0.f;

	/* Per-frame deltas of the current second, for the [fps] line's percentile.
	   Fixed size: a frame limiter of 0.005 gives 200 a second, and anything
	   past this is a second so pathological the average is the least of it. */
	static const uint32_t k_uiMaxFrameSamples = 4096;

	float m_fFrameSamples[k_uiMaxFrameSamples] = {};
	uint32_t m_uiFrameSamples = 0;

	bool m_bIsFullscreen = false;
	/* Off by default: physics colliders and the like are a development aid, not
	   something the game or a freshly opened editor should draw. The editor's
	   View menu still toggles it - Editor::m_bRenderDebugLines starts matched
	   to this.

	   m_bDebugCleared starts false so the first frame still runs the debug
	   pass's clear path once. Post processing samples that target
	   unconditionally, so a pass that never drew and never cleared leaves it
	   undefined. */
	bool m_bDebugEnabled = false;
	bool m_bDebugCleared = false;

	bool m_bFarFieldEnabled = true;

	/* The camera of the previous upload, which is the one the voxel image post
	   processing composites was rendered with - see CameraData.hlsl's
	   sceneInvMvp. Identity until the second upload; the first frame has no
	   previous image to be out of step with. */
	struct SceneCamera
	{
		Matrix4 m_InvMVP = Matrix4(1.f);
		Vector4 m_WorldPos = Vector4(0.f);
		Vector4 m_Offset = Vector4(0.f);
	};

	SceneCamera m_PreviousSceneCamera;

	CameraRenderData m_CameraData;

	/* Consecutive Present calls that found the GPU still busy. */
	uint32_t m_uiStalledFrames = 0;

	UVector2 m_v2RenderResolution = UVector2(1, 1);
	UVector2 m_v2ScreenResolution = UVector2(1, 1);

	float m_fRenderScale = 1.0f;

	uint32_t m_uiMissedFrames = 0;
	uint32_t m_uiDrawnFrames = 0;
	uint32_t m_uiFPS = 0;

	/* See GetVoxelGeneration. Starts at 1 so a BakeData that has never been
	   written (Generation 0) always reads as stale. */
	uint32_t m_uiVoxelGeneration = 1;

	bool m_bIsDrawTextureCopied = false;
	bool m_bCameraDataUpdated = false;
};
