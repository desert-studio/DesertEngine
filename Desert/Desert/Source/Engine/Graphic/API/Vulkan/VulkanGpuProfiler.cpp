#include <Engine/Graphic/API/Vulkan/VulkanGpuProfiler.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Core/RenderTask.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/GpuTimestampLayout.hpp>
#include <Engine/Graphic/ViewResources.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#if DESERT_DEV_INSTRUMENTS

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        // vkGetQueryPoolResults with VK_QUERY_RESULT_WITH_AVAILABILITY_BIT writes two uint64 per query:
        // the value, then a non-zero availability word.
        constexpr uint32_t kWordsPerQuery = 2;

        // The draw split's first pool: groups, two queries each. A field frame records a few hundred mesh x
        // material runs and GPU bins across its mesh passes; past that the pool grows as the scope pool does.
        constexpr uint32_t kDrawSplitInitialGroups = 512;

        // The query count a draw pool needs once a frame asked for @p groups groups: unchanged when they fit,
        // otherwise geometric and rounded to the scope pool's granularity (GpuGrownQueryCount's rule).
        [[nodiscard]] uint32_t DrawSplitQueryCount( uint32_t current, uint32_t groups )
        {
            const uint32_t required = groups * 2;
            if ( required <= current )
                return current;
            const uint32_t geometric = current + current / 2;
            const uint32_t target    = required > geometric ? required : geometric;
            return ( target + kGpuQueryGranularity - 1 ) / kGpuQueryGranularity * kGpuQueryGranularity;
        }

        // THE VIEW'S ROW PREFIX ("<view> | ", GpuViewScopeName's text), kept per thread and rebuilt only when the
        // active view's name changes: a scope's row id is then a hit in the thread's name cache on (prefix, name)
        // (Profiler::LocalScopeName) - no string is built per scope (CPU-ONE-SYNC; it was one heap string each).
        struct ViewRowPrefix
        {
            std::string View;
            std::string Prefix;
        };
        thread_local ViewRowPrefix t_ViewRowPrefix;

        [[nodiscard]] uint32_t GpuRowId( const std::string_view viewName, const std::string_view name )
        {
            auto& profiler = Common::Profiling::Profiler::Get();
            if ( viewName.empty() )
                return profiler.LocalScopeName( {}, name ).Id;
            ViewRowPrefix& cached = t_ViewRowPrefix;
            if ( cached.View != viewName )
            {
                cached.View.assign( viewName );
                cached.Prefix.assign( viewName ).append( " | " );
            }
            return profiler.LocalScopeName( cached.Prefix, name ).Id;
        }
    } // namespace

    VulkanGpuProfiler::~VulkanGpuProfiler()
    {
        Shutdown();
    }

    void VulkanGpuProfiler::Init()
    {
        const auto& caps = EngineContext::GetInstance().GetCapabilities();

        // CapabilityCatalog::Timing is the one answer to "can this device time GPU work" (the benchmark asks
        // it too). EncoderBounds (MoltenVK on Apple GPUs) still times each scope, but a timestamp lands at the
        // enclosing encoder's boundary, so a scope inside one encoder reads as zero or as its whole encoder.
        if ( caps.Catalog.Timing == Common::Scalability::GpuTiming::None )
        {
            LOG_WARN( "[GpuProfiler] Device reports no timestamp queries — the profiler stays CPU-only." );
            return;
        }
        if ( caps.Catalog.Timing == Common::Scalability::GpuTiming::EncoderBounds )
            LOG_INFO( "[GpuProfiler] GPU timestamps land at encoder boundaries on this device: a scope inside "
                      "one render/compute encoder reads its encoder's time, not its own." );
        if ( caps.TimestampPeriodNs <= 0.0f )
        {
            LOG_WARN( "[GpuProfiler] timestampPeriod is {} — without it a tick cannot be turned into a "
                      "time, so GPU timing stays off.",
                      caps.TimestampPeriodNs );
            return;
        }

        const auto device = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() );
        m_Device          = device->GetVulkanLogicalDevice();

        // timestampComputeAndGraphics being true already implies every graphics+compute queue has valid
        // bits, but the count is what says how many of the 64 are meaningful, and a zero here would make
        // every delta garbage rather than merely wrong.
        {
            const auto physical = device->GetPhysicalDevice()->GetVulkanPhysicalDevice();

            uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties( physical, &familyCount, nullptr );
            std::vector<VkQueueFamilyProperties> families( familyCount );
            vkGetPhysicalDeviceQueueFamilyProperties( physical, &familyCount, families.data() );

            const uint32_t graphicsFamily = device->GetPhysicalDevice()->GetGraphicsFamily();
            if ( graphicsFamily >= familyCount )
            {
                LOG_WARN( "[GpuProfiler] Graphics queue family {} is outside the {} the driver reports "
                          "here — GPU timing stays off.",
                          graphicsFamily, familyCount );
                return;
            }
            const uint32_t validBits = families[graphicsFamily].timestampValidBits;
            if ( validBits == 0 )
            {
                LOG_WARN( "[GpuProfiler] Graphics queue reports 0 valid timestamp bits — GPU timing stays "
                          "off." );
                return;
            }
        }

        m_PeriodNs = static_cast<double>( caps.TimestampPeriodNs );

        uint32_t framesInFlight = EngineContext::GetInstance().GetMaxFramesInFlight();
        if ( framesInFlight == 0 )
            framesInFlight = 2;

        // Every frame starts sized for kGpuInitialScopeCapacity scopes plus the whole-frame bracket — the
        // denominator the breakdown is checked against; without it "the parts sum to 14 ms" has nothing to
        // be 14 ms OF. More views than that holds grow the pools on their next turn (GrowIfNeeded).
        const uint32_t initialQueries = GpuGrownQueryCount( 0, kGpuInitialScopeCapacity );

        m_Frames = std::vector<FrameQueries>( framesInFlight );
        for ( FrameQueries& frame : m_Frames )
        {
            frame.Pool = CreatePool( initialQueries );
            if ( frame.Pool == VK_NULL_HANDLE )
            {
                LOG_ERROR( "[GpuProfiler] vkCreateQueryPool failed for {} queries — GPU timing stays off.",
                           initialQueries );
                Shutdown();
                return;
            }
            frame.QueryCount = initialQueries;
        }

        m_Active = true;
        Common::Profiling::Profiler::Get().SetGpuSink( this );

        LOG_INFO( "[GpuProfiler] Timestamps on: {} pools of {} queries ({} scopes each, grown on demand), "
                  "period {} ns/tick, results read {} frames late.",
                  framesInFlight, initialQueries, GpuScopeCapacity( initialQueries ), m_PeriodNs, framesInFlight );
    }

    void VulkanGpuProfiler::Shutdown()
    {
        if ( Common::Profiling::Profiler::Get().GetGpuSink() == this )
            Common::Profiling::Profiler::Get().SetGpuSink( nullptr );

        // Direct, not deferred: shutdown runs with the device idle, and a pool replaced earlier is already
        // in the allocator's queue, which the device teardown drains.
        if ( m_Device != VK_NULL_HANDLE )
            for ( const FrameQueries& frame : m_Frames )
            {
                if ( frame.Pool != VK_NULL_HANDLE )
                    vkDestroyQueryPool( m_Device, frame.Pool, nullptr );
                if ( frame.DrawPool != VK_NULL_HANDLE )
                    vkDestroyQueryPool( m_Device, frame.DrawPool, nullptr );
            }

        m_Frames.clear();
        m_Active    = false;
        m_Recording = nullptr;
    }

    VkQueryPool VulkanGpuProfiler::CreatePool( uint32_t queryCount ) const
    {
        VkQueryPoolCreateInfo createInfo{};
        createInfo.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        createInfo.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        createInfo.queryCount = queryCount;

        VkQueryPool pool = VK_NULL_HANDLE;
        if ( vkCreateQueryPool( m_Device, &createInfo, nullptr, &pool ) != VK_SUCCESS )
            return VK_NULL_HANDLE;
        return pool;
    }

    void VulkanGpuProfiler::GrowIfNeeded( FrameQueries& frame )
    {
        const uint32_t wanted = GpuGrownQueryCount( frame.QueryCount, m_HighWaterScopes );
        if ( wanted == frame.QueryCount )
            return;

        // The new pool first: if it cannot be made, the old one keeps timing what it can hold rather than
        // the frame losing every scope.
        auto* const grown = CreatePool( wanted );
        if ( grown == VK_NULL_HANDLE )
        {
            LOG_ERROR( "[GpuProfiler] vkCreateQueryPool failed growing a frame's pool from {} to {} queries "
                       "for {} scopes; keeping the old pool, scopes past {} stay untimed.",
                       frame.QueryCount, wanted, m_HighWaterScopes, GpuScopeCapacity( frame.QueryCount ) );
            return;
        }

        // Deferred like every other per-frame GPU object: its last use has been resolved, but the rule is
        // "the allocator decides when a frame's objects are free", not a judgement made at each call site.
        const auto ctx       = EngineContext::GetInstance().GetRendererContext();
        auto*      allocator = ctx ? SP_CAST( VulkanContext, ctx )->GetVulkanAllocator().get() : nullptr;
        if ( allocator != nullptr )
            allocator->RT_DestroyQueryPool( frame.Pool );
        else
            vkDestroyQueryPool( m_Device, frame.Pool, nullptr );

        LOG_INFO( "[GpuProfiler] A frame asked for {} GPU scopes; frame pool grown from {} to {} queries.",
                  m_HighWaterScopes, frame.QueryCount, wanted );

        frame.Pool       = grown;
        frame.QueryCount = wanted;
    }

    void VulkanGpuProfiler::BeginFrame()
    {
        const VkCommandBuffer commandBuffer = m_RecordingTarget();
        if ( !m_Active || commandBuffer == VK_NULL_HANDLE )
            return;

        const uint32_t frameIndex = EngineContext::GetInstance().GetCurrentFrameIndex();
        if ( frameIndex >= m_Frames.size() )
            return;
        FrameQueries& frame = m_Frames[frameIndex];

        // Everything this frame index submitted last time round has completed —
        // VulkanRendererAPI::PresentFinalImage() waited on its fence before handing the index back. So this read
        // never blocks, and afterwards nothing still refers to the pool, which is what makes this the one place it
        // may be replaced.
        ResolveDraws( frame );
        Resolve( frame );
        GrowIfNeeded( frame );

        frame.Recorder.Reset( GpuScopeCapacity( frame.QueryCount ) );
        frame.FrameTotalWritten = false;
        frame.DrawScopes.clear();
        frame.DrawRequested     = 0;
        m_Recording             = &frame;

        // Switched OFF means off: no reset, no writes, not one command in the buffer. The reset is
        // conditional with the writes rather than unconditional-and-cheap so that turning GPU timing off
        // costs exactly nothing and the A/B that prices this feature measures the whole of it.
        if ( !Common::Profiling::Profiler::Get().GpuEnabled() )
            return;

        // One reset for the frame's whole pool, outside any render pass. A timestamp written to a query
        // that was not reset is undefined, so this must precede every write.
        vkCmdResetQueryPool( commandBuffer, frame.Pool, 0, frame.QueryCount );

        vkCmdWriteTimestamp( commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.Pool,
                             kGpuFrameTotalQuery );
        frame.FrameTotalWritten = true;

        PrepareDrawPool( frame, commandBuffer );
    }

    void VulkanGpuProfiler::PrepareDrawPool( FrameQueries& frame, VkCommandBuffer commandBuffer )
    {
        frame.DrawWritten = false;
        if ( !Common::Profiling::Profiler::Get().DrawSplitActive() )
            return;

        // Created or grown only here, at the frame's start, for the pool this frame index used
        // MaxFramesInFlight frames ago (ResolveDraws has read it): never in the middle of a frame.
        const uint32_t wanted = DrawSplitQueryCount( frame.DrawQueryCount,
                                                     frame.DrawPool == VK_NULL_HANDLE
                                                          ? std::max( kDrawSplitInitialGroups, m_HighWaterDraws )
                                                          : m_HighWaterDraws );
        if ( frame.DrawPool == VK_NULL_HANDLE || wanted != frame.DrawQueryCount )
        {
            auto* const pool = CreatePool( wanted );
            if ( pool == VK_NULL_HANDLE )
            {
                LOG_ERROR(
                     "[GpuProfiler] vkCreateQueryPool failed for the mesh draw split's {} queries; {}.", wanted,
                     frame.DrawPool == VK_NULL_HANDLE ? "the split stays untimed this frame"
                                                      : "keeping the old pool, groups past it stay untimed" );
                if ( frame.DrawPool == VK_NULL_HANDLE )
                    return;
            }
            else
            {
                if ( frame.DrawPool != VK_NULL_HANDLE )
                {
                    const auto ctx  = EngineContext::GetInstance().GetRendererContext();
                    auto* allocator = ctx ? SP_CAST( VulkanContext, ctx )->GetVulkanAllocator().get() : nullptr;
                    if ( allocator != nullptr )
                        allocator->RT_DestroyQueryPool( frame.DrawPool );
                    else
                        vkDestroyQueryPool( m_Device, frame.DrawPool, nullptr );
                    LOG_INFO( "[GpuProfiler] A frame asked for {} mesh draw split groups; draw pool grown from "
                              "{} to {} queries.",
                              m_HighWaterDraws, frame.DrawQueryCount, wanted );
                }
                frame.DrawPool       = pool;
                frame.DrawQueryCount = wanted;
            }
        }

        vkCmdResetQueryPool( commandBuffer, frame.DrawPool, 0, frame.DrawQueryCount );
        frame.DrawWritten = true;
    }

    int32_t VulkanGpuProfiler::BeginDrawScope( std::string_view pass, std::string_view mesh,
                                               std::string_view material )
    {
        const std::lock_guard<std::mutex> lock( m_Lock );
        if ( !m_Active || m_Recording == nullptr || !m_Recording->DrawWritten ||
             Target() == VK_NULL_HANDLE )
            return -1;

        FrameQueries& frame = *m_Recording;
        ++frame.DrawRequested;
        const uint32_t index = static_cast<uint32_t>( frame.DrawScopes.size() );
        if ( ( index + 1 ) * 2 > frame.DrawQueryCount )
            return -1; // the pool grows to the high-water the next time this frame index begins

        // BeginScope's row prefix: the group reads against its pass's own row of the same view.
        ViewResources&         active       = ViewResourceRegistry::Active();
        const bool             frameContext = &active == &ViewResourceRegistry::FrameContext();
        const std::string_view viewName = frameContext ? std::string_view{} : std::string_view{ active.GetName() };

        frame.DrawScopes.push_back( DrawScope{ .Pass     = GpuViewScopeName( viewName, pass ),
                                               .Mesh     = std::string( mesh ),
                                               .Material = std::string( material ) } );
        vkCmdWriteTimestamp( Target(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.DrawPool, index * 2 );
        return static_cast<int32_t>( index );
    }

    void VulkanGpuProfiler::EndDrawScope( int32_t handle, const Common::Profiling::DrawSplitCounts& counts )
    {
        const std::lock_guard<std::mutex> lock( m_Lock );
        if ( handle < 0 || !m_Active || m_Recording == nullptr || !m_Recording->DrawWritten ||
             static_cast<size_t>( handle ) >= m_Recording->DrawScopes.size() ||
             Target() == VK_NULL_HANDLE )
            return;

        DrawScope& scope = m_Recording->DrawScopes[static_cast<size_t>( handle )];
        scope.Counts     = counts;
        scope.Ended      = true;
        vkCmdWriteTimestamp( Target(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_Recording->DrawPool,
                             static_cast<uint32_t>( handle ) * 2 + 1 );
    }

    void VulkanGpuProfiler::ResolveDraws( FrameQueries& frame )
    {
        if ( !frame.DrawWritten )
            return;
        frame.DrawWritten = false;

        const uint32_t groups = static_cast<uint32_t>( frame.DrawScopes.size() );
        if ( groups == 0 )
            return;

        m_ResultScratch.assign( static_cast<size_t>( groups ) * 2 * kWordsPerQuery, 0ull );
        const VkResult result = vkGetQueryPoolResults(
             m_Device, frame.DrawPool, 0, groups * 2, m_ResultScratch.size() * sizeof( uint64_t ),
             m_ResultScratch.data(), sizeof( uint64_t ) * kWordsPerQuery,
             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT );
        if ( result != VK_SUCCESS && result != VK_NOT_READY )
            return;

        auto& profiler = Common::Profiling::Profiler::Get();
        for ( uint32_t i = 0; i < groups; ++i )
        {
            const DrawScope& scope = frame.DrawScopes[i];
            if ( !scope.Ended )
                continue; // its end never recorded (a Record that returned early)
            const size_t beginWord = static_cast<size_t>( i ) * 2 * kWordsPerQuery;
            const size_t endWord   = beginWord + kWordsPerQuery;
            if ( m_ResultScratch[beginWord + 1] == 0 || m_ResultScratch[endWord + 1] == 0 )
                continue; // not available yet: one lost sample, as Resolve's pairMs
            const uint64_t beginTicks = m_ResultScratch[beginWord];
            const uint64_t endTicks   = m_ResultScratch[endWord];
            if ( endTicks < beginTicks )
                continue; // counter wrapped
            profiler.AddDrawSplitSample( scope.Pass, scope.Mesh, scope.Material,
                                         static_cast<double>( endTicks - beginTicks ) * m_PeriodNs * 1e-6,
                                         scope.Counts );
        }
    }

    void VulkanGpuProfiler::EndFrame()
    {
        const VkCommandBuffer commandBuffer = m_RecordingTarget();
        if ( !m_Active || commandBuffer == VK_NULL_HANDLE || m_Recording == nullptr )
            return;

        const FrameQueries& frame = *m_Recording;
        if ( frame.FrameTotalWritten )
            vkCmdWriteTimestamp( commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.Pool,
                                 kGpuFrameTotalQuery + 1 );

        if ( frame.Recorder.Requested() > m_HighWaterScopes )
            m_HighWaterScopes = frame.Recorder.Requested();
        if ( frame.DrawRequested > m_HighWaterDraws )
            m_HighWaterDraws = frame.DrawRequested;

        m_Recording = nullptr;
    }

    bool VulkanGpuProfiler::OwnsRecordingTarget()
    {
        // Only the game thread enqueues render work, so the game thread reading InFlight() false cannot race the
        // render task starting; workers follow RenderTask::RecordGuard's rule (they never fence).
        return ::Desert::Engine::RenderTask::OnRenderThread() || !::Desert::Engine::RenderTask::Get().InFlight();
    }

    int32_t VulkanGpuProfiler::BeginScope( const char* name )
    {
        VulkanRecordContext* const chunk = VulkanRecordContext::Bound();
        // A GPU scope on a thread that owns no recording target (the game thread while the views record on the
        // render task) times nothing: CPU-only code takes DESERT_PROFILE_SCOPE. Named once, so the call site is
        // fixed instead of the frame quietly losing a row.
        if ( m_Active && chunk == nullptr && !OwnsRecordingTarget() )
        {
            static std::atomic<bool> reported{ false };
            if ( !reported.exchange( true ) )
                LOG_WARN( "[GpuProfiler] GPU scope '{}' opened on a thread that owns no command buffer while the views "
                          "record on the render task: not timed. CPU-only code takes DESERT_PROFILE_SCOPE.",
                          name );
            return -1;
        }
        // FrameTotalWritten doubles as "this frame's queries have been reset". Without it, flipping the
        // toggle on mid-frame would write timestamps into queries nobody reset — undefined, and the kind
        // of thing that reads as a wild number rather than an error.
        // No buffer recording (between ExecuteGraph ending the frame's buffer and the graph arming its first
        // segment, or after a failure disarmed the target): there is nowhere for the timestamp to go.
        // m_Recording / m_Active change only in BeginFrame / EndFrame, on the recording thread, before any chunk
        // starts and after every chunk was joined: read here without a lock.
        if ( !m_Active || m_Recording == nullptr || !m_Recording->FrameTotalWritten ||
             Target() == VK_NULL_HANDLE )
            return -1;

        // The owner is whichever view the ActiveViewScope targets, else the frame context. Its NAME is
        // captured now, not read at resolve: the view may be gone by the time its queries are readable.
        ViewResources&         active       = ViewResourceRegistry::Active();
        const bool             frameContext = &active == &ViewResourceRegistry::FrameContext();
        const std::string_view viewName = frameContext ? std::string_view{} : std::string_view{ active.GetName() };
        const uint32_t         row      = GpuRowId( viewName, name );

        // A refused scope (pool full this frame) returns -1, and EndScope's matching -1 leaves the nesting
        // of the scopes that were timed untouched. The refusal raises the high-water mark, so the pool
        // is big enough the next time this frame index comes round.
        int32_t index = -1;
        if ( chunk != nullptr )
        {
            // A chunk's scope: no lock (GpuScopeRecorder, THREADS) - its own slot and its chunk's lists.
            const uint64_t order = ( chunk->GpuOrder << 24 ) | chunk->GpuOrderNext++;
            index = m_Recording->Recorder.BeginUnder( row, chunk->GpuRoot, chunk->GpuScopes, order );
            if ( index >= 0 )
                chunk->GpuScopesMade.push_back( index );
        }
        else
        {
            const std::lock_guard<std::mutex> lock( m_Lock );
            index = m_Recording->Recorder.Begin( row, &active );
        }
        if ( index < 0 )
            return -1;

        vkCmdWriteTimestamp( Target(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_Recording->Pool,
                             GpuScopeQueryBase( static_cast<uint32_t>( index ) ) );
        return index;
    }

    void VulkanGpuProfiler::EndScope( int32_t handle )
    {
        if ( handle < 0 || !m_Active || m_Recording == nullptr )
            return;

        // The handle is the scope's index in the recording frame, and the recorder remembers which view's
        // nesting it belongs to — so a scope closes against the view it opened in even if the active view
        // moved meanwhile.
        if ( VulkanRecordContext* const chunk = VulkanRecordContext::Bound(); chunk != nullptr )
            m_Recording->Recorder.EndUnder( handle, chunk->GpuScopes );
        else
        {
            const std::lock_guard<std::mutex> lock( m_Lock );
            m_Recording->Recorder.End( handle );
        }
        // A scope still open when a failure disarmed the target loses its end query; Resolve already reads a
        // scope whose two queries did not both land as untimed.
        if ( Target() == VK_NULL_HANDLE )
            return;
        vkCmdWriteTimestamp( Target(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_Recording->Pool,
                             GpuScopeQueryBase( static_cast<uint32_t>( handle ) ) + 1 );
    }

    GpuChunkRootScope VulkanGpuProfiler::ChunkRoot( Common::Profiling::GpuQueue queue )
    {
        const std::lock_guard<std::mutex> lock( m_Lock );
        if ( m_Recording == nullptr )
            return {};
        return m_Recording->Recorder.ChunkRoot( &ViewResourceRegistry::Active(), queue );
    }

    void VulkanGpuProfiler::SetQueue( Common::Profiling::GpuQueue queue )
    {
        const std::lock_guard<std::mutex> lock( m_Lock );
        // Outside recording there is no frame for the queue to belong to; Reset starts every frame on Graphics.
        if ( m_Recording != nullptr )
            m_Recording->Recorder.SetQueue( queue );
    }

    uint64_t VulkanGpuProfiler::ReserveChunkOrder( uint32_t chunks )
    {
        const std::lock_guard<std::mutex> lock( m_Lock );
        return m_Recording != nullptr ? m_Recording->Recorder.ReserveOrder( chunks ) : 0;
    }

    void VulkanGpuProfiler::StampChunkOrder( std::span<const int32_t> scopes )
    {
        const std::lock_guard<std::mutex> lock( m_Lock );
        if ( m_Recording == nullptr || scopes.empty() )
            return;
        const uint64_t tick = m_Recording->Recorder.ReserveOrder( 1 );
        for ( size_t k = 0; k < scopes.size(); ++k )
            m_Recording->Recorder.SetOrder( scopes[k], ( tick << 24 ) | static_cast<uint64_t>( k ) );
    }

    void VulkanGpuProfiler::RetractChunkScopes( std::span<const int32_t> scopes )
    {
        const std::lock_guard<std::mutex> lock( m_Lock );
        if ( m_Recording == nullptr )
            return;
        for ( const int32_t index : scopes )
            m_Recording->Recorder.Retract( index );
    }

    void VulkanGpuProfiler::Resolve( FrameQueries& frame )
    {
        if ( !frame.FrameTotalWritten )
            return;
        frame.FrameTotalWritten = false;

        const std::span<const GpuScopeRecorder<uint32_t>::Scope> scopes = frame.Recorder.Scopes();
        auto&                                                    profiler = Common::Profiling::Profiler::Get();

        // PASS ORDER TRACE: the frame's timed scopes in recording order, one '>' per nesting level, logged
        // whenever the sequence changes. It is how a change to the way the frame is recorded proves it kept
        // the pass order: two builds on one scene must print the same line.
        // CPU-ONE-SYNC: only under --gpu-order-trace (Profiler::GpuOrderTrace), where a capture asks for it; off,
        // the frame pays nothing. On, the order is compared by a hash of (depth, row id) - no string per frame -
        // and the text (~40 KB on FIELD_Barn) is built only when that hash changes.
        if ( profiler.GpuOrderTrace() )
        {
            // RDG-PARALLEL-1: in recording order (Scope::Order), not in the order threads opened the scopes.
            m_OrderScratch.resize( scopes.size() );
            for ( size_t at = 0; at < scopes.size(); ++at )
                m_OrderScratch[at] = static_cast<uint32_t>( at );
            std::stable_sort( m_OrderScratch.begin(), m_OrderScratch.end(), [&scopes]( uint32_t a, uint32_t b )
                              { return scopes[a].Order < scopes[b].Order; } );
            const auto depthOf = [&frame]( uint32_t at )
            {
                uint64_t depth = 0;
                for ( int32_t parent = frame.Recorder.LiveParent( static_cast<int32_t>( at ) );
                      parent != kGpuNoParent; parent = frame.Recorder.LiveParent( parent ) )
                    ++depth;
                return depth;
            };
            uint64_t hash = 14695981039346656037ull; // FNV-1a over (depth << 32 | row id)
            for ( const uint32_t at : m_OrderScratch )
            {
                // RDG-PARALLEL-5: a discarded chunk's scope is no row; its passes are in the trace once, inline.
                if ( scopes[at].Retracted )
                    continue;
                hash ^= ( depthOf( at ) << 32 ) | scopes[at].Name;
                hash *= 1099511628211ull;
            }
            if ( hash != m_LastOrderHash )
            {
                m_LastOrderHash = hash;
                std::string order;
                for ( const uint32_t at : m_OrderScratch )
                {
                    if ( scopes[at].Retracted )
                        continue;
                    order.append( static_cast<size_t>( depthOf( at ) ), '>' );
                    order += profiler.ScopeNameText( scopes[at].Name );
                    order += '|';
                }
                if ( order != m_LastLoggedOrder )
                {
                    LOG_INFO( "[GpuOrder] {}", order );
                    m_LastLoggedOrder = std::move( order );
                }
            }
        }
        const uint32_t queryCount = GpuQueriesForScopes( static_cast<uint32_t>( scopes.size() ) );
        m_ResultScratch.assign( static_cast<size_t>( queryCount ) * kWordsPerQuery, 0ull );

        // One read for the queries this frame actually wrote. WITHOUT VK_QUERY_RESULT_WAIT_BIT: the fence
        // guarantees the data is there, and asking Vulkan to wait would turn a free read into a GPU stall.
        const VkResult result = vkGetQueryPoolResults(
             m_Device, frame.Pool, 0, queryCount, m_ResultScratch.size() * sizeof( uint64_t ),
             m_ResultScratch.data(), sizeof( uint64_t ) * kWordsPerQuery,
             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT );

        if ( result != VK_SUCCESS && result != VK_NOT_READY )
            return;

        // Inclusive milliseconds of the pair starting at @p queryBase, or -1 when either end did not land.
        const auto pairMs = [this]( uint32_t queryBase ) -> double
        {
            const size_t beginWord = static_cast<size_t>( queryBase ) * kWordsPerQuery;
            const size_t endWord   = beginWord + kWordsPerQuery;
            if ( endWord + 1 >= m_ResultScratch.size() )
                return -1.0;
            if ( m_ResultScratch[beginWord + 1] == 0 || m_ResultScratch[endWord + 1] == 0 )
                return -1.0; // a scope whose end never recorded (early-out inside the pass)

            const uint64_t beginTicks = m_ResultScratch[beginWord];
            const uint64_t endTicks   = m_ResultScratch[endWord];
            if ( endTicks < beginTicks )
                return -1.0; // counter wrapped; one lost sample beats a nonsense one

            return static_cast<double>( endTicks - beginTicks ) * m_PeriodNs * 1e-6;
        };

        // The whole-frame bracket: one number per frame, belonging to no view, never anyone's child.
        const double frameMs = pairMs( kGpuFrameTotalQuery );
        if ( frameMs >= 0.0 )
            profiler.AddGpuSample( Common::Profiling::kGpuFrameTotalScope, frameMs, frameMs );

        const size_t scopeCount = scopes.size();
        m_Inclusive.assign( scopeCount, -1.0 );
        m_Parents.resize( scopeCount );
        for ( size_t i = 0; i < scopeCount; ++i )
        {
            // A retracted scope (a discarded chunk's) is untimed and no one's parent: its queries were never
            // executed, and the inline re-record timed the same passes under the live enclosing scope.
            m_Parents[i] = frame.Recorder.LiveParent( static_cast<int32_t>( i ) );
            if ( scopes[i].Retracted )
                continue;
            m_Inclusive[i] = pairMs( GpuScopeQueryBase( static_cast<uint32_t>( i ) ) );
        }

        // Self time = own interval minus the DIRECT children's, so the parts partition the frame instead
        // of counting a parent's microseconds again in each child. Parents never cross views
        // (GpuScopeRecorder), so one pass over the interleaved list is each view's partition at once. The
        // arithmetic lives in Engine/Graphic/GpuTimestampLayout.hpp and is asserted by
        // Tests/Engine/GpuTimestampLayout.
        GpuSelfTimes( m_Inclusive, m_Parents, m_Self );

        for ( size_t i = 0; i < scopeCount; ++i )
            if ( m_Inclusive[i] >= 0.0 )
                profiler.AddGpuSample( scopes[i].Name, m_Inclusive[i], m_Self[i] );

        if ( frameMs >= 0.0 )
            PublishUnaccounted( scopes );
    }

    bool VulkanGpuProfiler::IsSegmentRow( const uint32_t nameId )
    {
        if ( nameId == Common::Profiling::kNoScopeName )
            return false;
        if ( nameId >= m_SegmentById.size() )
            m_SegmentById.resize( static_cast<size_t>( nameId ) + 1, 0 );
        uint8_t& known = m_SegmentById[nameId];
        if ( known == 0 )
        {
            // The table's lock, once per row for the process' life (ScopeNameText).
            const std::string_view text = Common::Profiling::Profiler::Get().ScopeNameText( nameId );
            known = Common::Profiling::GpuAsyncSegmentLabelStart( text ) != std::string_view::npos ? 1 : 2;
        }
        return known == 1;
    }

    void VulkanGpuProfiler::PublishUnaccounted( std::span<const GpuScopeRecorder<uint32_t>::Scope> scopes )
    {
        using Common::Profiling::GpuQueue;

        // Timestamps as ms from the frame bracket's begin. Signed: an async compute stamp can precede the
        // graphics bracket's first one (the compute queue is another timeline of the same device clock).
        const uint64_t frameBeginTicks =
             m_ResultScratch[static_cast<size_t>( kGpuFrameTotalQuery ) * kWordsPerQuery];
        const auto intervalOf = [&]( uint32_t queryBase ) -> GpuInterval
        {
            const size_t beginWord = static_cast<size_t>( queryBase ) * kWordsPerQuery;
            const size_t endWord   = beginWord + kWordsPerQuery;
            if ( endWord + 1 >= m_ResultScratch.size() || m_ResultScratch[beginWord + 1] == 0 ||
                 m_ResultScratch[endWord + 1] == 0 || m_ResultScratch[endWord] < m_ResultScratch[beginWord] )
                return GpuInterval{};
            const auto toMs = [&]( uint64_t ticks )
            { return static_cast<double>( static_cast<int64_t>( ticks - frameBeginTicks ) ) * m_PeriodNs * 1e-6; };
            return GpuInterval{ toMs( m_ResultScratch[beginWord] ), toMs( m_ResultScratch[endWord] ), true };
        };

        const GpuInterval frameInterval = intervalOf( kGpuFrameTotalQuery );
        if ( !frameInterval.Valid )
            return;

        const size_t scopeCount = scopes.size();
        m_Intervals.resize( scopeCount );
        m_GraphicsScopes.clear();
        m_AsyncPasses.clear();
        m_AsyncSegments.clear();
        // A segment row of a view carries the view's name first ("scene view #1 | RDG Async: ..",
        // GpuViewScopeName): the prefix is matched on the row's text, after the view's (IsSegmentRow).
        for ( size_t i = 0; i < scopeCount; ++i )
        {
            // A retracted scope (a discarded chunk's) covers nothing: it is no member of any timeline.
            if ( scopes[i].Retracted )
            {
                m_Intervals[i] = GpuInterval{};
                continue;
            }
            m_Intervals[i]   = intervalOf( GpuScopeQueryBase( static_cast<uint32_t>( i ) ) );
            const auto index = static_cast<int32_t>( i );
            if ( scopes[i].Queue == GpuQueue::Graphics )
                m_GraphicsScopes.push_back( index );
            else if ( IsSegmentRow( scopes[i].Name ) )
                m_AsyncSegments.push_back( index );
            else
                m_AsyncPasses.push_back( index );
        }

        auto& profiler = Common::Profiling::Profiler::Get();

        // THE GRAPHICS QUEUE: the bracket minus the union of every graphics scope in it, and where it sits.
        // m_Coverage is read here before the async pass below reuses it.
        GpuCoverageOf( m_Intervals, m_GraphicsScopes, frameInterval.BeginMs, frameInterval.EndMs, m_Coverage );
        const GpuCoverage& graphics = m_Coverage;
        profiler.AddGpuSample( Common::Profiling::kGpuUnaccountedGraphicsScope, graphics.UncoveredMs,
                               graphics.UncoveredMs );
        for ( const GpuUncovered& gap : graphics.Gaps )
        {
            if ( gap.Ms < Common::Profiling::kGpuGapReportMs )
                continue;
            // Only the gaps over kGpuGapReportMs (a few a frame) look their neighbours' text up.
            std::string_view after = "frame begin";
            if ( gap.After != kGpuNoParent )
                after = profiler.ScopeNameText( scopes[static_cast<size_t>( gap.After )].Name );
            std::string_view before = "frame end";
            if ( gap.Before != kGpuNoParent )
                before = profiler.ScopeNameText( scopes[static_cast<size_t>( gap.Before )].Name );
            m_GapName.assign( Common::Profiling::kGpuGapScopePrefix );
            m_GapName.append( after ).append( " -> " ).append( before );
            profiler.AddGpuSample( m_GapName.c_str(), gap.Ms, gap.Ms );
        }

        // THE ASYNC COMPUTE QUEUE: each segment minus the union of the async passes inside it. No segment row
        // this frame (async off, or every segment demoted to the graphics queue) publishes nothing, which the
        // dump already reads as "no segment".
        if ( m_AsyncSegments.empty() )
            return;
        double asyncUncoveredMs = 0.0;
        for ( const int32_t segment : m_AsyncSegments )
        {
            const GpuInterval& span = m_Intervals[static_cast<size_t>( segment )];
            if ( span.Valid )
            {
                GpuCoverageOf( m_Intervals, m_AsyncPasses, span.BeginMs, span.EndMs, m_Coverage );
                asyncUncoveredMs += m_Coverage.UncoveredMs;
            }
        }
        profiler.AddGpuSample( Common::Profiling::kGpuUnaccountedAsyncScope, asyncUncoveredMs, asyncUncoveredMs );
    }
} // namespace Desert::Graphic::API::Vulkan

#endif // DESERT_DEV_INSTRUMENTS
