#include "MeshRendererInternal.hpp"

#include <Engine/Graphic/Materials/Mesh/MaterialShadow.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>

#include <algorithm>
#include <unordered_set>

// GPU-INSTANCED SECTIONS (GpuInstances.hpp): the landscape grass. The transforms and the instance count are the
// producer's compute output, so nothing here culls, sorts or uploads instances: each section is one indirect draw
// through its material's (Instanced x pass) cell. The cell is a COPY of the material (MaterialService::
// GetViewVariant, keyed per section and owned by this renderer, so it dies with the view) because a material owns one InstanceTransforms binding per frame and the
// CPU-batched ISMs of the same material fill the shared one.

namespace Desert::Graphic::System
{
    using namespace MeshRendererDetail;

    namespace
    {
        // View keys apart from the cascades' (0..kMaxCascades-1) and the cube faces' the masked casters use.
        constexpr uint32_t kGpuInstancedViewBase = 0x40000000u;

        const DataDrivenMaterial* SectionTemplate( const MaterialInstancePtr& instance )
        {
            return instance ? dynamic_cast<const DataDrivenMaterial*>( instance->GetParentMaterial() ) : nullptr;
        }

        // The LOD levels a GPU-instanced section is drawn in (MeshRenderer::GpuInstancedRenderData::Levels): a
        // blade strip's levels are its submeshes, drawn by one draw.
        uint32_t LevelsOf( const MeshRenderer::GpuInstancedRenderData& gpu )
        {
            return gpu.Path == MeshVertexPath::Instanced ? std::max( 1u, gpu.Levels ) : 1u;
        }

        // Where level @p level's records begin: one record per submesh per level (GpuInstances.hpp).
        uint64_t LevelArgsOffset( const MeshRenderer::GpuInstancedRenderData& gpu, uint32_t level )
        {
            return GpuInstanceArgsOffset( level * static_cast<uint32_t>( gpu.Mesh->GetSubmeshes().size() ) );
        }

        // GRASS-VIS: view keys of the post-occlusion draws' instances, apart from phase 1's of the same cell view.
        constexpr uint32_t kGpuInstancedPostViewOffset = 0x04000000u;

        // The producer's buffers into the cell: @p instances (the section's full list, or the camera's), and a
        // blade's shape (MeshPathOwnBinding).
        void BindGpuInstancedBuffers( DataDrivenMaterial& cell, const MeshRenderer::GpuInstancedRenderData& gpu,
                                      const std::shared_ptr<ShaderResources::StorageBuffer>& instances )
        {
            if ( auto* sb = cell.Get<StorageBufferProperty>( kInstanceTransformsBlock ) )
                sb->SetBuffer( instances );
            if ( gpu.Path == MeshVertexPath::GrassBlade )
                if ( auto* sb = cell.Get<StorageBufferProperty>( kGrassBladeShapeBlock ) )
                    sb->SetBuffer( gpu.BladeShape );
        }
    } // namespace

    MeshRenderer::~MeshRenderer()
    {
        // The section copies hold this view's buffers (the producers' instances, the Materials rows written
        // under the view): their instances first, then the copies, before the view's ledger check.
        m_GpuInstancedInstances.clear();
        m_GpuInstancedQueue.clear();
        if ( auto* materials = Runtime::ResourceRegistry::GetMaterialService() )
            materials->ReleaseViewVariants( this );
    }

    Core::ShaderVariant MeshRenderer::SurfaceDepthEqualVariant()
    {
        Core::ShaderVariant permutation;
        permutation.Defines.emplace_back( kSurfaceDepthEqualDefine );
        permutation.ShippedByCook = true;
        return permutation;
    }

