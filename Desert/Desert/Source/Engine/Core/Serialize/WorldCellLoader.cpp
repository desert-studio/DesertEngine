#include <Engine/Core/Serialize/WorldCellLoader.hpp>

#include <Common/Core/JobSystem.hpp>

#include <chrono>
#include <utility>

namespace Desert::Core
{
    WorldCellLoader::WorldCellLoader( std::shared_ptr<const Rules::WorldCellSource> source )
         : m_Source( std::move( source ) )
    {
    }

    WorldCellLoader::~WorldCellLoader()
    {
        for ( Flight& flight : m_Flights )
            flight.Result.wait();
    }

    void WorldCellLoader::Start( std::size_t unit, std::uint64_t ticket )
    {
        std::shared_ptr<const Rules::WorldCellSource> source = m_Source;
        Flight                                        flight;
        flight.Unit   = unit;
        flight.Ticket = ticket;
        flight.Result = Common::JobSystem::Get().Async(
             [source = std::move( source ), unit]
             {
                 const auto started = std::chrono::steady_clock::now();
                 Read       read;
                 auto       records = source->UnitRecords( unit );
                 if ( records )
                     read.Records = records.ExtractValue();
                 else
                     read.Error = records.GetError();
                 read.Ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - started )
                                .count();
                 return read;
             } );
        m_Flights.push_back( std::move( flight ) );
    }

    void WorldCellLoader::Cancel( std::size_t unit, std::uint64_t ticket )
    {
        for ( Flight& flight : m_Flights )
            if ( flight.Unit == unit && flight.Ticket == ticket )
                flight.Cancelled = true;
    }

    void WorldCellLoader::Unload( std::size_t unit )
    {
        m_Ready.erase( unit );
    }

    std::vector<Rules::LoadOutcome> WorldCellLoader::TakeFinished()
    {
        std::vector<Rules::LoadOutcome> finished;
        std::vector<Flight>             still;
        for ( Flight& flight : m_Flights )
        {
            if ( flight.Result.wait_for( std::chrono::seconds( 0 ) ) != std::future_status::ready )
            {
                still.push_back( std::move( flight ) );
                continue;
            }
            Read read = flight.Result.get();
            m_WorkerMs += read.Ms;
            if ( flight.Cancelled )
                continue;
            if ( read.Records.has_value() )
            {
                m_Ready[flight.Unit] = std::move( *read.Records );
                finished.push_back( { flight.Unit, flight.Ticket, true, {} } );
            }
            else
                finished.push_back( { flight.Unit, flight.Ticket, false, std::move( read.Error ) } );
        }
        m_Flights = std::move( still );
        return finished;
    }

    const std::vector<Assets::EntityData>* WorldCellLoader::Records( std::size_t unit ) const
    {
        const auto held = m_Ready.find( unit );
        return held != m_Ready.end() ? &held->second : nullptr;
    }
} // namespace Desert::Core
