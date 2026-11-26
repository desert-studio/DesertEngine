#include "Relevancy.hpp"

#include "Replication.hpp"
#include "WireCodec.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace Desert::Net
{
    std::optional<std::string> ValidateRelevancySettings( const RelevancySettings& settings )
    {
        if ( settings.Bands.empty() )
            return std::string(
                 "Net settings: Bands is empty - at least one rate band is needed (its EverySteps is "
                 "the snapshot cadence)" );
        if ( settings.Bands.size() > 255 )
            return std::format( "Net settings: {} bands; at most 255", settings.Bands.size() );
        for ( std::size_t i = 0; i < settings.Bands.size(); ++i )
        {
            const RateBand& band = settings.Bands[i];
            if ( band.EverySteps < 1 || band.EverySteps > 255 )
                return std::format(
                     "Net settings: Bands[{}].EverySteps = {}; a period is 1..255 server steps (one "
                     "byte on the wire)",
                     i, band.EverySteps );
            const bool open = i + 1 == settings.Bands.size(); // the last band's ToCm is not read
            if ( !open && !( std::isfinite( band.ToCm ) && band.ToCm > 0.f ) )
                return std::format( "Net settings: Bands[{}].ToCm = {}; a band reaches a positive distance in cm",
                                    i, band.ToCm );
            if ( !open && i > 0 && band.ToCm <= settings.Bands[i - 1].ToCm )
                return std::format( "Net settings: Bands[{}].ToCm = {} is not past Bands[{}].ToCm = {}; bands are "
                                    "ascending by ToCm",
                                    i, band.ToCm, i - 1, settings.Bands[i - 1].ToCm );
        }
        if ( settings.DormantEverySteps < settings.Bands.back().EverySteps || settings.DormantEverySteps > 255 )
            return std::format(
                 "Net settings: DormantEverySteps = {}; it is between the last band's period ({}) and "
                 "255 steps",
                 settings.DormantEverySteps, settings.Bands.back().EverySteps );
        if ( !( std::isfinite( settings.PawnCullRangeCm ) && settings.PawnCullRangeCm > 0.f ) )
            return std::format( "Net settings: PawnCullRangeCm = {}; the circle every connection sees is positive",
                                settings.PawnCullRangeCm );
        if ( !( std::isfinite( settings.MaxViewRangeCm ) && settings.MaxViewRangeCm >= settings.PawnCullRangeCm ) )
            return std::format( "Net settings: MaxViewRangeCm = {} is below PawnCullRangeCm = {}",
                                settings.MaxViewRangeCm, settings.PawnCullRangeCm );
        if ( !( std::isfinite( settings.ExitMargin ) && settings.ExitMargin >= 0.f ) )
            return std::format( "Net settings: ExitMargin = {} (the World Partition UnloadMargin); it is >= 0",
                                settings.ExitMargin );
        if ( settings.BudgetBytesPerSecond < 1024 )
            return std::format( "Net settings: BudgetBytesPerSecond = {}; at least 1024",
                                settings.BudgetBytesPerSecond );
        return std::nullopt;
    }

    std::vector<std::byte> EncodeViewReport( const ViewReport& report )
    {
        Wire::Writer out( EReplicationMessage::ViewReport );
        out.F32( report.FacingYawDeg );
        out.F32( report.SectorAngleDeg );
        out.F32( report.RangeCm );
        return out.Take();
    }

    std::optional<ViewReport> DecodeViewReport( std::span<const std::byte> data )
    {
        Wire::Reader in( data, EReplicationMessage::ViewReport );
        ViewReport   report;
        if ( !in.F32( report.FacingYawDeg ) || !in.F32( report.SectorAngleDeg ) || !in.F32( report.RangeCm ) ||
             !in.Done() )
            return std::nullopt;
        if ( !( report.SectorAngleDeg > 0.f && report.SectorAngleDeg <= 360.f ) || report.RangeCm < 0.f )
            return std::nullopt; // not a shape: refused like any malformed message
        return report;
    }

    ViewShape ResolveViewShape( const RelevancySettings& settings, float mainGridLoadingRangeCm,
                                const std::optional<ViewReport>& report )
    {
        ViewShape shape;
        shape.CircleRangeCm = std::max( settings.PawnCullRangeCm, mainGridLoadingRangeCm );
        if ( report && report->RangeCm > 0.f )
        {
            shape.SectorRangeCm  = std::min( report->RangeCm, settings.MaxViewRangeCm );
            shape.SectorAngleDeg = report->SectorAngleDeg;
            shape.FacingYawDeg   = report->FacingYawDeg;
        }
        return shape;
    }

    float GroundDistanceCm( const NetVec3& a, const NetVec3& b )
    {
        const float dx = b.X - a.X, dz = b.Z - a.Z;
        return std::sqrt( dx * dx + dz * dz );
    }

    float RangeRatio( const ViewShape& shape, const NetVec3& viewPosition, const NetVec3& at )
    {
        const float distance = GroundDistanceCm( viewPosition, at );
        float       reach    = shape.CircleRangeCm;
        if ( shape.SectorRangeCm > reach )
        {
            bool inSector = shape.SectorAngleDeg >= 360.f || distance <= 0.f;
            if ( !inSector )
            {
                // The view axis: yaw about +Y, 0 facing -Z (Relevancy.hpp ViewReport).
                constexpr float kDegToRad = 3.14159265358979f / 180.f;
                const float     yaw       = shape.FacingYawDeg * kDegToRad;
                const float     axisX = -std::sin( yaw ), axisZ = -std::cos( yaw );
                const float     cosine =
                     ( ( at.X - viewPosition.X ) * axisX + ( at.Z - viewPosition.Z ) * axisZ ) / distance;
                inSector = cosine >= std::cos( 0.5f * shape.SectorAngleDeg * kDegToRad );
            }
            if ( inSector )
                reach = shape.SectorRangeCm;
        }
        // ValidateRelevancySettings keeps the circle positive; a zero reach is only a point at the viewer.
        if ( reach <= 0.f )
            return distance <= 0.f ? 0.f : std::numeric_limits<float>::infinity();
        return distance / reach;
    }

    std::uint32_t SendPeriodSteps( const RelevancySettings& settings, float distanceCm )
    {
        for ( std::size_t i = 0; i + 1 < settings.Bands.size(); ++i )
            if ( distanceCm < settings.Bands[i].ToCm )
                return settings.Bands[i].EverySteps;
        return settings.Bands.back().EverySteps;
    }

    float SendPriority( std::uint32_t sinceSentSteps, std::uint32_t periodSteps, float distanceCm )
    {
        const float period = static_cast<float>( std::max<std::uint32_t>( periodSteps, 1 ) );
        return static_cast<float>( sinceSentSteps ) / period +
               1.f / ( 1.f + std::max( distanceCm, 0.f ) / 100000.f );
    }

    std::size_t SnapshotEntryBudgetBytes( const RelevancySettings& settings, std::size_t entryBytes )
    {
        const double cadenceSeconds = static_cast<double>( settings.Bands.front().EverySteps ) / kNetStepHz;
        const auto   bytes =
             static_cast<std::size_t>( static_cast<double>( settings.BudgetBytesPerSecond ) * cadenceSeconds );
        return std::max( bytes, entryBytes );
    }

    double PawnInterpolationDelay( std::uint32_t periodSteps, double jitterSeconds, double baseDelaySeconds )
    {
        const double interval = static_cast<double>( periodSteps ) / kNetStepHz;
        return std::max( 2.0 * interval + std::max( jitterSeconds, 0.0 ), baseDelaySeconds );
    }

    double PawnExtrapolationCap( std::uint32_t periodSteps )
    {
        const double interval = static_cast<double>( periodSteps ) / kNetStepHz;
        return std::max( 1.5 * interval, kMaxExtrapolationSeconds );
    }
} // namespace Desert::Net
