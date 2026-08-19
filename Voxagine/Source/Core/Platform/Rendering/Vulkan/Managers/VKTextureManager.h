#pragma once

#include "Core/Platform/Rendering/Managers/TextureManager.h"

#include <vector>

class VKRenderContext;
class CommandEngine;

/* Creates and owns the Views backing texture resources.
 *
 * As with VKModelManager, the DXHeapManager slot reservation has no Vulkan
 * counterpart; descriptor sets are allocated per pass from VKDescriptorLayout. */
class VKTextureManager : public TextureManager
{
public:
	VKTextureManager(VKRenderContext* pContext);
	virtual ~VKTextureManager();

	virtual uint32_t CreateTexture(CommandEngine* pEngine, const std::string& sName,
	                               uint8_t* pData, UVector2 uSize) override;

	virtual uint32_t CreateEmptyTexture() override;

	virtual bool LoadTexture(CommandEngine* pEngine, TextureReference* pTextureReference) override;
	virtual void DestroyTexture(const TextureReference* pTextureReference) override;

private:
	/* Texture IDs are identities, not descriptor array indices - the bindless
	   array is indexed by a per-frame slot, see
	   RenderContext::PackBindlessTextures. They are still reused rather than
	   handed out monotonically, but for a weaker reason than before: it keeps
	   the ID range dense, which keeps m_pViews and the packer's ID-indexed
	   scratch lookup proportional to the live set instead of to everything the
	   session has ever loaded. DXHeapManager did the same with
	   ReserveID/FreeID. */
	uint32_t AcquireID();
	void ReleaseID(uint32_t uiID);

	uint32_t m_uiNextID = 0;
	std::vector<uint32_t> m_FreeIDs;
};
