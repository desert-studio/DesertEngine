// The scene protocols every lit program reads (camera, lights, the skinning palette, the vertex inputs, the slot
// table): the twelve include paths the shaders already name, generated from their C++ declarations.
#include "IncludeLists.hpp"

#include <Engine/Core/ShaderCompiler/ShaderGraphBindings.hpp>
#include <Engine/Graphic/ShaderProtocols/CameraLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/LightLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/SkinnedBonesLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/VertexInputs.hpp>

#include <array>
#include <format>
#include <iterator>
#include <string>
#include <vector>

namespace Desert::ShaderProtocolGen
{
    namespace
    {
        namespace SP = Graphic::ShaderProtocols;

        /// THE SET-0 SLOTS THE SCENE PROTOCOLS ARE GENERATED AT, held apart where the numbers are chosen. Every
        /// lit program (the forward cells, Terrain.shader, the deferred composite) includes these blocks beside
        /// its own hand-numbered resources, and two declarations on one slot is no compile error:
        /// VulkanShader::Reflect drops the program at material apply. That is how DirectionLightsUB at 3 lost the
        /// landscape (TERRAIN-BIND14). SkinnedBones (1) is left out: it is declared only into the light-view
        /// caster cells, where 1 is its own.
        constexpr std::array kSceneProtocolBlocks = { &SP::kCameraUBBlock, &SP::kLightsMetadataBlock,
                                                      &SP::kPointLightsUBBlock, &SP::kDirectionLightsUBBlock,
                                                      &SP::kSpotLightsUBBlock };
        /// Slots a lit program including the protocols already spends by hand: the material params row (1, every
        /// surface template and Terrain), and the landscape layer arrays of Programs/Terrain/TerrainSurface.glslh
        /// (LayerAlbedo/Normal/Arm/HeightTexture at 2/3/13/15).
        constexpr std::array<uint32_t, 5> kSceneProtocolTakenBindings = { 1u, 2u, 3u, 13u, 15u };

        consteval bool SceneProtocolBindingsAreFree()
        {
            for ( size_t i = 0; i < kSceneProtocolBlocks.size(); ++i )
            {
                const uint32_t binding = kSceneProtocolBlocks[i]->Binding.Value;
                // An Overridable block is checked at its default: lit programs take the default, a compute
                // pass that #defines the macro (light grid, GI traces, path tracer) places it in its own table.
                if ( kSceneProtocolBlocks[i]->Binding.Kind == SP::ShaderBindingKind::Overridable &&
                     kSceneProtocolBlocks[i]->Binding.Macro.empty() )
                    return false;
                for ( size_t j = i + 1; j < kSceneProtocolBlocks.size(); ++j )
                    if ( kSceneProtocolBlocks[j]->Binding.Value == binding )
                        return false;
                for ( const uint32_t taken : kSceneProtocolTakenBindings )
                    if ( taken == binding )
                        return false;
                // Everything from the scene-read range up (it, the virtual-shadow raster's and the graph's
                // window).
                if ( binding >= Core::kSceneReadBindingFirst )
                    return false;
            }
            return true;
        }
        static_assert( SceneProtocolBindingsAreFree(),
                       "a scene protocol block is generated on a slot another protocol, a lit program's own "
                       "resource (material row / terrain layer arrays) or a reserved range already holds" );

        /// The set-0/set-1 slot table (Core::kSceneBindingNames): every scene-read, fog, virtual shadow, virtual
        /// geometry and graph-window slot as `#define <C++ name> <number>`, so a shader declares
        /// `layout( binding = kSceneFogVolumeBinding )` and never repeats the number.
        std::string SceneBindingsText()
        {
            std::string defines;
            for ( const Core::SceneBindingName& row : Core::kSceneBindingNames )
                std::format_to( std::back_inserter( defines ), "#define {} {}\n", row.Name, row.Value );
            return defines;
        }

        constexpr SP::ShaderIncludeDecl kSceneBindingsShaderInclude{
             .Path         = "Generated/SceneBindings.glslh",
             .SourceHeader = "Desert/Desert/Source/Engine/Core/ShaderCompiler/ShaderGraphBindings.hpp",
             .Text         = &SceneBindingsText };

        constexpr std::array<const ShaderIncludeDecl*, 12> kSceneIncludes = {
             &SP::kCameraUBShaderInclude,
             &SP::kDirectionLightsUBShaderInclude,
             &SP::kLightsMetadataShaderInclude,
             &SP::kLightSourceLayoutsShaderInclude,
             &SP::kPointLightsUBShaderInclude,
             &SP::kSpotLightsUBShaderInclude,
             &SP::kSkinnedBonesShaderInclude,
             &SP::kMeshVertexInputsShaderInclude,
             &SP::kMeshSkinnedVertexInputsShaderInclude,
             &SP::kMeshVertexAccessShaderInclude,
             &SP::kUIVertexInputsShaderInclude,
             &kSceneBindingsShaderInclude };

        // Every attribute the generated vertex formats own: the skinned mesh format holds the plain one's.
        constexpr auto kVertexInputNames = []
        {
            std::array<std::string_view, SP::kMeshSkinnedVertexInputs.size() + SP::kUIVertexInputs.size()> names{};
            size_t count = 0;
            for ( const SP::VertexInputField& field : SP::kMeshSkinnedVertexInputs )
                names[count++] = field.Name;
            for ( const SP::VertexInputField& field : SP::kUIVertexInputs )
                names[count++] = field.Name;
            return names;
        }();
    } // namespace

    std::span<const ShaderIncludeDecl* const> SceneIncludes()
    {
        return kSceneIncludes;
    }

    std::span<const std::string_view> GeneratedVertexInputNames()
    {
        return kVertexInputNames;
    }
} // namespace Desert::ShaderProtocolGen
