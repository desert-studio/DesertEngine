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
    // Drives the atmosphere sun's transform from the sky's clock, and only while the sky asks for it
    // (SkyAtmosphereData::DriveSunFromTimeOfDay).
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
            auto atmospheres = registry.view<ECS::SkyAtmosphereComponent>();
            if ( atmospheres.begin() == atmospheres.end() )
                return;

            // The same "lowest id wins" rule the sky collector uses, so the clock that drives the sun and
            // the sky that is drawn are never two different components.
            std::vector<entt::entity> skyEntities;
            std::vector<uint64_t>     skyIds;
            for ( const auto entity : atmospheres )
            {
                skyEntities.push_back( entity );
                skyIds.push_back( EntityId( registry, entity ) );
            }

            const auto primarySky = Graphic::SelectPrimarySky( skyIds );
            if ( !primarySky )
                return;

            auto& sky = registry.get<ECS::SkyAtmosphereComponent>( skyEntities[*primarySky] );
            if ( !sky.Data.DriveSunFromTimeOfDay )
                return;

            sky.Data.TimeOfDay =
                 Graphic::AdvanceTimeOfDay( sky.Data.TimeOfDay, ts.GetSeconds(), sky.Data.DayLengthSeconds );

            const glm::vec3 travel =
                 Graphic::SunDirectionFromTimeOfDay( sky.Data.TimeOfDay, sky.Data.Latitude, sky.Data.NorthOffset );

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
                     "[TimeOfDay] Sky Atmosphere 'Drive Sun From Time Of Day' is on: the sun light's direction "
                     "comes from the clock ({:.2f} h, latitude {:.1f} deg) at elevation {:.1f} deg, replacing "
                     "the authored {:.1f} deg. Untick it to keep the light's own direction.",
                     sky.Data.TimeOfDay, sky.Data.Latitude,
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
