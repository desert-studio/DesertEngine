#include "ServerWorld.hpp"

#include <Engine/Core/Serialize/WorldPartitionResidencyRules.hpp>

#include <format>
#include <utility>

namespace Desert::Server
{
    ServerWorld::ServerWorld( Core::CookedWorldStart world ) : m_World( std::move( world ) )
    {
    }

    Common::ResultStr<std::unique_ptr<ServerWorld>>
    ServerWorld::Begin( Core::CookedWorldStart world, const Core::Rules::ResidencySettings& settings )
    {
        using Result = std::unique_ptr<ServerWorld>;
        // Every read of `world` is resolved before the plan is moved out of it: an argument list is not
        // sequenced (clang evaluates it left to right, MSVC right to left).
        const std::size_t recordCount  = world.Indexed.RecordIds.size();
        auto              observations = world.Indexed.Observations;
        auto              plan         = std::move( world.Indexed.Plan );
        auto              partition    = world.Index->WorldPartition;
        Result            server( new ServerWorld( std::move( world ) ) );
        auto              begun = Core::Rules::ResidencyExecutor::BeginFromAlwaysLoaded(
             std::move( plan ), std::move( partition ), settings, recordCount, observations );
        if ( !begun )
            return Common::MakeError<Result>( "server world '" + server->m_World.Index->SceneName +
                                              "': " + begun.GetError() );
        server->m_Executor.emplace( begun.ExtractValue() );
        server->m_Held.assign( recordCount, false );
        // The same cell source the client streams through (CookedFileReader over the cooked directory), read on
        // JobSystem workers: a server step never waits on a disk read.
        server->m_Loader =
             std::make_unique<Core::WorldCellLoader>( std::make_shared<Core::WorldCells::CookedCellSource>(
                  *server->m_World.Index,
                  Core::CookedFileReader( server->m_World.Directory, server->m_World.RegionChunkOfFile ) ) );
        return Common::MakeSuccess( std::move( server ) );
    }

    Common::ResultStr<Core::Rules::ResidencyTick>
    ServerWorld::Step( std::span<const Core::Rules::StreamingSource> sources, double nowSeconds )
    {
        return m_Executor->Tick( sources, nowSeconds, *this );
    }

    std::vector<ResidentCell> ServerWorld::ResidentCells() const
    {
        std::vector<ResidentCell>              cells;
        const Core::Rules::WorldPartitionPlan& plan  = m_Executor->Plan();
        const auto&                            units = m_Executor->State().Units;
        for ( std::size_t unit = plan.AlwaysLoaded.size(); unit < units.size(); ++unit )
            if ( units[unit].State == Core::Rules::Residency::Activated )
                cells.push_back( { unit, m_World.Index->Units.at( unit ).Name,
                                   Core::Rules::ResidencyUnitRecords( plan, unit ) } );
        return cells;
    }

    Common::BoolResultStr ServerWorld::Activate( std::size_t unit, std::size_t firstMember,
                                                 std::span<const std::size_t> records )
    {
        // StepResidency lets through only a Loaded unit, and a unit is Loaded only once its read was reported Ok.
        const std::vector<Assets::EntityData>* unitRecords = m_Loader->Records( unit );
        if ( unitRecords == nullptr )
            return Common::MakeError( std::format( "server world '{}': unit {} is activated with no finished read "
                                                   "of its records",
                                                   m_World.Index->SceneName, unit ) );
        const std::size_t planned = Core::Rules::ResidencyUnitRecords( m_Executor->Plan(), unit );
        if ( unitRecords->size() != planned || firstMember + records.size() > planned )
            return Common::MakeError( std::format( "server world '{}': unit {} came back with {} record(s), the "
                                                   "plan holds {} (slice {}..{})",
                                                   m_World.Index->SceneName, unit, unitRecords->size(), planned,
                                                   firstMember, firstMember + records.size() ) );
        for ( const std::size_t record : records )
            m_Held.at( record ) = true;
        return Common::MakeSuccess( true );
    }

    void ServerWorld::Destroy( std::span<const std::size_t> records )
    {
        for ( const std::size_t record : records )
            m_Held.at( record ) = false;
    }

    void ServerWorld::StartLoad( std::size_t unit, std::uint64_t ticket )
    {
        m_Loader->Start( unit, ticket );
    }

    void ServerWorld::CancelLoad( std::size_t unit, std::uint64_t ticket )
    {
        m_Loader->Cancel( unit, ticket );
    }

    void ServerWorld::Unload( std::size_t unit )
    {
        m_Loader->Unload( unit );
    }

    std::vector<Core::Rules::LoadOutcome> ServerWorld::TakeLoadOutcomes()
    {
        return m_Loader->TakeFinished();
    }
} // namespace Desert::Server
