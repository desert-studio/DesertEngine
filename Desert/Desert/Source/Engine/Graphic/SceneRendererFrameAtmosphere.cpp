#include <Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp>
#include <Engine/Graphic/SceneRendererImpl.hpp>
#include <Engine/Graphic/FallbackTextures.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>





#include <algorithm>
#include <memory>
#include <format>
#include <string_view>
#include <vector>
#include <Engine/Graphic/SceneRendererFrame.hpp>
#include <Engine/Graphic/Systems/Scene/SceneFXSystem.hpp>

namespace Desert::Graphic
{
    void SceneRendererImpl::AddFrameParticlesSimulate( RDG::Builder& graph )
    {
        // UE PreRender: the FX system's PreOpaque chain, this view's sort and its Mesh rows' instances, before the
        // cascades and the base pass build their draws (the VFX module's ISceneFXSystem::AddPreOpaque).
        if ( auto* particles = m_Systems.Particle.get() )
            particles->AddPreOpaque( graph, m_Systems.Mesh.get() );
    }

    void SceneRendererImpl::AddFrameParticlesPostOpaque( RDG::Builder& graph, const RDG::TextureRef sceneDepth )
    {
        // UE PostRenderOpaque: the stacks that read the resolved scene depth, then the readbacks.
        if ( auto* particles = m_Systems.Particle.get() )
            particles->AddPostOpaque( graph, sceneDepth );
    }

    void SceneRendererImpl::AddFrameCloudShadowMap( RDG::Builder& graph, FrameTextures& textures )
    {
        // The cloud layer's shadow on the world. HERE, and not beside the cloud march at the other end of the
        // frame, because its readers are the lit passes (the deferred Composite, the forward meshes and terrain),
        // which take it as a scene view input (SceneViewInputsOf). It reads no scene depth, no G-buffer and no
        // exact type
        auto* clouds = m_Systems.VolumetricCloud.get();
        if ( clouds == nullptr )
            return;
        ComputeNodeList nodes = clouds->DeclareShadowMapNodes( graph.GetFrameArena() );
        // NOT ON THE ASYNC PIPE: the map is two persistent copies now (CloudAmortizedRefresh.hpp), and the copy a
        // node writes was read by the previous frame's lit passes on the graphics queue; on the one queue the
        // graph's barrier from the image's recorded state orders the two, across the queues nothing here does.
        clouds->AddInputUploads( graph, textures ); // what the declaration just baked, before its readers
        clouds->SettleShadowMapNodes( AddComputeNodes( graph, textures, std::move( nodes ) ) );
        // The front copy; its readers take the ref through SceneViewInputsOf (CloudShadowMapOrWhite).
        textures.Transients.CloudShadowMap = textures.Import( clouds->GetShadowMap(), "Clouds.ShadowMap" );
    }

    void SceneRendererImpl::AddFrameCloudSkyOcclusion( RDG::Builder& graph, FrameTextures& textures )
    {
        // A path-traced frame draws no screen clouds, but the layer's sky-occlusion volume is an input of the sky
        // capture that marches the clouds into the environment the paths read (PathTracerSky "Prepare"): without
        // it the capture waits on an undecided volume and the path-traced sky has no clouds.
        auto* clouds = m_Systems.VolumetricCloud.get();
        if ( clouds == nullptr )
            return;
        ComputeNodeList nodes = clouds->DeclareSkyOcclusionNodes( graph.GetFrameArena() );
        clouds->AddInputUploads( graph, textures ); // what the declaration just baked, before its readers
        clouds->SettleSkyOcclusionNodes( AddComputeNodes( graph, textures, std::move( nodes ) ) );
    }

    void SceneRendererImpl::AddFrameSkyLight( RDG::Builder& graph, FrameTextures& textures )
    {
        auto* sky = m_Systems.Skybox.get();
        if ( sky == nullptr )
            return;
        // The cloud inputs the environment bake made this update (OnUpdate, before this graph) reach their images
        // here, ahead of the capture that marches them.
        if ( auto* clouds =
                  m_Systems.VolumetricCloud.get() )
            clouds->AddInputUploads( graph, textures );
        sky->SettleAtmosphereCachedLutNodes(
             AddComputeNodes( graph, textures, sky->DeclareAtmosphereCachedLutNodes( graph.GetFrameArena() ) ) );
        sky->AddSkyCapturePasses( graph, textures );
    }

    void SceneRendererImpl::AddFrameSkyAtmosphereLuts( RDG::Builder& graph, FrameTextures& textures )
    {
        auto* sky = m_Systems.Skybox.get();
        if ( sky != nullptr )
            sky->SettleAtmosphereLutNodes( AddComputeNodes(
                 graph, textures,
                 sky->DeclareAtmosphereLutNodes( graph.GetFrameArena(), textures.GraphRefs(),
                                                 GetCloudShadowInput() ) ) );
    }

