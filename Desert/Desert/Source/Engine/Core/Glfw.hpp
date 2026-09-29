#pragma once

// THE ONE DOOR TO GLFW. Every engine, editor and runtime source reaches GLFW through this header and
// never through <GLFW/glfw3.h> directly (BuildScriptContract.GlfwIsIncludedOnlyThroughItsEntryHeader).
//
// glfw3.h decides ONCE, at its first inclusion in a translation unit, whether to declare its Vulkan
// entry points (glfwCreateWindowSurface, glfwGetRequiredInstanceExtensions, ...): only if a Vulkan
// header is already in. Its include guard makes every later inclusion a no-op, so a later
// `#define GLFW_INCLUDE_VULKAN` changes nothing. That made the declaration depend on which file was
// compiled FIRST in the translation unit -- and under the MSVC unity build a translation unit is a
// group of sources. SPAWN1 added one source to Desert, the groups shifted, VulkanSwapChain.cpp landed
// after a source that included glfw3.h bare, and every Windows job failed with
// 'glfwCreateWindowSurface': identifier not found. macOS (no unity) compiled it.
//
// Vulkan first, then GLFW: the answer no longer depends on inclusion order.
#include <vulkan/vulkan.h>

#ifndef GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_VULKAN
#endif
#include <GLFW/glfw3.h>
