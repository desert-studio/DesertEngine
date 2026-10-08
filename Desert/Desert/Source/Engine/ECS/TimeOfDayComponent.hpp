#pragma once

#include <Engine/Reflection/ReflectionMacros.hpp>

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
    // These five fields lived on SkyAtmosphereData until TOD-SPLIT; the SceneMigrator step
    // MigrateTimeOfDayComponentV42ToV43 moved them onto a TimeOfDay block on the sky's own entity.
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

        PROPERTY( DisplayName( "North Offset" ), Category( "Time Of Day" ), Range( 0.0f, 360.0f ), Units( "deg" ),
                  EditCondition( "DriveSunFromTimeOfDay" ) )
        float NorthOffset = 0.0f;
    };

    struct TimeOfDayComponent
    {
        TimeOfDayData Data;
    };
} // namespace Desert::ECS
