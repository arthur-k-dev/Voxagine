#include "pch.h"
#include "Core/Platform/Rendering/RenderDocCapture.h"

#include <cstdio>
#include <cstdlib>

#include "External/renderdoc/renderdoc_app.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace
{
	const char* EnvOrNull(const char* pName)
	{
		const char* pValue = std::getenv(pName);

		return (pValue != nullptr && pValue[0] != '\0') ? pValue : nullptr;
	}

	/* The library, or null. Two cases and they are not interchangeable:

	   - **Injected.** RenderDoc's UI (or renderdoccmd) launched this process and
	     its library is already mapped. `RTLD_NOLOAD` finds it without loading
	     anything; loading a second copy of it is undefined and is the documented
	     way to get a confusing crash.
	   - **Self-hosted.** Nobody injected anything and VOXAGINE_RENDERDOC asks us
	     to load it, which only works before the Vulkan instance exists. */
	void* OpenRenderDoc(bool bAllowLoad)
	{
#if defined(_WIN32)
		HMODULE handle = GetModuleHandleA("renderdoc.dll");

		if (handle == nullptr && bAllowLoad)
			handle = LoadLibraryA("renderdoc.dll");

		return reinterpret_cast<void*>(handle);
#else
		void* pHandle = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);

		if (pHandle == nullptr && bAllowLoad)
			pHandle = dlopen("librenderdoc.so", RTLD_NOW);

		return pHandle;
#endif
	}

	void* LookupSymbol(void* pHandle, const char* pName)
	{
#if defined(_WIN32)
		return reinterpret_cast<void*>(
			GetProcAddress(reinterpret_cast<HMODULE>(pHandle), pName));
#else
		return dlsym(pHandle, pName);
#endif
	}
}

RenderDocCapture& RenderDocCapture::Get()
{
	static RenderDocCapture s_Capture;
	return s_Capture;
}

void RenderDocCapture::Initialize()
{
	if (m_bInitialized)
		return;

	m_bInitialized = true;

	const bool bAllowLoad = EnvOrNull("VOXAGINE_RENDERDOC") != nullptr ||
		EnvOrNull("VOXAGINE_RENDERDOC_CAPTURE") != nullptr;

	void* pHandle = OpenRenderDoc(bAllowLoad);

	if (pHandle == nullptr)
	{
		/* Not installed, or not asked for. Both are the ordinary case and
		   neither is worth a line of output. */
		return;
	}

	pRENDERDOC_GetAPI pGetApi =
		reinterpret_cast<pRENDERDOC_GetAPI>(LookupSymbol(pHandle, "RENDERDOC_GetAPI"));

	if (pGetApi == nullptr)
	{
		fprintf(stderr, "[renderdoc] library found but RENDERDOC_GetAPI is missing\n");
		return;
	}

	RENDERDOC_API_1_6_0* pApi = nullptr;

	/* 1.6.0 is the oldest version carrying everything used here, and the API is
	   append-only: a newer RenderDoc answers a 1.6.0 request with a struct this
	   build can read. A version *older* than that returns 0, which is a clear
	   "your RenderDoc is too old" rather than a mis-shaped call. */
	if (pGetApi(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&pApi)) != 1 ||
		pApi == nullptr)
	{
		fprintf(stderr, "[renderdoc] RENDERDOC_GetAPI(1.6.0) refused - the installed "
		                "RenderDoc is older than this build expects\n");
		return;
	}

	m_pApi = pApi;

	const char* pPath = EnvOrNull("VOXAGINE_RENDERDOC_PATH");

	SetCapturePathTemplate(pPath != nullptr ? pPath : "captures/voxagine");

	/* Nothing here wants RenderDoc's own overlay or its key bindings: this is a
	   headless, scripted process and both only exist to be driven by hand. */
	pApi->MaskOverlayBits(eRENDERDOC_Overlay_None, eRENDERDOC_Overlay_None);
	pApi->SetCaptureKeys(nullptr, 0);

	int iMajor = 0;
	int iMinor = 0;
	int iPatch = 0;
	pApi->GetAPIVersion(&iMajor, &iMinor, &iPatch);

	fprintf(stderr, "[renderdoc] attached, API %d.%d.%d\n", iMajor, iMinor, iPatch);

	if (const char* pFrames = EnvOrNull("VOXAGINE_RENDERDOC_CAPTURE"))
	{
		const int iFrames = std::atoi(pFrames);

		if (iFrames > 0)
			TriggerCapture(static_cast<uint32_t>(iFrames));
	}
}

void RenderDocCapture::TriggerCapture(uint32_t uiFrames)
{
	if (m_pApi == nullptr || uiFrames == 0)
		return;

	RENDERDOC_API_1_6_0* pApi = static_cast<RENDERDOC_API_1_6_0*>(m_pApi);

	/* RenderDoc frames the capture from the swapchain present itself, so there
	   is nothing to wrap and no risk of bracketing the wrong submission - which
	   matters here, where Present is a blit from a render target rather than a
	   pass (CLAUDE.md, "The double-gamma present"). */
	pApi->TriggerMultiFrameCapture(uiFrames);
}

uint32_t RenderDocCapture::CaptureCount() const
{
	if (m_pApi == nullptr)
		return 0;

	return static_cast<RENDERDOC_API_1_6_0*>(m_pApi)->GetNumCaptures();
}

void RenderDocCapture::SetCapturePathTemplate(const std::string& sTemplate)
{
	if (m_pApi == nullptr)
		return;

	static_cast<RENDERDOC_API_1_6_0*>(m_pApi)->SetCaptureFilePathTemplate(sTemplate.c_str());
}
