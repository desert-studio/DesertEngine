#pragma once

#include <Engine/ECS/LandscapeRootOf.hpp>
#include <Engine/World/Landscape/LandscapeEditLayers.hpp>
#include <Engine/World/Landscape/LandscapeSculpt.hpp>

#include <Common/Core/ResultStr.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Desert::ECS
{
    /**
     * @brief What a landscape edit tool needs from a scene: the root, a tile lookup for the edit cache, and the
     *        landscape's sample extent.
     *
     * A tile ENTITY without heights is Unloaded (the streamer owns it and has not filled it), no entity is
     * Absent. The lookup keeps the entity handles, not component pointers, and resolves the component on
     * every call: a pointer into an EnTT pool dies with the next component added to that pool, a handle does
     * not. It is valid for as long as the registry is and the tiles are not destroyed — one stroke.
     */
    struct LandscapeEditTarget
    {
        Common::UUID                            Landscape;
        World::Landscape::LandscapeRoot         Root;
        World::Landscape::LandscapeTileLookup   Lookup;
        World::Landscape::LandscapeSampleBounds Bounds;
    };

    inline Common::ResultStr<LandscapeEditTarget> FindLandscapeEditTarget( entt::registry&     registry,
                                                                           const Common::UUID& landscape )
    {
        const auto root = FindLandscapeRoot( registry, landscape );
        if ( !root )
            return Common::MakeError<LandscapeEditTarget>( "landscape edit: entity " +
                                                           std::to_string( static_cast<uint64_t>( landscape ) ) +
                                                           " is not a loaded landscape" );

        std::map<std::pair<int32_t, int32_t>, entt::entity> tiles;
        int32_t                                             minX = 0;
        int32_t                                             minZ = 0;
        int32_t                                             maxX = -1;
        int32_t                                             maxZ = -1;
        for ( const auto entity : registry.view<LandscapeTileComponent>() )
        {
            const auto& tile = registry.get<LandscapeTileComponent>( entity );
            if ( tile.Landscape != landscape )
                continue;
            if ( tiles.empty() )
            {
                minX = maxX = tile.TileX;
                minZ = maxZ = tile.TileZ;
            }
            minX                              = std::min( minX, tile.TileX );
            minZ                              = std::min( minZ, tile.TileZ );
            maxX                              = std::max( maxX, tile.TileX );
            maxZ                              = std::max( maxZ, tile.TileZ );
            tiles[{ tile.TileX, tile.TileZ }] = entity;
        }
        if ( tiles.empty() )
            return Common::MakeError<LandscapeEditTarget>( "landscape edit: landscape " +
                                                           std::to_string( static_cast<uint64_t>( landscape ) ) +
                                                           " has no tiles" );

        const int32_t       q = static_cast<int32_t>( root->QuadsPerTile );
        LandscapeEditTarget target;
        target.Landscape = landscape;
        target.Root      = *root;
        target.Bounds    = { minX * q, minZ * q, ( maxX + 1 ) * q, ( maxZ + 1 ) * q };
        target.Lookup    = [&registry, tiles = std::move( tiles )](
                             int32_t tx, int32_t tz ) -> World::Landscape::LandscapeTileSlot
        {
            const auto it = tiles.find( { tx, tz } );
            if ( it == tiles.end() || !registry.valid( it->second ) )
                return { World::Landscape::LandscapeTileState::Absent, nullptr };
            auto* component = registry.try_get<LandscapeTileComponent>( it->second );
            if ( component == nullptr )
                return { World::Landscape::LandscapeTileState::Absent, nullptr };
            if ( !component->Heights.has_value() )
                return { World::Landscape::LandscapeTileState::Unloaded, nullptr };
            return { World::Landscape::LandscapeTileState::Present, &*component->Heights };
        };
        return Common::MakeSuccess( std::move( target ) );
    }

    /**
     * @brief The edit layer a brush on @p landscape writes (UE: ALandscape::GetEditingLayer): the root's stack,
     *        @p rules, and @p layer — the stack's bottom layer when @p layer is null, as UE edits layer 0 of a
     *        landscape nobody picked a layer on.
     *
     * Refuses a landscape whose root is not loaded or carries no edit layers, a layer the stack does not name
     * and a Locked layer (CheckLandscapeEditLayerTarget) — naming it.
     */
    inline Common::ResultStr<World::Landscape::LandscapeEditLayerTarget>
    FindLandscapeEditLayerTarget( entt::registry& registry, const Common::UUID& landscape,
                                  std::vector<World::Landscape::LandscapeLayerRule> rules,
                                  const Common::UUID&                               layer )
    {
        using Result     = World::Landscape::LandscapeEditLayerTarget;
        const auto  root = FindLandscapeRootEntity( registry, landscape );
        const auto* body = root == entt::null ? nullptr : registry.try_get<LandscapeComponent>( root );
        if ( body == nullptr )
            return Common::MakeError<Result>( std::format( "landscape edit: entity {} is not a loaded landscape",
                                                           static_cast<uint64_t>( landscape ) ) );
        if ( body->EditLayers.Layers.empty() )
            return Common::MakeError<Result>( "landscape edit: the landscape has no edit layers" );
        Result target{ body->EditLayers, std::move( rules ),
                       layer.IsNull() ? body->EditLayers.Layers.front().Guid : layer };
        if ( auto ok = World::Landscape::CheckLandscapeEditLayerTarget( target ); !ok )
            return Common::MakeError<Result>( std::format( "landscape edit: {}", ok.GetError() ) );
        return Common::MakeSuccess( std::move( target ) );
    }

    /// The first landscape root in the scene — the one the tool edits when nothing names another.
    inline std::optional<Common::UUID> FirstLandscape( entt::registry& registry )
    {
        for ( const auto entity : registry.view<LandscapeComponent, UUIDComponent>() )
            return registry.get<UUIDComponent>( entity ).UUID;
        return std::nullopt;
    }
} // namespace Desert::ECS
