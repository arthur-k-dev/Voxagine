#pragma once

#include "Core/Platform/Rendering/RenderDefines.h"

class Sampler
{
public:
	struct Info
	{
		PFilterMode			m_FilterMode = R_DEF_FILTER_MODE;
		PWrapMode			m_WrapMode = R_DEF_WRAP_MODE;

		/* Filtering *between* mip levels, which is a separate decision from
		   filtering within one. The coverage pyramid wants linear within a
		   level and point across them: a cone picks a level per step and the
		   level is what makes the step cost one fetch, so blending two of them
		   doubles the cost of the thing route B exists to make cheap. */
		PFilterMode			m_MipFilterMode = R_DEF_FILTER_MODE;
	};

	Sampler(PRenderContext* pContext, const Info& info);
	/* Defined by the backend; deleting a void* here was undefined. */
	virtual ~Sampler();

	const Info& GetInfo() const { return m_Info; }
	void*& GetNative() { return m_pNativeSampler; }

	bool IsInitialized() const { return m_bInitialized; }

protected:
	PRenderContext* m_pContext = nullptr;
	Info m_Info;

	void* m_pNativeSampler = nullptr;
	bool m_bInitialized = false;
};