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
#include <unordered_map>
#include <span>
#include <string>
#include <vector>

namespace Desert::Graphic
{
    namespace
    {
        // CullViewInput of one view: the matrices, frustum, LOD and distance-cull origins of the main camera, and
        // the flags; ViewIndex / FrustumPlanes are filled by InstanceCulling from the list position and Frustum. A
        // caster view also carries the shadow wind distance (MeshRenderer::GetShadowWindDistance, cm): its swaying
        // instances farther from the main camera cast their rest pose (GpuDriven::CastsRestPose).
        GpuDriven::CullViewInput MakeCullView( const GpuDriven::MeshPass pass, const glm::mat4& viewProjection,
                                               const glm::mat4& prevViewProjection, const Core::Frustum& frustum,
                                               const Geometry::LODView& lod, const float lensScale,
                                               const float shadowWindDistance, const uint32_t flags )
        {
            const bool               caster = ( flags & GpuDriven::CullViewFlags::kShadowCaster ) != 0u;
            GpuDriven::CullViewInput input{};
            input.View.ViewProjection     = viewProjection;
            input.View.PrevViewProjection = prevViewProjection;
            input.View.LodProjection      = lod.Projection;
            input.View.LodOrigin          = glm::vec4( lod.Origin, caster ? shadowWindDistance : 0.0f );
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
            // Per primitive with candidates: the submeshes whose DAG rows are appended (word i / 64), handed to
            // MeshRenderer::SetFrameClusterCandidates - the CPU walk's view of what InstanceCull takes out of the
            // bins (one source: the same spans as PrimitiveDags).
            std::unordered_map<GpuDriven::PrimitiveIndex, std::vector<uint64_t>> CandidateSubmeshes;
        };

        // From the static queue and the ISM queue: per primitive row, the DAG rows of every submesh one of its
        // queue entries shows (one entry per material slot: their union), appended for EVERY cull view of the draw
        // bins (the main view and the drawn shadow cascades), cull view v as cut view v. Capacity = instances x
        // DAGs, the rows of EACH cut view's region (InstanceCulling compacts the append per cut view);
        // VisibleCapacity = instances x the DAGs' ClusterCount, the bound of ONE view's cut
        // (AddFrameVirtualGeometry cuts each view in its own dispatch; a cut never draws more clusters than its
        // DAG holds), saturated to the cut's budget kMaxVisibleClustersPerCut. Refused when the candidate rows
        // pass the uint32 index range.
        Common::ResultStr<VirtualGeometryCandidateTables>
        MakeVirtualGeometryCandidates( RDG::Builder& graph, System::MeshRenderer& meshes,
                                       const GpuDriven::GpuScene& gpuScene, const uint32_t primitiveRows,
                                       const uint32_t viewCount )
        {
            using Result                                   = VirtualGeometryCandidateTables;
            const VirtualGeometry::VirtualMeshTable& table = VirtualGeometry::VirtualMeshTable::Get();
            std::map<uint32_t, std::vector<VirtualGeometry::VirtualMeshDagSpan>> byPrimitive;
            const auto addSpans = [&]( const uint32_t primitive, const Desert::Mesh* mesh, const uint64_t hidden )
            {
                if ( mesh == nullptr || primitive >= primitiveRows )
                    return;
                const auto* registration = mesh->GetVirtualMesh();
                if ( registration == nullptr || !*registration )
                    return;
                const size_t submeshes = mesh->GetSubmeshes().size();
                for ( const VirtualGeometry::VirtualMeshDagSpan& span :
                      table.DescribeUploaded( registration->Index() ) )
                {
                    if ( span.Submesh >= submeshes ||
                         ( span.Submesh < 64u && ( ( hidden >> span.Submesh ) & 1u ) != 0u ) )
                        continue;
                    auto& spans = byPrimitive[primitive];
                    if ( std::ranges::none_of( spans, [&span]( const VirtualGeometry::VirtualMeshDagSpan& known )
                                               { return known.Dag == span.Dag; } ) )
                        spans.push_back( span );
                }
            };
            for ( const auto& rd : meshes.GetStaticQueue() )
                addSpans( rd.GpuPrimitive, rd.Mesh, rd.HiddenSubmeshes );
            // THE ISM / FOLIAGE FIELDS (VIRT-GEOM step 23; UE: Nanite instanced static meshes): a field is one
            // primitive whose GpuScene instances are its instances, so each instance becomes one candidate per DAG
            // root (InstanceCull's append) and the cut places its clusters through the instance's transform. Only
            // a StaticMesh has DAG rows (a primitive / Foliage-built DynamicMesh has none).
            const auto fields     = meshes.GetInstancedQueue();
            const auto fieldPrims = meshes.GetFieldPrimitives();
            for ( size_t f = 0; f < fields.size() && f < fieldPrims.size(); ++f )
                if ( fields[f].Transforms && !fields[f].Transforms->empty() )
                    addSpans( fieldPrims[f], fields[f].Mesh, fields[f].HiddenSubmeshes );
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
                std::vector<uint64_t>& submeshWords = out.CandidateSubmeshes[primitive];
                for ( const VirtualGeometry::VirtualMeshDagSpan& span : spans )
                {
                    if ( submeshWords.size() <= span.Submesh / 64u )
                        submeshWords.resize( span.Submesh / 64u + 1u, uint64_t{ 0 } );
                    submeshWords[span.Submesh / 64u] |= uint64_t{ 1 } << ( span.Submesh % 64u );
                }
            }
            const uint32_t views = std::max( viewCount, 1u );
            if ( capacity * views > UINT32_MAX )
                return Common::MakeError<Result>( std::format(
                     "VirtualGeometry: {} drawn VG instances hold {} candidates, past the candidate-row "
                     "index range",
                     instancesTotal, capacity * views ) );
            if ( capacity == 0u || visible == 0u )
            {
                out.CandidateSubmeshes.clear(); // nothing appended: every instance stays in the bins
                return Common::MakeSuccess( std::move( out ) );
            }