    void MeshRenderer::BuildGpuInstancedDraws( MeshDrawList& list, GraphicsPipeline* instancedPipeline,
                                               const MeshPass pass, const SceneFrameBinding& frameState,
                                               const StaticPassCells* companion, const bool postOcclusion )
    {
        if ( m_GpuInstancedQueue.empty() || instancedPipeline == nullptr )
            return;
        auto* materials = Runtime::ResourceRegistry::GetMaterialService();
        if ( materials == nullptr )
            return;
        if ( materials->GetInvalidationVersion() != m_GpuInstancedStamp )
        {
            m_GpuInstancedInstances.clear();
            m_GpuInstancedStamp = materials->GetInvalidationVersion();
        }

        // A section with a box is drawn only where the view's frustum meets it (GpuInstancedRenderData::WorldBounds).
        const auto*                        camera = m_SceneRenderer != nullptr ? m_SceneRenderer->GetMainCamera() : nullptr;
        const std::optional<Core::Frustum> frustum =
             camera != nullptr ? std::optional<Core::Frustum>( camera->GetFrustum() ) : std::nullopt;

        for ( const GpuInstancedRenderData& gpu : m_GpuInstancedQueue )
        {
            if ( gpu.WorldBounds && frustum && !frustum->Intersects( *gpu.WorldBounds ) )
                continue;
            // OCCLUSION PER VIEW (GRASS-VIS): the camera passes draw the camera's list when the producer made one
            // (GpuInstancedRenderData::CameraInstances); the post-occlusion passes draw only phase 2's records of it.
            const bool cameraList = gpu.CameraInstances != nullptr && gpu.CameraRefs != nullptr;
            if ( postOcclusion && ( !cameraList || gpu.PostRefs == nullptr ) )
                continue;
            const GpuInstanceRefs& refs     = cameraList ? *gpu.CameraRefs : *gpu.Refs;
            const RDG::BufferRef   drawArgs = postOcclusion ? gpu.PostRefs->Args : refs.Args;
            const DataDrivenMaterial* parent = SectionTemplate( gpu.Material );
            if ( materials->AwaitsTextures( parent ) )
                continue; // a texture in flight: drawn from the frame its material is rebuilt bound (GRASS-SETTLE)
            // The deferred early-Z pass (the companion-less Depth call, DeclareMaskedDepthDraws) draws the sections
            // this frame's EarlyZPass admits (EarlyZAdmits: the Masked ones, or under All every opaque one too); the
            // G-buffer draws a section it drew with an EQUAL depth test, without writes and without the clip
            // (m_InstancedGBufferEqualPipeline, its cell in SurfaceDepthEqualVariant: UE's base pass after the
            // early-Z pass). The forward depth prepass (Depth with a companion) draws every one.
            const bool maskedPrepass = pass == MeshPass::Depth && companion == nullptr;
            // GRASS-VIS: a blade section this frame draws through the visibility buffer (m_GrassVisibilitySections,
            // chosen by DeclareMaskedDepthDraws) is in the Visibility raster and its resolve and nowhere else of the
            // deferred geometry: not in the masked prepass, not in the G-buffer. Every other section is in neither.
            const bool visibilityPass = pass == MeshPass::Visibility || pass == MeshPass::VisibilityResolve;
            const bool viaVisibility  = gpu.Path == MeshVertexPath::GrassBlade &&
                                       std::find( m_GrassVisibilitySections.begin(), m_GrassVisibilitySections.end(),
                                                  gpu.Key ) != m_GrassVisibilitySections.end();
            if ( visibilityPass && !viaVisibility )
                continue;
            if ( viaVisibility && ( maskedPrepass || pass == MeshPass::GBuffer ) )
                continue;
            if ( maskedPrepass && !EarlyZAdmits( parent ) )
                continue;
            // A post-occlusion draw was never in the early-Z pass: it keeps its own clip and depth writes.
            const bool prepassed = !postOcclusion && pass == MeshPass::GBuffer &&
                                   std::find( m_MaskedPrepassed.begin(), m_MaskedPrepassed.end(), gpu.Key ) !=
                                        m_MaskedPrepassed.end();
            // One cell (and instance) per section x view pass.
            const auto viewOf = [&gpu]( const MeshPass of )
            { return kGpuInstancedViewBase + gpu.Key * kMeshPassCount + static_cast<uint32_t>( of ); };
            // This view's copy of the section's (path x pass) cell; the Forward one in its virtual-shadow
            // permutation (ForwardVirtualCell: a blade or instance under a local light reads the VSM).
            const auto cellOf = [&]( const MeshPass of ) -> DataDrivenMaterial*
            {
                if ( parent == nullptr || !materials->Owns( parent ) )
                    return nullptr;
                return ForwardVirtualCell( materials->GetViewVariant( parent, gpu.Path, of, viewOf( of ), this ),
                                           parent, gpu.Path, of, viewOf( of ) );
            };
            // The G-buffer cell of a section the masked prepass drew: the clip-less equal-depth permutation.
            const auto depthEqualCell = [&]() -> DataDrivenMaterial*
            {
                if ( parent == nullptr || !materials->Owns( parent ) )
                    return nullptr;
                return materials->GetViewVariant( parent, gpu.Path, MeshPass::GBuffer, viewOf( MeshPass::GBuffer ),
                                                  SurfaceDepthEqualVariant(), this );
            };
            const uint32_t      view = viewOf( pass );
            DataDrivenMaterial* cell = prepassed ? depthEqualCell() : cellOf( pass );
            if ( cell == nullptr )
            {
                static std::unordered_set<const DataDrivenMaterial*> s_Refused;
                if ( s_Refused.insert( parent ).second )
                    LOG_ERROR( "[MeshRenderer] a GPU-instanced section's material has no ({} x {}) cell; the "
                               "section is not drawn (MaterialService named the cell it refused).",
                               MeshVertexPathName( gpu.Path ), MeshPassName( pass ) );
                continue;
            }
            // The resolve's full-screen triangle is never culled (its carrier is CullMode None).
            GraphicsPipeline* pipeline = CullPermutation(
                 CellPipeline( prepassed ? m_InstancedGBufferEqualPipeline.get() : instancedPipeline, *cell ),
                 gpu.Material->IsTwoSided() || pass == MeshPass::VisibilityResolve );
            if ( pipeline == nullptr || pipeline->GetReadiness() != PipelineReadiness::Ready )
                continue; // still in the driver: drawn from the frame it is Ready (RDG-PSO)
            if ( maskedPrepass )
            {
                // Paired like the forward prepass: the masked prepass draws a section only when its G-buffer half
                // (the equal-depth cell's pipeline) is Ready too, since that half shades exactly the texels whose
                // depth this pass wrote and nothing else would. Until then the G-buffer draws it with its own clip
                // and depth writes (not recorded in m_MaskedPrepassed).
                DataDrivenMaterial* const equalCell = depthEqualCell();
                if ( equalCell == nullptr )
                {
                    static std::unordered_set<const DataDrivenMaterial*> s_RefusedEqual;
                    if ( s_RefusedEqual.insert( parent ).second )
                        LOG_ERROR(
                             "[MeshRenderer] a Masked GPU-instanced section's material has no ({} x GBuffer) "
                             "{} permutation; the section is not in the masked depth prepass and pays its "
                             "alpha-tested overdraw in the G-buffer.",
                             MeshVertexPathName( gpu.Path ), kSurfaceDepthEqualDefine );
                    continue;
                }
                GraphicsPipeline* const equalPipeline =
                     CullPermutation( CellPipeline( m_InstancedGBufferEqualPipeline.get(), *equalCell ),
                                      gpu.Material->IsTwoSided() );
                if ( equalPipeline == nullptr || equalPipeline->GetReadiness() != PipelineReadiness::Ready )
                    continue;
            }
            if ( companion != nullptr )
            {
                // Paired with the forward depth prepass: the prepass draws a section only when its cell is Ready
                // in BOTH passes (and its textures are in) and records it; the base pass of the same frame draws
                // exactly the recorded ones.
                if ( pass == MeshPass::Forward && PrepassCurrent() )
                {
                    if ( !m_PrepassGpuSections.contains( static_cast<uint64_t>( gpu.Key ) ) )
                        continue;
                }
                else
                {
                    DataDrivenMaterial* const other = cellOf( companion->Pass );
                    GraphicsPipeline* const otherPipeline =
                         other != nullptr && companion->InstancedState != nullptr
                              ? CullPermutation( CellPipeline( companion->InstancedState, *other ),
                                                 gpu.Material->IsTwoSided() )
                              : nullptr;
                    if ( otherPipeline == nullptr || otherPipeline->GetReadiness() != PipelineReadiness::Ready )
                        continue;
                    if ( pass == MeshPass::Depth )
                        m_PrepassGpuSections.insert( static_cast<uint64_t>( gpu.Key ) );
                }
            }

            // Recreated when the view's cell changed (Shadows.Method switched the Forward cell's permutation).
            auto& instance = m_GpuInstancedInstances[postOcclusion ? view + kGpuInstancedPostViewOffset : view];
            if ( !instance || instance->GetParentMaterial() != cell )
                instance = cell->CreateInstance( "GpuInstanced" );
            BindGpuInstancedBuffers( *cell, gpu, cameraList ? gpu.CameraInstances : gpu.Instances );
            // The section's row is the scene material table's (SyncGpuScene), read by the cell of its template.
            const std::optional<uint32_t> materialRow = FindMaterialRow( parent, gpu.Material.get() );
            if ( !materialRow || !LendMaterialTable( *cell ) )
                continue;
            const uint32_t row = *materialRow;
            frameState.ApplyTo( instance.get() );
            cell->SetPushMatrix( glm::mat4( 1.0f ) ); // unused by the instanced vertex stage
            if ( maskedPrepass )
                m_MaskedPrepassed.push_back( gpu.Key );

            // THE RESOLVE (GRASS-VIS): one full-screen triangle per section - record 0 of the view's resolve table
            // (VirtualGeometry::VisibilityBuffer: {3, 1, 0, 0, 0}, the strip's first three indices, which
            // Vertex_GrassBlade.glslh's resolve branch places over the screen), every other submesh hidden. The
            // transforms are still the section's: the fragment stage rebuilds the blade its pixel's word names.
            if ( pass == MeshPass::VisibilityResolve )
            {
                if ( !m_GrassResolveArgs.IsValid() )
                    continue;
                list.Add( { .Pipeline           = pipeline,
                            .Mesh               = gpu.Mesh,
                            .Transform          = glm::mat4( 1.0f ),
                            .Material           = cell->GetMaterialExecutor(),
                            .HiddenSubmeshMask  = SubmeshMask( ~uint64_t{ 1 } ),
                            .IndirectArgs       = m_GrassResolveArgs,
                            .IndirectInstances  = refs.Instances,
                            .IndirectArgsOffset = GpuInstanceArgsOffset( 0 ),
                            .Push               = DrawPushFields{}
                                         .MaterialIndex( row )
                                         .Wind( PackViewInstanceWind( gpu.Wind, gpu.PrevInstanceOffset ) )
                                         .PrimitiveFlags( 0u ) } );
                continue;
            }

            // One draw per LOD level the producer sorted its instances into, each reading its level's records.
            for ( uint32_t level = 0; level < LevelsOf( gpu ); ++level )
                list.Add( { .Pipeline           = pipeline,
                            .Mesh               = gpu.Mesh,
                            .Transform          = glm::mat4( 1.0f ),
                            .Material           = cell->GetMaterialExecutor(),
                            .HiddenSubmeshMask  = gpu.HiddenSubmeshes,
                            .IndirectArgs       = drawArgs,
                            .IndirectInstances  = refs.Instances,
                            .IndirectArgsOffset = LevelArgsOffset( gpu, level ),
                            .Push               = DrawPushFields{}
                                         .MaterialIndex( row )
                                         .Wind( PackViewInstanceWind( gpu.Wind, gpu.PrevInstanceOffset ) )
                                         .PrimitiveFlags( 0u ) } );
        }
    }

