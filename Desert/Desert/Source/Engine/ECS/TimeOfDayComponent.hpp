#pragma once

#include <CoreReflection/ReflectionMacros.hpp>

namespace Desert::ECS
{
    // The clock that turns the sun: UE's SunPosition / SunSky split, where the hour, the latitude and the
    // north offset belong to their own actor and NOT to the sky. The sky reads the light; this component
    // rotates the light (ECS::TimeOfDayECSSystem); the atmosphere sun DirectionalLight stays the one source
    // of truth for where the sun is, so dragging the light's gizmo and driving it from the clock can never
    // disagree about a second direction.
    //
    // One clock per scene: when several entities carry this component, the lowest UUID drives the sun
    // (Graphic::SelectPrimarySky, the same rule the sky collector applies to skies).
    //
    // The first five fields lived on SkyAtmosphereData until TOD-SPLIT; the SceneMigrator step
    // MigrateTimeOfDayComponentV43ToV44 moved them onto a TimeOfDay block on the sky's own entity. The
    // calendar (date, longitude, time zone, daylight saving) is UE SunPosition's and arrived with v58
    // (MigrateSunCalendarV57ToV58): before it the sun ran the equinox path in solar time.
    //
    // Default date: the March 2026 equinox at longitude 0 in UTC, the calendar closest to that old path.
    struct TimeOfDayData
    {
        REFLECT()

        PROPERTY( DisplayName( "Drive Sun From Time Of Day" ), Category( "Time Of Day" ),
                  Tooltip( "Rotates the atmosphere sun light from the hour below. Off leaves the light where "
                           "it was authored." ) )
        bool DriveSunFromTimeOfDay = false;

        PROPERTY( DisplayName( "Time Of Day" ), Category( "Time Of Day" ), Range( 0.0f, 24.0f ), Units( "h" ),
                  EditCondition( "DriveSunFromTimeOfDay" ) )
        float TimeOfDay = 12.0f;

        PROPERTY( DisplayName( "Day Length" ), Category( "Time Of Day" ), Range( 0.0f, 86400.0f ), Units( "s" ),
                  EditCondition( "DriveSunFromTimeOfDay" ),
                  Tooltip( "Real seconds per in-game day. 0 freezes the sun at Time Of Day." ) )
        float DayLengthSeconds = 600.0f;

        PROPERTY( DisplayName( "Latitude" ), Category( "Time Of Day" ), Range( -90.0f, 90.0f ), Units( "deg" ),
                  EditCondition( "DriveSunFromTimeOfDay" ) )
        float Latitude = 45.0f;

        PROPERTY( DisplayName( "Longitude" ), Category( "Time Of Day" ), Range( -180.0f, 180.0f ), Units( "deg" ),
                  EditCondition( "DriveSunFromTimeOfDay" ), Tooltip( "Positive east of Greenwich." ) )
        float Longitude = 0.0f;

        PROPERTY( DisplayName( "Time Zone" ), Category( "Time Of Day" ), Range( -12.0f, 14.0f ), Units( "h" ),
                  EditCondition( "DriveSunFromTimeOfDay" ),
                  Tooltip( "Offset of the local clock from UTC, positive east (UTC+3 is 3). Time Of Day is read "
                           "in this zone." ) )
        float TimeZone = 0.0f;

        PROPERTY( DisplayName( "Daylight Saving Time" ), Category( "Time Of Day" ),
                  EditCondition( "DriveSunFromTimeOfDay" ),
                  Tooltip( "The local clock runs one hour ahead of the time zone." ) )
        bool DaylightSavingTime = false;

        // The calendar: the date decides the sun's declination, so how high noon gets and where the sun rises
        // and sets. The clock carries midnight into it while the day runs.
        PROPERTY( DisplayName( "Year" ), Category( "Time Of Day" ), Range( 1.0f, 9999.0f ),
                  EditCondition( "DriveSunFromTimeOfDay" ) )
        int Year = 2026;

        PROPERTY( DisplayName( "Month" ), Category( "Time Of Day" ), Range( 1.0f, 12.0f ),
                  EditCondition( "DriveSunFromTimeOfDay" ) )
        int Month = 3;

        PROPERTY( DisplayName( "Day" ), Category( "Time Of Day" ), Range( 1.0f, 31.0f ),
                  EditCondition( "DriveSunFromTimeOfDay" ),
                  Tooltip( "Day of the month; a date the calendar does not have (30 February) stops the clock "
                           "with an error." ) )
        int Day = 20;

        PROPERTY( DisplayName( "North Offset" ), Category( "Time Of Day" ), Range( 0.0f, 360.0f ), Units( "deg" ),
                  EditCondition( "DriveSunFromTimeOfDay" ) )
        float NorthOffset = 0.0f;
    };

    struct TimeOfDayComponent
    {
        COMPONENT( Key( "TimeOfDay" ), Block( Data ), Run( SkyAndAtmosphere ) )
        TimeOfDayData Data;
    };
} // namespace Desert::ECS
