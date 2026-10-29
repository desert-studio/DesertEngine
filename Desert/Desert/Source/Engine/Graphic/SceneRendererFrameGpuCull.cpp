// SceneRenderer's GPU-DRIVEN CULL node (phase CULL, DebugViewState::ValidateGpuCulling): the InstanceCulling
// phases over every view the CPU path culls this frame - the main view (two-phase FURTHEST-HZB occlusion, UE
// FInstanceCullingContext + HZB.cpp), the sun's cascades and the VSM clipmap levels (the "VSM: Cull Casters"
// flavour of the same pass) - while the drawing stays on the CPU path. The next frame's phase 1 reads the furthest
// HZB this node extracts into m_CullHzbHistory.
#include <Engine/Core/Camera.hpp>
#include <Engine/Core/Frustum.hpp>
#include <Engine/Graphic/GpuDriven/InstanceCulling.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/SceneRendererFrame.hpp>
#include <Engine/Graphic/Systems/Scene/Deferred/HZBRenderer.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp>
#include <Engine/Graphic/Systems/Scene/Shadows/VirtualShadowMap.hpp>
#include <Engine/Graphic/VisibilityCulling.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <string>
#include <vector>

namespace Desert::Graphic
{
    namespace
    {
        // CullViewInput of one view: the matrices, frustum, LOD and distance-cull origins of the main camera, and
        // the flags; ViewIndex / FrustumPlanes are filled by InstanceCulling from the list position and Frustum.
        GpuDriven::CullViewInput MakeCullView( const GpuDriven::MeshPass pass, const glm::mat4& viewProjection,
                                               const glm::mat4& prevViewProjection, const Core::Frustum& frustum,
                                               const Geometry::LODView& lod, const float lensScale,
                                               const uint32_t flags )
        {
            GpuDriven::CullViewInput input{};
            input.View.ViewProjection     = viewProjection;
            input.View.PrevViewProjection = prevViewProjection;
            input.View.LodProjection      = lod.Projection;
            input.View.LodOrigin          = glm::vec4( lod.Origin, 0.0f );
            input.View.CullOrigin         = glm::vec4( lod.Origin, lensScale );
            input.View.Pass               = static_cast<uint32_t>( pass );
            input.View.Flags              = flags;
            input.Frustum                 = frustum;
            return input;
        }
    } // namespace

