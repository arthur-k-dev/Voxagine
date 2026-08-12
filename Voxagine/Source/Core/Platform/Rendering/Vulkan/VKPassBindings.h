#pragma once

#include "Core/Platform/Rendering/RenderPass.h"
#include "Core/Platform/Rendering/ComputePass.h"

#include <Core/Platform/Rendering/Vulkan/VulkanAPI.h>

#include <cstdint>
#include <string>
#include <vector>

/* Resolves a pass's resources to HLSL registers, and from there to Vulkan
 * bindings.
 *
 * The D3D12 backend assigned root parameter slots by first-seen resource name
 * (DXRenderPass::GetViewID) while the actual shader registers came from
 * separate per-type counters advancing across the resource lists in a fixed
 * order. The shaders were authored against those registers, so the order is
 * part of the contract with the HLSL and cannot be changed:
 *
 *   1. Buffers    - constant -> b, read-write -> u, otherwise -> t
 *   2. Mappers    - read-write -> u, otherwise -> t
 *   3. PassOutput - one t per render view, per pass
 *   4. Textures   - t
 *   5. Bindless   - t, and always last
 *   6. Samplers   - s, in declaration order
 *
 * Computed once and used both to build the descriptor set layout and to write
 * descriptors at draw time. Deriving those separately is how a layout and its
 * writes drift apart, and Vulkan only reports the symptom. */
struct VKPassBinding
{
	/* Slots reserved for the shader's bindless texture array. Must match
	   UIRenderer.ps.hlsl's BindlessTextureCapacity and VoxelBaker.cs.hlsl's
	   BindlessModelCapacity exactly: glslc compiles an unsized HLSL resource
	   array as length one, and a platform-specific size would make the shared
	   SPIR-V output depend on whichever platform happened to build it last.

	   96 is a hardware ceiling on the iPad's A12Z, not a preference. MoltenVK
	   binds this set through a Metal indirect argument buffer, and Metal caps
	   the textures in one at 96 - it says so exactly, and refuses the pipeline:

	     Total number of indirect argument buffer resources exceeded for
	     indirect textures (120/96)
	     vkCreateGraphicsPipelines failed for 'UI Renderer'

	   (Raising it far enough also trips a second, separate limit: an individual
	   [[texture(N)]] index must be <= 127, which at 256 put the Voxel pass's
	   voxelWorldData at texture(262).)

	   **This is smaller than the content needs, and that is a known defect.**
	   The array is indexed by TextureManager ID, so it has to be as large as the
	   highest live ID rather than as large as one frame's working set. A level
	   here keeps more than 96 textures resident, so AcquireID runs out of slots
	   and returns UINT32_MAX; every texture past that point stops rendering,
	   which is what garbles in-game text while the main menu - far fewer
	   textures loaded - looks correct.

	   Raising this cannot fix that on A12Z. The fix is to stop using the
	   TextureManager ID as the descriptor index: pack only the textures a frame
	   actually references into the array and give the shader that slot instead,
	   so the array is sized by the working set. Desktop tolerates the current
	   scheme only because it allows far more descriptors. */
	static constexpr uint32_t m_uiBindlessCapacity = 96;

	enum Kind
	{
		E_CONSTANT_BUFFER,
		E_STORAGE_BUFFER,
		E_SAMPLED_IMAGE,
		E_STORAGE_IMAGE,
		E_SAMPLER,
		E_BINDLESS_TEXTURES,

		/* A Mapper with a colour format was a formatted SRV/UAV over a buffer
		   in D3D12. Vulkan calls that a texel buffer and binds it through a
		   VkBufferView - it is not a storage image, which is what the first
		   cut of this declared. */
		E_UNIFORM_TEXEL_BUFFER,
		E_STORAGE_TEXEL_BUFFER
	};

	Kind m_Kind = E_CONSTANT_BUFFER;

	/* What m_pSource points at. Both Buffer and Mapper can produce a storage
	   buffer binding but they are read differently, so the descriptor writer
	   cannot infer this from m_Kind. */
	enum Source
	{
		E_SOURCE_NONE,
		E_SOURCE_BUFFER,
		E_SOURCE_MAPPER,
		E_SOURCE_VIEW,
		E_SOURCE_SAMPLER,

		/* Another pass's render target, resolved at write time because
		   the source pass may flip back buffers between frames. */
		E_SOURCE_PASS
	};

	Source m_Source = E_SOURCE_NONE;

	/* HLSL register number within its class, before the VKBindings shift. */
	uint32_t m_uiRegister = 0;

	/* Vulkan binding, i.e. the register plus its class shift. */
	uint32_t m_uiBinding = 0;

	/* Greater than one only for the bindless array. */
	uint32_t m_uiCount = 1;

	VkShaderStageFlags m_Stages = 0;

	/* Whichever of Buffer/Mapper/View/Sampler produced this, or null for
	   pass outputs and bindless slots. Not owned. */
	const void* m_pSource = nullptr;

	/* Which of the source pass's render views this binding refers to. */
	uint32_t m_uiViewIndex = 0;

	std::string m_Name;
};

/* Order matches DXRenderPass::Init exactly. */
std::vector<VKPassBinding> VKBuildRenderPassBindings(const RenderPass::Data& data);

std::vector<VKPassBinding> VKBuildComputePassBindings(const ComputePass::Data& data);

/* Feeds the result into a VKDescriptorLayout. Separate so the binding table
   can be inspected and asserted against without building anything. */
class VKDescriptorLayout;
bool VKApplyBindings(VKDescriptorLayout& layout, const std::vector<VKPassBinding>& bindings);
