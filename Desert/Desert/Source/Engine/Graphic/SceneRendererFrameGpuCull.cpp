// SceneRenderer's GPU-DRIVEN CULL node (phase CULL, DebugViewState::ValidateGpuCulling): the InstanceCulling
// phases over every view the CPU path culls this frame - the main view (two-phase FURTHEST-HZB occlusion, UE
// FInstanceCullingContext + HZB.cpp), the sun's cascades and the VSM clipmap levels (the "VSM: Cull Casters"
// flavour of the same pass) - while the drawing stays on the CPU path. The next frame's phase 1 reads the furthest
// HZB this node extracts into m_CullHzbHistory (or the draw bins' phase 2 extracted, when they ran).
// The DRAW BINS (AddFrameDrawBins, AddFrameDrawBinsPostOcclusion): the same cull feeding the passes' indirect
// draws, the deferred main view in two phases around this frame's furthest HZB.
#include <Engine/Core/Camera.hpp>
#include <Engine/Core/Frustum.hpp>
#include <Engine/Geometry/StaticMesh.hpp>
#include <Engine/Graphic/GpuDriven/InstanceCulling.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/SceneRendererFrame.hpp>
#include <Engine/Graphic/Systems/Scene/Deferred/HZBRenderer.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp>
#include <Engine/Graphic/Systems/Scene/Shadows/VirtualShadowMap.hpp>
#include <Engine/Graphic/VirtualGeometry/VirtualGeometryLayout.hpp>
#include <Engine/Graphic/VirtualGeometry/VirtualGeometryRenderer.hpp>
#include <Engine/Graphic/VirtualGeometry/VirtualMeshTable.hpp>
#include <Engine/Graphic/VisibilityCulling.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <format>
#include <map>
#include <span>
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

        // The VG candidate tables of one frame (InstanceCulling's VirtualGeometryCandidateInput) and their exact
        // bounds. No tables (invalid Input.PrimitiveDags) when no queued mesh has uploaded DAG rows.
        struct VirtualGeometryCandidateTables
        {
            GpuDriven::VirtualGeometryCandidateInput Input;
            uint32_t                                 Instances       = 0;
            uint32_t                                 VisibleCapacity = 0;
        };

        // From the static queue: per primitive row, the DAG rows of every submesh one of its queue entries shows
        // (one entry per material slot: their union), appended for cull view 0 (the main view) only, as cut view
        // 0. Capacity = instances x DAGs; VisibleCapacity = instances x the DAGs' ClusterCount (a cut never draws
        // more clusters than its DAG holds). Refused when that bound passes the visible-list index range.
        Common::ResultStr<VirtualGeometryCandidateTables>
        MakeVirtualGeometryCandidates( RDG::Builder& graph, System::MeshRenderer& meshes,
                                       const GpuDriven::GpuScene& gpuScene, const uint32_t primitiveRows,
                                       const uint32_t viewCount )
        {
            using Result = VirtualGeometryCandidateTables;
            const VirtualGeometry::VirtualMeshTable& table = VirtualGeometry::VirtualMeshTable::Get();
            std::map<uint32_t, std::vector<VirtualGeometry::VirtualMeshDagSpan>> byPrimitive;
            for ( const auto& rd : meshes.GetStaticQueue() )
            {
                if ( rd.Mesh == nullptr || rd.GpuPrimitive >= primitiveRows )
                    continue;
                const auto* registration = rd.Mesh->GetVirtualMesh();
                if ( registration == nullptr || !*registration )
                    continue;
                const size_t submeshes = rd.Mesh->GetSubmeshes().size();
                for ( const VirtualGeometry::VirtualMeshDagSpan& span : table.DescribeUploaded( registration->Index() ) )
                {
                    if ( span.Submesh >= submeshes ||
                         ( span.Submesh < 64u && ( ( rd.HiddenSubmeshes >> span.Submesh ) & 1u ) != 0u ) )
                        continue;
                    auto& spans = byPrimitive[rd.GpuPrimitive];
                    if ( std::ranges::none_of( spans, [&span]( const VirtualGeometry::VirtualMeshDagSpan& known )
                                               { return known.Dag == span.Dag; } ) )
                        spans.push_back( span );
                }
            }
            Result out;
            if ( byPrimitive.empty() )
                return Common::MakeSuccess( std::move( out ) );

            std::vector<glm::uvec2> primitiveDags( primitiveRows, glm::uvec2( 0u ) );
            std::vector<glm::uvec2> entries;
            uint64_t                capacity       = 0;
            uint64_t                visible        = 0;
            uint64_t                instancesTotal = 0;
            for ( const auto& [primitive, spans] : byPrimitive )
            {
                const uint64_t instances = gpuScene.Primitive( primitive ).InstanceCount;
                primitiveDags[primitive] =
                     glm::uvec2( static_cast<uint32_t>( entries.size() ), static_cast<uint32_t>( spans.size() ) );
                for ( const VirtualGeometry::VirtualMeshDagSpan& span : spans )
                {
                    entries.emplace_back( span.Dag, span.RootNode );
                    visible += instances * span.ClusterCount;
                }
                capacity += instances * spans.size();
                instancesTotal += instances;
            }
            if ( visible > VirtualGeometry::kMaxVisibleClusters || capacity > UINT32_MAX )
                return Common::MakeError<Result>( std::format(
                     "VirtualGeometry: {} drawn VG instances hold {} clusters / {} candidates, past the visible-list "
                     "index range {}",
                     instancesTotal, visible, capacity, VirtualGeometry::kMaxVisibleClusters ) );
            if ( capacity == 0u || visible == 0u )
                return Common::MakeSuccess( std::move( out ) );

            std::vector<uint32_t> cutViewOf( std::max( viewCount, 1u ), GpuDriven::kInvalidIndex );
            cutViewOf[0] = 0u; // the main view is cull view 0 (AddFrameDrawBins) and the cut's only view

            const auto upload = [&graph]( const std::string& name, const std::span<const std::byte> bytes )
            {
                const RDG::BufferRef buffer = graph.CreateBuffer( RDG::BufferDesc{ .Bytes = bytes.size() }, name );
                graph.QueueBufferUpload( buffer, bytes );
                return buffer;
            };
            out.Input.PrimitiveDags = upload( "GPUDriven.VgPrimitiveDags", std::as_bytes( std::span( primitiveDags ) ) );
            out.Input.DagEntries    = upload( "GPUDriven.VgDagEntries", std::as_bytes( std::span( entries ) ) );
            out.Input.CutViewOf     = upload( "GPUDriven.VgCutViewOf", std::as_bytes( std::span( cutViewOf ) ) );
            out.Input.PrimitiveRows = primitiveRows;
            out.Input.Capacity      = static_cast<uint32_t>( capacity );
            out.Instances           = static_cast<uint32_t>( std::min<uint64_t>( instancesTotal, UINT32_MAX ) );
            out.VisibleCapacity     = static_cast<uint32_t>( visible );
            return Common::MakeSuccess( std::move( out ) );
        }
    } // namespace

    void SceneRenderer::AddFrameDrawBins( RDG::Builder& graph, FrameTextures& textures, const ViewFrame& frame )
    {
        m_FrameGpuSceneTables.reset();
        m_FrameDrawBins.reset();
        m_FramePrevHzb     = {};
        m_FrameFurthestHzb = {};
        const auto refuse = [this]( std::string message )
        {
            m_DrawBinPhaseCounts.Forget();
            if ( message != m_DrawBinsLastError )
                LOG_ERROR( "[SceneRenderer] {}: GPUDriven: DrawBins: {}; every pass draws on the CPU path",
                           m_ViewResources.GetName(), message );
            m_DrawBinsLastError = std::move( message );
        };
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* meshes = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
        const GpuDriven::GpuScene* gpuScene = meshes != nullptr ? meshes->GetGpuScene() : nullptr;
        const Core::Camera*        camera   = GetMainCamera();
        if ( meshes == nullptr || gpuScene == nullptr || camera == nullptr || meshes->GetDrawCommandCache() == nullptr )
        {
            m_DrawBinPhaseCounts.Forget();
            return; // nothing to draw from bins: the CPU lists draw everything
        }
        const Common::ResultStr<GpuDriven::DrawCommandBuffers> upload = meshes->BeginFrameBins( graph );
        if ( !upload )
        {
            refuse( upload.GetError() );
            return;
        }
        const Common::ResultStr<GpuDriven::GpuSceneBuffers> scene = gpuScene->ImportTables( graph );
        if ( !scene )
        {
            refuse( scene.GetError() );
            return;
        }
        m_FrameGpuSceneTables = scene.GetValue();

        const Geometry::LODView lod{ .Origin = camera->GetPosition(), .Projection = camera->GetProjectionMatrix() };
        const float             lensScale = Graphic::DrawDistanceScale( lod );
        const uint32_t          lodFlag   = meshes->IsLODEnabled() ? 0u : GpuDriven::CullViewFlags::kLodDisabled;

        // The main view's phase 1 tests last frame's furthest HZB (UE two-phase occlusion) only when phase 2 can
        // redraw what it holds back: the deferred G-buffer (AddFrameDrawBinsPostOcclusion splits it around this
        // frame's HZB), an HZB pipeline, a history of this depth's size and a previous frame PrevViewProjection
        // projects into. Otherwise frustum only, and phase 2 is empty.
        FrameDrawBins             bins;
        const GpuDriven::MeshPass mainPass = meshes->MainViewPass();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        const auto* hzbRenderer = UNIQUE_GET_AS( System::HZBRenderer, m_RenderSystems["HZBSystem"] );
        uint32_t    hzbWidth = 0, hzbHeight = 0;
        if ( mainPass == GpuDriven::MeshPass::GBuffer && m_GBuffer && hzbRenderer != nullptr &&
             hzbRenderer->CanBuild() )
        {
            if ( const auto depthDesc = graph.GetTextureDesc( textures.Depth( m_GBuffer, "GBuffer" ) ) )
            {
                hzbWidth  = depthDesc.GetValue().Size.Width;
                hzbHeight = depthDesc.GetValue().Size.Height;
            }
        }
        bins.MainTestsHzb = hzbWidth != 0u && m_CullHzbHistory.Physical != nullptr &&
                            m_CullHzbHistory.Desc.Size.Width == hzbWidth &&
                            m_CullHzbHistory.Desc.Size.Height == hzbHeight && frame.HistoryValid();
        {
            GpuDriven::CullViewInput mainInput =
                 MakeCullView( mainPass, frame.ViewProjection, frame.PrevViewProjection, camera->GetFrustum(), lod,
                               lensScale, lodFlag | ( bins.MainTestsHzb ? GpuDriven::CullViewFlags::kTestHzb : 0u ) );
            if ( bins.MainTestsHzb )
            {
                m_FramePrevHzb           = graph.RegisterExternal( m_CullHzbHistory, "GPUDriven.PrevHzb" );
                mainInput.PrevHzb        = m_FramePrevHzb;
                mainInput.View.HzbExtent = glm::uvec4(
                     hzbWidth, hzbHeight, static_cast<uint32_t>( std::bit_width( std::max( hzbWidth, hzbHeight ) ) ),
                     0u );
            }
            bins.Views.push_back( mainInput );
            bins.ViewKeys.push_back( System::MeshRenderer::kBinnedMainView );
            bins.ViewNames.emplace_back( mainPass == GpuDriven::MeshPass::GBuffer ? "gbuffer" : "forward" );
        }
        std::vector<GpuDriven::CullViewInput>& views    = bins.Views;
        std::vector<uint32_t>&                 viewKeys = bins.ViewKeys;
        if ( m_FrameCascades )
        {
            const uint32_t cascades = meshes->DirectionalShadows()[SceneResources::kDirectionalShadowSun].Count;
            for ( uint32_t c = 0; c < cascades; ++c )
            {
                const uint32_t view = SceneResources::kDirectionalShadowSun * System::MeshRenderer::kMaxCascades + c;
                if ( !meshes->CascadeBinsDrawn( view ) )
                    continue;
                const glm::mat4& vp = meshes->GetCasterViewProjection( view );
                views.push_back( MakeCullView( GpuDriven::MeshPass::ShadowCascade, vp, vp,
                                               Core::Frustum( vp, glm::mat4( 1.0f ) ), lod, lensScale,
                                               GpuDriven::CullViewFlags::kShadowCaster |
                                                    GpuDriven::CullViewFlags::kOrthographic | lodFlag ) );
                viewKeys.push_back( view );
                bins.ViewNames.push_back( std::format( "shadowCascade{}", c ) );
            }
        }

        // VIRT-GEOM: the main view's VG candidates ride phase 1 (AddFrameVirtualGeometry cuts them). A refusal is
        // logged once per message and the frame cuts nothing; the bins draw as before.
        VirtualGeometryCandidateTables vgTables;
        if ( auto made = MakeVirtualGeometryCandidates( graph, *meshes, *gpuScene, scene.GetValue().PrimitiveCount,
                                                        static_cast<uint32_t>( views.size() ) );
             made )
            vgTables = std::move( made.GetValue() );
        else if ( made.GetError() != m_VirtualGeometryLastError )
        {
            LOG_ERROR( "[SceneRenderer] {}: {}; no cluster cut this frame", m_ViewResources.GetName(),
                       made.GetError() );
            m_VirtualGeometryLastError = made.GetError();
        }

        const Common::ResultStr<GpuDriven::MainPhaseResult> culled = m_GpuCull.AddMainPhase(
             graph, scene.GetValue(), meshes->GetDrawCommandCache()->Passes(), upload.GetValue().Passes, views,
             textures.GraphRefs().System.Black, vgTables.Input );
        if ( !culled )
        {
            refuse( culled.GetError() );
            return;
        }
        for ( size_t v = 0; v < views.size(); ++v )
            meshes->SetFrameBinned( static_cast<GpuDriven::MeshPass>( views[v].View.Pass ), viewKeys[v],
                                    culled.GetValue().Draws[v], upload.GetValue(), scene.GetValue() );
        bins.Carry      = culled.GetValue().Carry;
        bins.Upload     = upload.GetValue();
        bins.Scene      = scene.GetValue();
        bins.VgCandidates      = culled.GetValue().VirtualGeometry;
        bins.VgVisibleCapacity = vgTables.VisibleCapacity;
        bins.VgInstances       = vgTables.Instances;
        // A forward main view has no phase 2 (AddFrameDrawBinsPostOcclusion runs on the deferred path only): its
        // counts are handed here.
        if ( mainPass != GpuDriven::MeshPass::GBuffer )
            m_DrawBinPhaseCounts.AddFrame( graph, frame.FrameIndex, bins.ViewNames, bins.Carry );
        m_FrameDrawBins = std::move( bins );
        m_DrawBinsLastError.clear();
    }

    void SceneRenderer::AddFrameDrawBinsPostOcclusion( RDG::Builder& graph, FrameTextures& textures,
                                                       System::MeshRenderer* meshRenderer, const ViewFrame& frame )
    {
        if ( !m_FrameDrawBins || meshRenderer == nullptr || meshRenderer->GetDrawCommandCache() == nullptr ||
             !m_GBuffer )
            return;
        FrameDrawBins& bins   = *m_FrameDrawBins;
        const auto     refuse = [this]( std::string message )
        {
            if ( message != m_DrawBinsLastError )
                LOG_ERROR( "[SceneRenderer] {}: GPUDriven: DrawBins phase 2: {}", m_ViewResources.GetName(), message );
            m_DrawBinsLastError = std::move( message );
        };

        // This frame's furthest HZB of the phase-1 depth: phase 2's occlusion input and next frame's phase 1.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto*                 hzbRenderer = UNIQUE_GET_AS( System::HZBRenderer, m_RenderSystems["HZBSystem"] );
        const bool            gbufferView =
             bins.Views.front().View.Pass == static_cast<uint32_t>( GpuDriven::MeshPass::GBuffer );
        const RDG::TextureRef depth     = textures.Depth( m_GBuffer, "GBuffer" );
        const auto            depthDesc = graph.GetTextureDesc( depth );
        const RDG::TextureRef furthest  = hzbRenderer != nullptr && depthDesc && gbufferView
                                               ? hzbRenderer->AddPasses( graph, depth, depthDesc.GetValue().Size.Width,
                                                                         depthDesc.GetValue().Size.Height,
                                                                         System::HZBKind::Furthest )
                                               : RDG::TextureRef{};
        if ( !furthest.IsValid() )
        {
            // No history either: next frame's phase 1 must not test an HZB no phase 2 follows.
            m_CullHzbHistory = {};
            if ( bins.MainTestsHzb )
                refuse( "this frame's furthest HZB could not be built; the instances phase 1 held back are not "
                        "drawn this frame" );
        }
        else
        {
            m_FrameFurthestHzb = furthest;
            if ( bins.MainTestsHzb )
            {
                bins.Views.front().Hzb = furthest;
                const Common::ResultStr<std::vector<GpuDriven::CulledDraws>> post = m_GpuCull.AddPostOcclusionPhase(
                     graph, bins.Scene, meshRenderer->GetDrawCommandCache()->Passes(), bins.Upload.Passes, bins.Views,
                     bins.Carry );
                if ( !post )
                    refuse( post.GetError() );
                else if ( meshRenderer->SetFramePostOcclusion( GpuDriven::MeshPass::GBuffer, bins.ViewKeys.front(),
                                                               post.GetValue().front() ) )
                    AddFrameGBufferPostOcclusion( graph, textures, meshRenderer );
            }
            graph.Extract( furthest, m_CullHzbHistory, RDG::Access::SampledCompute );
        }
        m_DrawBinPhaseCounts.AddFrame( graph, frame.FrameIndex, bins.ViewNames, bins.Carry );
    }

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

        // Refused while an upload is pending: this frame's tables are not what the CPU path culls. One import per
        // graph: AddFrameDrawBins' when it made one.
        const Common::ResultStr<GpuDriven::GpuSceneBuffers> scene =
             m_FrameGpuSceneTables ? Common::ResultStr<GpuDriven::GpuSceneBuffers>( *m_FrameGpuSceneTables )
                                   : gpuScene->ImportTables( graph );
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
        // The draw bins' HZBs when they built this frame's (AddFrameDrawBinsPostOcclusion): the history is
        // registered once per graph and extracted once.
        const bool binsHzb      = m_FrameFurthestHzb.IsValid();
        const bool prevHzbValid = binsHzb ? m_FramePrevHzb.IsValid()
                                          : m_CullHzbHistory.Physical != nullptr &&
                                                 m_CullHzbHistory.Desc.Size.Width == width &&
                                                 m_CullHzbHistory.Desc.Size.Height == height && frame.HistoryValid();
        {
            GpuDriven::CullViewInput mainInput =
                 MakeCullView( GpuDriven::MeshPass::GBuffer, frame.ViewProjection, frame.PrevViewProjection,
                               camera->GetFrustum(), lod, lensScale,
                               lodFlag | ( prevHzbValid ? GpuDriven::CullViewFlags::kTestHzb : 0u ) );
            if ( prevHzbValid )
            {
                mainInput.PrevHzb =
                     binsHzb ? m_FramePrevHzb : graph.RegisterExternal( m_CullHzbHistory, "GPUDriven.PrevHzb" );
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
        // drawn-page mask of this frame.
        if ( m_FrameVsmPages )
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
             m_GpuCull.AddMainPhase( graph, scene.GetValue(), tables, passes, views, neutralHzb,
                                     GpuDriven::VirtualGeometryCandidateInput{} ); // the validation cuts nothing
        if ( !mainPhase )
        {
            refuse( mainPhase.GetError() );
            return;
        }

        // This frame's furthest HZB of the CPU-drawn depth: phase 2's occlusion input and next frame's phase 1.
        const RDG::TextureRef furthest =
             binsHzb ? m_FrameFurthestHzb
                     : hzbRenderer->AddPasses( graph, depth, width, height, System::HZBKind::Furthest );
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
        if ( !binsHzb )
            graph.Extract( furthest, m_CullHzbHistory, RDG::Access::SampledCompute );
        RDG::BufferRef vsmDirtyMask{};
        if ( m_FrameVsmPages )
            vsmDirtyMask = m_FrameVsmPages->DirtyMask;
        m_GpuCullValidation.AddFrame( graph, frame.FrameIndex, *gpuScene, views, viewNames, mainPhase.GetValue().Carry,
                                      vsmDirtyMask );
        m_GpuCullLastError.clear();
    }
} // namespace Desert::Graphic

