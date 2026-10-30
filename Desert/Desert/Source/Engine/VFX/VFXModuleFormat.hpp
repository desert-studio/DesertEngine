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
//         Category Forces
//         Description "Adds a constant acceleration to the accumulated force."
//         Usage ParticleUpdate
//         Attribute PhysicsForce vec3
//         Input Gravity vec3 = 0 -981 0 Unit cm/s2
//
//         void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i ) { ... }
//     }
//
// The header is the shader header's first-line comment form (ShaderAssetHeader.hpp), since the body is not JSON.
// `Usage` (dev VFX-12, UE Module Usage) names the stack groups the module may be added to, and it decides what
// runs where: EmitterSpawn / EmitterUpdate run on the CPU, as engine C++ registered under the module's GUID
// (VFXEmitterSpawn) - such a module declares Inputs only, and one whose GUID the engine has no behaviour for is
// refused where the stack resolves it; ParticleSpawn / ParticleUpdate run the block's GLSL on the GPU. One module
// is one or the other (ParseModuleBlock). An `Input` states the default a new stack row starts from
// (`= <numbers>`), or `= Binding` for one the user must bind (SpawnFromChannel's Channel).
// An optional trailing `Unit <unit>` (one of VFX::kVFXInputUnits) is the input's unit metadata (UE: the module
// input's display unit): the value is stored in that unit and the system editor's stack widgets print it. Absent,
// the input is unitless; a file without any `Unit` reads as before, so the layout generation stays VFXM 1.

#include <Engine/Assets/AssetGuidRef.hpp>
#include <Engine/Assets/TextAssetHeaderStamp.hpp>
#include <Engine/VFX/VFXStackCompiler.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::VFX
{
    inline constexpr const char*      kVFXModuleExtension = ".dfxmod";
    /// The header's Versions key and the layout generation it states.
    ///   1 - VFX-PORT-1 (int/vfx VFX-MODULE-ASSET on dev): Module "<DisplayName>" block with Category /
    ///       Description, then the stack compiler's Usage / Attribute / Input declarations and the GLSL body.

    /// The header's stated subsystem: the file layout's generation (Assets::kVFXModuleSchemaTag).
    [[nodiscard]] inline std::span<const Common::Content::SubsystemVersion> VFXModuleTextSubsystems()
    {
        static const std::array<Common::Content::SubsystemVersion, 1> versions = {
             Common::Content::SubsystemVersion{ Assets::kVFXModuleSchemaTag, Assets::kVFXModuleSchemaVersion } };
        return versions;
    }

    /// One parsed `.dfxmod`.
    struct VFXModuleDefinition
    {
        Common::Content::AssetGuid Guid;
        std::string                DisplayName; ///< the `Module "<name>"` string
        std::string                Category;    ///< an id of the project's VFXCategories.json register
        std::string                Description;
        VFXParticleModule          Block; ///< Usage, declarations + GLSL (GLSL empty for a CPU module)

        [[nodiscard]] bool operator==( const VFXModuleDefinition& ) const = default;

        [[nodiscard]] bool AllowsStage( VFXStage stage ) const { return ( Block.Usage & UsageBit( stage ) ) != 0; }
        /// EmitterSpawn / EmitterUpdate: engine C++ registered under Guid runs it on the CPU (VFXEmitterSpawn).
        [[nodiscard]] bool HasCpuStage() const { return ( Block.Usage & kVFXCpuUsage ) != 0; }
        /// ParticleSpawn / ParticleUpdate: the block's GLSL runs it on the GPU.
        [[nodiscard]] bool HasParticleStage() const { return ( Block.Usage & kVFXParticleUsage ) != 0; }
    };

    /// Parses a whole `.dfxmod` text. @p where names the file in every error.
    Common::ResultStr<VFXModuleDefinition> ParseVFXModule( std::string_view text, std::string_view where );

    /// The canonical text of @p module (the header line, the block; the GLSL body verbatim).
    std::string WriteVFXModule( const VFXModuleDefinition& module );

    /// The format's rules for a definition held in memory (an editor's working copy): its canonical text parses
    /// back to the same definition. The refusal is the reader's own, naming the module.
    Common::BoolResultStr ValidateVFXModule( const VFXModuleDefinition& module );

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
        /// Every module this resolver can resolve, as the {Guid, Path} a stack row states, sorted by Path: the
        /// VFX System editor's module picker (UE's module list, filtered there by Usage).
        [[nodiscard]] virtual std::vector<Assets::AssetGuidRef> List() const = 0;
    };

    /// Where the engine's module library lies under the engine content mount (Constants::Path::
    /// ENGINE_CONTENT_PATH): the mirror of the project's VFX folder that ScanRootsOf walks for the VFXModule kind.
    inline constexpr std::string_view kVFXEngineModuleFolder = "VFX/Modules";

    /**
     * @brief Modules read straight from `.dfxmod` files and from memory, without the ContentRegistry: what a
     * program that runs before or without a registry resolves through (Tools/SceneMigrator raising `.dfx` files,
     * the stack-compile suite's in-memory modules). The engine resolves through the registry
     * (Assets::RegistryVFXModuleResolver), never through this.
     */
    class VFXModuleLibrary final : public VFXModuleResolver
    {
    public:
        /// Reads every `.dfxmod` under @p dir (recursively). @p pathOf turns a file into the Path a stack row
        /// states for it. A file that does not parse, or a GUID two modules state, is an error naming both.
        Common::BoolResultStr AddDirectory( const std::filesystem::path&                                dir,
                                            const std::function<std::string( const std::filesystem::path& )>& pathOf );

        /// Adds @p module, or replaces the one of the same GUID; @p path is the Path a row states for it.
        void Set( VFXModuleDefinition module, std::string path );

        [[nodiscard]] Common::ResultStr<VFXModuleDefinition>
        Resolve( const Common::Content::AssetGuid& guid ) const override;
        [[nodiscard]] std::vector<Assets::AssetGuidRef> List() const override;

        /// The row reference to the module whose DisplayName is @p name, if exactly one module has it.
        [[nodiscard]] std::optional<Assets::AssetGuidRef> FindByName( std::string_view name ) const;

    private:
        struct Entry
        {
            VFXModuleDefinition Module;
            std::string         Path;
        };
        std::map<std::pair<uint64_t, uint64_t>, Entry> m_Modules;
    };
} // namespace Desert::VFX
