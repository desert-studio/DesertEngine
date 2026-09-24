#pragma once

// A PARTITIONED WORLD'S CELLS, READ OFF THE MAIN THREAD.
//
// The residency executor asks for a unit's records with StartLoad and is told the outcome on a later frame
// (WorldPartitionResidencyExecutor.hpp, "the load port"). This is the port's asynchronous half: StartLoad hands
// the read AND THE PARSE — a cell file is JSON, and the parse is most of its cost — to a JobSystem worker and
// returns at once; TakeFinished, called on the main thread at the start of a tick, collects every read that
// has finished without waiting for one that has not. What the main thread is left with is the part that must
// be on it: SceneSerializer::InstantiateRecords, in the streamer's Activate.
//
// Pure of the Scene, so the suite WorldCells drives it with a source that blocks on a latch and proves that
// the main thread does not wait for it.

#include <Engine/Core/Serialize/WorldCellSource.hpp>
#include <Engine/Core/Serialize/WorldPartitionResidencyRules.hpp>

#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::Core
{
    class WorldCellLoader
    {
    public:
        // @p source is shared with the workers: a read in flight when this is destroyed still has it.
        explicit WorldCellLoader( std::shared_ptr<const Rules::WorldCellSource> source );
        // Waits for the reads in flight: they write into futures this owns.
        ~WorldCellLoader();
        WorldCellLoader( const WorldCellLoader& )            = delete;
        WorldCellLoader& operator=( const WorldCellLoader& ) = delete;

        // Queues unit @p unit's read on a worker and returns without reading anything.
        void Start( std::size_t unit, std::uint64_t ticket );
        // The load of @p ticket is no longer wanted: when it finishes, its records are dropped and nothing is
        // reported for it.
        void Cancel( std::size_t unit, std::uint64_t ticket );
        // Frees the records a finished load of @p unit holds.
        void Unload( std::size_t unit );

        // THE READS THAT HAVE FINISHED, WITHOUT WAITING FOR ANY OTHER: an Ok outcome for each whose records are
        // now held, a failed one (with the source's reason) for each that could not be read.
        [[nodiscard]] std::vector<Rules::LoadOutcome> TakeFinished();

        // The records of @p unit's finished load, or null when there is none.
        [[nodiscard]] const std::vector<Assets::EntityData>* Records( std::size_t unit ) const;

        [[nodiscard]] std::size_t InFlight() const
        {
            return m_Flights.size();
        }
        // Summed over every finished read: what the workers spent reading and parsing, which the main
        // thread did not.
        [[nodiscard]] double WorkerMs() const
        {
            return m_WorkerMs;
        }

    private:
        struct Read
        {
            std::optional<std::vector<Assets::EntityData>> Records;
            std::string                                    Error;
            double                                         Ms = 0.0;
        };
        struct Flight
        {
            std::size_t       Unit      = 0;
            std::uint64_t     Ticket    = 0;
            bool              Cancelled = false;
            std::future<Read> Result;
        };

        std::shared_ptr<const Rules::WorldCellSource>                    m_Source;
        std::vector<Flight>                                              m_Flights;
        std::unordered_map<std::size_t, std::vector<Assets::EntityData>> m_Ready;
        double                                                           m_WorkerMs = 0.0;
    };
} // namespace Desert::Core