namespace Desert::Graphic
{
    void SceneRenderer::AddFrameVirtualGeometry( RDG::Builder& graph, const ViewFrame& frame )
    {
        m_FrameVirtualGeometry.reset();
        m_VirtualGeometryInstances = 0;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* meshes = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
        if ( !m_FrameDrawBins || !m_FrameDrawBins->VgCandidates.Rows.IsValid() || meshes == nullptr ||
             meshes->GetDrawCommandCache() == nullptr )
        {
            if ( m_ClusterCutCounts )
                m_ClusterCutCounts->Forget();
            return; // no VG instance drawn this frame
        }
        const FrameDrawBins& bins = *m_FrameDrawBins;
        if ( !m_VirtualGeometry )
            m_VirtualGeometry = std::make_unique<VirtualGeometry::VirtualGeometryRenderer>();
        if ( !m_ClusterCutCounts )
            m_ClusterCutCounts = std::make_unique<VirtualGeometry::ClusterCutCounts>();

        // The cut's one view: the draw bins' main view (cull view 0, cut view 0 of the candidates).
        const GpuDriven::CullViewInput&        main = bins.Views.front();
        const uint32_t                         pass = main.View.Pass;
        const VirtualGeometry::ClusterCullView view{ .Instance         = main,
                                                     .MaxPixelsPerEdge = m_VirtualGeometryMaxPixelsPerEdge,
                                                     .ViewWidth        = frame.Split.Render.Width,
                                                     .ViewHeight       = frame.Split.Render.Height };
        const VirtualGeometry::ClusterCandidates candidates{ .Rows            = bins.VgCandidates.Rows,
                                                             .Count           = bins.VgCandidates.Count,
                                                             .Capacity        = bins.VgCandidates.Capacity,
                                                             .VisibleCapacity = bins.VgVisibleCapacity };
        const auto culled = m_VirtualGeometry->AddClusterCull(
             graph, GpuDriven::CullPhase::Main, bins.Scene, VirtualGeometry::VirtualMeshTable::Get().Import( graph ),
             meshes->GetDrawCommandCache()->Passes()[pass], bins.Upload.Passes[pass], bins.Upload.SubmeshTransforms,
             candidates, std::span( &view, 1 ) );
        if ( !culled )
        {
            m_ClusterCutCounts->Forget();
            if ( culled.GetError() != m_VirtualGeometryLastError )
                LOG_ERROR( "[SceneRenderer] {}: {}; no cluster cut this frame", m_ViewResources.GetName(),
                           culled.GetError() );
            m_VirtualGeometryLastError = culled.GetError();
            return;
        }
        m_ClusterCutCounts->AddFrame( graph, frame.FrameIndex, culled.GetValue() );
        m_FrameVirtualGeometry     = FrameVirtualGeometry{ .VisibleClusters = culled.GetValue().VisibleClusters,
                                                           .RasterArgs      = culled.GetValue().RasterArgs,
                                                           .VisibleCapacity = culled.GetValue().VisibleCapacity,
                                                           .Pass            = pass };
        m_VirtualGeometryInstances = bins.VgInstances;
        m_VirtualGeometryLastError.clear();
    }

    SceneRenderer::VirtualGeometryCounts SceneRenderer::GetVirtualGeometryCounts() const
    {
        return { .Instances       = m_VirtualGeometryInstances,
                 .ClustersVisible = m_ClusterCutCounts ? m_ClusterCutCounts->LastCount().ClustersVisible : 0u };
    }
} // namespace Desert::Graphic
