#pragma once

#include <Engine/Core/Formats/MaterialLayout.hpp>
#include <Engine/Graphic/Environment/SkyLook.hpp>
#include <Engine/Graphic/ShaderProtocols/Camera.hpp>
#include <Engine/Graphic/ShaderProtocols/DirectionLight.hpp>
#include <Engine/Graphic/ShaderProtocols/Metadata.hpp>
#include <Engine/Graphic/ShaderProtocols/PointLight.hpp>
#include <Engine/Graphic/ShaderProtocols/SpotLight.hpp>

#include <glm/glm.hpp>

#include <array>
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

    // How many shadow cascades the ShadowUB block carries PER DIRECTIONAL LIGHT, and therefore how many layers of
    // the cascade array one light may hold. ONE number for the whole chain: SceneFrameBinding's views and the
    // renderer's cascade count are defined from it.
    inline constexpr uint32_t kMaxCascades = 4;

    // HOW MANY DIRECTIONAL LIGHTS HAVE CASCADES OF THEIR OWN (UE: every directional light with a whole-scene
    // shadow gets its own CSM; the cap is the shadow budget, not "the one lamp"). The slot IS the light's
    // Atmosphere Sun Light Index (ECS::Rules::kAtmosphereLightCount, asserted in Core/Scene.cpp): 0 the sun,
    // 1 the moon - the only directional lights the frame shades. Each slot is fitted, cached, drawn and sampled
    // by the same code; nothing below names "the moon".
    inline constexpr uint32_t kMaxShadowedDirectionalLights = 2;
    inline constexpr uint32_t kDirectionalShadowSun         = 0; // the slot the sun's occluders are sampled from
    // Every cascade of every slot: ShadowUBData::LightViewProj is indexed [slot * kMaxCascades + cascade].
    inline constexpr uint32_t kMaxCascadeViews = kMaxShadowedDirectionalLights * kMaxCascades;

    // ONE DIRECTIONAL LIGHT'S CASCADES as the frame publishes them (MeshRenderer::DirectionalShadows) and every
    // receiver's block packs them (PackDirectionalShadow): the deferred composite, the forward meshes, the glass,
    // the terrain and the surface-cache cards read one value through one packer.
    struct DirectionalShadowView
    {
        const glm::mat4* CascadeVP     = nullptr; // [Count] light view-proj matrices (receiver side, no bias)
        uint32_t         Count         = 0;       // fitted this frame; 0 = this light has no cascades
        uint32_t         FirstLayer    = 0;       // layer of cascade 0 in the shared cascade array
        bool             CastShadows   = false;   // the light's CastShadows (a light outside its cascades is lit)
        float            SourceTanHalf = 0.0f;    // tan(SourceAngle / 2): the PCSS penumbra slope
        glm::vec4        CascadeWorldPerTexel = glm::vec4( 1.0f );
        glm::vec4        CascadeDepthRange    = glm::vec4( 1.0f );
    };

    // The C++ half of the `ShadowUB` block every lit surface declares. PUBLIC because the other half is
    // GLSL: reflecting the block and comparing it with this is the only way to assert the pair agrees on a
    // machine with no device (Desert/Tests/Engine/SceneFrameBinding).
    struct ShadowUBData
    {
        glm::mat4 LightViewProj[kMaxCascadeViews]; // [slot * kMaxCascades + cascade]
        // Per slot: x = tan(source angle / 2), y = casts shadows (1/0), z = cascades fitted, w = first array layer.
        glm::vec4 DirectionalShadow[kMaxShadowedDirectionalLights];
        glm::vec4 CascadeTexelWorld[kMaxShadowedDirectionalLights]; // per cascade: world size of one map texel
        glm::vec4 CascadeDepthRange[kMaxShadowedDirectionalLights]; // per cascade: world length of [0,1] map depth
        glm::vec4 DebugParams; // x = show normals, y = lighting debug, z = cascade debug mode (0/1/2), w = 0
        // The contact shadows' march (Common/ContactShadow.glslh): x = ray steps (0 = none), y = tan(fovY / 2),
        // z = dither frame 0..7, w unused. Packed by PackContactShadow.
        glm::vec4 ContactShadowView;
        // x = the sun's ContactShadowLength, y = 1 when it is in world centimetres, z = its Contact Shadow Casting
        // Intensity, w = its Non Casting Intensity (Common/ContactShadow.glslh's `light`).
        glm::vec4 SunContactShadow;
        // The PCSS cost (Mesh/CascadedShadow.glslh): x = blocker search taps, y = filter taps, zw unused.
        // Packed by PackShadowFilter.
        glm::vec4 ShadowFilter;
    };

    // THE ONE PACKER of a directional light's cascades into its slot of the block. A light with no view (no
    // such light this frame, or its shadows off) packs CastShadows 0 and Count 0: lit, never sampled.
    inline void PackDirectionalShadow( ShadowUBData& data, const uint32_t slot, const DirectionalShadowView& view )
    {
        const uint32_t n = view.CascadeVP != nullptr ? ( view.Count < kMaxCascades ? view.Count : kMaxCascades ) : 0u;
        for ( uint32_t c = 0; c < kMaxCascades; ++c )
            data.LightViewProj[slot * kMaxCascades + c] = c < n ? view.CascadeVP[c] : glm::mat4( 1.0f );
        data.DirectionalShadow[slot] = glm::vec4( view.SourceTanHalf, view.CastShadows ? 1.0f : 0.0f,
                                                  static_cast<float>( n ), static_cast<float>( view.FirstLayer ) );
        data.CascadeTexelWorld[slot] = view.CascadeWorldPerTexel;
        data.CascadeDepthRange[slot] = view.CascadeDepthRange;
    }

    using DirectionalShadowViews = std::array<DirectionalShadowView, kMaxShadowedDirectionalLights>;

    inline void PackDirectionalShadows( ShadowUBData& data, const DirectionalShadowViews& views )
    {
        for ( uint32_t slot = 0; slot < kMaxShadowedDirectionalLights; ++slot )
            PackDirectionalShadow( data, slot, views[slot] );
    }

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

    // The view's half of the march as the shaders read it (ContactShadowView, Common/ContactShadow.glslh): x =
    // steps, y = tan(fov / 2), z = dither frame. ShadowUB carries it; MegaLights' view block too.
    inline glm::vec4 ContactShadowViewVector( const ContactShadowSettings& contact )
    {
        return glm::vec4( static_cast<float>( contact.Steps ), contact.TanHalfFovY,
                          static_cast<float>( contact.DitherFrame ), 0.0f );
    }

    inline void PackContactShadow( ShadowUBData& data, const ContactShadowSettings& contact )
    {
        data.ContactShadowView = ContactShadowViewVector( contact );
        data.SunContactShadow  = glm::vec4( contact.SunLength, contact.SunInWorldSpace ? 1.0f : 0.0f,
                                            contact.SunCastingIntensity, contact.SunNonCastingIntensity );
    }

    inline constexpr const char* kShadowBlockName              = "ShadowUB";
    inline constexpr const char* kShadowMapName                = "ShadowCascadesTexture"; // texture2DArray, layer c
    inline constexpr const char* kEnvIrradianceName            = "EnvironmentIrradianceTexture";
    inline constexpr const char* kEnvSpecularName              = "EnvironmentSpecularTexture";
    inline constexpr const char* kBrdfLutName                  = "BrdfLutTexture";
    inline constexpr const char* kTimeBlockName                = "TimeUB";
    inline constexpr const char* kCloudShadowBlockName         = "CloudShadowUB";
    inline constexpr const char* kCloudShadowMapName           = "CloudShadowMapTexture";
    inline constexpr const char* kGpuScenePrimitivesName       = "GpuScenePrimitives";
    inline constexpr const char* kObjectBonesName              = "ObjectBones";
    inline constexpr const char* kWorldParametersName          = "WorldParameters";
    inline constexpr const char* kRainOcclusionBlockName       = "RainOcclusionUB";
    inline constexpr const char* kRainOcclusionMapName         = "RainOcclusionMapTexture";
    // The blocks a material property is looked up by (Material::Get), in their parameter form.
    inline constexpr RDG::UniformBufferParameter kShadowBlock{ kShadowBlockName };
    inline constexpr RDG::UniformBufferParameter kTimeBlock{ kTimeBlockName };
    inline constexpr RDG::UniformBufferParameter kCloudShadowBlock{ kCloudShadowBlockName };
    inline constexpr RDG::StorageBufferParameter kGpuScenePrimitives{ kGpuScenePrimitivesName };
    // The binned draw's tables (Common/GpuScene.glslh DESERT_GPU_SCENE_DRAWS, bindings 41-44): bound by the mesh
    // pass that issues the GPU-culled bins (MeshRendererDetail::MeshDrawList::SetBinned), never by a material.
    inline constexpr RDG::StorageBufferParameter kGpuSceneInstances{ "GpuSceneInstances" };
    inline constexpr RDG::StorageBufferParameter kGpuSceneVisibleInstances{ "GpuSceneVisibleInstances" };
    inline constexpr RDG::StorageBufferParameter kGpuSceneDrawSlots{ "GpuSceneDrawSlots" };
    inline constexpr RDG::StorageBufferParameter kGpuSceneSubmeshTransforms{ "GpuSceneSubmeshTransforms" };
    // The VG raster's tables (Common/GpuScene.glslh DESERT_GPU_SCENE_VIRTUAL_GEOMETRY, bindings 50-53): bound by the
    // mesh pass that draws the clusters (MeshRendererDetail::VirtualGeometryIssue), never by a material.
    inline constexpr RDG::StorageBufferParameter kVgRasterClusters{ "VgRasterClusters" };
    inline constexpr RDG::StorageBufferParameter kVgRasterDags{ "VgRasterDags" };
    inline constexpr RDG::StorageBufferParameter kVgRasterVisibleClusters{ "VgRasterVisibleClusters" };
    inline constexpr RDG::StorageBufferParameter kVgRasterPrimitiveDraws{ "VgRasterPrimitiveDraws" };
    inline constexpr RDG::StorageBufferParameter kObjectBones{ kObjectBonesName };
    inline constexpr RDG::StorageBufferParameter kWorldParameters{ kWorldParametersName };
    inline constexpr RDG::UniformBufferParameter kRainOcclusionBlock{ kRainOcclusionBlockName };

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
                 { kGpuScenePrimitivesName, SceneRead::GpuScenePrimitives },
                 { kObjectBonesName, SceneRead::ObjectBones },
                 { kWorldParametersName, SceneRead::WorldParameters },
                 { kRainOcclusionBlockName, SceneRead::RainOcclusion },
                 { kRainOcclusionMapName, SceneRead::RainOcclusion },
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