    void SceneRenderer::AddFrameGpuCull( RDG::Builder& graph, FrameTextures& textures, const ViewFrame& frame )
    {
        // Logged once per distinct message: the gate is a debug view held on for many frames.
        const auto refuse = [this]( std::string message )
        {
            if ( message != m_GpuCullLastError )
                LOG_ERROR( "[SceneRenderer] {}: {}", m_ViewResources.GetName(), message );
            m_GpuCullLastError = std::move( message );
        };

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* meshes = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* hzbRenderer                   = UNIQUE_GET_AS( System::HZBRenderer, m_RenderSystems["HZBSystem"] );
        const GpuDriven::GpuScene* gpuScene = meshes != nullptr ? meshes->GetGpuScene() : nullptr;
        const Core::Camera*        camera   = GetMainCamera();
        if ( meshes == nullptr || hzbRenderer == nullptr || gpuScene == nullptr || camera == nullptr )
        {
            refuse( "GPUDriven: Cull: no MeshSystem, HZBSystem, GPU scene or main camera; nothing is culled" );
            return;
        }

        // Refused while an upload is pending: this frame's tables are not what the CPU path culls.
        const Common::ResultStr<GpuDriven::GpuSceneBuffers> scene = gpuScene->ImportTables( graph );
        if ( !scene )
        {
            refuse( std::format( "GPUDriven: Cull: {}", scene.GetError() ) );
            return;
        }

        const RDG::TextureRef depth     = textures.Depth( m_GBuffer, "GBuffer" );
        const auto            depthDesc = graph.GetTextureDesc( depth );
        if ( !depthDesc )
        {
            refuse( std::format( "GPUDriven: Cull: GBuffer depth: {}", depthDesc.GetError() ) );
            return;
        }
        const uint32_t width  = depthDesc.GetValue().Size.Width;
        const uint32_t height = depthDesc.GetValue().Size.Height;

        // Every view selects LOD and distance-culls ISM from the main camera (MeshRenderer::ComputeLOD,
        // MeshRenderer.cpp:193-209; the shadows draw the main view's LOD).
        const Geometry::LODView lod{ .Origin     = camera->GetPosition(),
                                     .Projection = camera->GetProjectionMatrix() };
        const float             lensScale = Graphic::DrawDistanceScale( lod );
        const uint32_t          lodFlag   = meshes->IsLODEnabled() ? 0u : GpuDriven::CullViewFlags::kLodDisabled;

        std::vector<GpuDriven::CullViewInput> views;
        std::vector<std::string>              viewNames; // the validation log's name of each view

        // Main view: phase 1 against last frame's furthest HZB, only when it is this view's size and the view has
        // a previous frame (PrevViewProjection projects into it).
        const bool prevHzbValid = m_CullHzbHistory.Physical != nullptr &&
                                  m_CullHzbHistory.Desc.Size.Width == width &&
                                  m_CullHzbHistory.Desc.Size.Height == height && frame.HistoryValid();
        {
            GpuDriven::CullViewInput mainInput =
                 MakeCullView( GpuDriven::MeshPass::GBuffer, frame.ViewProjection, frame.PrevViewProjection,
                               camera->GetFrustum(), lod, lensScale,
                               lodFlag | ( prevHzbValid ? GpuDriven::CullViewFlags::kTestHzb : 0u ) );
            if ( prevHzbValid )
            {
                mainInput.PrevHzb        = graph.RegisterExternal( m_CullHzbHistory, "GPUDriven.PrevHzb" );
                mainInput.View.HzbExtent = glm::uvec4(
                     width, height, static_cast<uint32_t>( std::bit_width( std::max( width, height ) ) ), 0u );
            }
            views.push_back( mainInput );
            viewNames.emplace_back( "GBuffer" );
        }
        const size_t mainView = 0;

        // The sun's cascades, with the casters' matrices (MeshRendererShadow.cpp:820).
        if ( m_FrameCascades )
        {
            const SceneResources::DirectionalShadowViews shadows = meshes->DirectionalShadows();
            const uint32_t cascades = shadows[SceneResources::kDirectionalShadowSun].Count;
            for ( uint32_t c = 0; c < cascades; ++c )
            {
                const glm::mat4& vp = meshes->GetCasterViewProjection(
                     SceneResources::kDirectionalShadowSun * System::MeshRenderer::kMaxCascades + c );
                views.push_back( MakeCullView( GpuDriven::MeshPass::ShadowCascade, vp, vp,
                                               Core::Frustum( vp, glm::mat4( 1.0f ) ), lod, lensScale,
                                               GpuDriven::CullViewFlags::kShadowCaster |
                                                    GpuDriven::CullViewFlags::kOrthographic | lodFlag ) );
                viewNames.push_back( std::format( "cascade {}", c ) );
            }
        }

        // The VSM clipmap levels: the far plane open, as the casters' cull (MeshRendererShadow.cpp:826), and the
        // drawn-page mask of this frame. A frame with no sun (only local lights in the map) has none.
        if ( m_FrameVsmPages && m_FrameVsmPages->HasSun )
        {
            for ( uint32_t level = 0; level < System::kVsmClipmapLevelCount; ++level )
            {
                const glm::mat4& vp = m_FrameVsmPages->LevelViewProjection[level];
                Core::Frustum    frustum( vp, glm::mat4( 1.0f ) );
                frustum.Open( Core::PLANE_FAR );
                GpuDriven::CullViewInput input = MakeCullView(
                     GpuDriven::MeshPass::VirtualShadow, vp, vp, frustum, lod, lensScale,
                     GpuDriven::CullViewFlags::kShadowCaster | GpuDriven::CullViewFlags::kOrthographic |
                          GpuDriven::CullViewFlags::kVsmPageMask | lodFlag );
                input.View.VsmLevel = level;
                input.VsmDirtyMask  = m_FrameVsmPages->DirtyMask;
                views.push_back( input );
                viewNames.push_back( std::format( "VSM level {}", level ) );
            }
        }

        // No bins until DRAWBINS: the cull writes every Visibility word, emits into no bin.
        const std::array<GpuDriven::PassDrawTable, GpuDriven::kMeshPassCount>   tables{};
        const std::array<GpuDriven::PassDrawBuffers, GpuDriven::kMeshPassCount> passes{};
        const RDG::TextureRef neutralHzb = textures.GraphRefs().System.Black;

        const Common::ResultStr<GpuDriven::MainPhaseResult> mainPhase =
             m_GpuCull.AddMainPhase( graph, scene.GetValue(), tables, passes, views, neutralHzb );
        if ( !mainPhase )
        {
            refuse( mainPhase.GetError() );
            return;
        }

        // This frame's furthest HZB of the CPU-drawn depth: phase 2's occlusion input and next frame's phase 1.
        const RDG::TextureRef furthest =
             hzbRenderer->AddPasses( graph, depth, width, height, System::HZBKind::Furthest );
        if ( !furthest.IsValid() )
        {
            refuse( "GPUDriven: Cull: the furthest HZB could not be built (no HZB pipeline); no phase 2" );
            return;
        }
        // Phase 2 only re-tests what phase 1 occluded, which needs last frame's HZB.
        if ( prevHzbValid )
        {
            views[mainView].Hzb                                               = furthest;
            const Common::ResultStr<std::vector<GpuDriven::CulledDraws>> post = m_GpuCull.AddPostOcclusionPhase(
                 graph, scene.GetValue(), tables, passes, views, mainPhase.GetValue().Carry );
            if ( !post )
            {
                refuse( post.GetError() );
                return;
            }
        }
        graph.Extract( furthest, m_CullHzbHistory, RDG::Access::SampledCompute );
        RDG::BufferRef vsmDirtyMask{};
        if ( m_FrameVsmPages )
            vsmDirtyMask = m_FrameVsmPages->DirtyMask;
        m_GpuCullValidation.AddFrame( graph, frame.FrameIndex, *gpuScene, views, viewNames, mainPhase.GetValue().Carry,
                                      vsmDirtyMask );
        m_GpuCullLastError.clear();
    }
} // namespace Desert::Graphic
