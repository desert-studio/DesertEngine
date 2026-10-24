#pragma once

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/ECS/System/SystemRules.hpp>

#include <entt/entt.hpp>

#include <optional>
#include <vector>

namespace Desert::ECS
{
    // THE SUN OF THE SCENE — the one Directional Light every consumer of "where is the sun" reads.
    //
    // UE's model: the Sky Atmosphere takes its sun from the Directional Light flagged Atmosphere Sun Light,
    // and that SAME light is the one the deferred pass shades with, the shadow cascades fit to, the fog's
    // directional lobe and the clouds march toward. This engine used to answer the question four times —
    // the sky collector (marked light, hidden ones dropped), the deferred light list in Scene.cpp (every
    // light in group order, truncated to the first), the view's shadow settings (first light in view order)
    // and the time-of-day clock (marked light, hidden ones kept) — and with two directional lights in a
    // scene the sky could stand on one while the ground was lit and shadowed by the other. Now every one of
    // them calls PickAtmosphereSun and only the CANDIDACY differs, by name.
    //
    // NO DEFAULT SUN. A scene with no usable directional light has no sun: the caller says so in the log and
    // draws no sun, instead of lighting the sky from a direction nobody authored.
    enum class SunCandidacy
    {
        // What is LIT this frame: hidden lights and lights whose Translation is not a direction drop out.
        Lit,
        // What the time-of-day clock DRIVES: every light, hidden or degenerate, because the clock writes
        // the direction (a degenerate one gets a unit length) and must not jump when a sun is unhidden.
        Driven,
    };

    struct AtmosphereSunPick
    {
        std::vector<entt::entity>        Entities; // parallel to Candidates
        std::vector<Rules::SunCandidate> Candidates;
        Rules::AtmosphereSunSelection    Selection;

        [[nodiscard]] std::optional<entt::entity> Sun() const
        {
            if ( !Selection.Chosen )
                return std::nullopt;
            return Entities[*Selection.Chosen];
        }
    };

    [[nodiscard]] inline uint64_t SunCandidateId( const entt::registry& registry, entt::entity entity )
    {
        if ( const auto* id = registry.try_get<UUIDComponent>( entity ) )
            return static_cast<uint64_t>( id->UUID );
        return 0;
    }

    // The light at UE's Atmosphere Sun Light Index `atmosphereIndex` (0 = the sun, 1 = the moon;
    // Rules::kAtmosphereLightCount). Index 0 falls back to the lowest-id unmarked light; index 1 never does.
    [[nodiscard]] inline AtmosphereSunPick PickAtmosphereLight( const entt::registry& registry,
                                                                SunCandidacy candidacy, int atmosphereIndex )
    {
        AtmosphereSunPick pick;

        const auto lights = registry.view<const DirectionLightComponent, const TransformComponent>();
        for ( const entt::entity entity : lights )
        {
            if ( candidacy == SunCandidacy::Lit && IsHidden( registry, entity ) )
                continue;

            const DirectionalLightData& light       = lights.get<const DirectionLightComponent>( entity ).Data;
            const glm::vec3&            translation = lights.get<const TransformComponent>( entity ).Translation;

            pick.Entities.push_back( entity );
            pick.Candidates.push_back(
                 Rules::SunCandidate{ .Id             = SunCandidateId( registry, entity ),
                                      .Marked         = light.AtmosphereSunLight,
                                      .Index          = light.AtmosphereSunLightIndex,
                                      .DirectionValid = candidacy == SunCandidacy::Driven ||
                                                        Rules::IsSunDirectionValid( translation ) } );
        }

        pick.Selection = Rules::SelectAtmosphereSun( pick.Candidates, atmosphereIndex );
        return pick;
    }

    // THE sun: atmosphere light 0 — the one the clock drives and the cascaded shadow maps fit to.
    [[nodiscard]] inline AtmosphereSunPick PickAtmosphereSun( const entt::registry& registry, SunCandidacy candidacy )
    {
        return PickAtmosphereLight( registry, candidacy, 0 );
    }
} // namespace Desert::ECS
