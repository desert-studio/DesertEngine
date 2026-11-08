#include <Engine/World/Foliage/Procedural/ProceduralFoliageVolume.hpp>

#include <Common/Core/Math/RandomStream.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace Desert::World::Foliage::Procedural
{
    int32_t RandomSeedForPosition( glm::vec2 position )
    {
        // Teschner et al.'s spatial hash over the centimetre grid, as wide as the stream's seed.
        const auto     x    = static_cast<uint64_t>( static_cast<int64_t>( std::llround( position.x ) ) );
        const auto     z    = static_cast<uint64_t>( static_cast<int64_t>( std::llround( position.y ) ) );
        const uint64_t hash = ( x * 73856093ull ) ^ ( z * 19349663ull );
        return static_cast<int32_t>( static_cast<uint32_t>( hash ) );
    }

    bool IsInstanceFilteredByFalloff( const Assets::Serialization::FoliageDensityFalloff& falloff,
                                      glm::vec2 position, glm::vec2 origin, float maxDistance )
    {
        if ( !falloff.UseFalloffCurve )
            return false;
        const glm::vec2 d          = position - origin;
        const float     distance   = std::sqrt( d.x * d.x + d.y * d.y );
        const float     normalized = maxDistance > 0.0f ? std::min( distance / maxDistance, 1.0f ) : 1.0f;
        const float     keep       = Assets::Serialization::EvaluateFoliageCurve( falloff.FalloffCurve, normalized );
        if ( !( keep < 1.0f ) )
            return false;
        Common::Math::RandomStream stream( RandomSeedForPosition( position ) );
        return stream.FRand() > keep;
    }

    bool BlocksProceduralInstance( const ProceduralFoliageBlocker& blocker, const Common::UUID& volume,
                                   const glm::vec3& traceStart, const glm::vec3& ground )
    {
        if ( !( blocker.Volume == Common::UUID::Null() ) && !( blocker.Volume == volume ) )
            return false;
        const ProceduralFoliageBox& box = blocker.Box;
        if ( ground.x < box.Min.x || ground.x > box.Max.x || ground.z < box.Min.z || ground.z > box.Max.z )
            return false;
        if ( box.Max.y < ground.y || box.Min.y > traceStart.y )
            return false;
        if ( !blocker.DensityFalloff.UseFalloffCurve )
            return true;
        const glm::vec2 centre{ 0.5f * ( box.Min.x + box.Max.x ), 0.5f * ( box.Min.z + box.Max.z ) };
        const float     maxExtent = 0.5f * std::max( box.Max.x - box.Min.x, box.Max.z - box.Min.z );
        return IsInstanceFilteredByFalloff( blocker.DensityFalloff, { ground.x, ground.z }, centre, maxExtent );
    }

    std::vector<ProceduralFoliageDesired> DesiredInstancesInVolume( const ProceduralFoliageSpawner& spawner,
                                                                    const ProceduralFoliageBox&     volume,
                                                                    float                           tileOverlap )
    {
        const float     size = spawner.Settings().TileSize;
        const glm::vec2 lo{ volume.Min.x, volume.Min.z };
        const glm::vec2 hi{ volume.Max.x, volume.Max.z };
        const auto      layout = TileLayoutFor( lo, hi, size, tileOverlap );
        const glm::vec2 origin =
             glm::vec2( static_cast<float>( layout.BottomLeftX ), static_cast<float>( layout.BottomLeftY ) ) *
             size;

        // UE ActorVolumeLocation and ActorVolumeMaxExtent: what each type's DensityFalloff is measured from.
        const glm::vec2 centre    = 0.5f * ( lo + hi );
        const float     maxExtent = 0.5f * std::max( hi.x - lo.x, hi.y - lo.y );
        const auto      types     = spawner.Types();

        std::vector<ProceduralFoliageDesired> desired;
        for ( const auto& placement : GenerateProceduralContent( spawner, layout, origin, tileOverlap ) )
        {
            // UE ExtractDesiredInstances: only what stands inside the volume's footprint and survives the type's
            // falloff (an out-of-range type index is PlaceProceduralFoliage's refusal to name).
            const glm::vec2 at = placement.Location;
            if ( at.x < lo.x || at.x > hi.x || at.y < lo.y || at.y > hi.y )
                continue;
            if ( placement.TypeIndex < types.size() &&
                 IsInstanceFilteredByFalloff( types[placement.TypeIndex].Procedural.DensityFalloff, at, centre,
                                              maxExtent ) )
                continue;
            desired.push_back( ProceduralFoliageDesired{
                 placement, { at.x, volume.Max.y, at.y }, { at.x, volume.Min.y, at.y } } );
        }
        return desired;
    }

    Common::ResultStr<std::vector<ProceduralFoliageTypeField>>
    PlaceProceduralFoliage( std::span<const ProceduralFoliageDesired> desired, uint32_t typeCount,
                            const ProceduralFoliageTrace& trace, const ProceduralFoliagePlace& place,
                            std::optional<double> cellSize )
    {
        using Fields = std::vector<ProceduralFoliageTypeField>;
        if ( cellSize.has_value() && !( *cellSize > 0.0 ) )
            return Common::MakeFormattedError<Fields>( "cell size {} must be a positive distance, cm", *cellSize );
        std::map<std::tuple<uint32_t, int32_t, int32_t>, std::vector<glm::mat4>> filed;
        for ( const auto& d : desired )
        {
            if ( d.Placement.TypeIndex >= typeCount )
                return Common::MakeFormattedError<Fields>( "a placement names type {} of {}",
                                                           d.Placement.TypeIndex, typeCount );
            const auto ground = trace( d.TraceStart, d.TraceEnd );
            if ( !ground )
                continue;
            const auto instance = place( d.Placement, *ground );
            if ( !instance )
                continue;
            const CellCoord cell = cellSize.has_value() ? FoliageCellOf( *instance, *cellSize ) : CellCoord{};
            filed[{ d.Placement.TypeIndex, cell.X, cell.Z }].push_back( *instance );
        }
        Fields fields;
        for ( auto& [key, instances] : filed )
            fields.push_back( ProceduralFoliageTypeField{ std::get<0>( key ),
                                                          CellCoord{ std::get<1>( key ), std::get<2>( key ) },
                                                          std::move( instances ) } );
        return Common::MakeSuccess( std::move( fields ) );
    }

    ProceduralFoliageFieldPlan PlanProceduralFields( std::span<const ProceduralFoliageExistingField> existing,
                                                     const Common::UUID&                             owner,
                                                     std::span<const ProceduralFoliageTypeField>     fresh )
    {
        ProceduralFoliageFieldPlan plan;
        std::vector<bool>          reused( existing.size(), false );
        for ( size_t f = 0; f < fresh.size(); ++f )
        {
            const auto match = std::find_if( existing.begin(), existing.end(),
                                             [&]( const ProceduralFoliageExistingField& e )
                                             {
                                                 const auto i = static_cast<size_t>( &e - existing.data() );
                                                 return !reused[i] && e.Owner == owner &&
                                                        e.TypeIndex == fresh[f].TypeIndex &&
                                                        e.Cell == fresh[f].Cell;
                                             } );
            if ( match == existing.end() )
            {
                plan.Create.push_back( f );
                continue;
            }
            const auto i = static_cast<size_t>( match - existing.begin() );
            reused[i]    = true;
            plan.Rewrite.emplace_back( i, f );
        }
        for ( size_t i = 0; i < existing.size(); ++i )
            if ( !reused[i] && existing[i].Owner == owner )
                plan.Remove.push_back( i );
        return plan;
    }
} // namespace Desert::World::Foliage::Procedural
