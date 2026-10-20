#pragma once

#include <Engine/Graphic/ShaderProtocols/Camera.hpp>
#include <Engine/Graphic/ShaderProtocols/DirectionLight.hpp>
#include <Engine/Graphic/ShaderProtocols/Metadata.hpp>
#include <Engine/Graphic/ShaderProtocols/PointLight.hpp>
#include <Engine/Graphic/ShaderProtocols/SkinnedMaterialUB.hpp>
#include <Engine/Graphic/ShaderProtocols/SpotLight.hpp>

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

    /// THE INCLUDES WHOSE TEXT IS GENERATED FROM A C++ PROTOCOL (Std140Block.hpp) rather than read from disk.
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
        return nullptr;
    }
} // namespace Desert::Graphic::ShaderProtocols
