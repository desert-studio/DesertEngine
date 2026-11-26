#include <Engine/Assets/SyncLoadLedger.hpp>

#include <Engine/Core/TextTemplate.hpp>
#include <Common/Json/Document.hpp>
#include <Common/Json/Json.hpp>

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace Desert::Assets
{
    std::atomic<bool>& SyncLoadDetail::BootFinished()
    {
        static std::atomic<bool> finished{ false };
        return finished;
    }

    std::atomic<uint64_t>& SyncLoadDetail::LoadCount()
    {
        static std::atomic<uint64_t> loads{ 0 };
        return loads;
    }

    std::atomic<uint64_t>& SyncLoadDetail::InFrameCount()
    {
        static std::atomic<uint64_t> loads{ 0 };
        return loads;
    }

    std::atomic<uint64_t>& SyncLoadDetail::AsyncCount()
    {
        static std::atomic<uint64_t> loads{ 0 };
        return loads;
    }

    int& SyncLoadDetail::AsyncDepth()
    {
        static thread_local int depth = 0;
        return depth;
    }

    int& SyncLoadDetail::Depth()
    {
        static thread_local int depth = 0;
        return depth;
    }

    LoadTimingScope*& SyncLoadDetail::OpenScope()
    {
        static thread_local LoadTimingScope* open = nullptr;
        return open;
    }

    std::mutex& SyncLoadDetail::TotalsLock()
    {
        static std::mutex lock;
        return lock;
    }

    SyncLoadDetail::Totals& SyncLoadDetail::Sums()
    {
        static Totals totals;
        return totals;
    }

    std::atomic<uint64_t>& SyncLoadDetail::InFrameLogged()
    {
        static std::atomic<uint64_t> logged{ 0 };
        return logged;
    }

    namespace SyncLoadDetail
    {
        // The report. BOTH HALVES ARE NAMED SEPARATELY, and zero in-frame loads is a RESULT worth printing
        // rather than a silence: a report that went quiet when it was true would make the good case
        // indistinguishable from a detector that was not running. The unnamed in-frame remainder is BOUNDED
        // rather than merely counted: every one of them took no longer than the last named load, which is what
        // decides whether they are worth chasing. The async line is NAMED WHETHER OR NOT IT IS ZERO, for the
        // reason the in-frame line is.
        constexpr std::string_view kReportText =
             "loads={{ loads }} in {{ totalMs }} (outermost scopes only; nested loads are counted, not re-timed)"
             "\n  in-frame: {{ inFrame }} load(s) in {{ inFrameMs }}"
             "{% if inFrame == 0 %}{% if loads == 0 %} — nothing loaded at all yet"
             "{% else %} — every load so far happened at boot{% endif %}{% endif %}"
             "{% if rankedCount > 0 %}\n  slowest in-frame load(s) by their OWN time ({{ rankedCount }} of "
             "{{ inFrame }}):{% for r in ranked %}\n    {{ r.ms }} — '{{ r.path }}'{% endfor %}"
             "{% if others > 0 %}\n    the other {{ others }} took at most {{ lastMs }} each{% endif %}{% endif %}"
             "\n  async (on a JobSystem worker, blocking no frame): {{ asyncLoads }} load(s) in {{ asyncMs }}"
             "{% if loads > 0 %}\n  slowest single load by its OWN time: {{ slowestMs }} — "
             "'{{ slowestPath }}'{% endif %}";
    } // namespace SyncLoadDetail

    std::string SyncLoadLedger::Report()
    {
        const std::lock_guard<std::mutex> guard( SyncLoadDetail::TotalsLock() );

        const uint64_t loads   = SyncLoadDetail::LoadCount().load( std::memory_order_relaxed );
        const uint64_t inFrame = SyncLoadDetail::InFrameCount().load( std::memory_order_relaxed );
        const uint64_t async   = SyncLoadDetail::AsyncCount().load( std::memory_order_relaxed );

        static const Desert::Core::TextTemplate kReport( "SyncLoadReport", SyncLoadDetail::kReportText );
        const auto&                             sums   = SyncLoadDetail::Sums();
        const auto&                             ranked = sums.SlowestInFrame;
        Common::Json::Value::Array              rankedRows;
        for ( const auto& [ms, rankedPath] : ranked )
        {
            Common::Json::ObjectBuilder row;
            row.Set( "ms", SyncLoadDetail::Ms( ms ) ).Set( "path", rankedPath );
            rankedRows.emplace_back( row.Build() );
        }
        Common::Json::ObjectBuilder model;
        model.Set( "loads", loads )
             .Set( "totalMs", SyncLoadDetail::Ms( sums.TotalMs ) )
             .Set( "inFrame", inFrame )
             .Set( "inFrameMs", SyncLoadDetail::Ms( sums.InFrameMs ) )
             .Set( "rankedCount", ranked.size() )
             .Set( "ranked", Common::Json::Value( std::move( rankedRows ) ) )
             .Set( "others", inFrame > ranked.size() ? inFrame - ranked.size() : 0 )
             .Set( "lastMs", ranked.empty() ? std::string() : SyncLoadDetail::Ms( ranked.back().first ) )
             .Set( "asyncLoads", async )
             .Set( "asyncMs", SyncLoadDetail::Ms( sums.AsyncMs ) )
             .Set( "slowestMs", SyncLoadDetail::Ms( sums.SlowestMs ) )
             .Set( "slowestPath", sums.SlowestPath );
        return kReport.RenderOrLog( Common::Json::Value( model.Build() ) );
    }
} // namespace Desert::Assets
