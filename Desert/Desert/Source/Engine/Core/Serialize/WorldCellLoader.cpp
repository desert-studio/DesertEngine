#include <Engine/Core/Serialize/WorldCellLoader.hpp>

#include <Engine/Core/Serialize/SceneDependencies.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/JobSystem.hpp>

#include <chrono>
#include <filesystem>
#include <utility>

namespace Desert::Core
{
    WorldCellLoader::WorldCellLoader( std::shared_ptr<const Rules::WorldCellSource> source )
         : m_Source( std::move( source ) )
    {
    }

    WorldCellLoader::~WorldCellLoader()
    {
        for ( Flight const& flight : m_Flights )
        {
            flight.Result.wait();
            m_Source->ReleaseUnit( flight.Unit );
        }
        for ( const auto& [unit, records] : m_Ready )
            m_Source->ReleaseUnit( unit );
    }

    void WorldCellLoader::Start( std::size_t unit, std::uint64_t ticket )
    {
        std::shared_ptr<const Rules::WorldCellSource> source = m_Source;
        Flight                                        flight;
        flight.Unit   = unit;
        flight.Ticket = ticket;
        flight.HoldTicks = m_DebugDelayTicks;
        m_Source->HoldUnit( unit ); // released by Unload, or when the load turns out cancelled or failed
        // The assets root is resolved here, on the thread that owns the paths; the worker only reads under it.
        std::filesystem::path assetsRoot = Common::Constants::Path::FullPath(
             Common::Constants::Path::Dir( Common::Constants::Path::ContentDir::Assets ) );
        flight.Result = Common::JobSystem::Get().Async(
             [source = std::move( source ), assetsRoot = std::move( assetsRoot ), unit]() noexcept
             {
                 const auto started = std::chrono::steady_clock::now();
                 Read       read;
                 auto       records = source->UnitRecords( unit );
                 if ( records )
                 {
                     read.Records = records.ExtractValue();
                     // A prefab link that states no GUID is refused by name when the record is made
                     // (InstantiateRecords); here it simply names nothing to warm.
                     read.Dependencies = GatherSceneDependencies( *read.Records, nullptr, assetsRoot ).Guids;
                 }
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
        m_ReadyDependencies.erase( unit );
        if ( m_Ready.erase( unit ) > 0 )
            m_Source->ReleaseUnit( unit );
    }

    std::vector<Rules::LoadOutcome> WorldCellLoader::TakeFinished()
    {
        std::vector<Rules::LoadOutcome> finished;
        std::vector<Flight>             still;
        for ( Flight& flight : m_Flights )
        {
            if ( flight.HoldTicks > 0 )
            {
                --flight.HoldTicks;
                still.push_back( std::move( flight ) );
                continue;
            }
            if ( flight.Result.wait_for( std::chrono::seconds( 0 ) ) != std::future_status::ready )
            {
                still.push_back( std::move( flight ) );
                continue;
            }
            Read read = flight.Result.get();
            m_WorkerMs += read.Ms;
            if ( flight.Cancelled )
            {
                m_Source->ReleaseUnit( flight.Unit );
                continue;
            }
            if ( read.Records.has_value() )
            {
                m_Ready[flight.Unit]             = std::move( *read.Records );
                m_ReadyDependencies[flight.Unit] = std::move( read.Dependencies );
                finished.push_back( { flight.Unit, flight.Ticket, true, {} } );
            }
            else
            {
                m_Source->ReleaseUnit( flight.Unit );
                finished.push_back( { flight.Unit, flight.Ticket, false, std::move( read.Error ) } );
            }
        }
        m_Flights = std::move( still );
        return finished;
    }

    const std::vector<Assets::EntityData>* WorldCellLoader::Records( std::size_t unit ) const
    {
        const auto held = m_Ready.find( unit );
        return held != m_Ready.end() ? &held->second : nullptr;
    }

    const std::vector<std::string>* WorldCellLoader::Dependencies( std::size_t unit ) const
    {
        const auto held = m_ReadyDependencies.find( unit );
        return held != m_ReadyDependencies.end() ? &held->second : nullptr;
    }
} // namespace Desert::Core
