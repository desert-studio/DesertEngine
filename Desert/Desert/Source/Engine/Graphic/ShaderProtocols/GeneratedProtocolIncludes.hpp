#pragma once

#include <Engine/Core/ShaderCompiler/ShaderGraphBindings.hpp>
#include <Engine/Graphic/ShaderProtocols/Camera.hpp>
#include <Engine/Graphic/ShaderProtocols/DirectionLight.hpp>
#include <Engine/Graphic/ShaderProtocols/Metadata.hpp>
#include <Engine/Graphic/ShaderProtocols/PointLight.hpp>
#include <Engine/Graphic/ShaderProtocols/SkinnedMaterialUB.hpp>
#include <Engine/Graphic/ShaderProtocols/SpotLight.hpp>
#include <Engine/Graphic/ShaderProtocols/VertexInputs.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <iterator>
#include <string>
#include <string_view>

namespace Desert::Graphic::ShaderProtocols
{
    /// The include path of the analytic sources' std430 structs (`struct PointLight`, `struct SpotLight`):
    /// declared with no binding, so the binding-free Mesh/LightSources.glslh includes them and the storage buffers
    /// (Mesh/PointLightsUB.glslh, Mesh/SpotLightsUB.glslh) hold arrays of them.
    inline constexpr std::string_view kLightSourceLayoutsInclude = "Mesh/LightSourceLayouts.glslh";

    [[nodiscard]] inline const std::string& LightSourceLayoutsGlsl()
    {
        static const std::string glsl =
             GenerateGuardedGlsl( "DESERT_LIGHT_SOURCE_LAYOUTS_GLSLH",
                                  "Engine/Graphic/ShaderProtocols/PointLight.hpp and SpotLight.hpp",
                                  GenerateGlslStruct( "PointLight", kPointLightPayloadFields ) + "\n" +
                                       GenerateGlslStruct( "SpotLight", kSpotLightPayloadFields ) );
        return glsl;
    }

    /// THE SET-0 SLOTS THE SCENE PROTOCOLS ARE GENERATED AT, held apart where the numbers are chosen. Every lit
    /// program (the forward cells, Terrain.shader, the deferred composite) includes these blocks beside its own
    /// hand-numbered resources, and two declarations on one slot is no compile error (ShaderGraphBindings.hpp):
    /// VulkanShader::Reflect drops the program at material apply. That is how DirectionLightsUB at 3 lost the
    /// landscape (TERRAIN-BIND14). The whole tree is MEASURED by Tests/Engine/ShaderCacheKey (every shipped pass
    /// compiled and reflected, no collision); this keeps the generator from handing out a known-taken slot first.
    /// SkinnedBones (1) is left out: it is declared only into the light-view caster cells, where 1 is its own.
    inline constexpr std::array<uint32_t, 5> kSceneProtocolBindings = {
         kCameraUBBinding, kLightsMetadataBinding, kPointLightsUBBinding, kDirectionLightsUBBinding,
         kSpotLightsUBBinding };
    /// Slots a lit program including the protocols already spends by hand: the material params row (1, every
    /// surface template and Terrain), and the landscape layer arrays of Programs/Terrain/TerrainSurface.glslh
    /// (LayerAlbedo/Normal/Arm/HeightTexture at 2/3/13/15).
    inline constexpr std::array<uint32_t, 5> kSceneProtocolTakenBindings = { 1u, 2u, 3u, 13u, 15u };

    [[nodiscard]] consteval bool SceneProtocolBindingsAreFree()
    {
        for ( size_t i = 0; i < kSceneProtocolBindings.size(); ++i )
        {
            const uint32_t binding = kSceneProtocolBindings[i];
            for ( size_t j = i + 1; j < kSceneProtocolBindings.size(); ++j )
                if ( kSceneProtocolBindings[j] == binding )
                    return false;
            for ( const uint32_t taken : kSceneProtocolTakenBindings )
                if ( taken == binding )
                    return false;
            // Everything from the scene-read range up (it, the virtual-shadow raster's and the graph's window).
            if ( binding >= Core::kSceneReadBindingFirst )
                return false;
        }
        return true;
    }
    static_assert( SceneProtocolBindingsAreFree(),
                   "a scene protocol block is generated on a slot another protocol, a lit program's own resource "
                   "(material row / terrain layer arrays) or a reserved range already holds" );

    /// The include path of the set-0/set-1 slot table (Core::kSceneBindingNames, ShaderGraphBindings.hpp): every
    /// scene-read, fog, virtual shadow, virtual geometry and graph-window slot as `#define <C++ name> <number>`, so a
    /// shader declares `layout( binding = kSceneFogVolumeBinding )` and never repeats the number.
    inline constexpr std::string_view kSceneBindingsInclude = "Generated/SceneBindings.glslh";

    [[nodiscard]] inline const std::string& SceneBindingsGlsl()
    {
        static const std::string glsl = []
        {
            std::string defines;
            for ( const Core::SceneBindingName& row : Core::kSceneBindingNames )
                std::format_to( std::back_inserter( defines ), "#define {} {}\n", row.Name, row.Value );
            return GenerateGuardedGlsl( "DESERT_SCENE_BINDINGS_GLSLH",
                                        "Engine/Core/ShaderCompiler/ShaderGraphBindings.hpp", defines );
        }();
        return glsl;
    }

    /// THE INCLUDES WHOSE TEXT IS GENERATED FROM A C++ PROTOCOL (Std140Block.hpp uniform blocks, VertexInputs.hpp
    /// vertex inputs) rather than read from disk.
    /// @p requested is an angle include's shader-root-relative path, normalised with '/' separators. Asked by the
    /// shader includer (to serve the text) and by the shader cache key (to hash it and walk it) — the two must
    /// agree, so both ask here. nullptr: not a generated include.
    [[nodiscard]] inline const std::string* FindGeneratedProtocolInclude( const std::string_view requested )
    {
        if ( requested == kCameraUBInclude )
            return &CameraUBGlsl();
        if ( requested == kDirectionLightsUBInclude )
            return &DirectionLightsUBGlsl();
        if ( requested == kLightsMetadataInclude )
            return &LightsMetadataGlsl();
        if ( requested == kLightSourceLayoutsInclude )
            return &LightSourceLayoutsGlsl();
        if ( requested == kPointLightsUBInclude )
            return &PointLightsUBGlsl();
        if ( requested == kSpotLightsUBInclude )
            return &SpotLightsUBGlsl();
        if ( requested == kSkinnedBonesInclude )
            return &SkinnedBonesGlsl();
        if ( requested == kMeshVertexInputsInclude )
            return &MeshVertexInputsGlsl();
        if ( requested == kMeshSkinnedVertexInputsInclude )
            return &MeshSkinnedVertexInputsGlsl();
        if ( requested == kMeshVertexAccessInclude )
            return &MeshVertexAccessIncludeGlsl();
        if ( requested == kUIVertexInputsInclude )
            return &UIVertexInputsGlsl();
        if ( requested == kSceneBindingsInclude )
            return &SceneBindingsGlsl();
        return nullptr;
    }
} // namespace Desert::Graphic::ShaderProtocols
