#pragma once

// THE DOOR TO GLFW. Every engine, editor and runtime source reaches GLFW through this header (or, for
// the Vulkan entry points, through Engine/Core/GlfwVulkan.hpp) and never through <GLFW/glfw3.h>
// directly (BuildScriptContract.GlfwIsIncludedOnlyThroughItsEntryHeaders).
//
// This door does NOT pull in Vulkan: it is reached from Window.hpp -> Application.hpp -> Camera.hpp ->
// Components.hpp, i.e. from tools and suites (WorldGen, SceneMigrator, clang-tidy's header pass) that
// have no Vulkan SDK include directory. Whether glfw3.h declared its Vulkan entry points is therefore
// order-dependent here, and nothing that includes only this header may call them.
//
// No OpenGL header either (GLFW_INCLUDE_NONE): the engine draws through Vulkan only, and the dedicated server's
// Linux image, which compiles the engine headers that reach this door, has no GL headers at all (glfw3.h's
// default pulls <GL/gl.h> on Linux, <OpenGL/gl.h> on macOS, the SDK's GL/gl.h on Windows).
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>