    bool MeshRenderer::GrassVisibilityReady( const GpuInstancedRenderData& gpu )
    {
        auto* materials = Runtime::ResourceRegistry::GetMaterialService();
        if ( materials == nullptr )
            return false;
        const DataDrivenMaterial* parent = SectionTemplate( gpu.Material );
        if ( parent == nullptr || !materials->Owns( parent ) || materials->AwaitsTextures( parent ) )
            return false;
        // The same view keys BuildGpuInstancedDraws asks the cells under (one per section x view pass).
        for ( const auto& [of, carrier] :
              { std::pair{ MeshPass::Visibility, m_InstancedVisibilityPipeline.get() },
                std::pair{ MeshPass::VisibilityResolve, m_InstancedVisibilityResolvePipeline.get() } } )
        {
            if ( carrier == nullptr )
                return false;
            const uint32_t      view = kGpuInstancedViewBase + gpu.Key * kMeshPassCount + static_cast<uint32_t>( of );
            DataDrivenMaterial* cell = ForwardVirtualCell( materials->GetViewVariant( parent, gpu.Path, of, view, this ),
                                                           parent, gpu.Path, of, view );
            if ( cell == nullptr )
                return false;
            GraphicsPipeline* const pipeline = CullPermutation(
                 CellPipeline( carrier, *cell ), gpu.Material->IsTwoSided() || of == MeshPass::VisibilityResolve );
            if ( pipeline == nullptr || pipeline->GetReadiness() != PipelineReadiness::Ready )
                return false;
        }
        return true;
    }

