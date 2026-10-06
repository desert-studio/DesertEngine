#include <Engine/World/Foliage/Procedural/ProceduralFoliageVolume.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace Desert::World::Foliage::Procedural
{
    std::vector<ProceduralFoliageDesired> DesiredInstancesInVolume( const ProceduralFoliageSpawner& spawner,
                                                                    const ProceduralFoliageBox&     volume,
                                                                    float                           tileOverlap )
    {
        const float     size = spawner.Settings().TileSize;
        const glm::vec2 lo{ volume.Min.x, volume.Min.z };
        const glm::vec2 hi{ volume.Max.x, volume.Max.z };
        const auto      layout = TileLayoutFor( lo, hi, size, tileOverlap );
        const glm::vec2 origin = glm::vec2( static_cast<float>( layout.BottomLeftX ),
                                            static_cast<float>( layout.BottomLeftY ) ) *
                                 size;

        std::vector<ProceduralFoliageDesired> desired;
        for ( const auto& placement : GenerateProceduralContent( spawner, layout, origin, tileOverlap ) )
        {
            // UE ExtractDesiredInstances: only what stands inside the volume's footprint.
            const glm::vec2 at = placement.Location;
            if ( at.x < lo.x || at.x > hi.x || at.y < lo.y || at.y > hi.y )
                continue;
            desired.push_back( ProceduralFoliageDesired{ placement, { at.x, volume.Max.y, at.y },
                                                         { at.x, volume.Min.y, at.y } } );
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
                return Common::MakeFormattedError<Fields>( "a placement names type {} of {}", d.Placement.TypeIndex,
                                                           typeCount );
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
