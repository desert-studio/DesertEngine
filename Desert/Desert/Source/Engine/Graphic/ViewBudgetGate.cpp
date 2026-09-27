#include <Engine/Graphic/ViewBudgetGate.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>

#include <atomic>
#include <format>

namespace Desert::Graphic
{
    namespace
    {
        std::atomic<uint64_t> g_OverrideMiB{ 0 };
        std::atomic<bool>     g_CeilingLogged{ false };
    } // namespace

    void SetViewBudgetOverrideMiB( const uint64_t mib )
    {
        g_OverrideMiB.store( mib );
        g_CeilingLogged.store( false ); // the next reading says the new ceiling
    }

    Engine::ViewBudget::Reading ReadViewBudget()
    {
        const auto                 device = EngineContext::GetInstance().GetDevice();
        Engine::DeviceMemoryReport report;
        if ( device )
            report = device->QueryMemory();

        uint64_t heapSize = 0;
        for ( const Engine::DeviceMemoryHeap& heap : report.Heaps )
            if ( heap.DeviceLocal )
                heapSize += heap.Size;

        uint64_t held = 0;
        for ( const Engine::ViewBudget::HeldView& view : SceneRenderer::LiveHoldings() )
            held += view.Bytes;

        const Engine::ViewBudget::Reading reading = Engine::ViewBudget::ReadCeiling(
             report.BudgetKnown, report.DeviceLocalBudget(), report.DeviceLocalUsage(), heapSize,
             g_OverrideMiB.load() * 1024ull * 1024ull, held );
        if ( !g_CeilingLogged.exchange( true ) )
            LOG_INFO( "[ViewBudget] {}.", Engine::ViewBudget::DescribeCeiling( reading ) );
        return reading;
    }

    Common::BoolResultStr MayCreateView( const Engine::ViewBudget::Demand who, const std::string_view viewName,
                                         const ViewProfile& profile, const ViewExtent& extent )
    {
        // The same size rule every view is built under, not a second one written here.
        if ( !IsUsableViewExtent( extent ) )
            return Common::MakeFormattedError<bool>(
                 "View '{}' asked for {}x{}, which is not a usable view extent", viewName, extent.Width,
                 extent.Height );

        const uint64_t request =
             SumViewTargets( ViewTargetCensus( profile, extent.Width, extent.Height ) ).Total();

        // The reserve background work leaves: the main view's forecast at the current window size, so the
        // view a person actually looks at can always be rebuilt or resized (lead decision, RT2 plan §8).
        uint64_t reserve = 0;
        if ( who == Engine::ViewBudget::Demand::Background )
        {
            if ( const auto window = EngineContext::GetInstance().GetWindow() )
            {
                if ( IsUsableViewExtent( window->GetWidth(), window->GetHeight() ) )
                    reserve = SumViewTargets(
                                   ViewTargetCensus( kSceneViewProfile, window->GetWidth(), window->GetHeight() ) )
                                   .Total();
            }
        }

        const Engine::ViewBudget::Reading reading = ReadViewBudget();
        const Engine::ViewBudget::Verdict verdict =
             Engine::ViewBudget::MayCreate( who, request, reserve, reading );
        if ( verdict.Ok )
            return Common::MakeSuccess( true );
        return Common::MakeError<bool>(
             Engine::ViewBudget::DescribeRefusal( viewName, verdict, reading, SceneRenderer::LiveHoldings() ) );
    }

    std::optional<std::string> DescribeResizeOverrun( const std::string_view viewName, const ViewProfile& profile,
                                                      const ViewExtent& from, const ViewExtent& to )
    {
        const uint64_t current = SumViewTargets( ViewTargetCensus( profile, from.Width, from.Height ) ).Total();
        const uint64_t resized = SumViewTargets( ViewTargetCensus( profile, to.Width, to.Height ) ).Total();
        const Engine::ViewBudget::Reading reading = ReadViewBudget();
        const uint64_t overrun = Engine::ViewBudget::ResizeOverrunBytes( current, resized, reading );
        if ( overrun == 0 )
            return std::nullopt;
        std::vector<Engine::ViewBudget::HeldView> held      = SceneRenderer::LiveHoldings();
        uint64_t                                  heldTotal = 0;
        std::string                               views;
        for ( const Engine::ViewBudget::HeldView& view : held )
        {
            heldTotal += view.Bytes;
            views += std::format( "{} {} — {}", views.empty() ? ":" : ",", view.Name,
                                  Engine::ViewBudget::FormatMiB( view.Bytes ) );
        }
        return std::format(
             "View '{}' resized {}x{} -> {}x{} past the budget by {} ({}, in use {}); a live view's "
             "resize is never refused, close another view to get back under it. Open views hold {}{}",
             viewName, from.Width, from.Height, to.Width, to.Height, Engine::ViewBudget::FormatMiB( overrun ),
             Engine::ViewBudget::DescribeCeiling( reading ), Engine::ViewBudget::FormatMiB( reading.UsageBytes ),
             Engine::ViewBudget::FormatMiB( heldTotal ), views );
    }
} // namespace Desert::Graphic
