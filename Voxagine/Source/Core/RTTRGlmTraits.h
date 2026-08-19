#pragma once

#include "Core/Math.h"

#include <rttr/detail/misc/template_type_trait.h>

namespace rttr::detail
{
	// RTTR 0.9.6's generic template traits are ambiguous for GLM on newer Clang.
#define VOXAGINE_RTTR_NON_TEMPLATE_TYPE(Type) \
	template<> \
	struct template_type_trait<Type> : std::false_type \
	{ \
		static std::vector<::rttr::type> get_template_arguments() { return {}; } \
	}

	VOXAGINE_RTTR_NON_TEMPLATE_TYPE(Vector2);
	VOXAGINE_RTTR_NON_TEMPLATE_TYPE(Vector3);
	VOXAGINE_RTTR_NON_TEMPLATE_TYPE(Vector4);
	VOXAGINE_RTTR_NON_TEMPLATE_TYPE(IVector2);
	VOXAGINE_RTTR_NON_TEMPLATE_TYPE(IVector3);
	VOXAGINE_RTTR_NON_TEMPLATE_TYPE(IVector4);
	VOXAGINE_RTTR_NON_TEMPLATE_TYPE(UVector2);
	VOXAGINE_RTTR_NON_TEMPLATE_TYPE(UVector3);
	VOXAGINE_RTTR_NON_TEMPLATE_TYPE(UVector4);
	VOXAGINE_RTTR_NON_TEMPLATE_TYPE(Quaternion);
	VOXAGINE_RTTR_NON_TEMPLATE_TYPE(Matrix4);

#undef VOXAGINE_RTTR_NON_TEMPLATE_TYPE
}
