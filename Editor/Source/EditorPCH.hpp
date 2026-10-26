#pragma once

// THE EDITOR LIBRARIES' PRECOMPILED HEADER (Editor/premake5.lua, EditorLibrary), force-included into every
// source of every editor library. What is here is what the editor's translation units include most - counted
// over Editor/Source when this was written: the standard headers a third to a half of them name, ImGui, glm,
// the Common result/log/constant headers, and the four engine headers (scene, components, asset manager,
// resource registry) that most panels and commands start from and that cost the most to parse.
//
// A header belongs here only if including it in EVERY editor source changes nothing but compile time: no
// header that needs a macro defined before it (imgui_internal.h wants IMGUI_DEFINE_MATH_OPERATORS first, the
// glm/gtx headers GLM_ENABLE_EXPERIMENTAL, <windows.h> WIN32_LEAN_AND_MEAN), and nothing a TU may not see.
// Every source still includes what it uses; this file only makes that cheaper.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Objective-C++ sources (Splash/MacOS, Platform/MacOS) get the standard headers only: the engine headers are
// C++ and are not written to be parsed under Objective-C's keywords.
#ifndef __OBJC__
#include <ImGui/imgui.h>
#include <glm/glm.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/ResultStr.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#endif
