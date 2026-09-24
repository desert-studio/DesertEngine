#pragma once

// WHERE A STREAMED WORLD'S RECORDS COME FROM, SO THE STREAMER DOES NOT KNOW.
//
// The streamer (Engine/Core/WorldStreamer.hpp) turns one residency unit's records into entities. Today those
// records are the Play snapshot, parsed once and held in memory; with WP9 they are a cell file read through the
// VFS on a worker (the cells Tools/WorldCook writes, Engine/Core/Serialize/WorldCells.hpp). The streamer asks
// this interface for a unit and gets records back — the ONE place the two sources differ, so switching to files
// replaces an implementation and not the streamer.
//
// A unit's records are ResidencyUnitMembers( plan, unit ), in that order: the executor names records by that
// order, and a source that answered in another one would give an entity another record's id.

#include <Engine/Core/Serialize/WorldPartitionResidencyRules.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Desert::Core::Rules
{
    class WorldCellSource
    {
    public:
        virtual ~WorldCellSource() = default;

        // The records of residency unit @p unit, in ResidencyUnitMembers order. An error names the unit and why
        // (for a file source: which file, and what about it is wrong).
        //
        // CONST AND SAFE FROM ANY THREAD: the streamer calls it on JobSystem workers, several at once.
        [[nodiscard]] virtual Common::ResultStr<std::vector<Assets::EntityData>>
        UnitRecords( std::size_t unit ) const = 0;
    };

    // The records are already in memory: a unit is a copy of its members. Holds a VIEW of @p records, which must
    // outlive it (the streamer owns both).
    class MemoryCellSource final : public WorldCellSource
    {
    public:
        MemoryCellSource( const WorldPartitionPlan& plan, std::span<const Assets::EntityData> records )
             : m_Records( records )
        {
            m_Units.resize( ResidencyUnitCount( plan ) );
            for ( std::size_t unit = 0; unit < m_Units.size(); ++unit )
                m_Units[unit] = ResidencyUnitMembers( plan, unit );
        }

        [[nodiscard]] Common::ResultStr<std::vector<Assets::EntityData>>
        UnitRecords( std::size_t unit ) const override
        {
            using Result = std::vector<Assets::EntityData>;
            if ( unit >= m_Units.size() )
                return Common::MakeError<Result>( "memory cell source: unit " + std::to_string( unit ) + " of " +
                                                  std::to_string( m_Units.size() ) + " does not exist" );
            Result out;
            out.reserve( m_Units[unit].size() );
            for ( const std::size_t record : m_Units[unit] )
            {
                if ( record >= m_Records.size() )
                    return Common::MakeError<Result>( "memory cell source: unit " + std::to_string( unit ) +
                                                      " names record " + std::to_string( record ) + " of " +
                                                      std::to_string( m_Records.size() ) );
                out.push_back( m_Records[record] );
            }
            return Common::MakeSuccess( std::move( out ) );
        }

    private:
        std::span<const Assets::EntityData>   m_Records;
        std::vector<std::vector<std::size_t>> m_Units;
    };
} // namespace Desert::Core::Rules
