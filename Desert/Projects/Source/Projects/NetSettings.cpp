#include "NetSettings.hpp"

#include "EngineConfig.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string_view>

namespace Desert::Project
{
    namespace
    {
        // One byte on the wire carries a period (the snapshot entry's EverySteps).
        constexpr std::uint32_t kMaxPeriodSteps          = 255;
        constexpr std::size_t   kMaxBands                = 255;
        constexpr std::uint32_t kMinBudgetBytesPerSecond = 1024;

        // Nullopt when @p steps is a period the wire and the snapshot cadence can carry; otherwise the reason.
        std::optional<std::string> CheckPeriod( std::string_view what, std::uint32_t steps, std::uint32_t cadence )
        {
            if ( steps < 1 || steps > kMaxPeriodSteps )
                return std::format( "{} is {} steps: a period is 1..{} steps (one byte on the wire)", what, steps,
                                    kMaxPeriodSteps );
            if ( steps % cadence != 0 )
                return std::format(
                     "{} is {} steps, not a multiple of the first band's {} (snapshots leave only on "
                     "that cadence)",
                     what, steps, cadence );
            return std::nullopt;
        }
    } // namespace

    std::optional<std::string> ValidateNetSettings( const NetSettings& s )
    {
        if ( s.Bands.empty() )
            return std::string(
                 "the Net settings have no rate band: at least one is needed (its EverySteps is the "
                 "snapshot cadence)" );
        if ( s.Bands.size() > kMaxBands )
            return std::format( "the Net settings have {} rate bands: at most {}", s.Bands.size(), kMaxBands );

        const std::uint32_t cadence = s.Bands.front().EverySteps;
        if ( cadence < 1 || cadence > kMaxPeriodSteps )
            return std::format( "Net band 0 sends every {} steps: a period is 1..{} steps (one byte on the wire)",
                                cadence, kMaxPeriodSteps );
        for ( std::size_t i = 0; i < s.Bands.size(); ++i )
        {
            const NetRateBand& band = s.Bands[i];
            if ( auto bad = CheckPeriod( std::format( "Net band {} EverySteps", i ), band.EverySteps, cadence ) )
                return bad;
            if ( i > 0 && band.EverySteps < s.Bands[i - 1].EverySteps )
                return std::format( "Net band {} sends every {} steps, more often than the nearer band {} ({}): "
                                    "EverySteps must not decrease with distance",
                                    i, band.EverySteps, i - 1, s.Bands[i - 1].EverySteps );
            // The last band reaches everything farther; its ToCm is not read.
            if ( i + 1 == s.Bands.size() )
                continue;
            if ( !std::isfinite( band.ToCm ) )
                return std::format( "Net band {} reaches {} cm: ToCm must be a finite distance", i, band.ToCm );
            if ( i > 0 && band.ToCm <= s.Bands[i - 1].ToCm )
                return std::format( "Net band {} reaches {} cm, not past band {} ({} cm): ToCm must ascend", i,
                                    band.ToCm, i - 1, s.Bands[i - 1].ToCm );
        }
        if ( auto bad = CheckPeriod( "Net DormantEverySteps", s.DormantEverySteps, cadence ) )
            return bad;
        if ( s.DormantEverySteps < s.Bands.back().EverySteps )
            return std::format(
                 "Net DormantEverySteps {} is below the last band's EverySteps {}: an unchanged pawn "
                 "would go more often than a changing one",
                 s.DormantEverySteps, s.Bands.back().EverySteps );
        if ( !std::isfinite( s.PawnCullRangeCm ) || s.PawnCullRangeCm <= 0.0f )
            return std::format( "Net PawnCullRangeCm {} must be above 0", s.PawnCullRangeCm );
        if ( !std::isfinite( s.MaxViewRangeCm ) || s.MaxViewRangeCm < s.PawnCullRangeCm )
            return std::format( "Net MaxViewRangeCm {} is below PawnCullRangeCm {}: a reported view could not "
                                "reach the circle every connection sees",
                                s.MaxViewRangeCm, s.PawnCullRangeCm );
        if ( s.BudgetBytesPerSecond < kMinBudgetBytesPerSecond )
            return std::format( "Net BudgetBytesPerSecond {} is below {}", s.BudgetBytesPerSecond,
                                kMinBudgetBytesPerSecond );
        return std::nullopt;
    }

    const NetSettings& CurrentNetSettings()
    {
        return CurrentEngineConfig().Effective.Net;
    }

    const NetSettings& ProjectNetSettings()
    {
        return CurrentEngineConfig().Project.Net;
    }

    Common::BoolResultStr SaveNetSettings( const NetSettings& toSave )
    {
        if ( const auto invalid = ValidateNetSettings( toSave ); invalid.has_value() )
            return Common::MakeError<bool>( std::format( "the Net settings were not saved: {}", *invalid ) );
        // The parsed project layer with only its Net section replaced: the other sections are written back as
        // read.
        EngineConfig project = CurrentEngineConfig().Project;
        project.Net          = toSave;
        return SaveEngineConfig( project );
    }
} // namespace Desert::Project
