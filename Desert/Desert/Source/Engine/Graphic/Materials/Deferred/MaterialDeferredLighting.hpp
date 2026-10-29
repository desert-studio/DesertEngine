#pragma once

#include <Engine/Graphic/Materials/SceneResources.hpp>

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/Properties/StorageBufferProperty.hpp>
#include <Engine/Graphic/Materials/Properties/TextureCubeProperty.hpp>
#include <Engine/Graphic/Materials/Properties/UniformBufferProperty.hpp>
#include <Engine/Graphic/Materials/SceneLightingBinding.hpp>
#include <Engine/Graphic/DebugViewState.hpp>
#include <Engine/Graphic/DefaultTextures.hpp>
#include <Engine/Graphic/FallbackTextures.hpp>

#include <Engine/Graphic/Clouds/CloudShadowBinding.hpp>
#include <Engine/Graphic/Clouds/CloudShadowPayload.hpp>

#include <Engine/Graphic/ShaderProtocols/PointLight.hpp>
#include <Engine/Graphic/ShaderProtocols/SpotLight.hpp>
#include <Engine/Graphic/ShaderProtocols/Metadata.hpp>

#include <glm/glm.hpp>

#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <type_traits>
#include <unordered_set>
#include <variant>

namespace Desert::Graphic
{
    // Every shadowed directional light's cascades (the same views Graphic::SceneShadowBind packs): the composite's
    // default program samples them for the sun and for atmosphere light 1 alike.
    struct DeferredCascadeShadow
    {
        SceneResources::DirectionalShadowViews Lights{};
    };

    // "Shadows: Ray Traced Sun" (UE FScreenShadowMaskTexture): the denoised screen visibility mask of the SUN, read by
    // the composite's DESERT_SUN_SHADOW_SCREEN_MASK program in place of the sun's cascades. The other shadowed
    // directional lights keep their cascades under it (Lights below).
    struct DeferredScreenShadowMask
    {
        RDG::TextureRef Mask; // r = visibility, 1 = lit
        // The cascades of the lights the mask does not cover (slot kDirectionalShadowSun is the mask's and is
        // packed unshadowed-by-cascades).
        SceneResources::DirectionalShadowViews Lights{};
    };

    // What the deferred lighting pass shadows the sun with: the arm IS the program (no method uniform).
    struct DeferredShadowInput
    {
        std::variant<DeferredCascadeShadow, DeferredScreenShadowMask> Sun;
        // Shadows Stochastic: the local lights are MegaLights' (DeferredCompositeInputs::MegaLights) and the
        // composite draws its MegaLights program (the Sun arm is then the screen mask).
        bool MegaLightsLocalLights = false;
        // Shadows HardwareTraced: the local lights with a ShadowMaskSlot read the ray-traced masks
        // (DeferredCompositeInputs::LocalShadowMasks) - the composite draws its RayTracedLocal program.
        bool RayTracedLocalLights = false;
        // The view's contact shadows, packed into the same block (SceneResources::PackContactShadow).
        SceneResources::ContactShadowSettings Contact{};
        // The sun's PCSS taps (SceneResources::PackShadowFilter); read by the cascade program only.
        SceneResources::ShadowFilterSettings Filter{};
        // The view's directional-shadow view mode (UE "Shadow" / CSM visualization in the lighting pass): ShadowFactor
        // writes the sun's occlusion as grey, Cascades tints the lit colour by the sun's chosen cascade. Rides
        // ShadowUB DebugParams.z, the slot the forward mesh shaders read it from.
        ShadowDebugMode Debug = ShadowDebugMode::Off;
    };

    // Where the composite's indirect light comes from (DeferredLighting.shader Params.w / ScreenSpaceGi).
    struct DeferredGIInput
    {
        // 0 none, 1 the screen-space gather inside the composite, 2 the traced final gather in place of the sky's
        // two cubes.
        int   Mode      = 0;
        float Intensity = 0.0f; // scales the screen-space gather only (PostProcessSettings GIIntensity)
        // The screen-space gather's samples per pixel (Scalability GlobalIllumination.ScreenSpaceSamples).
        int ScreenSpaceSamples = 12;
        // ViewFrame::PreExposure: the composite writes the scene colour multiplied by it (ScreenSpaceGi.y).
        float PreExposure = 1.0f;
        // SSS-PROFILE (CONTRACT §4): the view scatters - the composite splits the screen-space scattered texels'
        // diffuse into SubsurfaceDiffuseTexture (ScreenSpaceGi.z).
        bool SubsurfaceSplit = false;
    };

