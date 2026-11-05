#pragma once

#include <Common/Core/GpuQueue.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Desert::Graphic
{
    // ------------------------------------------------------------------------------------------------
    // Where each GPU timestamp lives in a frame's query pool, how big that pool has to be, which scope
    // encloses which, and how a nested measurement is turned into a breakdown that adds up. Pure functions
    // of integers and strings, outside the Vulkan class that uses them: VulkanGpuProfiler cannot be
    // constructed without a device, so none of this would otherwise be assertable.
    //
    // Four relations live here:
    //
    //   1. ONE POOL PER FRAME IN FLIGHT, HANDED OUT LINEARLY. Query 0/1 bracket the whole command buffer;
    //      scope i owns queries 2+2i and 3+2i, in the order scopes open, whichever view opens them. There
    //      is no view dimension in the layout at all: views are unbounded, so a block per view would put a
    //      ceiling back on the number of views the profiler can see.
    //
    //   2. THE POOL GROWS, NEVER MID-FRAME. A frame that asks for more scopes than its pool holds drops the
    //      extra ones and raises the high-water mark; the pool is replaced when that frame index next
    //      begins, where its old queries have been read and nothing is recording into it. Growth is
    //      geometric so a slowly rising scope count reallocates a logarithmic number of times, not once per
    //      new scope.
    //
    //   3. A PARENT IS IN THE SAME VIEW. Scopes of different views interleave in one list, but nesting is
    //      tracked per view (the ActiveViewScope's target at BeginScope), so the preview's
    //      "VolumetricClouds" can never be charged to the viewport's open "SceneRenderer::OnUpdate".
    //      And in the same QUEUE: a scope is a stretch of its own queue's timeline, so an async compute
    //      segment recorded while a graphics scope is open (the graphics queue's wait across that segment,
    //      VulkanRdgBackend) is that queue's root, never subtracted from the graphics scope's self time.
    //
    //   4. SELF TIMES PARTITION THE ROOT. Passes nest — the cloud march sits inside "Clouds:
    //      ExecuteInFrame" inside "VolumetricClouds" — so adding up the inclusive times counts the same
    //      microseconds three times. The first breakdown ever printed by this feature summed to 159% of
    //      its own frame for exactly that reason. Subtracting each scope's DIRECT children makes the
    //      remainder a partition, and that is a property worth asserting rather than eyeballing.
    // ------------------------------------------------------------------------------------------------

    /// The pair bracketing the whole command buffer: begin at 0, end at 1.
    inline constexpr uint32_t kGpuFrameTotalQuery = 0;

    /// First query of the scope that opened @p scopeIndex-th in the frame; its end is the next query.
    [[nodiscard]] constexpr uint32_t GpuScopeQueryBase( uint32_t scopeIndex )
    {
        return 2 + scopeIndex * 2;
    }

    /// Queries a frame needs for @p scopeCount scopes plus its whole-frame bracket.
    [[nodiscard]] constexpr uint32_t GpuQueriesForScopes( uint32_t scopeCount )
    {
        return GpuScopeQueryBase( scopeCount );
    }

    /// How many scopes a pool of @p queryCount queries can time, after the whole-frame bracket.
    [[nodiscard]] constexpr uint32_t GpuScopeCapacity( uint32_t queryCount )
    {
        return queryCount < 2 ? 0 : ( queryCount - 2 ) / 2;
    }

    /// Scopes the first pool of each frame is sized for. The editor's frame has ~30 pass-level scopes per
    /// view, so one view fits from the first frame and every further view costs at most one growth.
    inline constexpr uint32_t kGpuInitialScopeCapacity = 64;

    /// Pool sizes are rounded up to this many queries, so a count that creeps up by one scope does not
    /// produce a pool of an odd size that is outgrown again the next frame.
    inline constexpr uint32_t kGpuQueryGranularity = 64;

    // The query count a frame's pool must have once the busiest frame so far recorded @p highWaterScopes
    // scopes. Returns @p currentQueries unchanged when that already fits — the caller replaces the pool
    // exactly when the result differs. Never shrinks: a view closed for a moment would otherwise make the
    // next frame drop scopes and grow again.
    [[nodiscard]] constexpr uint32_t GpuGrownQueryCount( uint32_t currentQueries, uint32_t highWaterScopes )
    {
        const uint32_t required = GpuQueriesForScopes( highWaterScopes );
        if ( required <= currentQueries )
            return currentQueries;

        const uint32_t geometric = currentQueries + currentQueries / 2;
        const uint32_t target    = required > geometric ? required : geometric;
        return ( target + kGpuQueryGranularity - 1 ) / kGpuQueryGranularity * kGpuQueryGranularity;
    }

    // The profiler row a scope lands in. A scope recorded inside a view is prefixed with the view's name so
    // that eight open views are eight sets of rows rather than one row averaging all of them; a scope
    // recorded in the frame context (UI, bakes, the whole-frame bracket) keeps its bare name.
    [[nodiscard]] inline std::string GpuViewScopeName( std::string_view viewName, std::string_view scope )
    {
        if ( viewName.empty() )
            return std::string( scope );

        std::string name;
        name.reserve( viewName.size() + 3 + scope.size() );
        name.append( viewName ).append( " | " ).append( scope );
        return name;
    }

    /// A scope with no enclosing scope.
    inline constexpr int32_t kGpuNoParent = -1;

    /**
     * @brief One frame's scopes, in the order they opened, with their nesting tracked per view.
     *
     * The bookkeeping half of the profiler: VulkanGpuProfiler owns one per frame in flight and writes the
     * timestamps at GpuScopeQueryBase( index ) for whatever index Begin hands out. Owners are opaque
     * addresses (the active ViewResources, or the frame context) used only as keys, never dereferenced, so
     * a view destroyed before its frame is resolved costs nothing here.
     *
     * THE KEY. @p Key is what a scope is named by: the profiler's row id (uint32_t, VulkanGpuProfiler - no string
     * is built or copied per scope) or its text (std::string, the default: tests and tools).
     *
     * THREADS (CPU-ONE-SYNC). The scope array is allocated to its capacity at Reset and a scope's slot is claimed
     * with one atomic add, so a graph chunk recording on a task worker (BeginUnder / EndUnder) takes no lock: it
     * writes only its own slot and its chunk's `open` list. Everything else - the shared per-(view, queue) stacks
     * (Begin / End / ChunkRoot), the queue, the order clock, SetOrder and Retract - belongs to ONE thread at a
     * time (the thread walking the graph; the caller serialises it). Scopes() / LiveParent are read after every
     * chunk of the frame has been joined (the resolve, frames later).
     */
    // Where a chunk's scopes hang: its (view, queue) stack, the scope open on it, and the queue.
    // RDG-PARALLEL-1-1: taken on the dispatching thread when the chunks are begun (ChunkRoot), because the
    // chunks now record WHILE that thread walks the graph - its stack and its queue (SetQueue at an async
    // segment) change under them, and a chunk is on its own pipe whatever segment the dispatching thread is
    // in. Not a member type of the recorder, so every key's recorder hands out the same root.
    struct GpuChunkRootScope
    {
        uint32_t                    Stack  = 0;
        int32_t                     Parent = kGpuNoParent;
        Common::Profiling::GpuQueue Queue  = Common::Profiling::GpuQueue::Graphics;
    };

    template <typename Key = std::string>
    class GpuScopeRecorder
    {
    public:
        using ChunkRootScope = GpuChunkRootScope;

        struct Scope
        {
            Key Name{};
            // Index of the enclosing scope OF THE SAME OWNER, or kGpuNoParent. Always an earlier index,
            // which is what GpuSelfTimes requires.
            int32_t Parent = kGpuNoParent;
            // Which of m_Stacks this scope was pushed on, so End pops the right one even if the active
            // view has changed in between.
            uint32_t Stack = 0;
            // The queue the scope's timestamps were written on (SetQueue at the time it opened): the
            // timeline its interval belongs to (GpuCoverage).
            Common::Profiling::GpuQueue Queue = Common::Profiling::GpuQueue::Graphics;
            // RDG-PARALLEL-1: the scope's place in the frame's recording order (the [GpuOrder] trace sorts by it):
            // Begin takes the next tick << 24; a chunk's scopes take its reserved tick << 24 | its count.
            uint64_t Order = 0;
            // RDG-PARALLEL-5: a scope of a chunk the graph discarded (its secondaries never run, and its passes
            // are recorded again inline as scopes of their own). It is no row of the frame: not in the [GpuOrder]
            // trace, the tables or the coverage, and no scope's parent (LiveParent).
            bool Retracted = false;
        };

        // Starts a frame with room for @p capacityScopes.
        void Reset( uint32_t capacityScopes )
        {
            m_Capacity = capacityScopes;
            m_Requested.Value.store( 0, std::memory_order_relaxed );
            m_Clock = 0;
            // Grows, never shrinks: the slots are claimed in place (Claim), so the array is never resized while
            // a frame records, and a frame start allocates nothing once the pool has stopped growing.
            if ( m_Scopes.size() < capacityScopes )
                m_Scopes.resize( capacityScopes );
            m_Stacks.clear();
            m_Queue = Common::Profiling::GpuQueue::Graphics;
        }

        // The queue every scope opened from now on is recorded on, until the next call or Reset.
        void SetQueue( Common::Profiling::GpuQueue queue ) noexcept
        {
            m_Queue = queue;
        }

        // The scope's index (its queries are GpuScopeQueryBase( index ) and the next), or -1 when the pool
        // is full. A refused scope still counts towards Requested(), which is what grows the pool.
        // A scope opened DIRECTLY inside an open scope of the same name and owner is that scope again - the render
        // graph times every node by its name (VulkanRdgBackend::BeginPass) and a node's record body may open its own
        // DESERT_PROFILE_PASS with the same name: it is not timed twice (-1, as a refused scope), so one pass is
        // one row of one count, not "x2" at twice its time (FPS-MAC: "Clouds: ShadowMap" / "Sky: SkyViewLut").
        [[nodiscard]] int32_t Begin( Key name, const void* owner )
        {
            const uint32_t stackIndex = StackOf( owner, m_Queue );
            auto&          stack      = m_Stacks[stackIndex].Open;
            if ( !stack.empty() && m_Scopes[static_cast<size_t>( stack.back() )].Name == name )
                return -1;
            const int32_t index = Claim();
            if ( index < 0 )
                return -1;

            const int32_t parent = stack.empty() ? kGpuNoParent : stack.back();
            Scope&        scope  = m_Scopes[static_cast<size_t>( index )];
            scope                = Scope{ std::move( name ), parent, stackIndex, m_Queue };
            scope.Order          = m_Clock++ << 24;
            stack.push_back( index );
            return index;
        }

        // Closes @p index. RAII scopes close innermost-first within their own view, so the top of that
        // view's stack is the scope being closed; -1 (a refused scope) is ignored.
        // RDG-PARALLEL-1 (graph chunks recorded on task workers). @p count ticks of the recording order, in chunk
        // order, reserved before the chunks record: chunk i's scopes sort after everything begun before the
        // reservation and before everything begun after it, whichever worker records first.
        [[nodiscard]] uint64_t ReserveOrder( uint32_t count ) noexcept
        {
            const uint64_t first = m_Clock;
            m_Clock += count;
            return first;
        }
        // Where a chunk's scopes hang (GpuChunkRootScope), taken on the dispatching thread.
        [[nodiscard]] ChunkRootScope ChunkRoot( const void* owner, Common::Profiling::GpuQueue queue )
        {
            const uint32_t stackIndex = StackOf( owner, queue );
            const auto&    shared     = m_Stacks[stackIndex].Open;
            ChunkRootScope root{ stackIndex, kGpuNoParent, queue };
            if ( !shared.empty() )
                root.Parent = shared.back();
            return root;
        }
        // A scope opened on a chunk's thread: it nests under @p open (the chunk's own open scopes), the first
        // under
        // @p root (ChunkRoot), and is never pushed onto the shared stack. @p order: the chunk's tick << 24 | the
        // chunk's scope count. LOCK-FREE: any number of chunk threads at once, and the dispatching thread's Begin
        // beside them; each writes only the slot it claimed. The parent's slot was written before this chunk
        // started (the root, on the dispatching thread) or by this chunk itself.
        [[nodiscard]] int32_t BeginUnder( Key name, const ChunkRootScope& root, std::vector<int32_t>& open,
                                          uint64_t order )
        {
            const int32_t parent = !open.empty() ? open.back() : root.Parent;
            if ( parent != kGpuNoParent && m_Scopes[static_cast<size_t>( parent )].Name == name )
                return -1;
            const int32_t index = Claim();
            if ( index < 0 )
                return -1;
            Scope& scope = m_Scopes[static_cast<size_t>( index )];
            scope        = Scope{ std::move( name ), parent, root.Stack, root.Queue };
            scope.Order  = order;
            open.push_back( index );
            return index;
        }
        // RDG-PARALLEL-3: scope @p index takes @p order (a chunk's scopes, re-stamped as the chunk is replayed).
        void SetOrder( int32_t index, uint64_t order ) noexcept
        {
            if ( index >= 0 && static_cast<uint32_t>( index ) < Count() )
                m_Scopes[static_cast<size_t>( index )].Order = order;
        }
        void EndUnder( int32_t index, std::vector<int32_t>& open )
        {
            if ( !open.empty() && open.back() == index )
                open.pop_back();
        }

        void End( int32_t index )
        {
            if ( index < 0 || static_cast<uint32_t>( index ) >= Count() )
                return;
            auto& stack = m_Stacks[m_Scopes[static_cast<size_t>( index )].Stack].Open;
            if ( !stack.empty() && stack.back() == index )
                stack.pop_back();
        }

        // RDG-PARALLEL-5: scope @p index belonged to a discarded chunk; -1 and out-of-range indices are ignored.
        // A chunk's scopes are never on a shared stack (BeginUnder), so no stack holds it.
        void Retract( int32_t index ) noexcept
        {
            if ( index >= 0 && static_cast<uint32_t>( index ) < Count() )
                m_Scopes[static_cast<size_t>( index )].Retracted = true;
        }

        // The nearest enclosing scope of @p index that was not retracted, or kGpuNoParent: what a scope nests
        // under once a discarded chunk's scopes are gone. Always an earlier index, as Scope::Parent is.
        [[nodiscard]] int32_t LiveParent( int32_t index ) const noexcept
        {
            if ( index < 0 || static_cast<uint32_t>( index ) >= Count() )
                return kGpuNoParent;
            int32_t parent = m_Scopes[static_cast<size_t>( index )].Parent;
            while ( parent != kGpuNoParent && m_Scopes[static_cast<size_t>( parent )].Retracted )
                parent = m_Scopes[static_cast<size_t>( parent )].Parent;
            return parent;
        }

        // The frame's scopes: the claimed slots, at most the capacity.
        [[nodiscard]] std::span<const Scope> Scopes() const noexcept
        {
            return std::span<const Scope>( m_Scopes.data(), Count() );
        }

        /// Scopes asked for this frame, refused ones included — the number the pool must grow to.
        [[nodiscard]] uint32_t Requested() const noexcept
        {
            return m_Requested.Value.load( std::memory_order_relaxed );
        }

        [[nodiscard]] uint32_t Capacity() const noexcept
        {
            return m_Capacity;
        }

    private:
        // The next slot, or -1 when the pool is full; a refused claim still counts (Requested).
        [[nodiscard]] int32_t Claim() noexcept
        {
            const uint32_t index = m_Requested.Value.fetch_add( 1, std::memory_order_relaxed );
            return index < m_Capacity ? static_cast<int32_t>( index ) : -1;
        }
        [[nodiscard]] uint32_t Count() const noexcept
        {
            const uint32_t requested = m_Requested.Value.load( std::memory_order_relaxed );
            return requested < m_Capacity ? requested : m_Capacity;
        }

        // One stack per (view, queue): see 3. above.
        uint32_t StackOf( const void* owner, Common::Profiling::GpuQueue queue )
        {
            // Linear: a frame has a handful of views, and a hash map would allocate every frame.
            for ( uint32_t i = 0; i < m_Stacks.size(); ++i )
                if ( m_Stacks[i].Owner == owner && m_Stacks[i].Queue == queue )
                    return i;
            m_Stacks.push_back( OwnerStack{ owner, queue, {} } );
            return static_cast<uint32_t>( m_Stacks.size() - 1 );
        }

        struct OwnerStack
        {
            const void*                 Owner = nullptr;
            Common::Profiling::GpuQueue Queue = Common::Profiling::GpuQueue::Graphics;
            std::vector<int32_t>        Open;
        };

        Common::Profiling::GpuQueue                               m_Queue = Common::Profiling::GpuQueue::Graphics;
        uint32_t                                                  m_Capacity  = 0;
        // Scopes asked for this frame: the claim counter (the slot index), refused ones included. Copyable so
        // the recorder stays a value (one per frame in flight, in a vector).
        struct Counter
        {
            std::atomic<uint32_t> Value{ 0 };
            Counter() = default;
            Counter( const Counter& other ) : Value( other.Value.load( std::memory_order_relaxed ) ) {}
            Counter& operator=( const Counter& other )
            {
                Value.store( other.Value.load( std::memory_order_relaxed ), std::memory_order_relaxed );
                return *this;
            }
        };
        Counter m_Requested;
        uint64_t m_Clock = 0; // the next recording-order tick
        std::vector<Scope>                                        m_Scopes;
        std::vector<OwnerStack>                                   m_Stacks;
    };

    // Turn inclusive per-scope times into EXCLUSIVE ones by subtracting each scope's direct children.
    //
    // @p inclusiveMs is indexed by scope; a negative entry marks a scope whose queries did not both land
    // and is passed through untouched (and contributes nothing to its parent). @p parents holds each
    // scope's parent index or kGpuNoParent, and must refer only to EARLIER indices — scopes are recorded
    // in the order they open, so a parent always precedes its children.
    //
    // The relation this exists for: for any tree whose children lie inside their parents, the self times
    // sum to the root's inclusive time. Nothing else in the breakdown may be summed.
    //
    // Written into @p self (CPU-ONE-SYNC: the profiler's resolve reuses one vector, no allocation per frame).
    inline void GpuSelfTimes( const std::vector<double>& inclusiveMs, const std::vector<int32_t>& parents,
                              std::vector<double>& self )
    {
        self.assign( inclusiveMs.begin(), inclusiveMs.end() );

        const size_t count = inclusiveMs.size() < parents.size() ? inclusiveMs.size() : parents.size();
        for ( size_t i = 0; i < count; ++i )
        {
            const int32_t parent = parents[i];
            if ( inclusiveMs[i] < 0.0 || parent == kGpuNoParent )
                continue;
            if ( static_cast<size_t>( parent ) < count && inclusiveMs[parent] >= 0.0 )
                self[parent] -= inclusiveMs[i];
        }

        // A parent whose children overlap it imperfectly can round below zero. Report zero rather than a
        // negative time: a negative would silently shrink the total and read as an unexplained gap.
        for ( size_t i = 0; i < self.size(); ++i )
            if ( inclusiveMs[i] >= 0.0 && self[i] < 0.0 )
                self[i] = 0.0;
    }

    // One scope's interval on its queue's timeline, in ms from the frame bracket's begin. A scope whose two
    // queries did not both land has Valid false and covers nothing.
    struct GpuInterval
    {
        double BeginMs = 0.0;
        double EndMs   = 0.0;
        bool   Valid   = false;
    };

    // One stretch of a span no member interval covers. After / Before are the scope that ended last before it
    // and the scope that began after it (indices into the interval list), kGpuNoParent at the span's edges.
    struct GpuUncovered
    {
        int32_t After  = kGpuNoParent;
        int32_t Before = kGpuNoParent;
        double  Ms     = 0.0;
    };

    struct GpuCoverage
    {
        double                    UncoveredMs = 0.0;
        std::vector<GpuUncovered> Gaps;
    };

    // What of [@p spanBeginMs, @p spanEndMs] the UNION of the @p members' intervals leaves uncovered, stretch by
    // stretch. A union, not a sum: nested and overlapping scopes cover a moment once, so the remainder is the
    // time no scope was around - work recorded outside every scope, barriers between them, and idle - and can
    // never go negative. Intervals are clipped to the span. The relation it exists for: UncoveredMs plus the
    // union's length is the span's length.
    //
    // NO ALLOCATION PER FRAME (CPU-ONE-SYNC): @p members is the caller's list, filtered and sorted IN PLACE (by
    // begin, then by index - the stable order of a list given in index order, without stable_sort's buffer, and
    // the same order again when the sorted list is passed once more), and @p coverage keeps its gaps' capacity.
    inline void GpuCoverageOf( const std::vector<GpuInterval>& intervals, std::vector<int32_t>& members,
                               const double spanBeginMs, const double spanEndMs, GpuCoverage& coverage )
    {
        coverage.UncoveredMs = 0.0;
        coverage.Gaps.clear();
        if ( spanEndMs <= spanBeginMs )
            return;

        std::erase_if( members,
                       [&]( int32_t index )
                       {
                           return index < 0 || static_cast<size_t>( index ) >= intervals.size() ||
                                  !intervals[static_cast<size_t>( index )].Valid;
                       } );
        std::sort( members.begin(), members.end(),
                   [&]( int32_t a, int32_t b )
                   {
                       const double beginA = intervals[static_cast<size_t>( a )].BeginMs;
                       const double beginB = intervals[static_cast<size_t>( b )].BeginMs;
                       return beginA != beginB ? beginA < beginB : a < b;
                   } );

        double  cursor    = spanBeginMs;
        int32_t lastEnded = kGpuNoParent;
        for ( const int32_t index : members )
        {
            const GpuInterval& interval = intervals[static_cast<size_t>( index )];
            const double       begin    = std::max( interval.BeginMs, spanBeginMs );
            const double       end      = std::min( interval.EndMs, spanEndMs );
            if ( end <= begin )
                continue;
            if ( begin > cursor )
            {
                coverage.Gaps.push_back( GpuUncovered{ lastEnded, index, begin - cursor } );
                coverage.UncoveredMs += begin - cursor;
            }
            if ( end > cursor )
            {
                cursor    = end;
                lastEnded = index;
            }
        }
        if ( spanEndMs > cursor )
        {
            coverage.Gaps.push_back( GpuUncovered{ lastEnded, kGpuNoParent, spanEndMs - cursor } );
            coverage.UncoveredMs += spanEndMs - cursor;
        }
    }
} // namespace Desert::Graphic
