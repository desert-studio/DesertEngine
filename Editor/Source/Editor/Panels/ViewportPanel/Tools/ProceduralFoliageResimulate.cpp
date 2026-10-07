#include "ProceduralFoliageResimulate.hpp"

#include <utility>

namespace Desert::Editor::Tools
{
    namespace Procedural = World::Foliage::Procedural;

    Common::ResultStr<ProceduralFoliageResimulated> ResimulateProceduralFoliage(
         const ECS::ProceduralFoliageData& volume, const glm::vec3& center, const Common::UUID& owner,
         std::span<const Assets::Serialization::FoliageTypeData> types, const ProceduralFoliageHost& host )
    {
        if ( !host.Trace || !host.Rewrite || !host.Remove || !host.Create )
            return Common::MakeError<ProceduralFoliageResimulated>(
                 "procedural foliage: the host supplies no trace or no writes" );

        Procedural::ProceduralFoliageSpawnerSettings settings;
        settings.TileSize            = volume.TileSize;
        settings.MinimumQuadTreeSize = volume.MinimumQuadTreeSize;
        settings.NumUniqueTiles      = volume.NumUniqueTiles;
        settings.RandomSeed          = volume.RandomSeed;
        Procedural::ProceduralFoliageSpawner spawner(
             settings, std::vector<Assets::Serialization::FoliageTypeData>( types.begin(), types.end() ) );
        if ( auto valid = spawner.Validate(); !valid )
            return Common::MakeFormattedError<ProceduralFoliageResimulated>( "procedural foliage: {}",
                                                                             valid.GetError() );
        spawner.Simulate();

        const Procedural::ProceduralFoliageBox box{ center - volume.Extent, center + volume.Extent };
        const auto desired = Procedural::DesiredInstancesInVolume( spawner, box, volume.TileOverlap );

        // UE FFoliagePaintingGeometryFilter from the volume's bAllowLandscape / bAllowStaticMesh.
        FoliageSurfaceFilter filter;
        filter.Landscape  = volume.AllowLandscape;
        filter.StaticMesh = volume.AllowStaticMesh;
        const Procedural::ProceduralFoliageTrace trace =
             [&]( const glm::vec3& start,
                  const glm::vec3& end ) -> std::optional<Procedural::ProceduralFoliageGround>
        {
            const auto hit = host.Trace( start, end, filter );
            if ( !hit || !filter.Allows( hit->Surface ) )
                return std::nullopt;
            return Procedural::ProceduralFoliageGround{ hit->Point, hit->Normal };
        };

        // One stream for the whole volume, in the order of the desired instances: a resimulation of an unchanged
        // world draws the same Z offsets and layer chances.
        FoliageRandom rng( static_cast<uint64_t>( static_cast<uint32_t>( volume.RandomSeed ) ) );
        const Procedural::ProceduralFoliagePlace place =
             [&]( const Procedural::ProceduralFoliagePlacement& p,
                  const Procedural::ProceduralFoliageGround&    ground ) -> std::optional<glm::mat4>
        {
            const auto&     type = types[p.TypeIndex];
            FoliageTraceHit hit;
            hit.Point  = ground.Point;
            hit.Normal = ground.Normal;
            if ( !type.LandscapeLayers.empty() && host.LayerWeightAt )
                hit.LayerWeight = host.LayerWeightAt( p.TypeIndex, ground.Point );
            return FoliageProceduralPlace( type, hit, p.YawDegrees, p.PitchDegrees, p.Scale, rng );
        };

        auto placed = Procedural::PlaceProceduralFoliage( desired, static_cast<uint32_t>( types.size() ), trace,
                                                          place, host.CellSize );
        if ( !placed )
            return Common::MakeFormattedError<ProceduralFoliageResimulated>( "procedural foliage: {}",
                                                                             placed.GetError() );
        auto       fresh = placed.ExtractValue();
        const auto plan  = Procedural::PlanProceduralFields( host.Existing, owner, fresh );

        ProceduralFoliageResimulated done;
        for ( const auto& field : fresh )
            done.Instances += field.Instances.size();
        // Creates first: a refused create leaves the volume's old fields standing instead of an emptied world.
        for ( const size_t i : plan.Create )
        {
            if ( auto created = host.Create( fresh[i] ); !created )
                return Common::MakeFormattedError<ProceduralFoliageResimulated>( "procedural foliage: {}",
                                                                                 created.GetError() );
            ++done.Created;
        }
        for ( const auto& [existing, i] : plan.Rewrite )
        {
            host.Rewrite( existing, std::move( fresh[i].Instances ) );
            ++done.Rewritten;
        }
        for ( const size_t existing : plan.Remove )
        {
            host.Remove( existing );
            ++done.Removed;
        }
        return Common::MakeSuccess( std::move( done ) );
    }
} // namespace Desert::Editor::Tools
