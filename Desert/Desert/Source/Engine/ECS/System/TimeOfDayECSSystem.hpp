#pragma once

#include "System.hpp"
#include "SystemRules.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/Graphic/SkyRules.hpp>

#include <glm/glm.hpp>

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

            const auto sun = FindAtmosphereSun( registry );
            if ( !sun )
                return;

            auto& transform = registry.get<ECS::TransformComponent>( *sun );

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

        // Same rule as the sky collector's, minus the logging: this system runs first and would otherwise
        // duplicate every warning the collector already emits about the very same lights.
        static std::optional<entt::entity> FindAtmosphereSun( entt::registry& registry )
        {
            std::vector<Rules::SunCandidate> candidates;
            std::vector<entt::entity>        entities;

            auto dirLights = registry.view<ECS::DirectionLightComponent, ECS::TransformComponent>();
            for ( const auto entity : dirLights )
            {
                // The TransformComponent is in the view because a sun must HAVE one to be driven, not
                // because this loop reads it: `DirectionValid` is unconditionally true a few lines below,
                // and the comment there says why a degenerate Translation is not a reason to skip a
                // candidate. Fetching it and dropping it is what `-Wunused-variable` pointed at.
                const auto& light = dirLights.get<ECS::DirectionLightComponent>( entity );

                entities.push_back( entity );
                candidates.push_back( Rules::SunCandidate{
                     .Id     = EntityId( registry, entity ),
                     .Marked = light.Data.AtmosphereSunLight,
                     .Index  = light.Data.AtmosphereSunLightIndex,
                     // A light being DRIVEN starts from whatever it holds, so a degenerate Translation is
                     // not a reason to skip it here — it is a reason to give it a unit-length one.
                     .DirectionValid = true } );
            }

            const auto selection = Rules::SelectAtmosphereSun( candidates, /*wantedIndex=*/0 );
            if ( !selection.Chosen )
                return std::nullopt;
            return entities[*selection.Chosen];
        }
    };
} // namespace Desert::ECS
