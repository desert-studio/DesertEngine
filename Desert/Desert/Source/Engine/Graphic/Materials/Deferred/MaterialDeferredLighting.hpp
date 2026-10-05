#pragma once

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/Properties/StorageBufferProperty.hpp>
#include <Engine/Graphic/Materials/Properties/TextureCubeProperty.hpp>
#include <Engine/Graphic/Materials/Properties/UniformBufferProperty.hpp>
#include <Engine/Graphic/Materials/SceneLightingBinding.hpp>
#include <Engine/Graphic/DefaultTextures.hpp>
#include <Engine/Graphic/FallbackTextures.hpp>

#include <Engine/Graphic/Clouds/CloudShadowBinding.hpp>
#include <Engine/Graphic/Clouds/CloudShadowPayload.hpp>

#include <Engine/Graphic/ShaderProtocols/PointLight.hpp>
#include <Engine/Graphic/ShaderProtocols/SpotLight.hpp>
#include <Engine/Graphic/ShaderProtocols/Metadata.hpp>

#include <glm/glm.hpp>

#include <unordered_set>

namespace Desert::Graphic
{
    // CSM data the deferred lighting pass needs to shadow the sun (mirrors Graphic::SceneShadowBind).
    struct DeferredShadowInput
    {
        const glm::mat4* CascadeVP            = nullptr; // [Count] light view-proj matrices
        uint32_t         Count                = 0;
        float            Bias                 = 0.005f;
        bool             Enabled              = true;
        glm::vec4        CascadeWorldPerTexel = glm::vec4( 1.0f );
    };

    // CloudShadowInput — the cloud layer's shadow, a SECOND independent occluder of the same sun — used
    // to be declared right here, which is precisely why nothing but this pass ever received it. It now
    // lives beside the uniform block it packs into (Engine/Graphic/Clouds/CloudShadowPayload.hpp), where
    // the forward mesh materials and the terrain can reach it too. It is a separate struct from
    // DeferredShadowInput on purpose: the cascades are a depth comparison against opaque geometry, this
    // is a volumetric transmittance reconstructed from one texel of one map, and they share nothing but
    // the light they attenuate.

    // THE BAKED SKY, as the deferred composite's ambient source — the same three images
    // Graphic::PBRSceneFrame hands the forward PBR materials (Graphic::SceneEnvironmentBind), and
    // deliberately the same struct shape as the two above: data gathered by SceneRenderer, consumed here.
    //
    // All three or none. The split-sum ambient is not separable — the diffuse cube without the
    // prefiltered one is an ambient with no reflections, and the prefiltered one without the LUT is a
    // reflection with no Fresnel weight. A partial set is a bake that went wrong, and the consumer says
    // so out loud rather than shading half a model.
    struct DeferredEnvironmentInput
    {
        ImageCube* Irradiance  = nullptr; // cosine-convolved sky -> the diffuse half
        ImageCube* Prefiltered = nullptr; // GGX-prefiltered radiance, roughness across mips
        Image2D*   BrdfLut     = nullptr; // split-sum BRDF integration (cosLo, roughness)
        SkyLook    Look{};                // how the two cubes are read (Environment::Look)

        bool IsComplete() const
        {
            return Irradiance != nullptr && Prefiltered != nullptr && BrdfLut != nullptr;
        }
    };

    // Fullscreen deferred-lighting material: binds the scene renderer's G-buffer color targets (albedo/metallic,
    // normal/roughness, world-position) + the sun (+ its CSM shadow maps) + ALL point & spot lights (uploaded
    // into the shared SSBO layout the mesh PBR shader also uses) + a debug-mode selector, driving
    // DeferredLighting.shader. Header-only (no new .cpp -> no premake regen).
    class MaterialDeferredLighting final : public Material
    {
    public:
        MaterialDeferredLighting() : Material( "MaterialDeferredLighting", "DeferredLighting" )
        {
        }

