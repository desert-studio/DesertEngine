#include "MeshRendererInternal.hpp"

#include <Engine/Graphic/Materials/Mesh/MaterialShadow.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>

#include <unordered_set>

// GPU-INSTANCED SECTIONS (GpuInstances.hpp): the landscape grass. The transforms and the instance count are the
// producer's compute output, so nothing here culls, sorts or uploads instances: each section is one indirect draw
// through its material's (Instanced x pass) cell. The cell is a COPY of the material (MaterialService::
// GetViewVariant, keyed per section) because a material owns one InstanceTransforms binding per frame and the
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

        for ( const GpuInstancedRenderData& gpu : m_GpuInstancedQueue )
        {
            const DataDrivenMaterial* parent = SectionTemplate( gpu.Material );
            const uint32_t            view = kGpuInstancedViewBase + gpu.Key * 2u + ( pass == MeshPass::GBuffer );
            DataDrivenMaterial*       cell = parent != nullptr && materials->Owns( parent )
                                                  ? materials->GetViewVariant( parent, gpu.Path, pass, view )
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
                        .BindState = [cell, inst = instance.get(), row, wind = PackViewInstanceWind( gpu.Wind )]
                        {
                            cell->SetMaterialIndex( row );
                            cell->SetInstancedWind( wind );
                            cell->SetPrimitiveFlags( 0u );
                            cell->Bind( inst );
                        } } );
        }
    }

    void MeshRenderer::BuildGpuInstancedShadowDraws( const uint32_t c, MeshDrawList& list )
    {
        auto* materials = Runtime::ResourceRegistry::GetMaterialService();
        if ( materials == nullptr )
            return;
        for ( const GpuInstancedRenderData& gpu : m_GpuInstancedQueue )
        {
            if ( !gpu.CastShadows )
                continue;
            const DataDrivenMaterial* parent = SectionTemplate( gpu.Material );
            const uint32_t            view   = kGpuInstancedViewBase + 0x08000000u + gpu.Key * kMaxCascades + c;
            // Its own (Instanced x ShadowDepth) cell, masked or not: the shared instanced caster's transforms are
            // the CPU-batched ISMs'.
            DataDrivenMaterial* caster =
                 parent != nullptr && materials->Owns( parent )
                      ? materials->GetViewVariant( parent, gpu.Path, MeshPass::ShadowDepth, view )
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

            WriteLightCamera( *caster, glm::mat4( 1.0f ), m_CascadeCasterVP[c], m_WorldTimeSeconds,
                              m_CascadeCasterBias[c], m_ParallaxQuality );
            BindGpuInstancedBuffers( *caster, gpu );
            std::vector<glm::vec4> rows;
            const uint32_t         row = AppendRow( rows, EffectiveRow( parent, gpu.Material.get() ) );
            // Find: a masked caster whose surface has no parameters (or whose mask reads none) declares no row block.
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
