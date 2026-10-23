#include <Engine/ECS/System/LandscapeSplineMeshDraw.hpp>

#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/Geometry/DynamicMesh.hpp>
#include <Engine/Geometry/PrimitiveMeshFactory.hpp>
#include <Engine/World/Landscape/LandscapeSplineMeshes.hpp>

#include <algorithm>
#include <span>
#include <unordered_set>

namespace Desert::ECS
{
    namespace
    {
        namespace WL = World::Landscape;

        /// An entry's source geometry and section materials; nullopt while a mesh asset is still loading.
        struct Source
        {
            WL::LandscapeSplineMeshSource Geometry;
            std::vector<Common::UUID>     Materials; // empty: the default surface
        };

        std::optional<Source> SourceOf( const WL::LandscapeSplineMeshEntry& entry,
                                        const LandscapeSplineMeshDraw::Services& services )
        {
            if ( entry.Primitive )
            {
                const DynamicMesh* shape = Geometry::PrimitiveMeshFactory::GetShared( *entry.Primitive );
                if ( shape == nullptr )
                    return std::nullopt;
                return Source{ { shape->GetVertices(), shape->GetIndices(), shape->GetSubmeshes() }, {} };
            }
            const Assets::StaticMeshAsset* asset = services.MeshAsset( entry.Mesh );
            if ( asset == nullptr )
                return std::nullopt;
            return Source{ { asset->GetVertices(), asset->GetIndices(), asset->GetSubmeshes() },
                           asset->GetMaterialHandles() };
        }

        Graphic::MaterialSlotBindingPtr SlotsFor( const std::vector<Common::UUID>&           materials,
                                                  const LandscapeSplineMeshDraw::Services& services )
        {
            auto binding = std::make_shared<Graphic::MaterialSlotBinding>();
            if ( materials.empty() )
                binding->Owned.push_back( services.DefaultInstance() );
            for ( const Common::UUID& material : materials )
            {
                auto instance = services.Instance( material );
                binding->Owned.push_back( instance ? std::move( instance ) : services.DefaultInstance() );
            }
            for ( const auto& instance : binding->Owned )
                binding->Slots.push_back( instance.get() );
            return binding;
        }
    } // namespace

    void LandscapeSplineMeshDraw::Record( entt::registry& registry, Graphic::Render::RenderCommandBuffer& commands,
                                          uint32_t materialsVersion, const Services& services )
    {
        std::unordered_set<entt::entity> seen;
        registry.view<LandscapeComponent>().each(
             [&]( entt::entity entity, const LandscapeComponent& landscape )
             {
                 seen.insert( entity );
                 Built& built = m_Built[entity];
                 if ( !built.Complete || built.MaterialsVersion != materialsVersion ||
                      !( built.Splines == landscape.Splines ) )
                 {
                     built.Splines          = landscape.Splines;
                     built.Complete         = true;
                     built.MaterialsVersion = materialsVersion;
                     built.Draws.clear();
                     const auto& segments = landscape.Splines.Segments;
                     for ( size_t s = 0; s < segments.size() && built.Complete; ++s )
                     {
                         std::vector<Source> sources;
                         for ( const WL::LandscapeSplineMeshEntry& entry : segments[s].SplineMeshes )
                         {
                             auto source = SourceOf( entry, services );
                             if ( !source )
                             {
                                 built.Complete = false; // retried next frame
                                 break;
                             }
                             sources.push_back( std::move( *source ) );
                         }
                         if ( !built.Complete || sources.empty() )
                             continue;
                         std::vector<WL::LandscapeSplineMeshSource> geometry;
                         for ( const Source& source : sources )
                             geometry.push_back( source.Geometry );
                         auto meshes = WL::BuildLandscapeSplineSegmentMeshes( landscape.Splines, s, geometry );
                         for ( size_t e = 0; e < meshes.size(); ++e )
                         {
                             if ( meshes[e].Vertices.empty() )
                                 continue;
                             Draw draw;
                             draw.Mesh  = std::make_shared<DynamicMesh>( meshes[e].Vertices, meshes[e].Indices,
                                                                        meshes[e].Submeshes );
                             draw.Slots = SlotsFor( sources[e].Materials, services );
                             draw.Part  = static_cast<uint32_t>( built.Draws.size() + 1u );
                             built.Draws.push_back( std::move( draw ) );
                         }
                     }
                 }

                 if ( IsHidden( registry, entity ) )
                     return;
                 for ( const Draw& draw : built.Draws )
                 {
                     const size_t sections = std::min<size_t>( draw.Mesh->GetSubmeshes().size(), 64u );
                     for ( size_t i = 0; i < sections; ++i )
                     {
                         const uint64_t only   = uint64_t( 1 ) << i;
                         const uint64_t hidden = sections == 64u ? ~only : ( ( uint64_t( 1 ) << sections ) - 1u ) & ~only;
                         commands.Emplace<Graphic::Render::DrawStaticMeshCommand>(
                              static_cast<uint32_t>( entity ), draw.Mesh.get(), draw.Slots,
                              static_cast<uint32_t>( SectionMaterialSlot( i, draw.Slots->Slots.size() ) ),
                              glm::mat4( 1.0f ), /*outlined*/ false, hidden, /*forcedLOD*/ -1, /*lodBias*/ 0,
                              /*castShadows*/ true, /*receiveShadows*/ true, /*sortPriority*/ 0,
                              /*maxDrawDistance*/ 0.0f, draw.Part );
                     }
                 }
             } );
        std::erase_if( m_Built, [&]( const auto& pair ) { return !seen.contains( pair.first ); } );
    }
} // namespace Desert::ECS