    bool MeshRenderer::BuildGpuInstancedShadowDraws( const uint32_t c, const uint32_t shadowView,
                                                     const ShadowCasterLayer layer, MeshDrawList& list )
    {
        auto* materials = Runtime::ResourceRegistry::GetMaterialService();
        if ( materials == nullptr )
            return false;
        bool pending = false;
        // The cascade's caster box (as BuildShadowCascadeDraws culls the static casters): a section with a box casts
        // only into the cascades it meets.
        const Core::Frustum cascadeFrustum( m_CascadeCasterVP[shadowView], glm::mat4( 1.0f ) );
        for ( const GpuInstancedRenderData& gpu : m_GpuInstancedQueue )
        {
            if ( !gpu.CastShadows )
                continue;
            if ( gpu.WorldBounds && !cascadeFrustum.Intersects( *gpu.WorldBounds ) )
                continue;
            // WHO SWAYS IN THE SHADOWS, for a still section (GpuInstancedRenderData::Still, the grass), per instance
            // in its vertex stage (CasterWindLayer::*, Mesh/Surface/CasterWindLayer.glslh; the GPU cull's
            // GpuCull_CastsRestPose, the CPU walk's ShadowIsmInstances): a swaying one casts its instances past the
            // shadow wind distance in their rest pose, into the static layer, and its nearer ones swaying, over the
            // cache; one that does not sway is all static. A section whose instances move by themselves is drawn
            // whole over the cache, as before the cache knew them.
            const bool still = gpu.Still.has_value();
            const bool sways = gpu.Wind.Sways();
            uint32_t   split = 0;
            if ( layer == ShadowCasterLayer::Static )
            {
                if ( !still )
                    continue;
                split = sways ? CasterWindLayer::kRule | CasterWindLayer::kDropNear : 0u;
            }
            else if ( layer == ShadowCasterLayer::Movable )
            {
                if ( still && !sways )
                    continue;
                split = still ? CasterWindLayer::kRule | CasterWindLayer::kDropFar : 0u;
            }
            else
                split = still && sways ? CasterWindLayer::kRule : 0u;
            // Only a static layer keeps what it drew: a still section it leaves out for now stales it.
            const bool keeps = layer == ShadowCasterLayer::Static;

            const DataDrivenMaterial* parent = SectionTemplate( gpu.Material );
            if ( materials->AwaitsTextures( parent ) )
            {
                pending = pending || keeps;
                continue; // casts from the frame it draws: a masked caster would cut the default alpha
            }
            // One caster cell per (section x cascade x layer): the static layer and the movers over it are drawn in
            // the same frame from light cameras of their own (the cascade's and its movable view's).
            const uint32_t layerBase = layer == ShadowCasterLayer::Movable ? 0x08000000u : 0x0A000000u;
            const uint32_t view      = kGpuInstancedViewBase + layerBase + gpu.Key * kCascadeViews + c;
            // Its own (Instanced x ShadowDepth) cell, masked or not: the shared instanced caster's transforms are
            // the CPU-batched ISMs'.
            DataDrivenMaterial* caster =
                 parent != nullptr && materials->Owns( parent )
                      ? materials->GetViewVariant( parent, gpu.Path, MeshPass::ShadowDepth, view, this )
                      : nullptr;
            if ( caster == nullptr )
            {
                static std::unordered_set<const DataDrivenMaterial*> s_Refused;
                if ( s_Refused.insert( parent ).second )
                    LOG_ERROR( "[Shadows] a GPU-instanced section's material has no ({} x ShadowDepth) cell; "
                               "the section casts no shadow (MaterialService named the cell it refused).",
                               MeshVertexPathName( gpu.Path ) );
                continue;
            }
            GraphicsPipeline* pipeline = MaskedCasterPipeline( *caster, gpu.Path, MeshPass::ShadowDepth );
            if ( pipeline != nullptr && pipeline->GetReadiness() == PipelineReadiness::Compiling )
                pending = pending || keeps;
            if ( pipeline == nullptr || pipeline->GetReadiness() != PipelineReadiness::Ready )
                continue;

            WriteLightCamera( *caster, glm::mat4( 1.0f ), m_CascadeCasterVP[shadowView], m_WorldTimeSeconds,
                              m_CascadeCasterBias[shadowView], m_ParallaxQuality, CasterWindOrigin() );
            // The full list (GRASS-VIS): the light sees what the camera's depth hides.
            BindGpuInstancedBuffers( *caster, gpu, gpu.Instances );
            // The section's row is the scene material table's (SyncGpuScene), read by its masked caster cell.
            const std::optional<uint32_t> materialRow = FindMaterialRow( parent, gpu.Material.get() );
            if ( !materialRow || !LendMaterialTable( *caster ) )
                continue;
            const uint32_t row = *materialRow;

            // A light view's B.w is the split (InstanceWindPush; no previous instance in a light view).
            InstanceWindPush wind = PackInstanceWind( gpu.Wind );
            wind.B.w              = std::bit_cast<float>( split );
            for ( uint32_t level = 0; level < LevelsOf( gpu ); ++level )
                list.Add( { .Pipeline           = pipeline,
                            .Mesh               = gpu.Mesh,
                            .Transform          = glm::mat4( 1.0f ),
                            .Material           = caster->GetMaterialExecutor(),
                            .HiddenSubmeshMask  = gpu.HiddenSubmeshes,
                            .IndirectArgs       = gpu.Refs->Args,
                            .IndirectInstances  = gpu.Refs->Instances,
                            .IndirectArgsOffset = LevelArgsOffset( gpu, level ),
                            .Push               = DrawPushFields{}
                                         .Transform( glm::mat4( 1.0f ) )
                                         .MaterialIndex( row )
                                         .Wind( wind ) } );
        }
        return pending;
    }
} // namespace Desert::Graphic::System
