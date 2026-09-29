#pragma once

#include <Engine/Core/Formats/MaterialLayout.hpp>
#include <Engine/Graphic/Environment/SkyLook.hpp>
#include <Engine/Graphic/ShaderProtocols/Camera.hpp>
#include <Engine/Graphic/ShaderProtocols/DirectionLight.hpp>
#include <Engine/Graphic/ShaderProtocols/Metadata.hpp>
#include <Engine/Graphic/ShaderProtocols/PointLight.hpp>
#include <Engine/Graphic/ShaderProtocols/SpotLight.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Graphic::SceneResources
{
    // WHAT THE SCENE HANDS A SURFACE, BY NAME — for ANY surface template, not for one C++ material class.
    //
    // These constants and the table below used to be the static members of MaterialPBRBase, which made
    // "reads the shadow cascades / the IBL environment" something a material got by INHERITING from the
    // PBR class. A template is what reads them, so the template says so: every cell's reconciled layout
    // carries `SceneReads` (Core::Formats::SceneRead), classified from the resources its compiled stages
    // declare by the one table here (ShaderReflection::ReconcileCellLayout), and PBRSceneFrame::ApplyTo
    // writes exactly the groups the layout names. A data-driven template that samples the shadow map gets
    // the cascades because it declares them — not because someone wrote a C++ class for it.

    // How many shadow cascades the ShadowUB block carries, and therefore how many cascade maps a lit draw
    // binds. ONE number for the whole chain: PBRSceneFrame::CascadeMaps and the renderer's cascade count
    // are defined from it.
    inline constexpr uint32_t kMaxCascades = 4;

    // The C++ half of the `ShadowUB` block every lit surface declares. PUBLIC because the other half is
    // GLSL: reflecting the block and comparing it with this is the only way to assert the pair agrees on a
    // machine with no device (Desert/Tests/Engine/PBRSceneFrame).
    struct ShadowUBData
    {
        glm::mat4 LightViewProj[kMaxCascades];
        glm::vec4 Params;            // x = bias, y = enabled, z = debug mode, w = cascade count
        glm::vec4 DebugParams;       // x = show normals, y = lighting debug
        glm::vec4 CascadeTexelWorld; // per-cascade world size of one shadow-map texel
    };

    inline constexpr const char* kShadowBlockName              = "ShadowUB";
    inline constexpr const char* kShadowMapNames[kMaxCascades] = { "u_ShadowMap0", "u_ShadowMap1", "u_ShadowMap2",
                                                                   "u_ShadowMap3" };
    inline constexpr const char* kEnvIrradianceName            = "u_EnvIrradianceTex";
    inline constexpr const char* kEnvSpecularName              = "u_EnvSpecularTex";
    inline constexpr const char* kBrdfLutName                  = "u_BRDFLUTTexture";
    inline constexpr const char* kTimeBlockName                = "TimeUB";
    inline constexpr const char* kCloudShadowBlockName         = "CloudShadowUB";
    inline constexpr const char* kCloudShadowMapName           = "u_CloudShadowMap";

    struct Entry
    {
        std::string              Name;
        Core::Formats::SceneRead Group;
    };

    // THE TABLE: every resource name the scene fills, and the group whose writer fills it. The writers in
    // SceneLightingBinding.hpp / CloudShadowBinding.hpp look up the same constants, so a name cannot be
    // written under one spelling and classified under another.
    inline const std::vector<Entry>& Table()
    {
        using Core::Formats::SceneRead;
        static const std::vector<Entry> table = []
        {
            std::vector<Entry> t{
                 { ShaderProtocols::Camera::Name, SceneRead::Camera },
                 { kTimeBlockName, SceneRead::Time },
                 { ShaderProtocols::PointLight::Name, SceneRead::Lights },
                 { ShaderProtocols::SpotLight::Name, SceneRead::Lights },
                 { ShaderProtocols::DirectionLight::Name, SceneRead::Lights },
                 { ShaderProtocols::LightsMetadata::Name, SceneRead::Lights },
                 { kShadowBlockName, SceneRead::Shadow },
                 { kEnvIrradianceName, SceneRead::Environment },
                 { kEnvSpecularName, SceneRead::Environment },
                 { kBrdfLutName, SceneRead::Environment },
                 { kSkyLookBlockName, SceneRead::Environment },
                 { kCloudShadowBlockName, SceneRead::CloudShadow },
                 { kCloudShadowMapName, SceneRead::CloudShadow },
            };
            for ( const char* map : kShadowMapNames )
                t.push_back( { map, SceneRead::Shadow } );
            return t;
        }();
        return table;
    }

    // The group @p name belongs to; None when the scene does not fill it (a material's own resource).
    inline Core::Formats::SceneRead GroupOf( std::string_view name )
    {
        for ( const auto& e : Table() )
            if ( e.Name == name )
                return e.Group;
        return Core::Formats::SceneRead::None;
    }

    // Every scene group a template reads, from the resource names its compiled stages declare.
    inline Core::Formats::SceneRead Classify( std::span<const std::string> declaredNames )
    {
        Core::Formats::SceneRead reads = Core::Formats::SceneRead::None;
        for ( const auto& name : declaredNames )
            reads = reads | GroupOf( name );
        return reads;
    }
} // namespace Desert::Graphic::SceneResources
