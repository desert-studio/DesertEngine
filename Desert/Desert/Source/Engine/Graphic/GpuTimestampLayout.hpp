#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Desert::Graphic
{
    // ------------------------------------------------------------------------------------------------
    // Where each GPU timestamp lives in a frame's query pool, how big that pool has to be, which scope
    // encloses which, and how a nested measurement is turned into a breakdown that adds up. Pure functions
    // of integers and strings, outside the Vulkan class that uses them, for the same reason
    // RenderGraphSort.hpp sits outside RenderGraphBuilder: VulkanGpuProfiler cannot be constructed without
    // a device, so none of this would otherwise be assertable.
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
     */
    class GpuScopeRecorder
    {
    public:
        struct Scope
        {
            std::string Name;
            // Index of the enclosing scope OF THE SAME OWNER, or kGpuNoParent. Always an earlier index,
            // which is what GpuSelfTimes requires.
            int32_t Parent = kGpuNoParent;
            // Which of m_Stacks this scope was pushed on, so End pops the right one even if the active
            // view has changed in between.
            uint32_t Stack = 0;
        };

        // Starts a frame with room for @p capacityScopes.
        void Reset( uint32_t capacityScopes )
        {
            m_Capacity  = capacityScopes;
            m_Requested = 0;
            m_Scopes.clear();
            m_Stacks.clear();
        }

        // The scope's index (its queries are GpuScopeQueryBase( index ) and the next), or -1 when the pool
        // is full. A refused scope still counts towards Requested(), which is what grows the pool.
        [[nodiscard]] int32_t Begin( std::string name, const void* owner )
        {
            ++m_Requested;
            if ( m_Scopes.size() >= m_Capacity )
                return -1;

            const uint32_t stackIndex = StackOf( owner );
            auto&          stack      = m_Stacks[stackIndex].second;

            const auto index = static_cast<int32_t>( m_Scopes.size() );
            m_Scopes.push_back(
                 Scope{ std::move( name ), stack.empty() ? kGpuNoParent : stack.back(), stackIndex } );
            stack.push_back( index );
            return index;
        }

        // Closes @p index. RAII scopes close innermost-first within their own view, so the top of that
        // view's stack is the scope being closed; -1 (a refused scope) is ignored.
        void End( int32_t index )
        {
            if ( index < 0 || static_cast<size_t>( index ) >= m_Scopes.size() )
                return;
            auto& stack = m_Stacks[m_Scopes[static_cast<size_t>( index )].Stack].second;
            if ( !stack.empty() && stack.back() == index )
                stack.pop_back();
        }

        [[nodiscard]] const std::vector<Scope>& Scopes() const noexcept
        {
            return m_Scopes;
        }

        /// Scopes asked for this frame, refused ones included — the number the pool must grow to.
        [[nodiscard]] uint32_t Requested() const noexcept
        {
            return m_Requested;
        }

        [[nodiscard]] uint32_t Capacity() const noexcept
        {
            return m_Capacity;
        }

    private:
        uint32_t StackOf( const void* owner )
        {
            // Linear: a frame has a handful of views, and a hash map would allocate every frame.
            for ( uint32_t i = 0; i < m_Stacks.size(); ++i )
                if ( m_Stacks[i].first == owner )
                    return i;
            m_Stacks.emplace_back( owner, std::vector<int32_t>{} );
            return static_cast<uint32_t>( m_Stacks.size() - 1 );
        }

        uint32_t                                                  m_Capacity  = 0;
        uint32_t                                                  m_Requested = 0;
        std::vector<Scope>                                        m_Scopes;
        std::vector<std::pair<const void*, std::vector<int32_t>>> m_Stacks;
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
    [[nodiscard]] inline std::vector<double> GpuSelfTimes( const std::vector<double>&  inclusiveMs,
                                                           const std::vector<int32_t>& parents )
    {
        std::vector<double> self = inclusiveMs;

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

        return self;
    }
} // namespace Desert::Graphic
