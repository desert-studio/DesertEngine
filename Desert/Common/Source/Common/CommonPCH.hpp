#pragma once

// THE COMMON PRECOMPILED HEADER (STUDIO-AUDIT B-PCH): the first PCH of the chain Common -> engine (pch.hpp) ->
// editor (EditorPCH.hpp). Precompiled once, in Common-Pch, and reused by every member of the PCH group "Common"
// (Common, DesertUI, Render2DCore: cmake/DesertWorkspace.cmake desert_shared_pch). The engine's pch.hpp opens with it.
// Only what nearly every translation unit includes: the standard containers and strings and Common/Core/Core.hpp
// (Logger, Constants, ResultStr, UUID, AssetHandle). Every source still includes what it uses.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <Common/Core/Core.hpp>
