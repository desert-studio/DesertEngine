// Pattern from UE 5.8 Engine/Source/Runtime/Foliage/Private/InstancedFoliage.cpp:3125-3140 (AInstancedFoliageActor::
// Get: the IFA for a location through UActorPartitionSubsystem) and :3222-3260 (FoliagePartitioningUtils::Update:
// instances whose location left the IFA's cell move to the IFA of their new cell), with
// Engine/Source/Runtime/Engine/Private/ActorPartition/ActorPartitionSubsystem.cpp:55-70 (the grid cell of a
// location). Not ported line by line: all of it hangs off UObject actors, levels and FFoliageInfo's component
// hash, none of which exist here. What is taken is the rule - an instance belongs to the field of the grid cell
// holding its location - over our CellOf, which the partitioner files the same point by.
#include <Engine/World/Foliage/FoliageCells.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace Desert::World::Foliage
{
    CellCoord FoliageCellOf( const glm::mat4& instance, double cellSize )
    {
        return Core::Rules::CellOf( instance[3].x, instance[3].z, cellSize );
    }

    glm::vec3 FoliageCellAnchor( const CellCoord& cell, double cellSize )
    {
        return { static_cast<float>( ( static_cast<double>( cell.X ) + 0.5 ) * cellSize ), 0.0f,
                 static_cast<float>( ( static_cast<double>( cell.Z ) + 0.5 ) * cellSize ) };
    }

    bool FoliageCellTouchesDisc( const CellCoord& cell, double cellSize, float x, float z, float radius )
    {
        const auto  square = Core::Rules::GridSquareOf( cell, cellSize );
        const float dx     = x - std::clamp( x, square.MinX, square.MaxX );
        const float dz     = z - std::clamp( z, square.MinZ, square.MaxZ );
        return dx * dx + dz * dz <= radius * radius;
    }

    Common::ResultStr<FoliageGathered> GatherFoliage( std::span<const FoliageCellField> fields )
    {
        FoliageGathered gathered;
        for ( std::size_t field = 0; field < fields.size(); ++field )
        {
            const auto offset = static_cast<std::uint32_t>( gathered.Instances.size() );
            for ( const std::uint32_t index : fields[field].Selected )
            {
                if ( index >= fields[field].Instances.size() )
                    return Common::MakeFormattedError<FoliageGathered>(
                         "foliage gather: field {} (cell {}, {}) selects instance {} of {}", field,
                         fields[field].Cell.X, fields[field].Cell.Z, index, fields[field].Instances.size() );
                gathered.Selected.push_back( offset + index );
            }
            gathered.Instances.insert( gathered.Instances.end(), fields[field].Instances.begin(),
                                       fields[field].Instances.end() );
        }
        return Common::MakeSuccess( std::move( gathered ) );
    }

    Common::ResultStr<std::vector<FoliageCellField>>
    ScatterFoliage( const FoliageGathered& gathered, std::span<const CellCoord> cells, double cellSize )
    {
        using Result = std::vector<FoliageCellField>;
        if ( !std::isfinite( cellSize ) || cellSize <= 0.0 )
            return Common::MakeFormattedError<Result>( "foliage scatter: cell size {} is not a positive number",
                                                       cellSize );

        // std::map keyed (X, Z): the new cells come out in one order whatever order the instances came in.
        std::map<std::pair<std::int32_t, std::int32_t>, std::size_t> slot;
        Result                                                       out;
        out.reserve( cells.size() );
        for ( const CellCoord& cell : cells )
        {
            if ( !slot.emplace( std::make_pair( cell.X, cell.Z ), out.size() ).second )
                return Common::MakeFormattedError<Result>( "foliage scatter: cell ({}, {}) is listed twice",
                                                           cell.X, cell.Z );
            out.push_back( FoliageCellField{ cell, {}, {} } );
        }

        std::vector<bool> selected( gathered.Instances.size(), false );
        for ( const std::uint32_t index : gathered.Selected )
        {
            if ( index >= gathered.Instances.size() )
                return Common::MakeFormattedError<Result>( "foliage scatter: selected instance {} of {}", index,
                                                           gathered.Instances.size() );
            selected[index] = true;
        }

        // Instances bound for cells nobody listed wait here so that they can be appended in (X, Z) order.
        std::map<std::pair<std::int32_t, std::int32_t>, FoliageCellField> fresh;
        for ( std::size_t i = 0; i < gathered.Instances.size(); ++i )
        {
            const CellCoord   cell = FoliageCellOf( gathered.Instances[i], cellSize );
            const auto        key  = std::make_pair( cell.X, cell.Z );
            FoliageCellField* into = nullptr;
            if ( const auto it = slot.find( key ); it != slot.end() )
                into = &out[it->second];
            else
            {
                auto& added = fresh[key];
                added.Cell  = cell;
                into        = &added;
            }
            if ( selected[i] )
                into->Selected.push_back( static_cast<std::uint32_t>( into->Instances.size() ) );
            into->Instances.push_back( gathered.Instances[i] );
        }
        for ( auto& [key, field] : fresh )
            out.push_back( std::move( field ) );
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::World::Foliage
