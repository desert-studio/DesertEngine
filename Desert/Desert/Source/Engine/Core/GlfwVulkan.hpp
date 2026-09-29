#pragma once

// GLFW's Vulkan entry points, declared whatever order the translation unit included things in.
//
// glfw3.h declares glfwCreateWindowSurface only if a Vulkan header was included BEFORE its first
// inclusion in the translation unit; its include guard ignores every later GLFW_INCLUDE_VULKAN. Under
// the MSVC unity build a translation unit is a group of sources, so a source that included
// Engine/Core/Glfw.hpp (bare GLFW) could land ahead of VulkanSwapChain.cpp -- SPAWN1 added one source,
// the groups shifted, and every Windows job failed with 'glfwCreateWindowSurface': identifier not
// found while macOS (no unity) stayed green.
//
// So this header declares the entry point it serves ITSELF, after Vulkan: if glfw3.h already declared
// it the redeclaration is identical (same GLFWAPI, same C linkage), and if it did not, this is the
// declaration. Only sources that include this header may call it
// (BuildScriptContract.GlfwIsIncludedOnlyThroughItsEntryHeaders).
#include <vulkan/vulkan.h>

#include <Engine/Core/Glfw.hpp>

extern "C"
{
    GLFWAPI VkResult glfwCreateWindowSurface( VkInstance instance, GLFWwindow* window,
                                              const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface );
}