        // The frame's values only: every graph texture the pass samples (G-buffer, AO, GI, shadow cascades)
        // is bound by name through RDG::PassBindings (DeferredLightingRenderer::Record). lightDir.xyz = direction
        // the sun travels; lightColor.rgb/.a = colour/intensity; cameraPos.xyz = camera world pos (view vector);
        // debugMode 0=Lit,1=Albedo,2=Normal,3=Metallic,4=Roughness; point/spot = the scene's dynamic lights.
        void BindInputs( const glm::vec4& lightDir, const glm::vec4& lightColor, const glm::vec4& cameraPos,
                         int debugMode, const ShaderProtocols::PointLight& pointLights,
                         const ShaderProtocols::SpotLight& spotLights, const DeferredShadowInput& shadow,
                         float giIntensity, bool ssaoEnabled, int giMode, const CloudShadowInput& cloudShadow,
                         const DeferredEnvironmentInput& environment )
        {
            // The baked sky's cubes and the BRDF LUT are pass parameters (SceneViewInputs, bound by the Composite
            // node: System.BlackCube / System.Black when absent, UE GBlackTextureCube) - a graph ref of THIS
            // frame, so no previous scene's sky can survive in a slot. Completeness stays a REPORT
            // (DeferredLightingRenderer::ReportEnvironmentGap). The look they are read with is written every
            // frame.
            SceneSkyLookBind( this, environment.Look );

            SetLightDir( lightDir );
            SetLightColor( lightColor );
            SetCameraPos( cameraPos );
            // u_Params: x = debug mode, y = GI intensity (0 = off), z = SSAO enabled (else shader uses AO=1),
            // w = GI mode (0 = off, 1 = screen-space gather, 2 = RSM buffer). Mode picks WHERE the indirect
            // light comes from; intensity scales it (the RSM path pre-applies it in GIResolve).
            SetParams( glm::vec4( static_cast<float>( debugMode ), giIntensity, ssaoEnabled ? 1.0f : 0.0f,
                                  static_cast<float>( giMode ) ) );

            UploadShadow( shadow );
            UploadCloudShadow( cloudShadow );

            // Upload the dynamic lights into the same SSBO/UB layout the mesh PBR shader uses (bindings 6/16/4),
            // EVERY frame: the shader loops 0..count, so with no lights one zeroed entry is written and never
            // read -- the buffer is still this frame's, and the draw's every-slot-filled check holds.
            {
                const ShaderProtocols::PointLightPayload noPoint{};
                const bool                               anyPoint = !pointLights.PointLights.empty();
                if ( auto* sb = Get<StorageBufferProperty>( ShaderProtocols::PointLight::Name ) )
                    sb->SetRawData( anyPoint ? (const std::byte*)pointLights.PointLights.data()
                                             : (const std::byte*)&noPoint,
                                    static_cast<uint32_t>( ( anyPoint ? pointLights.PointLights.size() : 1u ) *
                                                           sizeof( ShaderProtocols::PointLightPayload ) ) );
            }
            {
                const ShaderProtocols::SpotLightPayload noSpot{};
                const bool                              anySpot = !spotLights.SpotLights.empty();
                if ( auto* sb = Get<StorageBufferProperty>( ShaderProtocols::SpotLight::Name ) )
                    sb->SetRawData( anySpot ? (const std::byte*)spotLights.SpotLights.data()
                                            : (const std::byte*)&noSpot,
                                    static_cast<uint32_t>( ( anySpot ? spotLights.SpotLights.size() : 1u ) *
                                                           sizeof( ShaderProtocols::SpotLightPayload ) ) );
            }

            const uint32_t counts[3] = { 0u, static_cast<uint32_t>( pointLights.PointLights.size() ),
                                         static_cast<uint32_t>( spotLights.SpotLights.size() ) };
            if ( auto* meta = Get<UniformBufferProperty>( ShaderProtocols::LightsMetadata::Name ) )
                meta->SetRawData( (std::byte*)counts, sizeof( counts ) );

            UploadRegisteredProperties();
            // DeferredUB (the MPROPERTYs above) is the only field-filled buffer here. ShadowUB,
            // CloudShadowUB and LightsMetadata were just written whole, a few lines up, and this loop
            // used to reach them too: every field of every buffer starts dirty, so on the opening
            // frames it flushed uninitialised shadow copies straight over the cascade matrices it had
            // just been given. They decline now — see ShaderResources::BufferFillKind.hpp.
            FlushFieldFilledUniformBuffers();
        }

        // Uploads the CSM data into ShadowUB (the cascade maps u_ShadowMap0..3 are graph textures, bound by
        // DeferredLightingRenderer::Record) — mirrors Graphic::SceneShadowBind so the SAME sun shadows appear.
        void UploadShadow( const DeferredShadowInput& shadow )
        {
            struct ShadowUBData
            {
                glm::mat4 LightViewProj[4];
                glm::vec4 Params;            // x = bias, y = enabled, z = debug mode, w = cascade count
                glm::vec4 DebugParams;
                glm::vec4 CascadeTexelWorld;
            } data;

            const uint32_t n = shadow.Count < 4u ? shadow.Count : 4u;
            for ( uint32_t i = 0; i < 4u; ++i )
                data.LightViewProj[i] = ( i < n && shadow.CascadeVP ) ? shadow.CascadeVP[i] : glm::mat4( 1.0f );
            data.Params = glm::vec4( shadow.Bias, shadow.Enabled ? 1.0f : 0.0f, 0.0f, static_cast<float>( n ) );
            data.DebugParams       = glm::vec4( 0.0f );
            data.CascadeTexelWorld = shadow.CascadeWorldPerTexel;

            if ( auto* ub = Get<UniformBufferProperty>( "ShadowUB" ) )
                ub->SetRawData( reinterpret_cast<const std::byte*>( &data ), sizeof( data ) );
        }

        // Uploads the cloud layer's shadow into CloudShadowUB through the SAME packer the forward PBR
        // materials and the terrain material use (Graphic::CloudShadowUpload), so the two render paths cannot
        // be told different things about one map. The map itself (u_CloudShadowMap) is a graph resource the
        // composite exec binds through RDG::PassBindings (FrameTransients::CloudShadowMap, System.White when
        // the frame has none), never here.
        void UploadCloudShadow( const CloudShadowInput& cloudShadow )
        {
            CloudShadowUpload( this, cloudShadow );
        }

        MPROPERTY( glm::vec4, LightDir,   "u_LightDir",   ( glm::vec4( 0.0f, -1.0f, 0.0f, 0.0f ) ) )
        MPROPERTY( glm::vec4, LightColor, "u_LightColor", ( glm::vec4( 1.0f, 1.0f, 1.0f, 3.0f ) ) )
        MPROPERTY( glm::vec4, Params,     "u_Params",     ( glm::vec4( 0.0f ) ) )
        MPROPERTY( glm::vec4, CameraPos,  "u_CameraPos",  ( glm::vec4( 0.0f ) ) )

    private:
    };
} // namespace Desert::Graphic
