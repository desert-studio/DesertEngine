#pragma once

// THE SHADER PROGRAMS ONLY A DEVELOPER'S BUILD CAN LOAD.
//
// Every name below is fetched from the shader service ONLY inside `#if DESERT_DEV_INSTRUMENTS`
// (DevInstruments.hpp), by the pipelines the ShippingPipelines register marks DeveloperOnly: the AABB
// debug lines and the overdraw heat map with its resolve (MeshRenderer::SetupDebugLinePass /
// SetupOverdrawPass). A Shipping Runtime is compiled without those call sites, so a Shipping package
// that carried these programs would carry content its binary cannot ask for; Debug and Release
// runtimes create those pipelines at boot, so their packages must carry them.
//
// This list is not a second opinion next to the register: Desert/Tests/Runtime/ShippingPipelines derives
// the set of shader names the player's source set loads only behind the boundary and fails unless it
// equals this list in both directions, and Desert/Tests/Editor/PackagedContent checks that a package of
// each configuration carries exactly the programs its runtime can load.
//
// A program is named by its file stem, the name ShaderService registers it under.

#include <algorithm>
#include <array>
#include <string_view>

namespace Common
{
    inline constexpr std::array<std::string_view, 3> kDeveloperOnlyShaderPrograms = { "DebugLine", "Overdraw",
                                                                                      "OverdrawResolve" };

    inline bool IsDeveloperOnlyShaderProgram( std::string_view programName )
    {
        return std::find( kDeveloperOnlyShaderPrograms.begin(), kDeveloperOnlyShaderPrograms.end(),
                          programName ) != kDeveloperOnlyShaderPrograms.end();
    }

    // The package configuration whose runtime is built with DESERT_DEV_INSTRUMENTS=1 — every configuration
    // but Shipping (BuildScripts/Configurations.lua defines DESERT_CONFIG_SHIPPING for that one only).
    inline bool ConfigHasDeveloperInstruments( std::string_view configName )
    {
        return configName != "Shipping";
    }
} // namespace Common
