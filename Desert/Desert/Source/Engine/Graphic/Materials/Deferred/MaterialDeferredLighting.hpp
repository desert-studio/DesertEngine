#pragma once

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/Properties/StorageBufferProperty.hpp>
#include <Engine/Graphic/Materials/Properties/TextureCubeProperty.hpp>
#include <Engine/Graphic/Materials/Properties/UniformBufferProperty.hpp>
#include <Engine/Graphic/Materials/SceneLightingBinding.hpp>

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
        Image2D*         CascadeMaps[4]       = { nullptr, nullptr, nullptr, nullptr };
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
            m_GBufferA        = m_MaterialExecutor->GetTexture2DProperty( "u_GBufferA" ).get();
            m_GBufferB        = m_MaterialExecutor->GetTexture2DProperty( "u_GBufferB" ).get();
            m_GBufferC        = m_MaterialExecutor->GetTexture2DProperty( "u_GBufferC" ).get();
            m_GBufferEmissive = m_MaterialExecutor->GetTexture2DProperty( "u_GBufferEmissive" ).get();
            m_SSAO            = m_MaterialExecutor->GetTexture2DProperty( "u_SSAO" ).get();
            m_GI              = m_MaterialExecutor->GetTexture2DProperty( "u_GI" ).get();
            m_EnvIrradiance   = m_MaterialExecutor->GetTextureCubeProperty( "u_EnvIrradianceTex" ).get();
            m_EnvSpecular     = m_MaterialExecutor->GetTextureCubeProperty( "u_EnvSpecularTex" ).get();
            m_BrdfLut         = m_MaterialExecutor->GetTexture2DProperty( "u_BRDFLUTTexture" ).get();
        }

        // gA = Albedo+Metallic, gB = Normal+Roughness, gC = WorldPosition; lightDir.xyz = direction the sun
        // travels; lightColor.rgb/.a = colour/intensity; cameraPos.xyz = camera world pos (view vector);
        // debugMode 0=Lit,1=Albedo,2=Normal,3=Metallic,4=Roughness; point/spot = the scene's dynamic lights.
        void BindInputs( const std::shared_ptr<Image2D>& gA, const std::shared_ptr<Image2D>& gB,
                         const std::shared_ptr<Image2D>& gC, const std::shared_ptr<Image2D>& gE,
                         const glm::vec4& lightDir, const glm::vec4& lightColor, const glm::vec4& cameraPos,
                         int debugMode, const ShaderProtocols::PointLight& pointLights,
                         const ShaderProtocols::SpotLight& spotLights, const DeferredShadowInput& shadow,
                         const std::shared_ptr<Image2D>& aoImage, float giIntensity, bool ssaoEnabled, int giMode,
                         const std::shared_ptr<Image2D>& giImage, const CloudShadowInput& cloudShadow,
                         const DeferredEnvironmentInput& environment )
        {
            if ( m_GBufferA && gA )
                m_GBufferA->SetImage( gA.get() );
            if ( m_GBufferB && gB )
                m_GBufferB->SetImage( gB.get() );
            if ( m_GBufferC && gC )
                m_GBufferC->SetImage( gC.get() );
            if ( m_GBufferEmissive && gE )
                m_GBufferEmissive->SetImage( gE.get() );
            if ( m_SSAO && aoImage )
                m_SSAO->SetImage( aoImage.get() );
            // RSM mode only: the resolved indirect-light buffer. In the screen-space / off modes nothing is
            // bound here and the shader never samples it (the descriptor keeps its dummy image).
            if ( m_GI && giImage )
                m_GI->SetImage( giImage.get() );

            // The baked sky, SET EVERY FRAME INCLUDING WHEN IT IS ABSENT — matching
            // Graphic::SceneEnvironmentBind's shape, so the two paths cannot end up sampling different
            // generations of the same bake, and now also so neither can sample a different SCENE's.
            //
            // This used to be gated on `environment.IsComplete()`, and the gate is what let a renderer
            // carry one scene's sky into the next (Г14). The gate read as caution — "do not half-bind a
            // split-sum set" — but a slot that is not written keeps what it had, so refusing to write an
            // absent environment is precisely how the previous scene's environment survives. The
            // completeness of the set is still asserted, by the caller, as a REPORT
            // (DeferredLightingRenderer::ReportEnvironmentGap): it is a bake failure, not a mode.
            if ( m_EnvIrradiance )
                m_EnvIrradiance->SetTexture( environment.Irradiance );
            if ( m_EnvSpecular )
                m_EnvSpecular->SetTexture( environment.Prefiltered );
            SceneSkyLookBind( this, environment.Look );
            // Same asymmetry as SceneEnvironmentBind: the LUT is a renderer-global that is never
            // legitimately absent, so a null is a fault to leave visible rather than a state to bind.
            if ( m_BrdfLut && environment.BrdfLut )
                m_BrdfLut->SetImage( environment.BrdfLut );

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

            // Upload the dynamic lights into the same SSBO/UB layout the mesh PBR shader uses (bindings 6/16/4).
            // Empty is fine — the shader loops 0..count, so an untouched buffer is simply never read; but the
            // metadata counts must always be current.
            if ( !pointLights.PointLights.empty() )
            {
                if ( auto* sb = Get<StorageBufferProperty>( ShaderProtocols::PointLight::Name ) )
                    sb->SetRawData( (std::byte*)pointLights.PointLights.data(),
                                    static_cast<uint32_t>( pointLights.PointLights.size() *
                                                           sizeof( ShaderProtocols::PointLightPayload ) ) );
            }
            if ( !spotLights.SpotLights.empty() )
            {
                if ( auto* sb = Get<StorageBufferProperty>( ShaderProtocols::SpotLight::Name ) )
                    sb->SetRawData( (std::byte*)spotLights.SpotLights.data(),
                                    static_cast<uint32_t>( spotLights.SpotLights.size() *
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

        // Uploads the CSM data into ShadowUB + binds the cascade maps (u_ShadowMap0..3) — mirrors
        // Graphic::SceneShadowBind so the SAME sun shadows appear in the deferred path.
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

            static const char* kNames[4] = { "u_ShadowMap0", "u_ShadowMap1", "u_ShadowMap2", "u_ShadowMap3" };
            for ( uint32_t i = 0; i < 4u; ++i )
            {
                Image2D* img = ( i < n ) ? shadow.CascadeMaps[i] : nullptr;
                if ( img )
                    if ( auto* tex = Get<Texture2DProperty>( kNames[i] ) )
                        tex->SetImage( img );
            }
        }

        // Uploads the cloud layer's shadow into CloudShadowUB + binds the map, through the SAME writer
        // the forward PBR materials and the terrain material use (Graphic::CloudShadowBind). This pass
        // used to pack the block itself; the packing is now one function beside the block it fills, so
        // the two render paths cannot be told different things about one map.
        void UploadCloudShadow( const CloudShadowInput& cloudShadow )
        {
            CloudShadowBind( this, cloudShadow );
        }

        MPROPERTY( glm::vec4, LightDir,   "u_LightDir",   ( glm::vec4( 0.0f, -1.0f, 0.0f, 0.0f ) ) )
        MPROPERTY( glm::vec4, LightColor, "u_LightColor", ( glm::vec4( 1.0f, 1.0f, 1.0f, 3.0f ) ) )
        MPROPERTY( glm::vec4, Params,     "u_Params",     ( glm::vec4( 0.0f ) ) )
        MPROPERTY( glm::vec4, CameraPos,  "u_CameraPos",  ( glm::vec4( 0.0f ) ) )

    private:
        Texture2DProperty* m_GBufferA        = nullptr;
        Texture2DProperty* m_GBufferB        = nullptr;
        Texture2DProperty* m_GBufferC        = nullptr;
        Texture2DProperty* m_GBufferEmissive = nullptr;
        Texture2DProperty* m_SSAO            = nullptr;
        Texture2DProperty* m_GI              = nullptr;

        TextureCubeProperty* m_EnvIrradiance = nullptr;
        TextureCubeProperty* m_EnvSpecular   = nullptr;
        Texture2DProperty*   m_BrdfLut       = nullptr;
    };
} // namespace Desert::Graphic
