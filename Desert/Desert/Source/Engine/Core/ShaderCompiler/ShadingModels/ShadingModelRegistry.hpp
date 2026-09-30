#pragma once

// THE SET OF SHADING MODELS, read from Editor/Resources/Shaders/ShadingModels/*.shadingmodel.
//
// Contract:
//   * Identity is the Guid (ShadingModelManifest.hpp). Two files with one Guid are refused, naming both files.
//     Two files with one name cannot exist (the name is the file stem).
//   * The INDEX is what the G-buffer carries (the shading word's low four bits) and what the generated dispatch
//     switches on. It is DERIVED, never stored on disk: Unlit (kUnlitGuid) is 0, always; the others take 1..N in
//     ascending Guid order, so the same set of files gives the same indices on every machine and every run.
//     Adding a model may move others' indices — which is why IndexLayoutKey() is an input of the shader cache key.
//   * More than kMaxShadingModels files is refused, naming the directory and the count. No Unlit or no
//     DefaultLit file is refused, naming the missing Guid (the engine relies on both).
//   * Every Inputs field must be a SurfaceOutput field; the field list is read from the SurfaceOutput struct of
//     Mesh/Surface/SurfaceTypes.glslh (the struct is the one list), and an unknown field is refused naming the
//     model's file and the field.
//   * Any manifest refusal (ParseShadingModelManifest) refuses the whole scan: there is no partial registry,
//     and no fallback model for a file that failed.
//
// Generated output — GenerateGlsl(), written to kGeneratedInclude — is, in order: #include of kContractInclude;
// SHADING_MODEL_INDEX_<UPPER_SNAKE_NAME> per model; each model's body with Evaluate/EvaluateAmbient renamed to
// <Name>_Evaluate/<Name>_EvaluateAmbient; the two dispatch switches; the shading-word pack/unpack and payload
// quantization (ShadingModelContract.glslh lists the signatures). DeferredLighting.shader and the forward passes
// include it; no other file branches on a shading model.
//
// A material template's `ShadingModel <Name>` resolves through FindByName to the Guid; a template without the
// directive gets kDefaultLitGuid. A template that does not write one of the model's Inputs is refused at build,
// naming the field (MissingInput).

#include <Engine/Core/ShaderCompiler/ShadingModels/ShadingModelManifest.hpp>

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Core::ShadingModels
{
    struct ShadingModelEntry
    {
        ShadingModelManifest Manifest;
        std::uint8_t         Index = 0; // 0..kMaxShadingModels-1; 0 only for Unlit
    };

    // The fields of `struct SurfaceOutput` in @p surfaceTypesGlsl (the text of Mesh/Surface/SurfaceTypes.glslh),
    // in declaration order. Refused when the struct is not found.
    Common::ResultStr<std::vector<std::string>> ReadSurfaceOutputFields( std::string_view surfaceTypesGlsl );

    class ShadingModelRegistry
    {
    public:
        // Reads every *.shadingmodel in @p shaderRoot / kShadingModelDirectory and SurfaceOutput from
        // @p shaderRoot / "Mesh/Surface/SurfaceTypes.glslh", then Build.
        static Common::ResultStr<ShadingModelRegistry> Scan( const std::filesystem::path& shaderRoot );

        // The rules above over already-parsed manifests — the whole contract without the file system.
        static Common::ResultStr<ShadingModelRegistry> Build( std::vector<ShadingModelManifest> manifests,
                                                              std::span<const std::string> surfaceOutputFields );

        // Ordered by Index: Entries()[i].Index == i.
        std::span<const ShadingModelEntry> Entries() const;

        const ShadingModelEntry* FindByGuid( Common::UUID guid ) const;
        const ShadingModelEntry* FindByName( std::string_view name ) const;

        // "<guid>=<index>;..." in index order — the Guid->index layout, hashed into ShaderCacheKey so a cached
        // SPIR-V compiled against another layout is never reused.
        std::string IndexLayoutKey() const;

        // The text of kGeneratedInclude.
        std::string GenerateGlsl() const;

        // The first of @p model's Inputs (payload pins included) that @p writtenFields does not contain, or
        // nothing when the surface writes them all.
        static std::optional<std::string> MissingInput( const ShadingModelEntry&     model,
                                                        std::span<const std::string> writtenFields );

    private:
        std::vector<ShadingModelEntry> m_Entries;
    };
} // namespace Desert::Core::ShadingModels
