#include <Landscape/Private/LandscapeSerialization.hpp>

#include <Engine/Core/ModuleHooks.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/UUIDComponent.hpp>
#include <Engine/ECS/TransformComponent.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>
#include <Engine/World/Landscape/LandscapeTileFiles.hpp>
#include <Landscape/Public/LandscapeComponent.hpp>
#include <Landscape/Public/LandscapeRootOf.hpp>
#include <Landscape/Public/LandscapeTileComponent.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>

#include <cmath>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>

// THE LANDSCAPE'S FILES BESIDE A SCENE (UE: a landscape's external packages — its heights live outside the level
// and are loaded and saved with it). The scene serializer reads and writes entities; what the landscape adds runs
// through the scene hooks the module registers at StartupModule (ModuleHooks.hpp SceneLoadChecks /
// SceneSaveWriters): after a load every tile is checked against its root, before a save every tile's heights are
// written to the files the scene then names.

namespace Desert::Core
{
    namespace
    {
        // THE ROOT A TILE NAMES, as the frame LandscapeLayout computes with. nullopt when the id names no
        // entity with a LandscapeComponent — an observation that did not resolve, which the caller reports.
        std::optional<World::Landscape::LandscapeRoot> FindTileRoot( const Scene& scene, const Common::UUID& id )
        {
            const auto found = scene.FindEntityByID( id );
            if ( !found.has_value() || !found->get().HasComponent<ECS::LandscapeComponent>() )
                return std::nullopt;

            return ECS::LandscapeRootOf( found->get() );
        }

        // AFTER A LOAD: every tile's heights against the root it names. A tile of the wrong size, or under
        // a root that cannot be tiled, is REFUSED — its heights are dropped and the reason is logged with
        // the tile's coordinate — rather than kept at a size its neighbours do not share. A root that is
        // rotated or scaled is said out loud: the frame has neither (Components.hpp, LandscapeComponent),
        // and a landscape that silently ignores its own gizmo is the dead-setting shape.
        void CheckLandscapeTiles( const Scene& scene, std::string_view sceneName )
        {
            for ( const auto& entity : scene.GetAllEntities() )
            {
                if ( entity.HasComponent<ECS::LandscapeComponent>() )
                {
                    const glm::mat4 world = entity.GetWorldTransform().ToFloatMatrix();
                    const glm::vec3 x( world[0] );
                    const glm::vec3 y( world[1] );
                    const glm::vec3 z( world[2] );
                    const bool      axisAligned = std::abs( x.y ) + std::abs( x.z ) + std::abs( y.x ) +
                                                  std::abs( y.z ) + std::abs( z.x ) + std::abs( z.y ) <
                                             1e-5f;
                    const bool unitScale =
                         std::abs( x.x - 1.0f ) + std::abs( y.y - 1.0f ) + std::abs( z.z - 1.0f ) < 1e-5f;
                    if ( !axisAligned || !unitScale )
                        LOG_WARN( "[Landscape] '{0}': a landscape root is rotated or scaled; its tiles are placed "
                                  "by its position alone, because the landscape frame has no rotation or scale.",
                                  sceneName );
                }

                if ( !entity.HasComponent<ECS::LandscapeTileComponent>() )
                    continue;
                auto& tile = entity.GetComponent<ECS::LandscapeTileComponent>();
                if ( !tile.Heights )
                    continue;

                const auto root = FindTileRoot( scene, tile.Landscape );
                if ( !root.has_value() )
                {
                    LOG_WARN( "[Landscape] '{0}': tile ({1}, {2}) names landscape {3}, which this scene does not "
                              "contain; the tile keeps its heights and has no place in the world.",
                              sceneName, tile.TileX, tile.TileZ, static_cast<uint64_t>( tile.Landscape ) );
                    continue;
                }

                auto refused = World::Landscape::ValidateLandscapeRoot( *root );
                if ( refused )
                    refused = World::Landscape::CheckTileMatchesRoot( *tile.Heights, *root );
                if ( !refused )
                {
                    LOG_ERROR( "[Landscape] '{0}': tile ({1}, {2}) was refused: {3}", sceneName, tile.TileX,
                               tile.TileZ, refused.GetError() );
                    ECS::SetLandscapeTileHeights( *entity.GetRegistry(), entity.GetHandle(), std::nullopt );
                }
            }
        }

