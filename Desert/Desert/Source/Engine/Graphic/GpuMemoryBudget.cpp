#include <Engine/Graphic/GpuMemoryBudget.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Core/Device.hpp>

#include <algorithm>
#include <utility>

namespace Desert::Graphic
{
    namespace
    {
        // Every threshold is a FRACTION of the live budget: a count of MB is right on one card.
        // Never handed out: what the frame's transients, uploads and the driver grow by between two readings.
        constexpr uint64_t kReserveDivisor = 10;
        // The headroom below which a shrink waits for no hysteresis: half the reserve is gone.
        constexpr uint64_t kPressureDivisor = 20;
        constexpr uint64_t kShrinkDivisor   = 32;
        constexpr uint64_t kGrowDivisor     = 16;
        constexpr uint32_t kShrinkFrames    = 8;
        constexpr uint32_t kGrowFrames      = 60;
        // After a shrink: what the clients release stays in the usage until the frames in flight complete and the
        // allocator returns its blocks. Reading it as outside memory meanwhile would shrink the pool again and
        // again.
        constexpr uint32_t kSettleFrames = 30;
        constexpr uint32_t kLogFrames    = 120;
        // A device without VK_EXT_memory_budget: the share of its heap the clients are given, half, the rest being
        // the targets, meshes and acceleration structures nothing measures there.
        constexpr uint64_t kUnmeasuredHeapDivisor = 2;

        double MB( const uint64_t bytes )
        {
            return static_cast<double>( bytes ) / ( 1024.0 * 1024.0 );
        }

        // a - b, 0 where b is the larger.
        uint64_t Less( const uint64_t a, const uint64_t b )
        {
            return a > b ? a - b : 0;
        }
    } // namespace

    GpuMemoryBudget& GpuMemoryBudget::Get()
    {
        static GpuMemoryBudget budget;
        return budget;
    }

    GpuMemoryBudget::Client::Client( std::string name, const float weight )
         : m_Id( GpuMemoryBudget::Get().Register( std::move( name ), weight ) )
    {
    }

    GpuMemoryBudget::Client::~Client()
    {
        GpuMemoryBudget::Get().Unregister( m_Id );
    }

    void GpuMemoryBudget::Client::Declare( const uint64_t minimum, const uint64_t desired, const uint64_t held )
    {
        GpuMemoryBudget&      budget = GpuMemoryBudget::Get();
        const std::lock_guard lock( budget.m_Lock );
        Row* const            row = budget.Find( m_Id );
        if ( row == nullptr )
            return;
        row->Held             = held;
        const uint64_t wanted = std::max( minimum, desired );
        if ( row->Minimum == minimum && row->Desired == wanted )
            return;
        row->Minimum = minimum;
        row->Desired = wanted;
        budget.Distribute();
    }

    uint64_t GpuMemoryBudget::Client::Grant() const
    {
        GpuMemoryBudget&      budget = GpuMemoryBudget::Get();
        const std::lock_guard lock( budget.m_Lock );
        const Row* const      row = budget.Find( m_Id );
        return row != nullptr ? row->Grant : 0;
    }

    uint64_t GpuMemoryBudget::Register( std::string name, const float weight )
    {
        const std::lock_guard lock( m_Lock );
        Row&                  row = m_Rows.emplace_back();
        row.Id                    = m_NextId++;
        row.Name                  = std::move( name );
        row.Weight                = weight;
        return row.Id;
    }

    void GpuMemoryBudget::Unregister( const uint64_t id )
    {
        const std::lock_guard lock( m_Lock );
        std::erase_if( m_Rows, [id]( const Row& row ) { return row.Id == id; } );
        Distribute();
    }

    GpuMemoryBudget::Row* GpuMemoryBudget::Find( const uint64_t id )
    {
        const auto it =
             std::find_if( m_Rows.begin(), m_Rows.end(), [id]( const Row& row ) { return row.Id == id; } );
        return it != m_Rows.end() ? &*it : nullptr;
    }

