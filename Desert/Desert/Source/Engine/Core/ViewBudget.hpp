#pragma once

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Engine::ViewBudget
{
    /**
     * @brief Who is asking for a new view (a SceneRenderer), and therefore how much it may take.
     *
     * WHY THE RULE IS IN BYTES. Views used to be counted against six renderer slots, and a seventh shared
     * slot 0's per-frame state. Per-frame state now lives in each view's own ViewResources, so the count
     * stopped meaning anything: what actually runs out is device memory. A 4K scene view and a 256 px
     * Details preview differ by two orders of magnitude, and a rule that counted them as one each was
     * refusing the cheap one while waving the expensive one through.
     *
     * WHY IT IS ITS OWN HEADER. Several callers need the same answer — the Details preview, ThumbnailService
     * and the UI render-texture producer (which ships, so the rule lives in the engine, not the editor) —
     * and two spellings of one policy is how they come to disagree. It is pure and names no device so
     * Desert/Tests/Engine/ViewBudget can drive every branch without a GPU; the one place that reads the
     * device and the `--view-budget-mib` override is Graphic/ViewBudgetGate.
     */
    enum class Demand
    {
        /// A surface the person opened and is looking at: a scene view, a material document, the Details
        /// preview under the row they clicked, a render-texture element an author placed on a canvas. It
        /// may take the last byte — refusing it would refuse the thing that was asked for.
        UserSurface,

        /// Work nobody asked for by name: an asset thumbnail. It must leave room for the next thing the
        /// person opens, so it keeps a reserve (see MayCreate).
        Background
    };

    /// Where the ceiling came from. Every refusal and the start-up line say which, because the three are
    /// different facts and must not print as one number.
    enum class CeilingSource
    {
        DriverBudget, ///< VK_EXT_memory_budget: what the driver will let this process hold
        HeapSize,     ///< the driver would not say; the device-local heaps' total size stands in
        CommandLine   ///< `--view-budget-mib`, set to reproduce a small device on a large one
    };

    /// The device's side of the question, reduced to the numbers the rule needs.
    struct Reading
    {
        uint64_t      CeilingBytes = 0;
        CeilingSource Source       = CeilingSource::HeapSize;
        /// Bytes this process already holds in device-local memory. When `UsageKnown` is false the driver
        /// did not say, and this is the sum of what the open views hold — the only part this process can
        /// count itself — and the refusal text says so.
        uint64_t UsageBytes = 0;
        bool     UsageKnown = false;
    };

    /**
     * @brief Turns a device reading into the ceiling and usage the rule uses.
     *
     * `budgetKnown`/`driverBudget`/`driverUsage`/`deviceLocalHeapSize` are Engine::DeviceMemoryReport's
     * fields, passed flat so this header needs no device. `overrideBytes` is `--view-budget-mib` in bytes,
     * 0 when not given. `heldByViews` is what the open views hold; it stands in for usage only when the
     * driver gave none.
     */
    [[nodiscard]] constexpr Reading ReadCeiling( const bool budgetKnown, const uint64_t driverBudget,
                                                 const uint64_t driverUsage, const uint64_t deviceLocalHeapSize,
                                                 const uint64_t overrideBytes,
                                                 const uint64_t heldByViews ) noexcept
    {
        Reading reading;
        reading.UsageKnown = budgetKnown;
        reading.UsageBytes = budgetKnown ? driverUsage : heldByViews;
        if ( overrideBytes != 0 )
        {
            reading.CeilingBytes = overrideBytes;
            reading.Source       = CeilingSource::CommandLine;
        }
        else if ( budgetKnown )
        {
            reading.CeilingBytes = driverBudget;
            reading.Source       = CeilingSource::DriverBudget;
        }
        else
        {
            reading.CeilingBytes = deviceLocalHeapSize;
            reading.Source       = CeilingSource::HeapSize;
        }
        return reading;
    }

    struct Verdict
    {
        bool     Ok           = false;
        uint64_t RequestBytes = 0; ///< the new view's forecast (ViewTargetCensus at its extent)
        uint64_t ReserveBytes = 0; ///< kept free for the next user surface; 0 for a UserSurface
        uint64_t CeilingBytes = 0;
        uint64_t UsageBytes   = 0;
        uint64_t FreeBytes    = 0; ///< ceiling - usage, never negative
    };

    /**
     * @brief May @p who create a view whose forecast is @p requestBytes?
     *
     * The whole policy: the request plus the demand's reserve must fit in what is free. Background work
     * keeps `backgroundReserveBytes` free — the forecast of the main view at the current window size, so
     * that re-opening or resizing the view a person actually looks at is never what runs out — and a user
     * surface keeps nothing. Written as one reservation so the two answers cannot drift apart. Total, and
     * safe against overflow at every step.
     */
    [[nodiscard]] constexpr Verdict MayCreate( const Demand who, const uint64_t requestBytes,
                                               const uint64_t backgroundReserveBytes,
                                               const Reading& reading ) noexcept
    {
        Verdict verdict;
        verdict.RequestBytes = requestBytes;
        verdict.ReserveBytes = who == Demand::Background ? backgroundReserveBytes : 0;
        verdict.CeilingBytes = reading.CeilingBytes;
        verdict.UsageBytes   = reading.UsageBytes;
        verdict.FreeBytes =
             reading.UsageBytes >= reading.CeilingBytes ? 0 : reading.CeilingBytes - reading.UsageBytes;
        verdict.Ok = requestBytes <= verdict.FreeBytes && verdict.ReserveBytes <= verdict.FreeBytes - requestBytes;
        return verdict;
    }

    /**
     * @brief May an open view resize its targets from @p currentBytes to @p resizedBytes?
     *
     * The resize releases the old targets, so only the growth has to fit: shrinking is always allowed, and
     * growing asks MayCreate for the difference as a user surface (a view being resized is on screen). A
     * refusal leaves the view at its old size: its old targets are still valid, which a half-done rebuild
     * would not be. The verdict's RequestBytes is the growth, so the refusal text states what was missing.
     */
    [[nodiscard]] constexpr Verdict MayResize( const uint64_t currentBytes, const uint64_t resizedBytes,
                                               const Reading& reading ) noexcept
    {
        const uint64_t growth = resizedBytes > currentBytes ? resizedBytes - currentBytes : 0;
        return MayCreate( Demand::UserSurface, growth, 0, reading );
    }

    /// One open view and what it holds (SceneRenderer::HeldBytes), for the refusal text.
    struct HeldView
    {
        std::string Name;
        uint64_t    Bytes = 0;
    };

    [[nodiscard]] inline std::string FormatMiB( const uint64_t bytes )
    {
        return std::format( "{:.1f} MiB", static_cast<double>( bytes ) / ( 1024.0 * 1024.0 ) );
    }

    /// The ceiling and where it came from, in words. Printed once at start-up and inside every refusal.
    [[nodiscard]] inline std::string DescribeCeiling( const Reading& reading )
    {
        switch ( reading.Source )
        {
            case CeilingSource::DriverBudget:
                return std::format( "device-local budget {} (driver, VK_EXT_memory_budget)",
                                    FormatMiB( reading.CeilingBytes ) );
            case CeilingSource::HeapSize:
                return std::format( "device-local budget unknown (VK_EXT_memory_budget absent), ceiling = "
                                    "device-local heap size {}",
                                    FormatMiB( reading.CeilingBytes ) );
            case CeilingSource::CommandLine:
                return std::format( "device-local budget {} (--view-budget-mib)",
                                    FormatMiB( reading.CeilingBytes ) );
        }
        return "device-local budget: unrecognised source";
    }

    /**
     * @brief The refusal, with every number that decided it: what was requested, the ceiling and its
     *        source, what is in use, the reserve, and what each open view holds.
     */
    [[nodiscard]] inline std::string DescribeRefusal( const std::string_view viewName, const Verdict& verdict,
                                                      const Reading& reading, std::vector<HeldView> held )
    {
        std::sort( held.begin(), held.end(),
                   []( const HeldView& a, const HeldView& b ) { return a.Bytes > b.Bytes; } );
        uint64_t heldTotal = 0;
        for ( const HeldView& view : held )
            heldTotal += view.Bytes;

        std::string text =
             std::format( "View '{}' needs {}; {}, in use {}{}", viewName, FormatMiB( verdict.RequestBytes ),
                          DescribeCeiling( reading ), FormatMiB( verdict.UsageBytes ),
                          reading.UsageKnown ? "" : " (driver reports no usage: counted from open views only)" );
        if ( verdict.ReserveBytes != 0 )
            text += std::format( "; background work keeps {} free for the main view",
                                 FormatMiB( verdict.ReserveBytes ) );
        text += std::format( "; open views hold {}", FormatMiB( heldTotal ) );
        for ( std::size_t i = 0; i < held.size(); ++i )
            text += std::format( "{} {} — {}", i == 0 ? ":" : ",", held[i].Name, FormatMiB( held[i].Bytes ) );
        return text;
    }
} // namespace Desert::Engine::ViewBudget
