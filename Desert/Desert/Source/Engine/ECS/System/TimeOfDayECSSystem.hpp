#pragma once

#include "System.hpp"
#include "SystemRules.hpp"

#include <Engine/ECS/AtmosphereSunLight.hpp>
#include <Engine/ECS/Components.hpp>
#include <Common/Core/Logger.hpp>
#include <Engine/Graphic/SkyRules.hpp>
#include <Engine/Graphic/SunPosition.hpp>

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>

#include <vector>

namespace Desert::ECS
{
    // Drives the atmosphere sun's transform from the scene's clock, ECS::TimeOfDayComponent, and only while
    // the clock asks for it (TimeOfDayData::DriveSunFromTimeOfDay). The clock is its own component, as UE's
    // SunPosition / SunSky is its own actor: the sky never owns the hour, it reads the light this writes.
    //
    // It writes the LIGHT'S TRANSFORM rather than publishing a direction of its own, because the engine
    // already has exactly one source of truth for where the sun is — the atmosphere sun light — and a
    // second one would be a value that disagrees with itself the moment somebody drags the gizmo.
    //
    // The arithmetic is in Graphic::SunPositionAt / AdvanceTimeOfDay (Engine/Graphic/SunPosition.hpp, UE
    // SunPosition's calendar); this class only fetches
    // its arguments and writes the result back, which is what keeps the interesting half testable with no
    // GPU and no scene.
    // DOES NOT HONOUR VisibilityComponent, AND MUST NOT: it is a CLOCK. It advances TimeOfDay and writes the
    // sun's transform, so freezing it on hide would make unhiding a sun a jump back in time. Nothing lit is
    // affected -- SkyboxECSSystem drops the hidden light from its sun candidates and Scene.cpp drops it from
    // the deferred light list. Verdict and mutation gate: Desert/Tests/Engine/VisibilityHonoured.
    class TimeOfDayECSSystem : public System
    {
    public:
        using System::System;

        // MUST stay false. It writes TransformComponent, which the light collector, the shadow path and
        // the sky collector all read within the same frame — a `true` here is a data race, not a speedup.
        bool CanRunParallel() const override
        {
            return false;
        }

        void Update( entt::registry& registry, Graphic::Render::RenderCommandBuffer& /*renderCommandBuffer*/,
                     const Common::Timestep& ts ) override
        {
            auto clocks = registry.view<ECS::TimeOfDayComponent>();
            if ( clocks.begin() == clocks.end() )
                return;

            // One clock per scene: the lowest id wins, by the same rule the sky collector picks the primary
            // sky with, so two clocks never take turns at the same light.
            std::vector<entt::entity> clockEntities;
            std::vector<uint64_t>     clockIds;
            for ( const auto entity : clocks )
            {
                clockEntities.push_back( entity );
                clockIds.push_back( EntityId( registry, entity ) );
            }

            const auto primaryClock = Graphic::SelectPrimarySky( clockIds );
            if ( !primaryClock )
                return;

            auto& clock = registry.get<ECS::TimeOfDayComponent>( clockEntities[*primaryClock] ).Data;
            if ( !clock.DriveSunFromTimeOfDay )
                return;

            // A date the calendar does not have stops the clock rather than becoming a neighbouring day: the
            // sun would otherwise sit at a declination nobody authored. Said once per bad date.
            if ( !Graphic::IsValidSunDate( clock.Year, clock.Month, clock.Day ) )
            {
                const int64_t badDate = ( int64_t( clock.Year ) * 100 + clock.Month ) * 100 + clock.Day;
                if ( badDate != m_InvalidDateLogged )
                {
                    m_InvalidDateLogged = badDate;
                    LOG_ERROR( "[TimeOfDay] Time Of Day date {:04}-{:02}-{:02} is not a calendar date; the sun is "
                               "not driven until Year / Month / Day name one.",
                               clock.Year, clock.Month, clock.Day );
                }
                return;
            }
            m_InvalidDateLogged = 0;

            Graphic::SunCalendar calendar;
            calendar.Year               = clock.Year;
            calendar.Month              = clock.Month;
            calendar.Day                = clock.Day;
            calendar.TimeZoneHours      = clock.TimeZone;
            calendar.DaylightSavingTime = clock.DaylightSavingTime;
            calendar.LatitudeDeg        = clock.Latitude;
            calendar.LongitudeDeg       = clock.Longitude;
            calendar.NorthOffsetDeg     = clock.NorthOffset;

            // Midnight carries into the date, so a running day walks the calendar (and the declination) on.
            const Graphic::ClockAdvance advance =
                 Graphic::AdvanceTimeOfDay( clock.TimeOfDay, ts.GetSeconds(), clock.DayLengthSeconds );
            clock.TimeOfDay = advance.Hours;
            if ( advance.Days != 0 )
            {
                Graphic::AddSunDays( calendar, advance.Days );
                clock.Year  = calendar.Year;
                clock.Month = calendar.Month;
                clock.Day   = calendar.Day;
            }

            const Graphic::SunPosition position = Graphic::SunPositionAt( calendar, clock.TimeOfDay );
            const glm::vec3            travel   = position.Travel;

            // The SAME light the sky, the deferred pass and the shadows read (ECS::PickAtmosphereSun), with
            // the clock's candidacy: hidden lights are still driven.
            const auto sun = PickAtmosphereSun( registry, SunCandidacy::Driven ).Sun();
            if ( !sun )
                return;

            // THE CLOCK OVERRIDES THE AUTHORED DIRECTION, and it says so once per sun with the numbers. A
            // scene saved with a sun at +50 deg and the clock at 18.9 h renders a night sky at -9.5 deg, and
            // nothing used to connect the two: the light's Rotation in the file and the sky on screen
            // simply disagreed (Mirage MainMenu, SUN-SOURCE).
            if ( *sun != m_DriveLoggedFor )
            {
                m_DriveLoggedFor     = *sun;
                const float authored = glm::degrees(
                     std::asin( glm::clamp( -DirectionalLightTravel( registry, *sun ).y, -1.0f, 1.0f ) ) );
                LOG_INFO(
                     "[TimeOfDay] Time Of Day 'Drive Sun From Time Of Day' is on: the sun light's rotation "
                     "comes from the clock ({:04}-{:02}-{:02} {:.2f} h, UTC{:+.1f}{}, latitude {:.1f} deg, "
                     "longitude {:.1f} deg) at elevation {:.1f} deg, azimuth {:.1f} deg, declination {:.1f} deg, "
                     "replacing the authored {:.1f} deg. Untick it to keep the light's own rotation.",
                     clock.Year, clock.Month, clock.Day, clock.TimeOfDay, clock.TimeZone,
                     clock.DaylightSavingTime ? " DST" : "", clock.Latitude, clock.Longitude,
                     position.CorrectedElevationDeg, position.AzimuthDeg, position.DeclinationDeg, authored );
            }

            // UE SunPosition sets the light's world rotation; the light's Translation (its icon's place) is
            // not the clock's business.
            AimDirectionalLight( registry, *sun, travel );
        }

    private:
        static uint64_t EntityId( entt::registry& registry, entt::entity entity )
        {
            if ( auto* id = registry.try_get<ECS::UUIDComponent>( entity ) )
                return static_cast<uint64_t>( id->UUID );
            return 0;
        }

        // The sun the clock last announced it drives; a different light (or a reloaded scene) says it again.
        entt::entity m_DriveLoggedFor = entt::null;
        // The invalid date (yyyymmdd) last refused; 0 = the date is valid.
        int64_t m_InvalidDateLogged = 0;
    };
} // namespace Desert::ECS