            std::vector<uint32_t> cutViewOf( views, 0u );
            for ( uint32_t v = 0; v < views; ++v )
                cutViewOf[v] = v; // cull view v (main = 0, cascades after it: AddFrameDrawBins) is cut view v

            const auto upload = [&graph]( const std::string& name, const std::span<const std::byte> bytes )
            {
                const RDG::BufferRef buffer = graph.CreateBuffer( RDG::BufferDesc{ .Bytes = bytes.size() }, name );
                graph.QueueBufferUpload( buffer, bytes );
                return buffer;
            };
            out.Input.PrimitiveDags =
                 upload( "GPUDriven.VgPrimitiveDags", std::as_bytes( std::span( primitiveDags ) ) );
            out.Input.DagEntries    = upload( "GPUDriven.VgDagEntries", std::as_bytes( std::span( entries ) ) );
            out.Input.CutViewOf     = upload( "GPUDriven.VgCutViewOf", std::as_bytes( std::span( cutViewOf ) ) );
            out.Input.PrimitiveRows = primitiveRows;
            out.Input.Capacity      = static_cast<uint32_t>( capacity );
            out.Input.CutViews      = views;
            out.Instances           = static_cast<uint32_t>( std::min<uint64_t>( instancesTotal, UINT32_MAX ) );
            // The exact bound, past the cut's budget saturated to it (AddClusterCut caps each dispatch by
            // kMaxVisibleClustersPerCut and counts what does not fit).
            out.VisibleCapacity = static_cast<uint32_t>(
                 std::min<uint64_t>( visible, VirtualGeometry::kMaxVisibleClustersPerCut ) );
            return Common::MakeSuccess( std::move( out ) );
        }
    } // namespace

    void SceneRenderer::AddFrameDrawBins( RDG::Builder& graph, FrameTextures& textures, const ViewFrame& frame )
    {
        m_FrameGpuSceneTables.reset();
        m_FrameDrawBins.reset();
        m_FramePrevHzb     = {};
        m_FrameFurthestHzb = {};
        const auto refuse  = [this]( std::string message )
        {
            m_DrawBinPhaseCounts.Forget();
            if ( message != m_DrawBinsLastError )
                LOG_ERROR( "[SceneRenderer] {}: GPUDriven: DrawBins: {}; every pass draws on the CPU path",
                           m_ViewResources.GetName(), message );
            m_DrawBinsLastError = std::move( message );
        };
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto*                      meshes   = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
        const GpuDriven::GpuScene* gpuScene = meshes != nullptr ? meshes->GetGpuScene() : nullptr;
        const Core::Camera*        camera   = GetMainCamera();
        if ( meshes == nullptr || gpuScene == nullptr || camera == nullptr ||
             meshes->GetDrawCommandCache() == nullptr )
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

        const Geometry::LODView lod{ .Origin     = camera->GetPosition(),
                                     .Projection = camera->GetProjectionMatrix() };
        const float             lensScale    = Graphic::DrawDistanceScale( lod );
        const float             windDistance = meshes->GetShadowWindDistance(); // Shadows.WindAnimationDistance
        const uint32_t          lodFlag      = meshes->IsLODEnabled() ? 0u : GpuDriven::CullViewFlags::kLodDisabled;

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
            GpuDriven::CullViewInput mainInput = MakeCullView(
                 mainPass, frame.ViewProjection, frame.PrevViewProjection, camera->GetFrustum(), lod, lensScale,
                 windDistance, lodFlag | ( bins.MainTestsHzb ? GpuDriven::CullViewFlags::kTestHzb : 0u ) );
            if ( bins.MainTestsHzb )
            {
                m_FramePrevHzb    = graph.RegisterExternal( m_CullHzbHistory, "GPUDriven.PrevHzb" );
                mainInput.PrevHzb = m_FramePrevHzb;
                mainInput.View.HzbExtent =
                     glm::uvec4( hzbWidth, hzbHeight,
                                 static_cast<uint32_t>( std::bit_width( std::max( hzbWidth, hzbHeight ) ) ), 0u );
            }
            bins.Views.push_back( mainInput );
            bins.ViewKeys.push_back( System::MeshRenderer::kBinnedMainView );
            bins.ViewNames.emplace_back( mainPass == GpuDriven::MeshPass::GBuffer ? "gbuffer" : "forward" );
        }
        std::vector<GpuDriven::CullViewInput>& views    = bins.Views;
        std::vector<uint32_t>&                 viewKeys = bins.ViewKeys;
        // The deferred path's depth prepass for what the base pass does not draw ("Deferred: Forward
        // Depth/Normal", the generic programs' DepthNormal.Binned cells): its own view of the main camera, frustum
        // only - it is drawn once, before this frame's HZB exists, so it has no phase 2. Views.front() stays the
        // G-buffer view.
        if ( mainPass == GpuDriven::MeshPass::GBuffer &&
             !meshes->GetDrawCommandCache()->Pass( GpuDriven::MeshPass::DepthPrepass ).Bins.empty() )
        {
            views.push_back( MakeCullView( GpuDriven::MeshPass::DepthPrepass, frame.ViewProjection,
                                           frame.PrevViewProjection, camera->GetFrustum(), lod, lensScale,
                                           windDistance, lodFlag ) );
            viewKeys.push_back( System::MeshRenderer::kBinnedMainView );
            bins.ViewNames.emplace_back( "depthPrepass" );
        }
        if ( m_FrameCascades )
        {
            const uint32_t cascades = meshes->DirectionalShadows()[SceneResources::kDirectionalShadowSun].Count;
            // Each cascade in the cache's two layers (MeshRenderer::PlanCascadeCache): the static layer the frame
            // the cache is redrawn, the movable layer every frame it has movers, the same pass's bins split by
            // PrimitiveFlags::kMovableCaster. Frustum only: the static layer is redrawn when its matrix moved,
            // so last frame's cascade depth is no HZB of this one (UE's cached CSM culls its casters the same
            // way).
            const uint32_t casterFlags =
                 GpuDriven::CullViewFlags::kShadowCaster | GpuDriven::CullViewFlags::kOrthographic | lodFlag;
            for ( uint32_t c = 0; c < cascades; ++c )
            {
                const uint32_t view =
                     SceneResources::kDirectionalShadowSun * System::MeshRenderer::kMaxCascades + c;
                const glm::mat4& vp = meshes->GetCasterViewProjection( view );
                if ( meshes->CascadeBinsDrawn( view ) )
                {
                    views.push_back( MakeCullView( GpuDriven::MeshPass::ShadowCascade, vp, vp,
                                                   Core::Frustum( vp, glm::mat4( 1.0f ) ), lod, lensScale,
                                                   windDistance,
                                                   casterFlags | GpuDriven::CullViewFlags::kCasterStaticLayer ) );
                    viewKeys.push_back( view );
                    bins.ViewNames.push_back( std::format( "shadowCascade{}", c ) );
                }
                if ( meshes->CascadeMovableBinsDrawn( view ) )
                {
                    views.push_back( MakeCullView( GpuDriven::MeshPass::ShadowCascade, vp, vp,
                                                   Core::Frustum( vp, glm::mat4( 1.0f ) ), lod, lensScale,
                                                   windDistance,
                                                   casterFlags | GpuDriven::CullViewFlags::kCasterMovableLayer ) );
                    viewKeys.push_back( System::MeshRenderer::CascadeMovableView( view ) );
                    bins.ViewNames.push_back( std::format( "shadowCascade{}Movable", c ) );
                }
            }
        }

        // VIRT-GEOM: every view's VG candidates ride phase 1 (AddFrameVirtualGeometry cuts them). A refusal is
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

        bins.NeutralHzb = textures.GraphRefs().System.Black;
        const Common::ResultStr<GpuDriven::MainPhaseResult> culled =
             m_GpuCull.AddMainPhase( graph, scene.GetValue(), meshes->GetDrawCommandCache()->Passes(),
                                     upload.GetValue().Passes, views, bins.NeutralHzb, vgTables.Input );
        if ( !culled )
        {
            refuse( culled.GetError() );
            return;
        }
        bool mainIssued = false;
        for ( size_t v = 0; v < views.size(); ++v )
        {
            const bool issued =
                 meshes->SetFrameBinned( static_cast<GpuDriven::MeshPass>( views[v].View.Pass ), viewKeys[v],
                                         culled.GetValue().Draws[v], upload.GetValue(), scene.GetValue() );
            if ( v == 0u )
                mainIssued = issued;
        }
        // THE CLUSTERS' TWO-PASS DECISION (UE Nanite: the main pass tests last frame's HZB only when the post pass
        // runs), made here, before AddFrameVirtualGeometry's main cut, by the instances' rule plus what phase 2's
        // post cut needs on top of it:
        //  - MainTestsHzb: AddFrameDrawBinsPostOcclusion builds this frame's furthest HZB from the same depth, HZB
        //    pipeline and history this rule checked, and re-runs the instance cull with the program phase 1 just
        //    ensured - so the HZB is there and the instance phase 2 is accepted;
        //  - mainIssued: the main view issues its bins, the precondition of SetFramePostOcclusion (the phase-2
        //  slice
        //    the post cut's raster attaches to);
        //  - the post cut itself: its checks are the main cut's (same tables, pass, view, view key and carriers,
        //    MeshRenderer::SetFrameVirtualGeometry) plus this frame's HZB (the first point), so it is accepted
        //    whenever the main cut is; only DebugViewState::ForceVirtualGeometryPostCutRefusal refuses it.
        // A "no" leaves the main cut frustum-only (AddFrameVirtualGeometry): it holds nothing back, so a refused
        // post cut loses no cluster.
        const bool forcePostRefusal = m_DebugView.ForceVirtualGeometryPostCutRefusal;
        if ( forcePostRefusal && !m_VirtualGeometryPostRefusalLogged )
            LOG_WARN( "[SceneRenderer] {}: VirtualGeometry: the post-occlusion cluster cut refused (Debug 'Force "
                      "Virtual Geometry Post Cut Refusal'); the main cut tests no HZB and draws every visible "
                      "cluster in phase 1",
                      m_ViewResources.GetName() );
        m_VirtualGeometryPostRefusalLogged = forcePostRefusal;
        bins.ClusterMainTestsHzb           = bins.MainTestsHzb && mainIssued && !forcePostRefusal;
        // The primitives InstanceCull just took out of every cull view's bins (only when it appended candidates):
        // the CPU walk draws what of them no cluster raster draws (MeshRenderer::CpuWalkHidden).
        if ( vgTables.Input.PrimitiveDags.IsValid() )
            meshes->SetFrameClusterCandidates( std::move( vgTables.CandidateSubmeshes ) );
        bins.Carry             = culled.GetValue().Carry;
        bins.Upload            = upload.GetValue();
        bins.Scene             = scene.GetValue();
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
                LOG_ERROR( "[SceneRenderer] {}: GPUDriven: DrawBins phase 2: {}", m_ViewResources.GetName(),
                           message );
            m_DrawBinsLastError = std::move( message );
        };

        // This frame's furthest HZB of the phase-1 depth: phase 2's occlusion input and next frame's phase 1.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto*      hzbRenderer = UNIQUE_GET_AS( System::HZBRenderer, m_RenderSystems["HZBSystem"] );
        const bool gbufferView =
             bins.Views.front().View.Pass == static_cast<uint32_t>( GpuDriven::MeshPass::GBuffer );
        const RDG::TextureRef depth     = textures.Depth( m_GBuffer, "GBuffer" );
        const auto            depthDesc = graph.GetTextureDesc( depth );
        const RDG::TextureRef furthest =
             hzbRenderer != nullptr && depthDesc && gbufferView
                  ? hzbRenderer->AddPasses( graph, depth, depthDesc.GetValue().Size.Width,
                                            depthDesc.GetValue().Size.Height, System::HZBKind::Furthest )
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
                const Common::ResultStr<std::vector<GpuDriven::CulledDraws>> post =
                     m_GpuCull.AddPostOcclusionPhase( graph, bins.Scene,
                                                      meshRenderer->GetDrawCommandCache()->Passes(),
                                                      bins.Upload.Passes, bins.Views, bins.Carry );
                if ( !post )
                    refuse( post.GetError() );
                else if ( meshRenderer->SetFramePostOcclusion( GpuDriven::MeshPass::GBuffer, bins.ViewKeys.front(),
                                                               post.GetValue().front() ) )
                {
                    AddFrameVirtualGeometryPostOcclusion( graph, frame );
                    AddFrameGBufferPostOcclusion( graph, textures, meshRenderer );
                }
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
             [&]() -> Common::ResultStr<GpuDriven::GpuSceneBuffers>
        {
            if ( m_FrameGpuSceneTables )
                return Common::MakeSuccess( *m_FrameGpuSceneTables );
            return gpuScene->ImportTables( graph );
        }();
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
        const float             lensScale    = Graphic::DrawDistanceScale( lod );
        const float             windDistance = meshes->GetShadowWindDistance(); // Shadows.WindAnimationDistance
        const uint32_t          lodFlag      = meshes->IsLODEnabled() ? 0u : GpuDriven::CullViewFlags::kLodDisabled;

        std::vector<GpuDriven::CullViewInput> views;
        std::vector<std::string>              viewNames; // the validation log's name of each view

        // Main view: phase 1 against last frame's furthest HZB, only when it is this view's size and the view has
        // a previous frame (PrevViewProjection projects into it).
        // The draw bins' HZBs when they built this frame's (AddFrameDrawBinsPostOcclusion): the history is
        // registered once per graph and extracted once.
        const bool binsHzb = m_FrameFurthestHzb.IsValid();
        const bool prevHzbValid =
             binsHzb ? m_FramePrevHzb.IsValid()
                     : m_CullHzbHistory.Physical != nullptr && m_CullHzbHistory.Desc.Size.Width == width &&
                            m_CullHzbHistory.Desc.Size.Height == height && frame.HistoryValid();
        {
            GpuDriven::CullViewInput mainInput =
                 MakeCullView( GpuDriven::MeshPass::GBuffer, frame.ViewProjection, frame.PrevViewProjection,
                               camera->GetFrustum(), lod, lensScale, windDistance,
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
                                               windDistance,
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
                     GpuDriven::MeshPass::VirtualShadow, vp, vp, frustum, lod, lensScale, windDistance,
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
    namespace
    {
        // One view's cut of one phase, end to end: "Cluster Cut" -> "Raster Binning" on the view's mesh pass ->
        // attached to (pass, view key)'s issue of @p phase -> handed to @p counts with the groups the pass draws.
        // The cut's result is returned even when its raster could not be attached (logged by MeshRenderer).
        struct VirtualGeometryCutInput
        {
            const GpuDriven::GpuSceneBuffers&          Scene;
            const GpuDriven::DrawCommandBuffers&       Upload;
            const VirtualGeometry::VirtualMeshBuffers& Meshes;
            RDG::TextureRef                            NeutralHzb;
        };
        Common::ResultStr<VirtualGeometry::ClusterCullResult> AddVirtualGeometryCut(
             RDG::Builder& graph, const VirtualGeometry::VirtualGeometryRenderer& renderer,
             VirtualGeometry::ClusterCutCounts& counts, System::MeshRenderer& meshes,
             const VirtualGeometryCutInput& in, const VirtualGeometry::ClusterCandidates& candidates,
             const VirtualGeometry::ClusterCullView& view, const uint32_t cutView, const uint32_t viewKey,
             const GpuDriven::CullPhase phase, const VirtualGeometry::ClusterCutKind kind )
        {
            using Result                              = VirtualGeometry::ClusterCullResult;
            const uint32_t                  pass      = view.Instance.View.Pass;
            const GpuDriven::PassDrawTable& passTable = meshes.GetDrawCommandCache()->Passes()[pass];
            const auto                      culled    = renderer.AddClusterCull(
                 graph, phase, in.Scene, in.Meshes, passTable, in.Upload.Passes[pass], in.Upload.SubmeshTransforms,
                 candidates, std::span( &view, 1 ), cutView, in.NeutralHzb );
            if ( !culled )
                return Common::MakeError<Result>( culled.GetError() );
            // THE RASTER (VIRT-GEOM step 11/12): the cut's HW clusters sorted into the pass's material groups,
            // drawn by that pass's mesh list after its bins with each group's `.VirtualGeometry` cell into the
            // same targets and depth. The VG instances are in no bin of a view with a cut view
            // (InstanceCull.shader).
            const auto rasterBins = renderer.AddRasterBinning( graph, in.Scene, in.Meshes, passTable,
                                                               in.Upload.Passes[pass], culled.GetValue() );
            if ( !rasterBins )
            {
                counts.Add( graph, kind, culled.GetValue(), nullptr, {} );
                return Common::MakeError<Result>(
                     std::format( "{}; the cut's clusters are not drawn this frame", rasterBins.GetError() ) );
            }
            const VirtualGeometry::RasterBins& bins = rasterBins.GetValue();
            const bool                         attached =
                 meshes.SetFrameVirtualGeometry( static_cast<GpuDriven::MeshPass>( pass ), viewKey, phase,
                                                 System::MeshRendererDetail::VirtualGeometryIssue{
                                                      .GroupArgs       = bins.GroupArgs,
                                                      .Rows            = bins.Rows,
                                                      .VisibleClusters = bins.VisibleClusters,
                                                      .Clusters        = in.Meshes.Clusters,
                                                      .Dags            = in.Meshes.Dags,
                                                      .PrimitiveDraws  = in.Upload.Passes[pass].PrimitiveDraws,
                                                      .GroupCount      = bins.GroupCount } );
            // The groups the pass draws (SetFrameVirtualGeometry made a carrier for each of them, or refused).
            std::vector<uint8_t> drawn;
            if ( attached )
            {
                drawn.reserve( passTable.Groups.size() );
                for ( const GpuDriven::IndirectGroup& group : passTable.Groups )
                    drawn.push_back( static_cast<uint8_t>( group.BinCount != 0u && group.Pipeline != nullptr &&
                                                           group.VirtualGeometryPipeline != nullptr ) );
            }
            counts.Add( graph, kind, culled.GetValue(), attached ? &bins : nullptr, std::move( drawn ) );
            return culled;
        }
    } // namespace

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
        m_ClusterCutCounts->BeginFrame( frame.FrameIndex );

        // One cut per cull view of the draw bins (cull view v = cut view v): the main view, then each drawn
        // shadow cascade. Each cut selects LOD in ITS OWN pixels (UE: shadow LOD by the shadow view's
        // resolution): the main view through its perspective LodProjection, a cascade by its own world size per
        // texel (orthographic). The cascades' instance bins keep the main camera's mesh LOD (MakeCullView).
        const VirtualGeometry::ClusterCandidates candidates{
             .Rows            = bins.VgCandidates.Rows,
             .Count           = bins.VgCandidates.Count,
             .Capacity        = bins.VgCandidates.Capacity,
             .VisibleCapacity = bins.VgVisibleCapacity,
             .Layout          = VirtualGeometry::ClusterCandidateLayout::PerCutView,
             .CutViews        = bins.VgCandidates.CutViews };
        const VirtualGeometry::VirtualMeshBuffers vgMeshes =
             VirtualGeometry::VirtualMeshTable::Get().Import( graph );
        const VirtualGeometryCutInput in{
             .Scene = bins.Scene, .Upload = bins.Upload, .Meshes = vgMeshes, .NeutralHzb = bins.NeutralHzb };
        const SceneResources::DirectionalShadowViews shadows = meshes->DirectionalShadows();
        const SceneResources::DirectionalShadowView& sun     = shadows[SceneResources::kDirectionalShadowSun];
        const auto viewOf = [&]( const size_t v ) -> Common::ResultStr<VirtualGeometry::ClusterCullView>
        {
            using Result = VirtualGeometry::ClusterCullView;
            if ( v == 0u )
                return Common::MakeSuccess(
                     Result{ .Instance         = bins.Views[v],
                             .MaxPixelsPerEdge = m_VirtualGeometryMaxPixelsPerEdge,
                             .Lod              = VirtualGeometry::ClusterLodMeasure::Perspective(
                                  frame.Split.Render.Height, bins.Views[v].View.LodProjection ) } );
            // A cascade's view key is kDirectionalShadowSun x kMaxCascades + its cascade (AddFrameDrawBins).
            const uint32_t key   = bins.ViewKeys[v];
            const uint32_t first = SceneResources::kDirectionalShadowSun * System::MeshRenderer::kMaxCascades;
            if ( key < first || key - first >= System::MeshRenderer::kMaxCascades )
                return Common::MakeError<Result>(
                     std::format( "VirtualGeometry: cut view {} (key {}) is not a sun cascade", v, key ) );
            const float worldPerTexel = sun.CascadeWorldPerTexel[static_cast<glm::length_t>( key - first )];
            return Common::MakeSuccess(
                 Result{ .Instance         = bins.Views[v],
                         .MaxPixelsPerEdge = m_VirtualGeometryMaxPixelsPerEdge,
                         .Lod = VirtualGeometry::ClusterLodMeasure::OrthographicTexel( worldPerTexel ) } );
        };
        const auto logOnce = [this]( const std::string& message )
        {
            if ( message != m_VirtualGeometryLastError )
                LOG_ERROR( "[SceneRenderer] {}: {}", m_ViewResources.GetName(), message );
            m_VirtualGeometryLastError = message;
        };
        // DebugViewState::ForceVirtualGeometryCutRefusal: every cut of the frame refused AFTER InstanceCull took
        // the candidates out of the bins - the state a refused cut leaves, so the CPU walk's fallback
        // (MeshRenderer::CpuWalkHidden) is what draws the VG primitives ([vg_refused_fallback]).
        if ( m_DebugView.ForceVirtualGeometryCutRefusal )
        {
            logOnce(
                 "VirtualGeometry: every cluster cut refused (Debug 'Force Virtual Geometry Cut Refusal'); the VG "
                 "primitives are drawn by their own submeshes" );
            return;
        }
        // Shadow cascades: the cascade's static layer draws the clusters (only drawn cascades are cull views).
        bool cascadesCut = true;
        for ( size_t v = 1; v < bins.Views.size(); ++v )
        {
            const auto cascadeView = viewOf( v );
            if ( !cascadeView )
            {
                logOnce( std::format( "{} ({})", cascadeView.GetError(), bins.ViewNames[v] ) );
                cascadesCut = false;
                continue;
            }
            const auto cascade = AddVirtualGeometryCut(
                 graph, *m_VirtualGeometry, *m_ClusterCutCounts, *meshes, in, candidates, cascadeView.GetValue(),
                 static_cast<uint32_t>( v ), bins.ViewKeys[v], GpuDriven::CullPhase::Main,
                 VirtualGeometry::ClusterCutKind::ShadowCascade );
            if ( !cascade )
            {
                logOnce( std::format( "{} ({})", cascade.GetError(), bins.ViewNames[v] ) );
                cascadesCut = false;
            }
        }

        // The main view never refuses. It tests last frame's HZB (Views[0] carries it when MainTestsHzb) only when
        // its post cut runs (FrameDrawBins::ClusterMainTestsHzb, decided before this cut); otherwise frustum only:
        // the cut carries nothing (VirtualGeometryRenderer::AddClusterCull: no HZB -> no carry) and
        // AddFrameVirtualGeometryPostOcclusion has nothing to re-cut.
        const auto                       mainView = viewOf( 0 );
        VirtualGeometry::ClusterCullView view     = mainView.GetValue();
        if ( !bins.ClusterMainTestsHzb )
        {
            view.Instance.PrevHzb = RDG::TextureRef{};
            view.Instance.View.Flags &= ~GpuDriven::CullViewFlags::kTestHzb;
        }
        const uint32_t pass = view.Instance.View.Pass;
        const auto     culled =
             AddVirtualGeometryCut( graph, *m_VirtualGeometry, *m_ClusterCutCounts, *meshes, in, candidates, view,
                                    0u, System::MeshRenderer::kBinnedMainView, GpuDriven::CullPhase::Main,
                                    VirtualGeometry::ClusterCutKind::Main );
        if ( !culled )
        {
            logOnce( culled.GetError() );
            return;
        }
        m_FrameVirtualGeometry     = FrameVirtualGeometry{ .VisibleClusters    = culled.GetValue().VisibleClusters,
                                                           .RasterArgs         = culled.GetValue().RasterArgs,
                                                           .VisibleCapacity    = culled.GetValue().VisibleCapacity,
                                                           .Pass               = pass,
                                                           .OccludedCandidates = culled.GetValue().OccludedCandidates,
                                                           .OccludedCount      = culled.GetValue().OccludedCount };
        m_VirtualGeometryInstances = bins.VgInstances;
        if ( cascadesCut )
            m_VirtualGeometryLastError.clear();
    }

    void SceneRenderer::AddFrameVirtualGeometryPostOcclusion( RDG::Builder& graph, const ViewFrame& frame )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* meshes = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
        if ( !m_FrameVirtualGeometry || !m_FrameVirtualGeometry->OccludedCandidates.IsValid() ||
             !m_FrameDrawBins || !m_VirtualGeometry || !m_ClusterCutCounts || meshes == nullptr ||
             meshes->GetDrawCommandCache() == nullptr )
            return; // the main cut tested no HZB (FrameDrawBins::ClusterMainTestsHzb false): nothing was held back
        const FrameDrawBins& bins = *m_FrameDrawBins;
        // The main phase's carry, re-cut against this frame's HZB (AddFrameDrawBinsPostOcclusion set the main
        // view's Hzb) with the main cut's view.
        const VirtualGeometry::ClusterCandidates candidates{
             .Rows            = m_FrameVirtualGeometry->OccludedCandidates,
             .Count           = m_FrameVirtualGeometry->OccludedCount,
             .Capacity        = m_FrameVirtualGeometry->VisibleCapacity,
             .VisibleCapacity = m_FrameVirtualGeometry->VisibleCapacity,
             .Layout          = VirtualGeometry::ClusterCandidateLayout::PostCarry,
             .CutViews        = 1u };
        const VirtualGeometry::VirtualMeshBuffers vgMeshes =
             VirtualGeometry::VirtualMeshTable::Get().Import( graph );
        const VirtualGeometry::ClusterCullView view{
             .Instance         = bins.Views.front(),
             .MaxPixelsPerEdge = m_VirtualGeometryMaxPixelsPerEdge,
             .Lod              = VirtualGeometry::ClusterLodMeasure::Perspective( frame.Split.Render.Height,
                                                                                  bins.Views.front().View.LodProjection ) };
        const auto post = AddVirtualGeometryCut(
             graph, *m_VirtualGeometry, *m_ClusterCutCounts, *meshes,
             VirtualGeometryCutInput{
                  .Scene = bins.Scene, .Upload = bins.Upload, .Meshes = vgMeshes, .NeutralHzb = bins.NeutralHzb },
             candidates, view, 0u, bins.ViewKeys.front(), GpuDriven::CullPhase::PostOcclusion,
             VirtualGeometry::ClusterCutKind::PostOcclusion );
        // Unreachable by the two-pass decision (FrameDrawBins::ClusterMainTestsHzb accepts the post cut before the
        // main cut carries anything): a refusal here is a broken contract between the two, said as an error.
        if ( !post && post.GetError() != m_VirtualGeometryLastError )
        {
            LOG_ERROR( "[SceneRenderer] {}: {}: the post cut refused after the two-pass decision accepted it; the "
                       "clusters the main cut held back are not drawn this frame",
                       m_ViewResources.GetName(), post.GetError() );
            m_VirtualGeometryLastError = post.GetError();
        }
    }

    void SceneRenderer::AddFrameVirtualGeometryShadowPages( RDG::Builder&                        graph,
                                                            const System::VirtualShadowMapFrame& pages )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto*               meshes = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
        const Core::Camera* camera = GetMainCamera();
        // The VG candidates exist only when the draw bins appended some this frame (a VG mesh in the queue) and
        // AddFrameVirtualGeometry began this frame's counts.
        if ( !m_FrameDrawBins || !m_FrameDrawBins->VgCandidates.Rows.IsValid() || meshes == nullptr ||
             camera == nullptr || meshes->GetDrawCommandCache() == nullptr || meshes->GetGpuScene() == nullptr ||
             !m_VirtualGeometry || !m_ClusterCutCounts )
            return;
        // Only the sun's clipmap levels take cluster cuts; without a sun this frame they are not drawn.
        if ( !pages.HasSun )
            return;
        const FrameDrawBins& bins    = *m_FrameDrawBins;
        const auto           logOnce = [this]( const std::string& message )
        {
            if ( message != m_VirtualGeometryLastError )
                LOG_ERROR( "[SceneRenderer] {}: {}; the virtual shadow map draws its virtual geometry on the CPU "
                           "walk this frame",
                           m_ViewResources.GetName(), message );
            m_VirtualGeometryLastError = message;
        };
        if ( m_DebugView.ForceVirtualGeometryCutRefusal )
        {
            logOnce( "VSM levels: the cluster cut refused (Debug 'Force Virtual Geometry Cut Refusal')" );
            return;
        }

        // THE LEVELS AS CULL VIEWS (the validation path's VSM views, AddFrameGpuCull): the far plane open, as the
        // casters' cull, and the level's drawn-page mask; cull view = cut view = level.
        const Geometry::LODView lod{ .Origin     = camera->GetPosition(),
                                     .Projection = camera->GetProjectionMatrix() };
        const float             lensScale    = Graphic::DrawDistanceScale( lod );
        const float             windDistance = meshes->GetShadowWindDistance(); // Shadows.WindAnimationDistance
        const uint32_t          lodFlag      = meshes->IsLODEnabled() ? 0u : GpuDriven::CullViewFlags::kLodDisabled;
        std::vector<GpuDriven::CullViewInput> views;
        views.reserve( System::kVsmClipmapLevelCount );
        for ( uint32_t level = 0; level < System::kVsmClipmapLevelCount; ++level )
        {
            const glm::mat4& vp = pages.LevelViewProjection[level];
            Core::Frustum    frustum( vp, glm::mat4( 1.0f ) );
            frustum.Open( Core::PLANE_FAR );
            GpuDriven::CullViewInput input =
                 MakeCullView( GpuDriven::MeshPass::VirtualShadow, vp, vp, frustum, lod, lensScale, windDistance,
                               GpuDriven::CullViewFlags::kShadowCaster | GpuDriven::CullViewFlags::kOrthographic |
                                    GpuDriven::CullViewFlags::kVsmPageMask | lodFlag );
            input.View.VsmLevel = level;
            input.VsmDirtyMask  = pages.DirtyMask;
            views.push_back( input );
        }

        // The levels' VG candidates: one more instance cull over the levels, appending only: no table is handed
        // (the VirtualShadow bins are not issued - the levels' CPU walk draws the other casters - so culling them
        // would be work nobody reads).
        auto vgTables =
             MakeVirtualGeometryCandidates( graph, *meshes, *meshes->GetGpuScene(), bins.Scene.PrimitiveCount,
                                            static_cast<uint32_t>( views.size() ) );
        if ( !vgTables )
        {
            logOnce( vgTables.GetError() );
            return;
        }
        if ( !vgTables.GetValue().Input.PrimitiveDags.IsValid() )
            return; // no VG instance casts
        const Common::ResultStr<GpuDriven::MainPhaseResult> culled = m_GpuCull.AddMainPhase(
             graph, bins.Scene, std::array<GpuDriven::PassDrawTable, GpuDriven::kMeshPassCount>{},
             std::array<GpuDriven::PassDrawBuffers, GpuDriven::kMeshPassCount>{}, views, bins.NeutralHzb,
             vgTables.GetValue().Input );
        if ( !culled )
        {
            logOnce( std::format( "VSM levels: {}", culled.GetError() ) );
            return;
        }
        const GpuDriven::VirtualGeometryCandidateOutput& appended = culled.GetValue().VirtualGeometry;
        const VirtualGeometry::ClusterCandidates         candidates{
                     .Rows     = appended.Rows,
                     .Count    = appended.Count,
                     .Capacity = appended.Capacity,
             // One cut of every level: each level may emit every
             // cluster its candidates' DAGs hold, within the one
             // dispatch's budget (kMaxVisibleClustersPerCut, UE
             // MaxVisibleClusters; past it counted as overflow).
                     .VisibleCapacity = static_cast<uint32_t>( std::min<uint64_t>(
                  uint64_t{ vgTables.GetValue().VisibleCapacity } * System::kVsmClipmapLevelCount,
                  VirtualGeometry::kMaxVisibleClustersPerCut ) ),
                     .Layout          = VirtualGeometry::ClusterCandidateLayout::PerCutView,
                     .CutViews        = appended.CutViews };
        const VirtualGeometry::VirtualMeshBuffers vgMeshes =
             VirtualGeometry::VirtualMeshTable::Get().Import( graph );

        // ONE CUT OF EVERY LEVEL (UE: Nanite culls all the VSM views in one pass, the view per cluster): cut view
        // = level, each at its own texel (shadow LOD by the shadow view's resolution); binned into the VSM's own
        // table's groups (MeshPass::VirtualShadow) and rastered by ONE "VSM: Raster VirtualGeometry"
        // (MeshRenderer::SetVirtualShadowGeometry).
        constexpr GpuDriven::MeshPass   kTablePass = GpuDriven::MeshPass::VirtualShadow;
        const auto                      tableIndex = static_cast<size_t>( kTablePass );
        const GpuDriven::PassDrawTable& passTable  = meshes->GetDrawCommandCache()->Passes()[tableIndex];
        std::vector<VirtualGeometry::ClusterCullView> cutViews;
        cutViews.reserve( views.size() );
        for ( uint32_t level = 0; level < System::kVsmClipmapLevelCount; ++level )
            cutViews.push_back( { .Instance         = views[level],
                                  .MaxPixelsPerEdge = m_VirtualGeometryMaxPixelsPerEdge,
                                  .Lod              = VirtualGeometry::ClusterLodMeasure::OrthographicTexel(
                                       pages.LevelTexelWorld[level] ) } );
        const auto cut = m_VirtualGeometry->AddClusterCull(
             graph, GpuDriven::CullPhase::Main, bins.Scene, vgMeshes, passTable, bins.Upload.Passes[tableIndex],
             bins.Upload.SubmeshTransforms, candidates, cutViews, 0u, bins.NeutralHzb );
        if ( !cut )
        {
            logOnce( std::format( "VSM levels: {}", cut.GetError() ) );
            return;
        }
        const auto rasterBins = m_VirtualGeometry->AddRasterBinning(
             graph, bins.Scene, vgMeshes, passTable, bins.Upload.Passes[tableIndex], cut.GetValue(),
             System::kVsmClipmapLevelCount );
        if ( !rasterBins )
        {
            m_ClusterCutCounts->Add( graph, VirtualGeometry::ClusterCutKind::VirtualShadow, cut.GetValue(),
                                     nullptr, {} );
            logOnce( std::format( "VSM levels: {}", rasterBins.GetError() ) );
            return;
        }
        const VirtualGeometry::RasterBins& raster = rasterBins.GetValue();
        auto                               drawn  = meshes->SetVirtualShadowGeometry(
             System::MeshRendererDetail::VirtualGeometryIssue{ .GroupArgs = raster.GroupArgs,
                                                                                              .Rows = raster.Rows,
                                                                                              .VisibleClusters = raster.VisibleClusters,
                                                                                              .Clusters = vgMeshes.Clusters,
                                                                                              .Dags = vgMeshes.Dags,
                                                                                              .PrimitiveDraws =
                                                                    bins.Upload.Passes[tableIndex].PrimitiveDraws,
                                                                                              .GroupCount = raster.GroupCount },
             bins.Upload, bins.Scene );
        if ( !drawn )
        {
            m_ClusterCutCounts->Add( graph, VirtualGeometry::ClusterCutKind::VirtualShadow, cut.GetValue(),
                                     nullptr, {} );
            logOnce( std::format( "VSM levels: {}", drawn.GetError() ) );
            return;
        }
        m_ClusterCutCounts->Add( graph, VirtualGeometry::ClusterCutKind::VirtualShadow, cut.GetValue(), &raster,
                                 std::move( drawn.GetValue() ) );
    }

    SceneRenderer::VirtualGeometryCounts SceneRenderer::GetVirtualGeometryCounts() const
    {
        const auto                  meshSystem = m_RenderSystems.find( "MeshSystem" );
        const System::MeshRenderer* meshes =
             meshSystem == m_RenderSystems.end()
                  ? nullptr
                  : static_cast<const System::MeshRenderer*>( meshSystem->second.get() );
        const uint32_t refused  = meshes != nullptr ? meshes->VsmRefusedGroups() : 0u;
        const uint32_t fallback = meshes != nullptr ? meshes->VsmFallbackSubmeshes() : 0u;
        if ( !m_ClusterCutCounts )
            return { .Instances            = m_VirtualGeometryInstances,
                     .VsmRefusedGroups     = refused,
                     .VsmFallbackSubmeshes = fallback };
        const VirtualGeometry::ClusterCutCount& last = m_ClusterCutCounts->LastCount();
        return { .Instances            = m_VirtualGeometryInstances,
                 .ClustersVisible      = last.ClustersVisible,
                 .ClustersDrawn        = last.ClustersDrawn,
                 .ShadowClustersDrawn  = last.ShadowClustersDrawn,
                 .VsmClustersDrawn     = last.VsmClustersDrawn,
                 .VsmRefusedGroups     = refused,
                 .VsmFallbackSubmeshes = fallback,
                 .Overflow = last.Status.StackOverflow + last.Status.VisibleOverflow + last.Status.MissingSlot,
                 .OccludedClusters  = last.Status.OccludedClusters,
                 .BinnedVgInstances = last.Status.BinnedVgInstances };
    }
} // namespace Desert::Graphic
