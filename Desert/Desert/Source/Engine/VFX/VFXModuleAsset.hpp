#pragma once

// VFX-MODULE-ASSET. A VFX MODULE AS AN ASSET (`.dfxmod`) - UE's UNiagaraScript with Usage Module (a Niagara
// Module Script): a file with its own GUID that a system's stack names by that GUID, so a project writes its own
// modules next to the engine's and every stack row references one the same way. Our own text format, not a port:
// UE's module is a graph in a UObject package; ours is the stack compiler's existing module block (the
// `Attribute` / `Input` declarations, then GLSL - ParseModuleBlock in VFXStackCompiler.hpp), wrapped with the
// metadata the editor lists it by. One declaration of the inputs serves the editor and the compiler.
//
//     // DesertAsset {"Kind":"VFXModule","Guid":"<32 hex>","Versions":{"VFXM":1},"Dependencies":[]}
//     // free comment lines
//     Module "Gravity"
//     {
//         Stage ParticleUpdate
//         Category Forces
//         Description "Adds a constant acceleration to the accumulated force."
//         Attribute PhysicsForce vec3
//         Input Gravity vec3
//
//         void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i ) { ... }
//     }
//
// The header is the shader header's first-line comment form (ShaderAssetHeader.hpp), since the body is not JSON.
// `Stage` names the stack groups the module may be added to (one or more of EmitterUpdate, ParticleSpawn,
// ParticleUpdate). A module whose stages include EmitterUpdate is a CPU module: its behaviour is engine C++
// registered under its GUID (VFXEmitterSpawn), so its block declares inputs only - no attributes, no GLSL - and a
// file stating EmitterUpdate whose GUID the engine has no behaviour for is refused where the stack resolves it.

#include <Engine/VFX/VFXStackCompiler.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::VFX
{
    inline constexpr const char*      kVFXModuleExtension = ".dfxmod";
    inline constexpr std::string_view kVFXModuleKindName  = "VFXModule";
    /// The header's Versions key and the layout generation it states.
    ///   1 - VFX-MODULE-ASSET: Module "<DisplayName>" block with Stage / Category / Description, the declarations
    ///       and the GLSL body.
    inline constexpr std::string_view kVFXModuleSchemaTagName = "VFXM";
    inline constexpr uint32_t         kVFXModuleSchemaVersion = 1;

    /// The stack groups, as a module's `Stage` line names them.
    enum class VFXModuleStage : uint8_t
    {
        EmitterUpdate  = 1u << 0,
        ParticleSpawn  = 1u << 1,
        ParticleUpdate = 1u << 2,
    };

    /// One parsed `.dfxmod`.
    struct VFXModuleDefinition
    {
        Common::Content::AssetGuid Guid;
        std::string                DisplayName; ///< the `Module "<name>"` string
        std::string                Category;    ///< an id of the project's VFXCategories.json register
        std::string                Description;
        uint8_t                    Stages = 0; ///< VFXModuleStage bits
        VFXParticleModule          Block;      ///< declarations + GLSL (GLSL empty for a CPU module)

        [[nodiscard]] bool AllowsStage( VFXModuleStage stage ) const
        {
            return ( Stages & static_cast<uint8_t>( stage ) ) != 0;
        }
        [[nodiscard]] bool IsCpuModule() const
        {
            return AllowsStage( VFXModuleStage::EmitterUpdate );
        }
    };

    /// Parses a whole `.dfxmod` text. @p where names the file in every error.
    Common::ResultStr<VFXModuleDefinition> ParseVFXModule( std::string_view text, std::string_view where );

    /// The canonical text of @p module (the header line, the block; the GLSL body verbatim).
    std::string WriteVFXModule( const VFXModuleDefinition& module );

    /**
     * @brief What the stack compiler asks for a module: the definition a GUID names.
     *
     * The engine answers through the ContentRegistry (the `.dfxmod` row of that GUID, engine library or project
     * Content alike); tests answer from memory. A GUID that names no module is an error naming it.
     */
    class VFXModuleResolver
    {
    public:
        virtual ~VFXModuleResolver() = default;
        [[nodiscard]] virtual Common::ResultStr<VFXModuleDefinition>
        Resolve( const Common::Content::AssetGuid& guid ) const = 0;
    };
} // namespace Desert::VFX