    // PURE. The ShadowUB values of @p shadow (every shadowed directional light's slot, PackDirectionalShadows; the
    // debug word 0; the contact shadow settings and the PCSS taps) - the deferred composite's and the surface-cache
    // lighting's one packer. Under a screen mask the sun's slot keeps its CastShadows but no cascade: the mask
    // program reads the mask for it.
    [[nodiscard]] inline SceneResources::ShadowUBData PackDeferredShadow( const DeferredShadowInput& shadow )
    {
        SceneResources::ShadowUBData data{};
        std::visit(
             [&data]( const auto& sun )
             {
                 using Arm = std::decay_t<decltype( sun )>;
                 SceneResources::DirectionalShadowViews lights = sun.Lights;
                 if constexpr ( std::is_same_v<Arm, DeferredScreenShadowMask> )
                 {
                     lights[SceneResources::kDirectionalShadowSun].CascadeVP = nullptr;
                     lights[SceneResources::kDirectionalShadowSun].Count     = 0;
                 }
                 SceneResources::PackDirectionalShadows( data, lights );
             },
             shadow.Sun );
        data.DebugParams = glm::vec4( 0.0f, 0.0f, static_cast<float>( shadow.Debug ), 0.0f );
        SceneResources::PackContactShadow( data, shadow.Contact );
        SceneResources::PackShadowFilter( data, shadow.Filter );
        return data;
    }

    // CloudShadowInput — the cloud layer's shadow, a SECOND independent occluder of the same sun — used
    // to be declared right here, which is precisely why nothing but this pass ever received it. It now
    // lives beside the uniform block it packs into (Engine/Graphic/Clouds/CloudShadowPayload.hpp), where
    // the forward mesh materials and the terrain can reach it too. It is a separate struct from
    // DeferredShadowInput on purpose: the cascades are a depth comparison against opaque geometry, this
    // is a volumetric transmittance reconstructed from one texel of one map, and they share nothing but
    // the light they attenuate.

    // THE BAKED SKY, as the deferred composite's ambient source — the same three images
    // Graphic::SceneFrameBinding hands the forward lit materials (Graphic::SceneEnvironmentBind), and
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
        // Whether there is a sky light. NoSky makes the two absent cubes the defined answer (zero sky ambient);
        // Ready promises them.
        SkyLightState Sky = SkyLightState::NoSky;

