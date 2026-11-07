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
#include <utility>
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

    // ── THE ONE WRITER OF A TILE (UE: FLandscapeEditDataInterface over ULandscapeComponent) ───────────────
    //
    // A tile's fields and the PRESENCE / SHAPE of its Heights change only through a registry.patch, so
    // every change raises on_update<LandscapeTileComponent> and the tile table below is rebuilt at its
    // next read. Sample VALUES edited in place through `&*Heights` (sculpt, paint, edit layers) are not a
    // table change: they keep the tile's size and its frame, and every consumer of the values already
    // learns of them through the tile's own per-consumer dirty rectangles (LandscapeDirtyConsumer).

    /// Edits @p entity's LandscapeTileComponent through a patch: @p edit gets the component by reference.
    template <typename Edit>
    void EditLandscapeTile( entt::registry& registry, entt::entity entity, Edit&& edit )
    {
        registry.patch<LandscapeTileComponent>( entity, std::forward<Edit>( edit ) );
    }

    /// Replaces @p entity's loaded heights (nullopt unloads them) — the load, the refusal, a new landscape.
    inline void SetLandscapeTileHeights( entt::registry& registry, entt::entity entity,
                                         std::optional<World::Landscape::LandscapeTileData> heights )
    {
        registry.patch<LandscapeTileComponent>( entity, [&heights]( LandscapeTileComponent& tile )
                                                { tile.Heights = std::move( heights ); } );
    }

    // ── THE TILE TABLE, BUILT ONCE AND SHARED (UE: the landscape proxy's component list) ─────────────────
    //
    // Before this, LandscapeECSSystem, WaterZoneECSSystem and PhysicsECSSystem each walked every tile, looked
    // its root up by UUID and checked it against that root, three times a frame. The table is kept in the
    // registry's context and rebuilt only when (a) a LandscapeTileComponent was constructed, patched or
    // destroyed (the revision the signals below raise — construct/destroy also move entt pool elements, so
    // the `Component` pointers are re-taken exactly then), or (b) a landscape root appeared, went away,
    // moved or changed its tiling (compared root by root; a scene has a handful of roots).
    struct LandscapeTileTable
    {
        struct BuiltRoot
        {
            entt::entity                    Entity = entt::null;
            Common::UUID                    Id     = Common::UUID::Null();
            World::Landscape::LandscapeRoot Root;
        };

        uint64_t               Revision      = 1; // raised by every tile construct / patch / destroy
        uint64_t               BuiltRevision = 0; // Revision the table was built at; 0 = never built
        std::vector<BuiltRoot> Roots;             // the roots the table was built against, in view order
        DrawableLandscape      Drawable;
    };

    inline void RaiseLandscapeTileRevision( entt::registry& registry, entt::entity )
    {
        if ( auto* table = registry.ctx().find<LandscapeTileTable>() )
            ++table->Revision;
    }

    inline bool SameLandscapeRoot( const World::Landscape::LandscapeRoot& a,
                                   const World::Landscape::LandscapeRoot& b )
    {
        return a.Origin == b.Origin && a.QuadsPerTile == b.QuadsPerTile && a.SpacingCm == b.SpacingCm &&
               a.ZScale == b.ZScale;
    }

    /// True when the roots @p table was built against are still the loaded ones, in place. Reads only.
    inline bool LandscapeRootsUnchanged( entt::registry& registry, const LandscapeTileTable& table )
    {
        size_t index = 0;
        for ( const auto entity : registry.view<LandscapeComponent, UUIDComponent>() )
        {
            if ( index >= table.Roots.size() )
                return false;
            const auto& built = table.Roots[index];
            if ( built.Entity != entity || built.Id != registry.get<UUIDComponent>( entity ).UUID ||
                 !SameLandscapeRoot( built.Root, LandscapeRootOf( Entity( entity, registry ) ) ) )
                return false;
            ++index;
        }
        return index == table.Roots.size();
    }

    /// Brings the shared tile table up to date and returns it. Scene::ExecuteSystems calls it serially
    /// before every parallel system group, so inside a group the call only reads (a parallel system adds
    /// or removes no tile and moves no root).
    inline LandscapeTileTable& RefreshLandscapeTileTable( entt::registry& registry )
    {
        auto* found = registry.ctx().find<LandscapeTileTable>();
        if ( found == nullptr )
        {
            found = &registry.ctx().emplace<LandscapeTileTable>();
            registry.on_construct<LandscapeTileComponent>().connect<&RaiseLandscapeTileRevision>();
            registry.on_update<LandscapeTileComponent>().connect<&RaiseLandscapeTileRevision>();
            registry.on_destroy<LandscapeTileComponent>().connect<&RaiseLandscapeTileRevision>();
        }
        LandscapeTileTable& table = *found;
        if ( table.BuiltRevision == table.Revision && LandscapeRootsUnchanged( registry, table ) )
            return table;

        table.Roots.clear();
        for ( const auto entity : registry.view<LandscapeComponent, UUIDComponent>() )
            table.Roots.push_back( { entity, registry.get<UUIDComponent>( entity ).UUID,
                                     LandscapeRootOf( Entity( entity, registry ) ) } );

        DrawableLandscape& out = table.Drawable;
        out.Tiles.clear();
        out.Refused.clear();
        for ( const auto entity : registry.view<LandscapeTileComponent>() )
        {
            auto& component = registry.get<LandscapeTileComponent>( entity );
            if ( !component.Heights.has_value() )
                continue;
            const LandscapeTileTable::BuiltRoot* root = nullptr;
            if ( component.Landscape != Common::UUID::Null() )
                for ( const auto& candidate : table.Roots )
                    if ( candidate.Id == component.Landscape )
                    {
                        root = &candidate;
                        break;
                    }
            if ( root == nullptr )
            {
                out.Refused.push_back( { entity, "its root is not loaded or is not a landscape" } );
                continue;
            }
            const auto fits = World::Landscape::CheckTileMatchesRoot( *component.Heights, root->Root );
            if ( !fits.IsSuccess() )
            {
                out.Refused.push_back( { entity, fits.GetError() } );
                continue;
            }
            out.Tiles.push_back(
                 { entity, &component, root->Root,
                   World::Landscape::LandscapeTileFrame( root->Root, component.TileX, component.TileZ ) } );
        }
        table.BuiltRevision = table.Revision;
        return table;
    }

    /// Every tile the renderer draws, and every loaded tile it refuses with the reason. ONE test for every
    /// consumer: LandscapeECSSystem draws `Tiles` and reports `Refused`; collision builds bodies for
    /// exactly `Tiles`, so a refused tile has neither and the one warning covers both. Unloaded tiles
    /// are in neither list — the loader already said why a tile was refused at load.
    ///
    /// The reference is the shared table: valid until the next call after a tile is added, patched or
    /// removed or a root changes. Hold it for one pass, never across frames.
    inline const DrawableLandscape& DrawableLandscapeTiles( entt::registry& registry )
    {
        return RefreshLandscapeTileTable( registry ).Drawable;
    }
} // namespace Desert::ECS
