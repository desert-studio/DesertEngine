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
    // These constants and the table below used to be the static members of the lit base class, which made
    // "reads the shadow cascades / the IBL environment" something a material got by INHERITING from the
    // Lit class. A template is what reads them, so the template says so: every cell's reconciled layout
    // carries `SceneReads` (Core::Formats::SceneRead), classified from the resources its compiled stages
    // declare by the one table here (ShaderReflection::ReconcileCellLayout), and SceneFrameBinding::ApplyTo
    // writes exactly the groups the layout names. A data-driven template that samples the shadow map gets
    // the cascades because it declares them — not because someone wrote a C++ class for it.

    // How many shadow cascades the ShadowUB block carries, and therefore how many layers the cascade array a
    // lit draw binds may hold. ONE number for the whole chain: SceneFrameBinding::CascadeViewProj and the
    // renderer's cascade count are defined from it.
    inline constexpr uint32_t kMaxCascades = 4;

    // The C++ half of the `ShadowUB` block every lit surface declares. PUBLIC because the other half is
    // GLSL: reflecting the block and comparing it with this is the only way to assert the pair agrees on a
    // machine with no device (Desert/Tests/Engine/SceneFrameBinding).
    struct ShadowUBData
    {
        glm::mat4 LightViewProj[kMaxCascades];
        glm::vec4 Params; // x = tan(sun source angle / 2), y = enabled, z = debug mode, w = cascade count
        glm::vec4 DebugParams;       // x = show normals, y = lighting debug
        glm::vec4 CascadeTexelWorld; // per-cascade world size of one shadow-map texel
        glm::vec4 CascadeDepthRange; // per-cascade world length the [0,1] map depth spans (the PCSS penumbra)
        // The contact shadows' march (Common/ContactShadow.glslh): x = ray steps (0 = none), y = tan(fovY / 2),
        // z = dither frame 0..7, w unused. Packed by PackContactShadow.
        glm::vec4 ContactShadowView;
        // x = the sun's ContactShadowLength, y = 1 when it is in world centimetres, z = its Contact Shadow Casting
        // Intensity, w = its Non Casting Intensity (Common/ContactShadow.glslh's `light`).
        glm::vec4 SunContactShadow;
        // The sun's PCSS cost (Mesh/CascadedShadow.glslh): x = blocker search taps, y = filter taps, zw unused.
        // Packed by PackShadowFilter.
        glm::vec4 ShadowFilter;
    };

    // THE COST OF THE SUN'S SOFT SHADOW (Scalability Shadows.BlockerSearchTaps / Shadows.FilterTaps; UE: the
    // r.ShadowQuality kernel of sg.ShadowQuality): how many Vogel-disc taps the PCSS blocker search and filter
    // walk. One value per view, so every lit surface - deferred composite, forward meshes, glass, surface-cache
    // cards - filters the cascades with one kernel. The initialisers are the High column.
    struct ShadowFilterSettings
    {
        int BlockerSearchTaps = 16;
        int FilterTaps        = 32;
    };

    inline void PackShadowFilter( ShadowUBData& data, const ShadowFilterSettings& filter )
    {
        data.ShadowFilter = glm::vec4( static_cast<float>( filter.BlockerSearchTaps ),
                                       static_cast<float>( filter.FilterTaps ), 0.0f, 0.0f );
    }

    // THE CONTACT SHADOWS OF A VIEW (UE: the light's ContactShadowLength, marched by ShadowRayCast): the
    // screen-space ray every light with a length marches through the opaque scene depth. It rides the SAME
    // ShadowUB block as the cascades, so the deferred composite, the forward meshes and the glass read one
    // packing of one value. The point and spot lights carry their own lengths in their payloads.
    struct ContactShadowSettings
    {
        // Scalability Shadows.ContactShadowSteps. 0 = no march for any light — also what a view without an
        // opaque scene depth to march (the forward render path draws its opaque surfaces with no prepass) is
        // handed, since there is nothing for the ray to hit.
        int      Steps                  = 0;
        float    TanHalfFovY            = 1.0f;  // turns a screen-fraction length into centimetres at a depth
        uint32_t DitherFrame            = 0;     // 0..7, UE's StateFrameIndexMod8
        float    SunLength              = 0.0f;  // DirectionalLightData::ContactShadowLength (0 = none)
        bool     SunInWorldSpace        = false; // DirectionalLightData::ContactShadowLengthInWorldSpace
        float    SunCastingIntensity    = 1.0f;  // DirectionalLightData::ContactShadowCastingIntensity
        float    SunNonCastingIntensity = 0.0f;  // DirectionalLightData::ContactShadowNonCastingIntensity
    };

    inline void PackContactShadow( ShadowUBData& data, const ContactShadowSettings& contact )
    {
        data.ContactShadowView = glm::vec4( static_cast<float>( contact.Steps ), contact.TanHalfFovY,
                                            static_cast<float>( contact.DitherFrame ), 0.0f );
        data.SunContactShadow  = glm::vec4( contact.SunLength, contact.SunInWorldSpace ? 1.0f : 0.0f,
                                            contact.SunCastingIntensity, contact.SunNonCastingIntensity );
    }

    inline constexpr const char* kShadowBlockName              = "ShadowUB";
    inline constexpr const char* kShadowMapName                = "u_ShadowCascades"; // texture2DArray, layer c
    inline constexpr const char* kEnvIrradianceName            = "u_EnvIrradianceTex";
    inline constexpr const char* kEnvSpecularName              = "u_EnvSpecularTex";
    inline constexpr const char* kBrdfLutName                  = "u_BRDFLUTTexture";
    inline constexpr const char* kTimeBlockName                = "TimeUB";
    inline constexpr const char* kCloudShadowBlockName         = "CloudShadowUB";
    inline constexpr const char* kCloudShadowMapName           = "u_CloudShadowMap";
    inline constexpr const char* kObjectMotionsName            = "ObjectMotions";
    inline constexpr const char* kObjectBonesName              = "ObjectBones";

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
                 { kObjectMotionsName, SceneRead::ObjectMotion },
                 { kObjectBonesName, SceneRead::ObjectBones },
            };
            t.push_back( { kShadowMapName, SceneRead::Shadow } );
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