        // BEFORE A SAVE: every tile's heights, into files derived from the destination. The first failure
        // stops the save and names the file, so the scene is never written pointing at a tile file that
        // did not land. A tile holding no heights keeps whatever file it named — this save did not lose
        // them, and writing nothing over that file is the only answer that does not invent terrain.
        Common::BoolResultStr WriteLandscapeTiles( Scene& scene, const std::filesystem::path& scenePath )
        {
            // The path the .desce records is spelled the way the scene's other files are: relative to the
            // PROJECT's directory (FPaths::ProjectDir, never the working directory), so a scene saved on one
            // machine opens on another and the editor may be started from any folder.
            const std::filesystem::path& projectDir = Common::Constants::Path::ProjectDir();
            const std::filesystem::path  relative   = scenePath.is_absolute() && !projectDir.empty()
                                                           ? scenePath.lexically_proximate( projectDir )
                                                           : scenePath;

            for ( const auto& entity : scene.GetAllEntities() )
            {
                if ( !entity.HasComponent<ECS::LandscapeTileComponent>() )
                    continue;
                auto& tile = entity.GetComponent<ECS::LandscapeTileComponent>();
                if ( !tile.Heights )
                {
                    LOG_WARN( "[Landscape] tile ({0}, {1}) holds no heights; the scene keeps naming '{2}'.",
                              tile.TileX, tile.TileZ, tile.HeightFile );
                    continue;
                }

                // The file is named by the entity's id, so an entity without one has no file name that
                // could not collide with another's; refused rather than numbered zero.
                if ( !entity.HasComponent<ECS::UUIDComponent>() )
                    return Common::MakeFormattedError( "tile ({}, {}) has no entity id to name its file by",
                                                       tile.TileX, tile.TileZ );
                const auto id = static_cast<uint64_t>( entity.GetComponent<ECS::UUIDComponent>().UUID );
                const std::filesystem::path file = World::Landscape::LandscapeTileBlobPath( relative, id );
                if ( const auto written = World::Landscape::WriteLandscapeTileFile(
                          Common::Constants::Path::FullPath( file ), *tile.Heights );
                     !written )
                    return Common::MakeFormattedError( "tile ({}, {}): {}", tile.TileX, tile.TileZ,
                                                       written.GetError() );
                // The save relocated the tile's file (a Save As, or a scene whose tiles lived elsewhere): the
                // entity's record now differs from its file, so it is marked like any other edit — otherwise a
                // partitioned world skips (and a Debug save refuses) the entity this save itself changed.
                //
                // Stored as a CONTENT KEY ("assets:<dir>/<scene>_Landscape/<id>.dlht", UE's /Game/... package
                // path), not as the editor's project-relative folder: a package mounts the assets root under its
                // own name, so only the key resolves there (AssetHandle::PathForStableKey on the read side).
                if ( std::string spelled =
                          Common::AssetHandle::StableKeyForPath( Common::Constants::Path::FullPath( file ) );
                     spelled != tile.HeightFile )
                {
                    tile.HeightFile = std::move( spelled );
                    scene.MarkModified( entity.GetComponent<ECS::UUIDComponent>().UUID );
                }
            }
            return BOOLSUCCESS;
        }
    } // namespace
} // namespace Desert::Core

namespace Desert::Landscape
{
    void RegisterSceneFiles()
    {
        Core::ModuleHookLists& hooks = Core::ModuleHooks();
        hooks.SceneLoadChecks.push_back( []( const Core::Scene& scene, std::string_view sceneName )
                                         { Core::CheckLandscapeTiles( scene, sceneName ); } );
        hooks.SceneSaveWriters.push_back(
             []( Core::Scene& scene, const std::filesystem::path& scenePath ) -> std::optional<std::string>
             {
                 if ( const auto tiles = Core::WriteLandscapeTiles( scene, scenePath ); !tiles )
                     return std::format( "the landscape: {}", tiles.GetError() );
                 return std::nullopt;
             } );
    }
} // namespace Desert::Landscape
