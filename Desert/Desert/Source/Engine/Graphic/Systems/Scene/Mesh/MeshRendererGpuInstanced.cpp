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

        // The producer's buffers into the cell: the instances, and a blade's shape (MeshPathOwnBinding).
        void BindGpuInstancedBuffers( DataDrivenMaterial& cell, const MeshRenderer::GpuInstancedRenderData& gpu )
        {
            if ( auto* sb = cell.Get<StorageBufferProperty>( kInstanceTransformsBlock ) )
                sb->SetBuffer( gpu.Instances );
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

    void MeshRenderer::BuildGpuInstancedDraws( MeshDrawList& list, GraphicsPipeline* instancedPipeline,
                                               const MeshPass pass, const SceneFrameBinding& frameState,
                                               const StaticPassCells* companion )
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
            const DataDrivenMaterial* parent = SectionTemplate( gpu.Material );
            if ( materials->AwaitsTextures( parent ) )
                continue; // a texture in flight: drawn from the frame its material is rebuilt bound (GRASS-SETTLE)
            // The masked depth prepass (the companion-less Depth call, DeclareMaskedDepthDraws) draws the Masked
            // sections alone; the G-buffer draws a section it drew at its depth, without writes
            // (m_InstancedGBufferEqualPipeline). The forward depth prepass (Depth with a companion) draws every one.
            const bool masked        = IsMaskedSurface( parent );
            const bool maskedPrepass = pass == MeshPass::Depth && companion == nullptr;
            if ( maskedPrepass && !masked )
                continue;
            const bool prepassed = pass == MeshPass::GBuffer && masked &&
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
            const uint32_t      view = viewOf( pass );
            DataDrivenMaterial* cell = cellOf( pass );
            if ( cell == nullptr )
            {
                static std::unordered_set<const DataDrivenMaterial*> s_Refused;
                if ( s_Refused.insert( parent ).second )
                    LOG_ERROR( "[MeshRenderer] a GPU-instanced section's material has no ({} x {}) cell; the "
                               "section is not drawn (MaterialService named the cell it refused).",
                               MeshVertexPathName( gpu.Path ), MeshPassName( pass ) );
                continue;
            }
            GraphicsPipeline* pipeline = CullPermutation(
                 CellPipeline( prepassed ? m_InstancedGBufferEqualPipeline.get() : instancedPipeline, *cell ),
                 gpu.Material->IsTwoSided() );
            if ( pipeline == nullptr || pipeline->GetReadiness() != PipelineReadiness::Ready )
                continue; // still in the driver: drawn from the frame it is Ready (RDG-PSO)
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
            auto& instance = m_GpuInstancedInstances[view];
            if ( !instance || instance->GetParentMaterial() != cell )
                instance = cell->CreateInstance( "GpuInstanced" );
            BindGpuInstancedBuffers( *cell, gpu );
            // The section's row is the scene material table's (SyncGpuScene), read by the cell of its template.
            const std::optional<uint32_t> materialRow = FindMaterialRow( parent, gpu.Material.get() );
            if ( !materialRow || !LendMaterialTable( *cell ) )
                continue;
            const uint32_t row = *materialRow;
            frameState.ApplyTo( instance.get() );
            cell->SetPushMatrix( glm::mat4( 1.0f ) ); // unused by the instanced vertex stage
            if ( maskedPrepass )
                m_MaskedPrepassed.push_back( gpu.Key );

            // One draw per LOD level the producer sorted its instances into, each reading its level's records.
            for ( uint32_t level = 0; level < LevelsOf( gpu ); ++level )
                list.Add( { .Pipeline           = pipeline,
                            .Mesh               = gpu.Mesh,
                            .Transform          = glm::mat4( 1.0f ),
                            .Material           = cell->GetMaterialExecutor(),
                            .HiddenSubmeshMask  = gpu.HiddenSubmeshes,
                            .IndirectArgs       = gpu.Refs->Args,
                            .IndirectInstances  = gpu.Refs->Instances,
                            .IndirectArgsOffset = LevelArgsOffset( gpu, level ),
                            .Push               = DrawPushFields{}
                                       .MaterialIndex( row )
                                       .Wind( PackViewInstanceWind( gpu.Wind, gpu.PrevInstanceOffset ) )
                                       .PrimitiveFlags( 0u ) } );
        }
    }

    void MeshRenderer::BuildGpuInstancedShadowDraws( const uint32_t c, const uint32_t shadowView,
                                                     MeshDrawList& list )
    {
        auto* materials = Runtime::ResourceRegistry::GetMaterialService();
        if ( materials == nullptr )
            return;
        // The cascade's caster box (as BuildShadowCascadeDraws culls the static casters): a section with a box casts
        // only into the cascades it meets.
        const Core::Frustum cascadeFrustum( m_CascadeCasterVP[shadowView], glm::mat4( 1.0f ) );
        for ( const GpuInstancedRenderData& gpu : m_GpuInstancedQueue )
        {
            if ( !gpu.CastShadows )
                continue;
            if ( gpu.WorldBounds && !cascadeFrustum.Intersects( *gpu.WorldBounds ) )
                continue;
            const DataDrivenMaterial* parent = SectionTemplate( gpu.Material );
            if ( materials->AwaitsTextures( parent ) )
                continue; // casts from the frame it draws: a masked caster would cut the default alpha
            const uint32_t            view   = kGpuInstancedViewBase + 0x08000000u + gpu.Key * kCascadeViews + c;
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
            if ( pipeline == nullptr || pipeline->GetReadiness() != PipelineReadiness::Ready )
                continue;

            WriteLightCamera( *caster, glm::mat4( 1.0f ), m_CascadeCasterVP[shadowView], m_WorldTimeSeconds,
                              m_CascadeCasterBias[shadowView], m_ParallaxQuality );
            BindGpuInstancedBuffers( *caster, gpu );
            // The section's row is the scene material table's (SyncGpuScene), read by its masked caster cell.
            const std::optional<uint32_t> materialRow = FindMaterialRow( parent, gpu.Material.get() );
            if ( !materialRow || !LendMaterialTable( *caster ) )
                continue;
            const uint32_t row = *materialRow;

            for ( uint32_t level = 0; level < LevelsOf( gpu ); ++level )
                list.Add( { .Pipeline           = pipeline,
                            .Mesh               = gpu.Mesh,
                            .Transform          = glm::mat4( 1.0f ),
                            .Material           = caster->GetMaterialExecutor(),
                            .HiddenSubmeshMask  = gpu.HiddenSubmeshes,
                            .IndirectArgs       = gpu.Refs->Args,
                            .IndirectInstances  = gpu.Refs->Instances,
                            .IndirectArgsOffset = LevelArgsOffset( gpu, level ),
                            .Push = DrawPushFields{}
                                         .Transform( glm::mat4( 1.0f ) )
                                         .MaterialIndex( row )
                                         .Wind( PackInstanceWind( gpu.Wind ) ) } );
        }
    }
} // namespace Desert::Graphic::System