    void GpuMemoryBudget::Distribute()
    {
        if ( !m_Measured )
        {
            for ( Row& row : m_Rows )
                row.Grant = row.Desired;
            return;
        }
        uint64_t minimums = 0;
        for ( Row& row : m_Rows )
        {
            row.Grant = row.Minimum;
            minimums += row.Minimum;
        }
        // A minimum is granted whatever the pool: a client below it does not work at all, and the driver paging is
        // the lesser evil - said once, with the numbers.
        if ( minimums > m_Pool && !m_WarnedMinimums )
        {
            m_WarnedMinimums = true;
            LOG_WARN( "[GpuMemoryBudget] the clients' minimums ({:.0f} MB) exceed the pool ({:.0f} MB): they are "
                      "granted past it",
                      MB( minimums ), MB( m_Pool ) );
        }
        // Each round satisfies at least one client or hands out everything left.
        uint64_t left = Less( m_Pool, minimums );
        while ( left > 0 )
        {
            double weights = 0.0;
            for ( const Row& row : m_Rows )
                if ( row.Grant < row.Desired )
                    weights += row.Weight;
            if ( weights <= 0.0 )
                return;
            const uint64_t round     = left;
            bool           satisfied = false;
            for ( Row& row : m_Rows )
            {
                if ( row.Grant >= row.Desired )
                    continue;
                const auto share = static_cast<uint64_t>( static_cast<double>( round ) * row.Weight / weights );
                const uint64_t given = std::min( { share, row.Desired - row.Grant, left } );
                row.Grant += given;
                left -= given;
                satisfied = satisfied || row.Grant == row.Desired;
            }
            if ( !satisfied )
                return;
        }
    }

    void GpuMemoryBudget::Accept( const uint64_t pool, const uint64_t budget, const uint64_t usage )
    {
        LOG_INFO( "[GpuMemoryBudget] pool {:.0f} -> {:.0f} MB (device-local usage {:.0f} MB of a live budget of "
                  "{:.0f} MB)",
                  MB( m_Pool ), MB( pool ), MB( usage ), MB( budget ) );
        m_Pool         = pool;
        m_Measured     = true;
        m_ShrinkFrames = 0;
        m_GrowFrames   = 0;
        Distribute();
        LogClients();
    }

    void GpuMemoryBudget::LogClients()
    {
        m_LogWaitFrames = kLogFrames;
        for ( Row& row : m_Rows )
        {
            row.LoggedLimited = row.Grant < row.Desired;
            if ( row.Desired == 0 && row.Held == 0 )
                continue;
            LOG_INFO(
                 "[GpuMemoryBudget] {}: granted {:.0f} MB of {:.0f} MB wanted (minimum {:.0f}, held {:.0f}){}",
                 row.Name, MB( row.Grant ), MB( row.Desired ), MB( row.Minimum ), MB( row.Held ),
                 row.LoggedLimited ? ", limited" : "" );
        }
    }

    void GpuMemoryBudget::Update( const Engine::DeviceMemoryReport& report )
    {
        const std::lock_guard lock( m_Lock );
        if ( report.Heaps.empty() )
            return; // no device
        if ( !report.BudgetKnown )
        {
            if ( m_WarnedUnknown )
                return;
            m_WarnedUnknown = true;
            uint64_t size   = 0;
            for ( const Engine::DeviceMemoryHeap& heap : report.Heaps )
                if ( heap.DeviceLocal )
                    size += heap.Size;
            m_Pool     = size / kUnmeasuredHeapDivisor;
            m_Measured = true;
            LOG_WARN(
                 "[GpuMemoryBudget] the device reports no live budget (VK_EXT_memory_budget): the pool is fixed "
                 "at {:.0f} MB of its {:.0f} MB device-local heap",
                 MB( m_Pool ), MB( size ) );
            Distribute();
            LogClients();
            return;
        }

        const uint64_t budget = report.DeviceLocalBudget();
        const uint64_t usage  = report.DeviceLocalUsage();
        uint64_t       held   = 0;
        for ( const Row& row : m_Rows )
            held += row.Held;
        const uint64_t outside  = Less( usage, held );
        const uint64_t measured = Less( Less( budget, budget / kReserveDivisor ), outside );

        if ( m_SettleFrames > 0 )
            --m_SettleFrames;
        if ( m_LogWaitFrames > 0 )
            --m_LogWaitFrames;
        if ( !m_Measured )
        {
            Accept( measured, budget, usage );
            return;
        }

        const bool less     = measured + budget / kShrinkDivisor < m_Pool;
        const bool more     = measured > m_Pool + budget / kGrowDivisor;
        const bool pressure = measured < m_Pool && usage + budget / kPressureDivisor > budget;
        m_ShrinkFrames      = less ? m_ShrinkFrames + 1 : 0;
        m_GrowFrames        = more ? m_GrowFrames + 1 : 0;
        if ( ( m_ShrinkFrames >= kShrinkFrames || pressure ) && m_SettleFrames == 0 )
        {
            m_SettleFrames = kSettleFrames;
            Accept( measured, budget, usage );
            return;
        }
        if ( m_GrowFrames >= kGrowFrames )
        {
            Accept( measured, budget, usage );
            return;
        }
        // The pool stands and the demand moved: a client went over or back under it.
        const bool flipped = std::any_of( m_Rows.begin(), m_Rows.end(), []( const Row& row )
                                          { return row.LoggedLimited != ( row.Grant < row.Desired ); } );
        if ( flipped && m_LogWaitFrames == 0 )
            LogClients();
    }
} // namespace Desert::Graphic
