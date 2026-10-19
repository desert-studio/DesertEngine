#include "MeshRendererInternal.hpp"

#include <Engine/Geometry/StaticMesh.hpp>
#include <Engine/Graphic/Materials/Mesh/MeshVertexPath.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/DisplacementCache.hpp>

#include <Common/Core/Logger.hpp>

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
            cache.Request( data.Entity, data.MotionPart, *data.Mesh, data.Transform, materials, view,
                           viewHeightPixels, frame );
        }
        if ( const Common::BoolResultStr displaced = cache.DisplaceFrame( frame ); !displaced )
            LOG_ERROR( "{}", displaced.GetError() );
        for ( StaticMeshRenderData& data : m_StaticQueue )
            if ( const std::shared_ptr<StaticMesh> twin = cache.Find( data.Entity, data.MotionPart ) )
                data.Mesh = twin.get(); // the cache holds the twin past this frame's draws (EndFrame of the next)
    }
} // namespace Desert::Graphic::System
