#pragma once

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/ECS/System/SystemRules.hpp>

#include <entt/entt.hpp>
#include <glm/gtc/quaternion.hpp>

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
    // NO DEFAULT SUN. A scene with no visible directional light has no sun: the caller says so in the log and
    // draws no sun, instead of lighting the sky from a direction nobody authored.
    enum class SunCandidacy
    {
        // What is LIT this frame: hidden lights drop out.
        Lit,
        // What the time-of-day clock DRIVES: every light, hidden or not, because the clock writes the
        // light's Rotation and the sun must not jump when it is unhidden.
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

    [[nodiscard]] inline AtmosphereSunPick PickAtmosphereSun( const entt::registry& registry,
                                                              SunCandidacy          candidacy )
    {
        AtmosphereSunPick pick;

        const auto lights = registry.view<const DirectionLightComponent>();
        for ( const entt::entity entity : lights )
        {
            if ( candidacy == SunCandidacy::Lit && IsHidden( registry, entity ) )
                continue;

            const DirectionalLightData& light = lights.get<const DirectionLightComponent>( entity ).Data;

            pick.Entities.push_back( entity );
            pick.Candidates.push_back( Rules::SunCandidate{ .Id     = SunCandidateId( registry, entity ),
                                                            .Marked = light.AtmosphereSunLight,
                                                            .Index  = light.AtmosphereSunLightIndex } );
        }

        // The engine renders exactly one directional light, so index 0 is the only one that can be the sun.
        pick.Selection = Rules::SelectAtmosphereSun( pick.Candidates, /*wantedIndex=*/0 );
        return pick;
    }

    // The entity's WORLD rotation: the parents' rotations composed down the hierarchy, as UE composes
    // FTransform rotations. Scale is not part of it, so a zero or mirrored parent scale cannot turn a
    // light's direction into NaN or flip it.
    [[nodiscard]] inline glm::quat WorldRotation( const entt::registry& registry, entt::entity entity )
    {
        glm::quat world = glm::quat( 1.0f, glm::vec3( 0.0f ) );
        for ( entt::entity current = entity; current != entt::null; )
        {
            if ( const auto* transform = registry.try_get<TransformComponent>( current ) )
                world = glm::quat( transform->Rotation ) * world;
            const auto* link = registry.try_get<RelationshipComponent>( current );
            current          = link ? link->Parent : entt::null;
        }
        return glm::normalize( world );
    }

    // The direction a directional light travels (sun -> scene), from its world rotation — what the
    // deferred pass, the shadows, the sky (negated, Rules::AtmosphereSunDirection) and the gizmo all read.
    [[nodiscard]] inline glm::vec3 DirectionalLightTravel( const entt::registry& registry, entt::entity entity )
    {
        return Rules::DirectionalLightTravel( WorldRotation( registry, entity ) );
    }

    // Aims a directional light along `travel` in WORLD space by writing its local Rotation (the parent's
    // world rotation is divided out), so a sun parented under a rotated actor still points where the clock or
    // the direction widget says. UE: SetWorldRotation on the light component.
    inline void AimDirectionalLight( entt::registry& registry, entt::entity entity, const glm::vec3& travel )
    {
        const auto*     link   = registry.try_get<RelationshipComponent>( entity );
        const glm::quat parent = link && link->Parent != entt::null ? WorldRotation( registry, link->Parent )
                                                                    : glm::quat( 1.0f, glm::vec3( 0.0f ) );
        registry.get<TransformComponent>( entity ).Rotation =
             glm::eulerAngles( glm::inverse( parent ) * Rules::RotationForTravel( travel ) );
    }
} // namespace Desert::ECS
