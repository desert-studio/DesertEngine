#include "Relevancy.hpp"

#include "Replication.hpp"
#include "WireCodec.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Desert::Net
{
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
        // Project::ValidateNetSettings keeps the circle positive; a zero reach is only a point at the viewer.
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
