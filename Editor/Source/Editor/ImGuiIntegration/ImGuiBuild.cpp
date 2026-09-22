// The Dear ImGui BACKENDS (GLFW + Vulkan), compiled into the Editor.
//
// BuildScripts/ThirdParty/ImGui.lua builds the toolkit's core only; the two backend .cpp files are
// pulled in here instead of being listed there, because they need the Vulkan headers and the GLFW
// headers, which the core library is deliberately independent of. This translation unit used to be
// Engine/imgui/ImGuiBuild.cpp — which is how the engine static library came to contain an interface
// toolkit no engine code draws with.
#define IMGUI_IMPL_API
#include <vulkan/vulkan.h>
#include <ImGui/imgui.h>

// NOLINTBEGIN(bugprone-suspicious-include) — and this is the rare case the suppression is FOR.
// The check exists to catch a `.cpp` pulled in by accident, which compiles the same code twice and
// gives duplicate symbols at link. Here the inclusion is the entire purpose of the translation unit:
// these two backends are not in the ImGui library target precisely because they need the Vulkan and
// GLFW headers, and this is the one place that has both. Restructuring to satisfy the check would
// mean adding them to a library that would then have to grow those dependencies — the check's
// remedy, applied here, is worse than what it prevents.
#include <ImGui/backends/imgui_impl_glfw.cpp>
#include <ImGui/backends/imgui_impl_vulkan.cpp>
// NOLINTEND(bugprone-suspicious-include)
