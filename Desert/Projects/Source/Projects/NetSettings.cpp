#include "NetSettings.hpp"

#include "EngineConfig.hpp"

#include <cmath>
#include <format>

namespace Desert::Project
{
    std::optional<std::string> ValidateNetSettings( const NetSettings& s )
    {
        if ( s.Bands.empty() )
            return std::string(
                 "the Net settings have no rate band: at least one is needed (its EverySteps is the "
                 "snapshot cadence)" );
        for ( std::size_t i = 0; i < s.Bands.size(); ++i )
        {
            const NetRateBand& band = s.Bands[i];
            if ( band.EverySteps < 1 )
                return std::format( "Net band {} sends every {} steps: EverySteps must be at least 1", i,
                                    band.EverySteps );
            // The last band reaches everything farther; its ToCm is not read.
            if ( i + 1 == s.Bands.size() )
                break;
            if ( !std::isfinite( band.ToCm ) || band.ToCm <= 0.0f )
                return std::format( "Net band {} reaches {} cm: ToCm must be a positive distance", i, band.ToCm );
            if ( i > 0 && band.ToCm <= s.Bands[i - 1].ToCm )
                return std::format( "Net band {} reaches {} cm, not past band {} ({} cm): ToCm must ascend", i,
                                    band.ToCm, i - 1, s.Bands[i - 1].ToCm );
        }
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
        if ( s.BudgetBytesPerSecond < 1024 )
            return std::format( "Net BudgetBytesPerSecond {} is below 1024", s.BudgetBytesPerSecond );
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
} // namespace Desert::Project
