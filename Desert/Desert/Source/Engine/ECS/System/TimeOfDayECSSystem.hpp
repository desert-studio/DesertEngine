#pragma once

#include "System.hpp"
#include "SystemRules.hpp"

#include <Engine/ECS/AtmosphereSunLight.hpp>
#include <Engine/ECS/Components.hpp>
#include <Common/Core/Logger.hpp>
#include <Engine/Graphic/SkyRules.hpp>

#include <glm/glm.hpp>

#include <cmath>

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
    // The arithmetic is in Graphic::SunDirectionFromTimeOfDay / AdvanceTimeOfDay; this class only fetches
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

            clock.TimeOfDay =
                 Graphic::AdvanceTimeOfDay( clock.TimeOfDay, ts.GetSeconds(), clock.DayLengthSeconds );

            const glm::vec3 travel =
                 Graphic::SunDirectionFromTimeOfDay( clock.TimeOfDay, clock.Latitude, clock.NorthOffset );

            // The SAME light the sky, the deferred pass and the shadows read (ECS::PickAtmosphereSun), with
            // the clock's candidacy: hidden and degenerate lights are still driven.
            const auto sun = PickAtmosphereSun( registry, SunCandidacy::Driven ).Sun();
            if ( !sun )
                return;

            auto& transform = registry.get<ECS::TransformComponent>( *sun );

            // THE CLOCK OVERRIDES THE AUTHORED DIRECTION, and it says so once per sun with the numbers. A
            // scene saved with a sun at +50 deg and the clock at 18.9 h renders a night sky at -9.5 deg, and
            // nothing used to connect the two: the light's Translation in the file and the sky on screen
            // simply disagreed (Mirage MainMenu, SUN-SOURCE).
            if ( *sun != m_DriveLoggedFor )
            {
                m_DriveLoggedFor     = *sun;
                const float authored = Rules::IsSunDirectionValid( transform.Translation )
                                            ? glm::degrees( std::asin( glm::clamp(
                                                   -glm::normalize( transform.Translation ).y, -1.0f, 1.0f ) ) )
                                            : 0.0f;
                LOG_INFO(
                     "[TimeOfDay] Time Of Day 'Drive Sun From Time Of Day' is on: the sun light's direction "
                     "comes from the clock ({:.2f} h, latitude {:.1f} deg) at elevation {:.1f} deg, replacing "
                     "the authored {:.1f} deg. Untick it to keep the light's own direction.",
                     clock.TimeOfDay, clock.Latitude,
                     glm::degrees( std::asin( glm::clamp( -travel.y, -1.0f, 1.0f ) ) ), authored );
            }

            // The MAGNITUDE is preserved, not normalized away: some scenes author the sun's Translation as
            // a position-like vector and the editor's own direction widget keeps its length for exactly
            // that reason. Only the heading is ours to drive.
            const float length    = glm::length( transform.Translation );
            transform.Translation = travel * ( length > Rules::kSunDirectionEpsilon ? length : 1.0f );
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
    };
} // namespace Desert::ECS
