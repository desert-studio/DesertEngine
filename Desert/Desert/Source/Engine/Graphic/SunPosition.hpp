#pragma once

// Ported from UE 5.8 Engine/Plugins/Runtime/SunPosition/Source/SunPosition/Private/SunPosition.cpp:11-149
// (USunPositionFunctionLibrary::GetSunPosition, itself NOAA's "Solar Calculations" spreadsheet), adapted: the
// Julian day carries the hour and the time zone (UE feeds the date's midnight only), the refraction fit keeps
// NOAA's bracketing (UE divides its last term by 3600 twice), the direction is built from the hour angle and
// the declination instead of acos() of an azimuth (UE's divides by cos(latitude) * sin(zenith), which is 0 at
// the poles and at the zenith), the date is std::chrono instead of FDateTime, and sunrise / sunset / solar
// noon are not computed because nothing in the engine reads them yet.

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace Desert::Graphic
{
    // The calendar and the place the sun is computed for (UE ASunSky / SunPosition's inputs). Longitude and
    // the time zone are positive EAST of Greenwich; the clock hour handed to SunPositionAt is LOCAL time in
    // that zone, daylight saving included when DaylightSavingTime says so.
    struct SunCalendar
    {
        // No defaults worth the name: the authored ones live on ECS::TimeOfDayData, which fills every field.
        int   Year               = 2000;
        int   Month              = 1;
        int   Day                = 1;
        float TimeZoneHours      = 0.0f;
        bool  DaylightSavingTime = false;
        float LatitudeDeg        = 0.0f;
        float LongitudeDeg       = 0.0f;
        float NorthOffsetDeg     = 0.0f;
    };

    struct SunPosition
    {
        // Above the horizon, before and after atmospheric refraction (UE Elevation / CorrectedElevation,
        // without UE's +180 that only fits its own rotator convention).
        float ElevationDeg          = 0.0f;
        float CorrectedElevationDeg = 0.0f;
        // Clockwise from solar north (east = 90), before NorthOffsetDeg (UE Azimuth).
        float AzimuthDeg     = 0.0f;
        float DeclinationDeg = 0.0f;
        // The direction the sunlight TRAVELS (sun -> scene) at the CORRECTED elevation, after NorthOffsetDeg:
        // exactly what a directional light's TransformComponent::Translation encodes.
        glm::vec3 Travel = glm::vec3( 0.0f, -1.0f, 0.0f );
    };

    inline bool IsValidSunDate( int year, int month, int day )
    {
        return std::chrono::year_month_day( std::chrono::year( year ),
                                            std::chrono::month( static_cast<unsigned>( month ) ),
                                            std::chrono::day( static_cast<unsigned>( day ) ) )
             .ok();
    }

    // The civil date @p days after @p calendar's (negative goes back); the clock carries midnight into it.
    inline void AddSunDays( SunCalendar& calendar, int days )
    {
        using namespace std::chrono;
        const year_month_day next(
             sys_days( year_month_day( year( calendar.Year ), month( static_cast<unsigned>( calendar.Month ) ),
                                       day( static_cast<unsigned>( calendar.Day ) ) ) ) +
             std::chrono::days( days ) );
        calendar.Year  = static_cast<int>( next.year() );
        calendar.Month = static_cast<int>( static_cast<unsigned>( next.month() ) );
        calendar.Day   = static_cast<int>( static_cast<unsigned>( next.day() ) );
    }

    // Where the sun is at @p localHours on @p calendar's date. The date must be valid (IsValidSunDate): the
    // caller refuses a bad one by name, this function does not invent a neighbouring day.
    //
    // Horizon frame: -X east, +Y up, +Z north. The engine is right-handed (X x Y = Z), so with north on +Z east
    // must be -X: +X east made the sky a mirror image (facing south, the sun rose on the right). NorthOffsetDeg
    // rotates solar north onto the scene's north about world +Y.
    inline SunPosition SunPositionAt( const SunCalendar& calendar, float localHours )
    {
        using namespace std::chrono;
        const double toRad = glm::pi<double>() / 180.0;
        const double toDeg = 180.0 / glm::pi<double>();

        const double utcOffsetHours =
             static_cast<double>( calendar.TimeZoneHours ) + ( calendar.DaylightSavingTime ? 1.0 : 0.0 );

        // Julian day of the date's 00:00 UTC (the Unix epoch is JD 2440587.5), plus the UTC fraction of the day.
        const double daysSinceEpoch = static_cast<double>(
             sys_days( year_month_day( year( calendar.Year ), month( static_cast<unsigned>( calendar.Month ) ),
                                       day( static_cast<unsigned>( calendar.Day ) ) ) )
                  .time_since_epoch()
                  .count() );
        const double julianDay =
             daysSinceEpoch + 2440587.5 + ( static_cast<double>( localHours ) - utcOffsetHours ) / 24.0;
        const double T = ( julianDay - 2451545.0 ) / 36525.0;

        const double meanLongDeg  = std::fmod( 280.46646 + T * ( 36000.76983 + T * 0.0003032 ), 360.0 );
        const double meanAnomDeg  = 357.52911 + T * ( 35999.05029 - 0.0001537 * T );
        const double meanLong     = meanLongDeg * toRad;
        const double meanAnom     = meanAnomDeg * toRad;
        const double eccentricity = 0.016708634 - T * ( 0.000042037 + 0.0000001267 * T );

        const double centre = std::sin( meanAnom ) * ( 1.914602 - T * ( 0.004817 + 0.000014 * T ) ) +
                              std::sin( 2.0 * meanAnom ) * ( 0.019993 - 0.000101 * T ) +
                              std::sin( 3.0 * meanAnom ) * 0.000289;
        const double trueLongDeg = meanLongDeg + centre;
        const double omega       = ( 125.04 - 1934.136 * T ) * toRad;
        const double appLong     = ( trueLongDeg - 0.00569 - 0.00478 * std::sin( omega ) ) * toRad;

        const double meanObliqDeg =
             23.0 + ( 26.0 + ( 21.448 - T * ( 46.815 + T * ( 0.00059 - T * 0.001813 ) ) ) / 60.0 ) / 60.0;
        const double obliq       = ( meanObliqDeg + 0.00256 * std::cos( omega ) ) * toRad;
        const double declination = std::asin( std::sin( obliq ) * std::sin( appLong ) );

        const double y = std::pow( std::tan( obliq / 2.0 ), 2.0 );
        const double equationOfTimeMinutes =
             4.0 * toDeg *
             ( y * std::sin( 2.0 * meanLong ) - 2.0 * eccentricity * std::sin( meanAnom ) +
               4.0 * eccentricity * y * std::sin( meanAnom ) * std::cos( 2.0 * meanLong ) -
               0.5 * y * y * std::sin( 4.0 * meanLong ) -
               1.25 * eccentricity * eccentricity * std::sin( 2.0 * meanAnom ) );

        // True solar time in [0, 1440) minutes; the hour angle is 0 at solar noon, positive in the afternoon.
        double trueSolarMinutes =
             std::fmod( static_cast<double>( localHours ) * 60.0 + equationOfTimeMinutes +
                             4.0 * static_cast<double>( calendar.LongitudeDeg ) - 60.0 * utcOffsetHours,
                        1440.0 );
        if ( trueSolarMinutes < 0.0 )
            trueSolarMinutes += 1440.0;
        const double hourAngle = ( trueSolarMinutes / 4.0 - 180.0 ) * toRad;

        // Toward the sun in the horizon frame, from the hour angle and the declination directly: no acos, so
        // no pole or zenith where the azimuth is 0/0.
        const double latitude = static_cast<double>( calendar.LatitudeDeg ) * toRad;
        const double east     = -std::cos( declination ) * std::sin( hourAngle );
        const double north    = std::sin( declination ) * std::cos( latitude ) -
                             std::cos( declination ) * std::cos( hourAngle ) * std::sin( latitude );
        const double up = std::sin( declination ) * std::sin( latitude ) +
                          std::cos( declination ) * std::cos( hourAngle ) * std::cos( latitude );

        const double elevationDeg = std::asin( std::clamp( up, -1.0, 1.0 ) ) * toDeg;

        // NOAA's refraction fit, in arc seconds.
        double refractionArcSec = 0.0;
        if ( elevationDeg <= 85.0 )
        {
            const double tanE = std::tan( elevationDeg * toRad );
            if ( elevationDeg > 5.0 )
                refractionArcSec = 58.1 / tanE - 0.07 / std::pow( tanE, 3.0 ) + 0.000086 / std::pow( tanE, 5.0 );
            else if ( elevationDeg > -0.575 )
                refractionArcSec =
                     1735.0 +
                     elevationDeg *
                          ( -518.2 + elevationDeg * ( 103.4 + elevationDeg * ( -12.79 + elevationDeg * 0.711 ) ) );
            else
                refractionArcSec = -20.772 / tanE;
        }
        const double correctedDeg = elevationDeg + refractionArcSec / 3600.0;

        // Refraction lifts the sun in its own vertical plane: the heading is kept, the elevation replaced.
        // Above 85 degrees the correction is 0 and the horizontal part may vanish, so the vector is kept.
        const double horizontal = std::sqrt( east * east + north * north );
        glm::dvec3   towardSolar( -east, up, north );
        if ( refractionArcSec != 0.0 && horizontal > 1e-9 )
        {
            const double scale = std::cos( correctedDeg * toRad ) / horizontal;
            towardSolar        = glm::dvec3( -east * scale, std::sin( correctedDeg * toRad ), north * scale );
        }

        const double northOffset = static_cast<double>( calendar.NorthOffsetDeg ) * toRad;
        const double c           = std::cos( northOffset );
        const double s           = std::sin( northOffset );
        // A rotation of +northOffset about +Y (right-handed): x' = x cos + z sin, z' = -x sin + z cos.
        const glm::dvec3 towardSun( towardSolar.x * c + towardSolar.z * s, towardSolar.y,
                                    -towardSolar.x * s + towardSolar.z * c );

        double azimuthDeg = std::atan2( east, north ) * toDeg;
        if ( azimuthDeg < 0.0 )
            azimuthDeg += 360.0;

        SunPosition result;
        result.ElevationDeg          = static_cast<float>( elevationDeg );
        result.CorrectedElevationDeg = static_cast<float>( correctedDeg );
        result.AzimuthDeg            = static_cast<float>( azimuthDeg );
        result.DeclinationDeg        = static_cast<float>( declination * toDeg );
        result.Travel                = glm::vec3( -glm::normalize( towardSun ) );
        return result;
    }

    // One frame of the clock: the new local hour and the whole days it crossed (the caller carries them into
    // the date with AddSunDays). @p dayLengthSeconds == 0 freezes the sun at the authored hour, which is what
    // makes "Drive Sun From Time Of Day" usable as a posing tool and not only as an animation.
    struct ClockAdvance
    {
        float Hours = 0.0f;
        int   Days  = 0;
    };

    inline ClockAdvance AdvanceTimeOfDay( float hours, float dtSeconds, float dayLengthSeconds )
    {
        if ( dayLengthSeconds <= 0.0f )
            return { hours, 0 };

        const double next = static_cast<double>( hours ) +
                            static_cast<double>( dtSeconds ) * 24.0 / static_cast<double>( dayLengthSeconds );
        const double days = std::floor( next / 24.0 );
        // Rounding can land a hair under the next midnight on exactly 24.0f; that hour is the next day's 0.
        const float hoursOut = static_cast<float>( next - days * 24.0 );
        if ( hoursOut >= 24.0f )
            return { 0.0f, static_cast<int>( days ) + 1 };
        return { hoursOut, static_cast<int>( days ) };
    }
} // namespace Desert::Graphic