    System::VolumetricFogSunInput SceneRendererImpl::GatherVolumeSunInput( const float scale ) const
    {
        // The sun exactly as the surfaces receive it: DirLights[0] after the atmosphere's transmittance
        // (BeginScene), times @p scale, shadowed by the same cascades.
        System::VolumetricFogSunInput sun;
        if ( const auto& lights = m_DirectionLights.DirectionLights; !lights.empty() )
        {
            sun.Present   = true;
            sun.TowardSun = glm::normalize( -glm::vec3( lights[0].Direction ) );
            sun.Radiance  = glm::vec3( lights[0].ColorIntensity ) * lights[0].ColorIntensity.w * scale;
        }
        // The sun is directional-shadow slot 0, published by the mesh renderer exactly as every receiver packs it.
        if ( const auto* mesh = m_Systems.Mesh.get(); mesh != nullptr && mesh->AreShadowsEnabled() )
        {
            // The sun is directional slot 0 (DirLights[0]); its cascades as this frame publishes them, the same
            // value the opaque receivers pack (MeshRenderer::DirectionalShadows).
            const SceneResources::DirectionalShadowView view = mesh->DirectionalShadows()[0];
            if ( view.CastShadows && view.CascadeVP != nullptr && view.FirstLayer == 0 )
            {
                sun.CascadeCount = std::min<uint32_t>( view.Count, 4u );
                for ( uint32_t c = 0; c < sun.CascadeCount; ++c )
                    sun.CascadeViewProj[c] = view.CascadeVP[c];
                sun.CascadeWorldPerTexel = view.CascadeWorldPerTexel;
                sun.CascadeDepthRange    = view.CascadeDepthRange;
            }
        }
        // The cloud layer's shadow on the sun term (UE "Shadow On Atmosphere"): the surfaces' very pair, valid
        // once AddFrameCloudShadowMap declared this frame's map. The sky term's visibility: the view's rain
        // occlusion, the top-down depth of every caster around the camera (a roof hides the sky).
        sun.CloudShadow = CloudShadowPackUniforms( GetCloudShadowInput() );
        if ( const auto* mesh = m_Systems.Mesh.get() )
            sun.SkyOcclusion = mesh->GetRainOcclusionUniforms();
        return sun;
    }

    void SceneRendererImpl::AddFrameVolumetricFog( RDG::Builder& graph, FrameTextures& textures,
                                                   const ViewFrame& frame )
    {
        auto* volume = m_Systems.VolumetricFog.get();
        if ( volume == nullptr )
            return;
        // The sun as the surfaces receive it, times its VolumetricScatteringIntensity.
        const System::VolumetricFogSunInput sun =
             GatherVolumeSunInput( glm::max( m_SunVolumetricScatteringIntensity, 0.0f ) );
        // Inactive (fog off, VolumetricFog off, machine ceiling 0): no node, and the frame result says so.
        // The froxels read the local lights' shadows by the opaque lighting's rule (atlas, or virtual maps).
        // The heterogeneous volumes' lighting cache first (once per view frame, nothing without a volume on
        // screen): its sun transmittance shadows the froxels below. Lit by the sun as the surfaces are (scale 1).
        System::HeterogeneousVolumeShadowInput heterogeneousShadow{};
        if ( auto* smoke = m_Systems.HeterogeneousVolume.get(); smoke != nullptr && !m_HeterogeneousVolumes.empty() )
        {
            AddComputeNodes( graph, textures,
                             smoke->DeclareLightingNodes( graph.GetFrameArena(), textures.GraphRefs(), frame,
                                                          GatherVolumeSunInput( 1.0f ), LocalShadowsAreVirtual(),
                                                          m_HeterogeneousVolumes ) );
            heterogeneousShadow = smoke->GetShadowInput();
        }
        AddComputeNodes( graph, textures,
                         volume->DeclareFrameNodes( graph.GetFrameArena(), textures.GraphRefs(), frame, sun,
                                                    LocalShadowsAreVirtual(), m_LocalFogVolumes,
                                                    heterogeneousShadow ) );
    }

    void SceneRendererImpl::AddFrameTranslucencyLightingVolume( RDG::Builder& graph, FrameTextures& textures,
                                                                const ViewFrame& frame )
    {
        auto* volume    = m_Systems.TranslucencyLightingVolume.get();
        auto* particles = m_Systems.Particle.get();
        // Zero cost when unused: no lit particle in this view's frame, no node and no volume read. Asked here,
        // right before the particles' DrawPass, where every draw record of the frame exists (PrepareFrame, the
        // simulate declare, the PostOpaque collision import): the volume is filled exactly when a draw reads it.
        if ( volume == nullptr || particles == nullptr || m_ViewProfile.SceneCapture ||
             !particles->DrawsLitSprites() )
            return;
        // The sun's irradiance as the surfaces receive it (no volumetric scattering scale: a particle is lit, not
        // a medium's in-scatter).
        const System::VolumetricFogSunInput sun = GatherVolumeSunInput( 1.0f );
        AddComputeNodes( graph, textures,
                         volume->DeclareFrameNodes( graph.GetFrameArena(), textures.GraphRefs(), frame, sun,
                                                    LocalShadowsAreVirtual() ) );
        if ( const std::shared_ptr<Image3D>& lit = volume->GetFrameVolume() )
            textures.Transients.TranslucencyLightingVolume = textures.Import(
                 std::shared_ptr<Image>( lit ), System::TranslucencyLightingVolumeRenderer::kVolumeName );
    }

