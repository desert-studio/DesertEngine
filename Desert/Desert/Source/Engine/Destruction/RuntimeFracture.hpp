#pragma once

// FRACTURES MADE AT RUN TIME (DST-RUNTIME, owner decision 10-13): the On Load and At Impact sources of a
// destructible. The cut is FractureBake's own (BakeFracture), run on a JobSystem worker, and its result is the
// FractureData a `.dfrac` decodes to — one format, one simulation (DestructionWorld); only where the pieces come
// from differs. The frame never waits for a cut: Poll answers "pending" until the worker is done, and the object
// stays whole meanwhile.
//
// Where UE stops: Chaos simulates only Geometry Collections fractured in the editor beforehand; a cut per
// instance (two identical crates breaking differently) or centred on the hit does not exist there.
//
// LIMITS. At most kMaxRuntimeSites sites per cut (the pieces a cut makes are bounded by its sites times the
// islands each cell meets; the caller refuses a component asking for more), and at most MaxConcurrent() cuts on
// the workers at once — the rest wait in submission order, so a burst of hits cannot take every core from the
// frame's own jobs.
//
// SEEDS. The caller derives each cut's seed from the entity's UUID (and, At Impact, the hit point in the
// fracture's own space): the same object hit at the same place cuts the same pieces on every machine and every
// replay, since the bake is deterministic by seed (FractureBake.hpp).

#include <Engine/Destruction/FractureFormat.hpp>
#include <Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::Destruction
{
    inline constexpr int kMaxRuntimeSites = 128;

    /// The source geometry of a run-time cut, made ON THE WORKER (a file read and lift, or a generator).
    using RuntimeSourceMaker = std::function<Common::ResultStr<std::shared_ptr<const Geometry::DynamicMesh3>>()>;

    struct RuntimeFractureRequest
    {
        /// What the source is (an asset file, a primitive, an editable mesh): requests naming the same source
        /// share one lift of it.
        std::string        SourceKey;
        RuntimeSourceMaker MakeSource;
        FractureSettings   Settings;
        /// Carried into the FractureData as a `.dfrac` carries them (the piece draw's material rules).
        Common::Content::AssetGuid SourceMesh;
        Common::Content::AssetGuid InteriorMaterial;
    };

    /// One cut's outcome and the worker time it took.
    struct RuntimeFractureResult
    {
        std::shared_ptr<const FractureData> Data;
        double                              BakeMs = 0.0;
    };

    class RuntimeFractureQueue
    {
    public:
        using Ticket = uint64_t;

        RuntimeFractureQueue();
        ~RuntimeFractureQueue();

        RuntimeFractureQueue( const RuntimeFractureQueue& )            = delete;
        RuntimeFractureQueue& operator=( const RuntimeFractureQueue& ) = delete;

        /// Queues @p request; it starts at once when a worker slot is free, else on a later Poll.
        Ticket Submit( RuntimeFractureRequest request );
        /// Success with a null Data while pending; the result once (the ticket is then forgotten); an error once.
        Common::ResultStr<RuntimeFractureResult> Poll( Ticket ticket );
        /// Forgets @p ticket: a queued cut never starts, a running one finishes into nothing.
        void Cancel( Ticket ticket );

        /// How many cuts may run on the workers at once: half the pool, at least one.
        [[nodiscard]] static uint32_t MaxConcurrent();
        [[nodiscard]] uint32_t        Running() const
        {
            return static_cast<uint32_t>( m_Running.size() + m_Cancelled.size() );
        }

    private:
        struct SourceCache
        {
            std::mutex                                                                     Mutex;
            std::unordered_map<std::string, std::shared_ptr<const Geometry::DynamicMesh3>> Meshes;
        };

        void Start();

        Ticket                                                                            m_Next = 1;
        std::deque<std::pair<Ticket, RuntimeFractureRequest>>                             m_Queued;
        std::unordered_map<Ticket, std::future<Common::ResultStr<RuntimeFractureResult>>> m_Running;
        // Cancelled while running: still on a worker, so still counted against the limit until done.
        std::vector<std::future<Common::ResultStr<RuntimeFractureResult>>> m_Cancelled;
        std::shared_ptr<SourceCache>                                       m_Sources;
    };

    /// The cut itself, on the calling thread: the source, the bake, the FractureData. What a worker runs.
    [[nodiscard]] Common::ResultStr<RuntimeFractureResult>
    BakeRuntimeFracture( const Geometry::DynamicMesh3& source, const RuntimeFractureRequest& request );

    /// SplitMix64 of @p a then @p b: the seed of a run-time cut from its parts.
    [[nodiscard]] uint64_t MixSeed( uint64_t a, uint64_t b );
} // namespace Desert::Destruction
