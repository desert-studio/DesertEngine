#pragma once

// VFX-12. THE MODULE CATALOG as the VFX System editor lists it (UE: the Niagara module picker over the module
// scripts). Every module is a `.dfxmod` (VFXModuleFormat.hpp) — engine library and project Content alike — and its
// `Input` lines are the ONE declaration of its inputs, editor metadata (Units / Range / Default) included.
//
// The three EmitterUpdate modules the engine runs in C++ (VFXEmitterSpawn.hpp) additionally have a CPU CONTRACT:
// the inputs, in order and by type, that the C++ reads. The contract is not a second declaration — the stack
// compiler refuses a module whose `.dfxmod` inputs differ from it (CheckCpuModuleContract), so the file and the
// C++ cannot drift apart, and the editor still reads only the file.

#include <Engine/VFX/VFXModuleFormat.hpp>

#include <Engine/Assets/Serialization/VFXSystem.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::VFX
{
    /// One module as the editor offers it.
    struct VFXModuleInfo
    {
        Assets::AssetGuidRef       Module; ///< the stack row's reference ({Guid, Path})
        std::string                DisplayName;
        std::string                Category;
        uint8_t                    Stages = 0; ///< VFXModuleStage bits
        std::vector<VFXModuleDecl> Inputs;     ///< in declaration order, with the editor metadata
        /// False when a row of it cannot be made from defaults alone: SpawnFromChannel needs its channel bound,
        /// SpawnFromEvent is appended by the stack compiler for an emitter's EventHandlers and never named by a
        /// stack. The editor edits such a row but does not offer the module in "Add module".
        bool Addable = true;

        [[nodiscard]] bool AllowsStage( VFXModuleStage stage ) const
        {
            return ( Stages & static_cast<uint8_t>( stage ) ) != 0;
        }
    };

    /// The catalog entry of a parsed module; @p path is the Path a stack row states for it.
    [[nodiscard]] VFXModuleInfo MakeModuleInfo( const VFXModuleDefinition& module, std::string path );

    /// The inputs, in order and by type, the engine C++ reads for the CPU module of @p guid (HasCpuBehaviour);
    /// empty for every other module. Names and types only — the metadata is the `.dfxmod`'s.
    [[nodiscard]] std::span<const VFXModuleDecl> CpuModuleContract( const Common::Content::AssetGuid& guid );

    /// A module with C++ behaviour must declare exactly its contract (names, types, order). The refusal names
    /// the module, the first input that differs and both sides.
    Common::BoolResultStr CheckCpuModuleContract( const VFXModuleDefinition& module, std::string_view where );

    /// The row a newly added module starts as: every input a Value of its declared Default (zero when none).
    [[nodiscard]] Assets::Serialization::VFXModuleUse DefaultModuleUse( const VFXModuleInfo& module );

    /// The catalog entry of the module @p ref names (by GUID; the Path is not compared), or nullptr.
    [[nodiscard]] const VFXModuleInfo* FindModule( std::span<const VFXModuleInfo> catalog,
                                                   const Assets::AssetGuidRef&    ref );

    /// The declaration of @p input in the module @p ref names, or nullptr.
    [[nodiscard]] const VFXModuleDecl* FindModuleInput( std::span<const VFXModuleInfo> catalog,
                                                        const Assets::AssetGuidRef& ref, std::string_view input );
} // namespace Desert::VFX