        bool IsComplete() const
        {
            return Irradiance != nullptr && Prefiltered != nullptr && BrdfLut != nullptr;
        }
    };

    // Fullscreen deferred-lighting material: binds the scene renderer's G-buffer color targets (albedo/metallic,
    // normal/roughness, world-position) + the sun (+ its CSM shadow maps) + ALL point & spot lights (uploaded
    // into the shared SSBO layout the mesh lit shader also uses) + a debug-mode selector, driving
    // DeferredLighting.shader. Header-only (no new .cpp -> no premake regen).
    class MaterialDeferredLighting final : public Material
    {
    public:
        // Over one program of DeferredLighting (the base one, or its DESERT_SUN_SHADOW_SCREEN_MASK permutation): a
        // permutation's bindings are its own (Material's shader constructor).
        explicit MaterialDeferredLighting( const std::shared_ptr<Shader>& shader )
             : Material( "MaterialDeferredLighting", shader )
        {
        }

        // The frame's values only: every graph texture the pass samples (G-buffer, AO, GI, shadow cascades)
        // is bound by name through RDG::PassBindings (DeferredLightingRenderer::Record). lightDir.xyz = direction
        // the sun travels; lightColor.rgb/.a = colour/intensity; cameraPos.xyz = camera world pos (view vector);
        // debugMode 0=Lit,1=Albedo,2=Normal,3=Metallic,4=Roughness; point/spotCount = the uploaded lights' counts.
        // light1Dir / light1Color: atmosphere light 1 (the moon) in the same form; light1Color.a == 0 = none.
        void BindInputs( const glm::vec4& lightDir, const glm::vec4& lightColor, const glm::vec4& light1Dir,
                         const glm::vec4& light1Color, const glm::vec4& cameraPos,
                         const glm::mat4& invJitteredViewProjection, int debugMode, uint32_t pointCount,
                         uint32_t spotCount, const DeferredShadowInput& shadow, const DeferredGIInput& gi,
                         bool ssaoEnabled, const CloudShadowInput& cloudShadow,
                         const DeferredEnvironmentInput& environment, const glm::mat4& jitteredViewProjection )
        {
            // The baked sky's cubes and the BRDF LUT are pass parameters (SceneViewInputs, bound by the Composite
            // node: System.BlackCube / System.Black when absent, UE GBlackTextureCube) - a graph ref of THIS
            // frame, so no previous scene's sky can survive in a slot. Completeness stays a REPORT
            // (DeferredLightingRenderer::ReportEnvironmentGap). The look they are read with is written every
            // frame.
            SceneSkyLookBind( this, environment.Look );

            SetLightDir( lightDir );
            SetLightColor( lightColor );
            SetLight1Dir( light1Dir );
            SetLight1Color( light1Color );
            SetCameraPos( cameraPos );
            // World position is rebuilt from the G-buffer depth (Common/ReconstructPosition.glslh) with
            // ViewFrame::InvJitteredViewProjection, the view's one copy of that inverse.
            SetInvJitteredViewProjection( invJitteredViewProjection );
            // Params: x = debug mode, y = GI intensity (0 = off), z = SSAO enabled (else shader uses AO=1),
            // w = GI mode (0 = off, 1 = screen-space gather, 2 = the traced GI's final gather in place of the
            // sky's two cubes). Mode picks WHERE the indirect light comes from; intensity scales the screen-space
            // gather only.
            SetParams( glm::vec4( static_cast<float>( debugMode ), gi.Intensity, ssaoEnabled ? 1.0f : 0.0f,
                                  static_cast<float>( gi.Mode ) ) );
            // ScreenSpaceGi: x = the screen-space gather's samples per pixel (read under mode 1 only), y = the
            // view's PreExposure the lit colour is stored at.
            SetScreenSpaceGI(
                 glm::vec4( static_cast<float>( gi.ScreenSpaceSamples ), gi.PreExposure,
                            gi.SubsurfaceSplit ? 1.0f : 0.0f, 0.0f ) );

            // The contact shadows march the G-buffer depth in the space it was rasterised in
            // (Common/ContactShadow.glslh); their steps and the sun's length ride ShadowUB (UploadShadow).
            SetJitteredViewProjection( jitteredViewProjection );

            UploadShadow( shadow );
            UploadCloudShadow( cloudShadow );

            // The dynamic lights themselves are NOT material data: the Composite node uploads them into graph
            // buffers (DeferredLightingRenderer::UploadLights, Builder::QueueBufferUpload) and binds them as
            // block entries, so the PointLight / SpotLight storage properties stay unwritten and the route fill
            // leaves those slots to the pass. Only the counts the shader loops to are written here.
            const ShaderProtocols::LightsMetadata counts{
                 .DirectionLightsCount = 0U, .PointLightsCount = pointCount, .SpotLightsCount = spotCount };
            if ( auto* meta = Get<UniformBufferProperty>( ShaderProtocols::LightsMetadata::Parameter ) )
                meta->SetRawData( reinterpret_cast<const std::byte*>( &counts ), sizeof( counts ) );

            // DeferredUB (the MPROPERTYs above) is the only field-filled buffer here and is uploaded where the
            // composite reads it (MaterialExecutor::Apply). ShadowUB, CloudShadowUB and LightsMetadata were just
            // written whole and are never flushed from field copies -- see ShaderResources::BufferFillKind.hpp.
        }

        // Uploads the CSM data into ShadowUB (the cascade maps ShadowCascadesTexture are a graph texture, bound by
        // DeferredLightingRenderer::Record) — mirrors Graphic::SceneShadowBind so the SAME sun shadows appear.
        void UploadShadow( const DeferredShadowInput& shadow )
        {
            const SceneResources::ShadowUBData data = PackDeferredShadow( shadow );

            if ( auto* ub = Get<UniformBufferProperty>( SceneResources::kShadowBlock ) )
                ub->SetRawData( reinterpret_cast<const std::byte*>( &data ), sizeof( data ) );
        }

        // Uploads the cloud layer's shadow into CloudShadowUB through the SAME packer the forward lit
        // materials and the terrain material use (Graphic::CloudShadowUpload), so the two render paths cannot
        // be told different things about one map. The map itself (CloudShadowMapTexture) is a graph resource the
        // composite exec binds through RDG::PassBindings (FrameTransients::CloudShadowMap, System.White when
        // the frame has none), never here.
        void UploadCloudShadow( const CloudShadowInput& cloudShadow )
        {
            CloudShadowUpload( this, cloudShadow );
        }

        MPROPERTY( glm::vec4, LightDir,   "LightDirection",   ( glm::vec4( 0.0f, -1.0f, 0.0f, 0.0f ) ) )
        MPROPERTY( glm::vec4, LightColor, "LightColor", ( glm::vec4( 1.0f, 1.0f, 1.0f, 3.0f ) ) )
        MPROPERTY( glm::vec4, Params,     "Params",     ( glm::vec4( 0.0f ) ) )
        MPROPERTY( glm::vec4, CameraPos,  "CameraPosition",  ( glm::vec4( 0.0f ) ) )
        MPROPERTY( glm::mat4, InvJitteredViewProjection, "InverseJitteredViewProjection", ( glm::mat4( 1.0f ) ) )
        MPROPERTY( glm::mat4, JitteredViewProjection, "JitteredWorldToClip", ( glm::mat4( 1.0f ) ) )
        MPROPERTY( glm::vec4, ScreenSpaceGI, "ScreenSpaceGi", ( glm::vec4( 12.0f, 1.0f, 0.0f, 0.0f ) ) )
        MPROPERTY( glm::vec4, Light1Dir,   "Light1Direction", ( glm::vec4( 0.0f, -1.0f, 0.0f, 0.0f ) ) )
        MPROPERTY( glm::vec4, Light1Color, "Light1Color",     ( glm::vec4( 0.0f ) ) )

    private:
    };
} // namespace Desert::Graphic
