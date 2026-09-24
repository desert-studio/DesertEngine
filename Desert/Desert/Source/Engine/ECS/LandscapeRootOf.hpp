#pragma once

// THE ROOT A TILE NAMES, as the frame LandscapeLayout computes with — built in one place. The loader
// validates tiles against it, the renderer draws with it, picking and physics place the surface with it;
// four copies of these five lines would be four chances for the surface to be in four places.

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>
#include <Engine/World/Landscape/LandscapePaint.hpp>

#include <entt/entt.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Desert::ECS
{
    /// @p entity must carry a LandscapeComponent. Only the world TRANSLATION is taken: the frame has no
    /// rotation or scale (Components.hpp, LandscapeComponent), and the loader says so out loud.
    inline World::Landscape::LandscapeRoot LandscapeRootOf( const Entity& entity )
    {
        const auto&                     component = entity.GetComponent<LandscapeComponent>();
        World::Landscape::LandscapeRoot root;
        root.Origin       = glm::vec3( entity.GetWorldTransform()[3] );
        root.QuadsPerTile = component.QuadsPerTile;
        root.SpacingCm    = component.SpacingCm;
        root.ZScale       = component.ZScale;
        return root;
    }

    /// The paint stroke's layer rules, in the component's order — the one conversion from the authored
    /// layer list, so the panel's Hardness and the stroke's normalisation cannot read two different lists.
    inline std::vector<World::Landscape::LandscapeLayerRule> LandscapeLayerRulesOf( const LandscapeComponent& c )
    {
        std::vector<World::Landscape::LandscapeLayerRule> rules;
        rules.reserve( c.Layers.size() );
        for ( const auto& layer : c.Layers )
            rules.push_back( { layer.Name, layer.Hardness, layer.NoWeightBlend } );
        return rules;
    }

    /// The root entity (the one holding the LandscapeComponent) whose UUID is @p id; entt::null when none is
    /// loaded.
    inline entt::entity FindLandscapeRootEntity( entt::registry& registry, const Common::UUID& id )
    {
        if ( id == Common::UUID::Null() )
            return entt::null;
        for ( const auto entity : registry.view<LandscapeComponent, UUIDComponent>() )
        {
            if ( registry.get<UUIDComponent>( entity ).UUID == id )
                return entity;
        }
        return entt::null;
    }

    /// The root entity whose UUID is @p id, or nullopt when no loaded entity with a LandscapeComponent has it.
    inline std::optional<World::Landscape::LandscapeRoot> FindLandscapeRoot( entt::registry&     registry,
                                                                             const Common::UUID& id )
    {
        if ( id == Common::UUID::Null() )
            return std::nullopt;
        for ( const auto entity : registry.view<LandscapeComponent, UUIDComponent>() )
        {
            if ( registry.get<UUIDComponent>( entity ).UUID == id )
                return LandscapeRootOf( Entity( entity, registry ) );
        }
        return std::nullopt;
    }

    /// A tile with loaded heights, under a loaded root it matches, and where that root puts it.
    struct LandscapeTileRef
    {
        entt::entity                     Entity    = entt::null;
        LandscapeTileComponent*          Component = nullptr;
        World::Landscape::LandscapeRoot  Root;
        World::Landscape::LandscapeFrame Frame;
    };

    /// A tile with loaded heights that is NOT drawn, and why.
    struct RefusedLandscapeTile
    {
        entt::entity Entity = entt::null;
        std::string  Reason;
    };

    struct DrawableLandscape
    {
        std::vector<LandscapeTileRef>     Tiles;
        std::vector<RefusedLandscapeTile> Refused;
    };

    /// Every tile the renderer draws, and every loaded tile it refuses with the reason. ONE test for both
    /// consumers: LandscapeECSSystem draws `Tiles` and reports `Refused`; collision builds bodies for
    /// exactly `Tiles`, so a refused tile has neither and the one warning covers both. Unloaded tiles
    /// are in neither list — the loader already said why a tile was refused at load.
    inline DrawableLandscape DrawableLandscapeTiles( entt::registry& registry )
    {
        DrawableLandscape out;
        for ( const auto entity : registry.view<LandscapeTileComponent>() )
        {
            auto& component = registry.get<LandscapeTileComponent>( entity );
            if ( !component.Heights.has_value() )
                continue;
            const auto root = FindLandscapeRoot( registry, component.Landscape );
            const auto fits = root ? World::Landscape::CheckTileMatchesRoot( *component.Heights, *root )
                                   : Common::MakeError<bool>( "its root is not loaded or is not a landscape" );
            if ( !fits.IsSuccess() )
            {
                out.Refused.push_back( { entity, fits.GetError() } );
                continue;
            }
            out.Tiles.push_back(
                 { entity, &component, *root,
                   World::Landscape::LandscapeTileFrame( *root, component.TileX, component.TileZ ) } );
        }
        return out;
    }
} // namespace Desert::ECS
