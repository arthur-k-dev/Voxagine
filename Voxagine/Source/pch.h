#pragma once

#include <iostream>
#include <memory>
#include <utility>
#include <algorithm>
#include <functional>

#include <string>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <assert.h>


#include "Core/Utils/Utils.h"
#include "Core/Math.h"
#include "Core/VColors.h"
#include "Core/GameTimer.h"
/* Core/FileBrowser.h is deliberately not here. It pulls in nativefiledialog,
   which has exactly one consumer (Editor.cpp) and no implementation on iOS or
   Android - and a pch include put it in front of all 147 translation units. */

#include "Core/ECS/Systems/ScriptSystem.h"

#include "Core/ECS/ComponentSystem.h"
#include "Core/ECS/Component.h"
#include "Core/ECS/Components/Transform.h"

#include "Core/ECS/Entity.h"

#define VX_UNUSED(_VAR)             ((void)(_VAR))                                // Used to silence "unused variable warnings". Often useful as asserts may be stripped out from final builds.