    void SceneRendererImpl::AddFrameOpaqueFog( RDG::Builder& graph, FrameTextures& textures,
                                               const ViewFrame& frame, const OpaqueFogFroxels froxels )
    {
        AddFrameSceneDepthResolve( graph, textures );
        if ( froxels == OpaqueFogFroxels::AddHere )
            AddFrameVolumetricFog( graph, textures, frame );
        AddFrameAtmosphericFog( graph, textures );
    }

    void SceneRendererImpl::AddFrameAtmosphericFog( RDG::Builder& graph, FrameTextures& textures )
    {
        auto* fog = m_Systems.HeightFog.get();
        auto* volume = m_Systems.VolumetricFog.get();
        const System::VolumetricFogFrameResult volumeResult =
             volume != nullptr ? volume->GetFrameResult() : System::VolumetricFogFrameResult{};
        if ( fog == nullptr )
            return;
        // The evaluation of the OPAQUE depth, then its apply over the scene target - the opaque composite only.
        AddComputeNodes( graph, textures, fog->DeclareFrameNodes( graph, textures.Transients, volumeResult ) );
        // No fog image this frame (neither fog nor aerial perspective): no apply node at all. A G-buffer view
        // (IsGBufferView) shows the stored surface value and the wireframe view its edges (HidesAtmosphere): the
        // inputs below still publish, the apply does not run.
        if ( textures.Transients.HeightFog.IsValid() && !m_DebugView.HidesAtmosphere() )
            AddSystemRaster( graph, textures, fog->ApplyPass() );

        // What every translucent surface after this point fogs itself with, at its own depth
        // (Common/ViewFog.glslh, bound by name through SceneViewInputs): DeclareFrameNodes published the parameter
        // block (always - gates 0 without fog); here the three images it reads, each the engine's fallback when
        // absent (the block's gates say it is not sampled), so a translucent cell never has an unbound slot.
        const AtmosphereEnv&         atmosphere = GetAtmosphere();
        const std::shared_ptr<Image> fallback3D =
             FallbackTextures::Get().GetFallbackTexture3D( Core::Formats::ImageFormat::RGBA8F );
        FrameTransients& t = textures.Transients;
        if ( atmosphere.AerialPerspectiveVolume )
            t.FogAerialPerspective = textures.Import( std::shared_ptr<Image>( atmosphere.AerialPerspectiveVolume ),
                                                      "Sky.AerialPerspectiveLut" );
        else
            t.FogAerialPerspective = textures.Import( fallback3D, "HeightFog.AerialPerspectiveFallback" );
        if ( atmosphere.DistantSkyLight )
            t.FogDistantSkyLight =
                 textures.Import( std::shared_ptr<Image>( atmosphere.DistantSkyLight ), "Sky.DistantLight" );
        else
            t.FogDistantSkyLight = textures.Import(
                 FallbackTextures::Get().GetFallbackTexture2D( Core::Formats::ImageFormat::RGBA8F ),
                 "HeightFog.DistantSkyLightFallback" );
        if ( volumeResult.Integrated != nullptr )
            t.FogVolume = textures.Import( volumeResult.Integrated, "VolumetricFog.Integrated" );
        else
            t.FogVolume = textures.Import( fallback3D, "HeightFog.VolumetricFogFallback" );
    }

    void SceneRendererImpl::AddFrameVolumetricClouds( RDG::Builder& graph, FrameTextures& textures,
                                                      const ViewFrame& frame )
    {
        auto* clouds = m_Systems.VolumetricCloud.get();
        if ( clouds == nullptr )
            return;
        VolumetricCloudHistory&             history = m_ViewState.Clouds();
        ComputeNodeList                     nodes   = clouds->DeclareFrameNodes( graph, frame, history );
        clouds->AddInputUploads( graph, textures ); // what the declaration just baked, before its readers
        // One AddPass per node from here: the resolve's index among the nodes (FrameResult::ResolveNode) plus this
        // is the writer whose loss the view's EndFrame checks.
        const uint32_t firstPass = graph.GetPassCount();
        const bool     hasNodes  = !nodes.empty();
        const bool     accepted  = AddComputeNodes( graph, textures, std::move( nodes ) );
        clouds->SettleFrameNodes( accepted );
        // The reconstruction pair is the view's history (SceneViewState::Clouds); the composite (a translucency
        // pass, added after this by AddFrameTranslucency) reads the full-resolution upsample by graph ref.
        const System::VolumetricCloudRenderer::FrameResult result = clouds->GetFrameResult();
        if ( accepted && hasNodes && result.Reconstruction.Scatter.IsValid() )
            history.SetWriterPass( firstPass + result.ResolveNode );
        textures.Transients.CloudScatter = result.Upsampled;
    }
} // namespace Desert::Graphic
