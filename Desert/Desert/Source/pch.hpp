#pragma once

// THE ENGINE MODULE LIBRARIES' PRECOMPILED HEADER (Desert/Desert/CMakeLists.txt: PCH group "Engine"), force-included
// into every source of every engine module library. The chain's second link (STUDIO-AUDIT B-PCH): the Common PCH,
// then the engine headers most engine sources name. No singleton header: a source that derives from one includes it.

#include <Common/CommonPCH.hpp>

#include <glm/glm.hpp>

#include <Engine/Assets/Common.hpp>
#include <Engine/Assets/AssetGuidRef.hpp>
