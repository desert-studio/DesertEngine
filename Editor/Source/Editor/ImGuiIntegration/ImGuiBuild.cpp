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

#include <ImGui/backends/imgui_impl_glfw.cpp>
#include <ImGui/backends/imgui_impl_vulkan.cpp>
