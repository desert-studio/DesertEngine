#include "MeshRendererInternal.hpp"

#include <Engine/Geometry/StaticMesh.hpp>
#include <Engine/Graphic/Materials/Mesh/MeshVertexPath.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/DisplacementCache.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <bit>
#include <memory>
#include <optional>
#include <vector>

namespace Desert::Graphic::System
{
    void MeshRenderer::ApplyDisplacement( DisplacementCache& cache, const Geometry::LODView& view,
                                          const float viewHeightPixels, const uint64_t frame,
                                          const bool requestLevels )
    {
        std::vector<std::optional<DisplacementMaterialRecord>> materials;
        for ( const StaticMeshRenderData& data : m_StaticQueue )
        {
            if ( !requestLevels )
                break;
            if ( data.Mesh == nullptr || !data.Mesh->GetDisplacementTopology() || !data.MaterialSlots ||
                 data.MaterialSlots->Slots.empty() )
                continue;
            const std::vector<Submesh>& submeshes = data.Mesh->GetSubmeshes();
            materials.assign( submeshes.size(), std::nullopt );
            for ( size_t s = 0; s < submeshes.size(); ++s )
            {
                const SurfaceSlot slot = SectionSurface(
                     data.MaterialSlots.get(),
                     static_cast<uint32_t>( SectionMaterialSlot( s, data.MaterialSlots->Slots.size() ) ) );
                if ( slot.Surface == nullptr )
                    continue;
                // The same predicate that picks the template's `.Parallax` cells: the material assigns its Height.
                const auto record =
                     ResolveDisplacementMaterial( slot.Surface->GetSchema(), slot.Surface->GetMaterialLayout(),
                                                  EffectiveRow( slot.Surface, slot.Instance ),
                                                  IsParallaxCell( slot.Surface->GetShaderName() ) );
                if ( !record.IsSuccess() )
                {
                    LOG_ERROR( "[Displacement] entity {} section {}: not displaced: {}", data.Entity, s,
                               record.GetError() );
                    continue;
                }
                materials[s] = record.GetValue();
            }
            // No section moves (no material assigns its Height): nothing to tessellate.
            if ( std::none_of( materials.begin(), materials.end(),
                               []( const std::optional<DisplacementMaterialRecord>& m ) { return m.has_value(); } ) )
                continue;
            cache.Request( data.Entity, data.MotionPart, *data.Mesh, data.Transform, materials, view,
                           viewHeightPixels, frame );
        }
        // ISM members (§9.8): one request per instance - equal levels share one twin, so a field of stones costs
        // one geometry per distinct level combination. Entry part = (instance + 1) << 6 | section group, a
        // cache-only key (each section group of the component is its own record with its own material).
        const auto ismPart = []( size_t instance, uint64_t hidden )
        {
            const uint32_t group = static_cast<uint32_t>( std::countr_zero( ~hidden ) ) & 63u;
            return ( static_cast<uint32_t>( instance + 1 ) << 6u ) | group;
        };
        for ( const InstancedMeshRenderData& ism : m_InstancedQueue )
        {
            if ( !requestLevels )
                break;
            if ( ism.Mesh == nullptr || ism.Mesh->GetType() != MeshType::Static || !ism.DisplacedMaterial ||
                 !ism.Mesh->GetDisplacementTopology() )
                continue;
            auto* surface = dynamic_cast<DataDrivenMaterial*>( ism.Material->GetParentMaterial() );
            if ( surface == nullptr )
                continue;
            const auto record = ResolveDisplacementMaterial( surface->GetSchema(), surface->GetMaterialLayout(),
                                                             EffectiveRow( surface, ism.Material.get() ),
                                                             IsParallaxCell( surface->GetShaderName() ) );
            if ( !record.IsSuccess() )
            {
                LOG_ERROR( "[Displacement] ISM entity {}: not displaced: {}", ism.Entity, record.GetError() );
                continue;
            }
            if ( !record.GetValue() )
                continue;
            const auto& mesh = *static_cast<const StaticMesh*>( ism.Mesh );
            materials.assign( mesh.GetSubmeshes().size(), std::nullopt );
            for ( size_t s = 0; s < materials.size(); ++s )
                if ( s >= 64 || ( ism.HiddenSubmeshes & ( 1ull << s ) ) == 0 )
                    materials[s] = record.GetValue();
            const std::vector<glm::mat4>& transforms = *ism.Transforms;
            for ( size_t i = 0; i < transforms.size(); ++i )
                cache.Request( ism.Entity, ismPart( i, ism.HiddenSubmeshes ), mesh, transforms[i], materials, view,
                               viewHeightPixels, frame );
        }

        if ( const Common::BoolResultStr displaced = cache.DisplaceFrame( frame ); !displaced )
            LOG_ERROR( "{}", displaced.GetError() );
        for ( StaticMeshRenderData& data : m_StaticQueue )
            if ( const std::shared_ptr<StaticMesh> twin = cache.Find( data.Entity, data.MotionPart ) )
            {
                data.Mesh = twin.get(); // the cache holds the twin past this frame's draws (EndFrame of the next)
                // No double relief: the displaced slots draw base cells (MaterialSlotBinding::Displaced).
                if ( data.MaterialSlots && data.MaterialSlots->Displaced )
                    data.MaterialSlots = data.MaterialSlots->Displaced;
            }

        // Split each displaced ISM record by the twin its instances draw: the base keeps the undisplaced ones, every
        // twin gets a record of its own with the displaced material, each instance keeping its GPU-scene row.
        std::vector<InstancedMeshRenderData> twins;
        for ( InstancedMeshRenderData& ism : m_InstancedQueue )
        {
            if ( ism.Mesh == nullptr || !ism.DisplacedMaterial || !ism.Mesh->GetDisplacementTopology() )
                continue;
            const std::vector<glm::mat4>& transforms = *ism.Transforms;
            std::vector<std::pair<std::shared_ptr<StaticMesh>, uint32_t>> drawn;
            drawn.reserve( transforms.size() );
            bool anyTwin = false;
            for ( size_t i = 0; i < transforms.size(); ++i )
            {
                drawn.emplace_back( cache.Find( ism.Entity, ismPart( i, ism.HiddenSubmeshes ) ),
                                    static_cast<uint32_t>( i ) );
                anyTwin = anyTwin || drawn.back().first != nullptr;
            }
            if ( !anyTwin )
                continue;
            // Group by twin, in first-seen order (deterministic draw order).
            std::vector<std::shared_ptr<StaticMesh>> order;
            for ( const auto& [twin, i] : drawn )
                if ( std::find( order.begin(), order.end(), twin ) == order.end() )
                    order.push_back( twin );
            InstancedMeshRenderData base = ism;
            bool                    keepBase = false;
            for ( const std::shared_ptr<StaticMesh>& twin : order )
            {
                auto subset = std::make_shared<std::vector<glm::mat4>>();
                auto ids    = std::make_shared<std::vector<uint32_t>>();
                for ( const auto& [t, i] : drawn )
                    if ( t == twin )
                    {
                        subset->push_back( transforms[i] );
                        ids->push_back( i );
                    }
                InstancedMeshRenderData group = ism;
                group.Transforms               = std::move( subset );
                group.InstanceIds              = std::move( ids );
                if ( twin == nullptr )
                {
                    base     = group;
                    keepBase = true;
                    continue;
                }
                group.Mesh     = twin.get(); // held by the cache past this frame's draws
                group.Material = ism.DisplacedMaterial;
                twins.push_back( std::move( group ) );
            }
            if ( keepBase )
                ism = std::move( base );
            else
                ism.Transforms.reset(); // every instance draws a twin: the record is dropped below
        }
        std::erase_if( m_InstancedQueue, []( const InstancedMeshRenderData& ism ) { return !ism.Transforms; } );
        for ( InstancedMeshRenderData& twin : twins )
            m_InstancedQueue.push_back( std::move( twin ) );
    }
} // namespace Desert::Graphic::System
