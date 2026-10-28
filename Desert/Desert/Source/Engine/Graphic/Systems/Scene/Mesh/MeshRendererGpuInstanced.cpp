#include "MeshRendererInternal.hpp"

#include <Engine/Graphic/Materials/Mesh/MaterialShadow.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>

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
                                               const MeshPass pass, const SceneFrameBinding& frameState )
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
            const uint32_t            view = kGpuInstancedViewBase + gpu.Key * 2u + ( pass == MeshPass::GBuffer );
            DataDrivenMaterial*       cell = parent != nullptr && materials->Owns( parent )
                                                  ? materials->GetViewVariant( parent, gpu.Path, pass, view, this )
                                                  : nullptr;
            if ( cell == nullptr )
            {
                static std::unordered_set<const DataDrivenMaterial*> s_Refused;
                if ( s_Refused.insert( parent ).second )
                    LOG_ERROR( "[MeshRenderer] a GPU-instanced section's material has no ({} x {}) cell; the "
                               "section is not drawn (MaterialService named the cell it refused).",
                               MeshVertexPathName( gpu.Path ), MeshPassName( pass ) );
                continue;
            }
            GraphicsPipeline* pipeline =
                 CullPermutation( CellPipeline( instancedPipeline, *cell ), gpu.Material->IsTwoSided() );
            if ( pipeline == nullptr || pipeline->GetReadiness() != PipelineReadiness::Ready )
                continue; // still in the driver: drawn from the frame it is Ready (RDG-PSO)

            auto& instance = m_GpuInstancedInstances[view];
            if ( !instance )
                instance = cell->CreateInstance( "GpuInstanced" );
            BindGpuInstancedBuffers( *cell, gpu );
            std::vector<glm::vec4> rows;
            const uint32_t         row = AppendRow( rows, EffectiveRow( parent, gpu.Material.get() ) );
            // Find: a surface template with no parameters declares no Materials row block.
            if ( auto* sb = cell->Find<StorageBufferProperty>( kMaterialRowBlock ) )
                sb->SetRawData( rows.data(), static_cast<uint32_t>( rows.size() * sizeof( glm::vec4 ) ) );
            frameState.ApplyTo( instance.get() );
            cell->SetPushMatrix( glm::mat4( 1.0f ) ); // unused by the instanced vertex stage

            list.Add( { .Pipeline          = pipeline,
                        .Mesh              = gpu.Mesh,
                        .Transform         = glm::mat4( 1.0f ),
                        .Material          = cell->GetMaterialExecutor(),
                        .HiddenSubmeshMask = gpu.HiddenSubmeshes,
                        .IndirectArgs      = gpu.Refs->Args,
                        .IndirectInstances = gpu.Refs->Instances,
                        .BindState         = [cell, inst = instance.get(), row,
                                      wind = PackViewInstanceWind( gpu.Wind, gpu.PrevInstanceOffset )]
                        {
                            cell->SetMaterialIndex( row );
                            cell->SetInstancedWind( wind );
                            cell->SetPrimitiveFlags( 0u );
                            cell->Bind( inst );
                        } } );
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
            std::vector<glm::vec4> rows;
            const uint32_t         row = AppendRow( rows, EffectiveRow( parent, gpu.Material.get() ) );
            // Find: a masked caster whose surface has no parameters (or whose mask reads none) declares no row
            // block.
            if ( auto* sb = caster->Find<StorageBufferProperty>( kMaterialRowBlock ) )
                sb->SetRawData( rows.data(), static_cast<uint32_t>( rows.size() * sizeof( glm::vec4 ) ) );

            list.Add( { .Pipeline          = pipeline,
                        .Mesh              = gpu.Mesh,
                        .Transform         = glm::mat4( 1.0f ),
                        .Material          = caster->GetMaterialExecutor(),
                        .HiddenSubmeshMask = gpu.HiddenSubmeshes,
                        .IndirectArgs      = gpu.Refs->Args,
                        .IndirectInstances = gpu.Refs->Instances,
                        .BindState = [caster, inst = gpu.Material.get(), row, wind = PackInstanceWind( gpu.Wind )]
                        {
                            caster->SetPushMatrix( glm::mat4( 1.0f ) );
                            caster->SetMaterialIndex( row );
                            caster->SetInstancedWind( wind );
                            caster->Bind( inst );
                        } } );
        }
    }
} // namespace Desert::Graphic::System
