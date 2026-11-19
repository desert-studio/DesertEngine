#include "EditorLoadedRegions.hpp"

#include <Common/Core/Constants.hpp>
#include <Common/Json/Json.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Editor::EditorLoadedRegions
{
    namespace
    {
        // An entity-list loader as kept (UE FLoaderAdapterActorList, Load Region from Selection).
        struct SavedEntityList
        {
            std::vector<std::uint64_t> Entities;
            bool                       Loaded = true;
        };

        // One world's loaders for this user (WP-LOADER-ADAPTERS), one list per kind, so a file written before a kind
        // existed reads (absent = none) and an empty list is never written. `Regions` are the LOADED rectangles
        // (UE EditorLoadedRegions); the ones switched off are kept apart so the panel still lists them.
        struct WorldRegions
        {
            std::string                                    World; // scene path, relative to the project folder
            std::vector<::Desert::Core::Rules::CellBounds> Regions;
            bool                                           Whole = false;
            std::optional<std::vector<::Desert::Core::Rules::CellBounds>> NotLoadedRegions;
            // UE EditorLoadedLocationVolumes, by entity id; and the Auto Load volumes this user unloaded.
            std::optional<std::vector<std::uint64_t>>   LoadedLocationVolumes;
            std::optional<std::vector<std::uint64_t>>   NotLoadedLocationVolumes;
            std::optional<std::vector<SavedEntityList>> EntityLists;
            std::optional<std::vector<std::uint64_t>>   PinnedEntities; // UE FLoaderAdapterPinnedActors
            // The data layers this user loaded / unloaded against the world's InitiallyLoadedInEditor (UE
            // UWorldPartitionEditorPerProjectUserSettings::{LoadedDataLayers, NotLoadedDataLayers}). Absent = none,
            // so a file written before data layers reads, and an empty list is never written.
            std::optional<std::vector<std::string>> LoadedDataLayers;
            std::optional<std::vector<std::string>> NotLoadedDataLayers;
        };

        struct RegionsFile
        {
            std::vector<WorldRegions> Worlds;
        };

        std::filesystem::path FilePath()
        {
            return Common::Constants::Path::ProjectDir() / "Saved" / "Config" / "WorldPartitionEditor.json";
        }

        // The scene path as the project names it: relative to the project folder when inside it (forward slashes,
        // the same key on every platform), absolute otherwise.
        std::string KeyOf( const std::filesystem::path& scene )
        {
            const std::filesystem::path full = std::filesystem::absolute( scene ).lexically_normal();
            const std::filesystem::path root =
                 std::filesystem::absolute( Common::Constants::Path::ProjectDir() ).lexically_normal();
            const std::filesystem::path relative = full.lexically_relative( root );
            const bool inside = !relative.empty() && *relative.begin() != std::filesystem::path( ".." );
            return ( inside ? relative : full ).generic_string();
        }

        Common::ResultStr<RegionsFile> ReadAll()
        {
            const auto raw = Common::Utils::FileSystem::ReadFileContentIfExists( FilePath() );
            if ( !raw )
                return Common::MakeError<RegionsFile>( raw.GetError() );
            if ( !raw.GetValue().has_value() )
                return Common::MakeSuccess( RegionsFile{} );
            auto parsed = Common::Json::Read<RegionsFile>( *raw.GetValue() );
            if ( !parsed )
                return Common::MakeError<RegionsFile>(
                     std::format( "'{}': {}", FilePath().string(), parsed.GetError() ) );
            return parsed;
        }
    } // namespace

    Common::ResultStr<::Desert::Core::EditorRegions::HeldRegions> Remembered( const std::filesystem::path& scene )
    {
        using Result     = ::Desert::Core::EditorRegions::HeldRegions;
        const auto saved = ReadAll();
        if ( !saved )
            return Common::MakeError<Result>( saved.GetError() );
        const std::string key = KeyOf( scene );
        Result            held;
        for ( const WorldRegions& world : saved.GetValue().Worlds )
            if ( world.World == key )
            {
                namespace Regions = ::Desert::Core::EditorRegions;
                held              = Result{};
                for ( const ::Desert::Core::Rules::CellBounds& region : world.Regions )
                    held.Regions.push_back( Regions::RegionLoader{ region, true } );
                for ( const ::Desert::Core::Rules::CellBounds& region :
                      world.NotLoadedRegions.value_or( std::vector<::Desert::Core::Rules::CellBounds>{} ) )
                    held.Regions.push_back( Regions::RegionLoader{ region, false } );
                for ( const std::uint64_t volume : world.LoadedLocationVolumes.value_or( std::vector<std::uint64_t>{} ) )
                    held.Volumes.push_back( Regions::VolumeLoader{ volume, true } );
                for ( const std::uint64_t volume :
                      world.NotLoadedLocationVolumes.value_or( std::vector<std::uint64_t>{} ) )
                    held.Volumes.push_back( Regions::VolumeLoader{ volume, false } );
                for ( const SavedEntityList& list : world.EntityLists.value_or( std::vector<SavedEntityList>{} ) )
                    held.EntityLists.push_back( Regions::EntityListLoader{ list.Entities, list.Loaded } );
                held.Pinned = world.PinnedEntities.value_or( std::vector<std::uint64_t>{} );
                held.Whole  = world.Whole;
                held.DataLayers.Loaded    = world.LoadedDataLayers.value_or( std::vector<std::string>{} );
                held.DataLayers.NotLoaded = world.NotLoadedDataLayers.value_or( std::vector<std::string>{} );
            }
        return Common::MakeSuccess( std::move( held ) );
    }

    Common::BoolResultStr Remember( const std::filesystem::path&                      scene,
                                    const ::Desert::Core::EditorRegions::HeldRegions& held )
    {
        auto saved = ReadAll();
        if ( !saved )
            return Common::MakeError<bool>( saved.GetError() );
        RegionsFile       file = saved.ExtractValue();
        const std::string key  = KeyOf( scene );
        std::erase_if( file.Worlds, [&key]( const WorldRegions& world ) { return world.World == key; } );
        const auto listed = []( const std::vector<std::string>& names )
        { return names.empty() ? std::nullopt : std::optional<std::vector<std::string>>( names ); };
        const auto listedIds = []( const std::vector<std::uint64_t>& ids )
        { return ids.empty() ? std::nullopt : std::optional<std::vector<std::uint64_t>>( ids ); };
        WorldRegions world;
        world.World = key;
        world.Whole = held.Whole;
        std::vector<::Desert::Core::Rules::CellBounds> notLoadedRegions;
        for ( const ::Desert::Core::EditorRegions::RegionLoader& region : held.Regions )
            ( region.Loaded ? world.Regions : notLoadedRegions ).push_back( region.Region );
        if ( !notLoadedRegions.empty() )
            world.NotLoadedRegions = std::move( notLoadedRegions );
        std::vector<std::uint64_t> loadedVolumes;
        std::vector<std::uint64_t> notLoadedVolumes;
        for ( const ::Desert::Core::EditorRegions::VolumeLoader& volume : held.Volumes )
            ( volume.Loaded ? loadedVolumes : notLoadedVolumes ).push_back( volume.Volume );
        world.LoadedLocationVolumes    = listedIds( loadedVolumes );
        world.NotLoadedLocationVolumes = listedIds( notLoadedVolumes );
        std::vector<SavedEntityList> lists;
        for ( const ::Desert::Core::EditorRegions::EntityListLoader& list : held.EntityLists )
            lists.push_back( SavedEntityList{ list.Entities, list.Loaded } );
        if ( !lists.empty() )
            world.EntityLists = std::move( lists );
        world.PinnedEntities      = listedIds( held.Pinned );
        world.LoadedDataLayers    = listed( held.DataLayers.Loaded );
        world.NotLoadedDataLayers = listed( held.DataLayers.NotLoaded );
        file.Worlds.push_back( std::move( world ) );

        std::error_code dirError;
        std::filesystem::create_directories( FilePath().parent_path(), dirError );
        if ( dirError )
            return Common::MakeError<bool>( std::format( "the folder of '{}' could not be created: {}",
                                                         FilePath().string(), dirError.message() ) );
        return Common::Json::WriteFileAtomic( FilePath(), file );
    }
} // namespace Desert::Editor::EditorLoadedRegions
