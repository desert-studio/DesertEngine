#pragma once

// VFX-12. THE ENGINE MODULE LIBRARY as the VFX System editor lists it (UE: the Niagara module picker over the
// engine's module scripts). Two kinds of `engine:` module exist: the PARTICLE modules, one `.shader` each in
// EngineModuleDir() (VFXStackCompiler reads them; their `Input` lines carry Units / Range / Default), and the CPU
// modules of the EmitterUpdate group (VFXEmitterSpawn reads them), whose inputs are stated here because no shader
// declares them. One entry per module: what a stack row of it must hold and how the editor shows each input.

#include <Engine/VFX/VFXStackCompiler.hpp>

#include <Common/Core/ResultStr.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::VFX
{
    struct VFXModuleInfo
    {
        std::string                Module; ///< the stack row's reference, "engine:<Name>"
        std::vector<VFXModuleDecl> Inputs; ///< in declaration order
        /// False when a row of it cannot be made from defaults alone (SpawnFromChannel needs its channel bound):
        /// the editor edits such a row but does not offer the module in "Add module".
        bool Addable = true;
    };

    /// The EmitterUpdate group's CPU modules (VFXEmitterSpawn.hpp states what each input means).
    [[nodiscard]] std::span<const VFXModuleInfo> CpuModuleLibrary();

    /// Every particle module in @p dir (`<Name>.shader` -> "engine:<Name>"), sorted by name. A module that does
    /// not parse is an error naming its file.
    Common::ResultStr<std::vector<VFXModuleInfo>> ReadParticleModuleLibrary( const std::filesystem::path& dir );

    /// The row a newly added module starts as: every input a Value of its declared Default (zero when none).
    [[nodiscard]] Assets::Serialization::VFXModuleUse DefaultModuleUse( const VFXModuleInfo& module );

    /// The declaration of @p input in @p module of @p library, or nullptr.
    [[nodiscard]] const VFXModuleDecl* FindModuleInput( std::span<const VFXModuleInfo> library,
                                                        std::string_view module, std::string_view input );
} // namespace Desert::VFX
