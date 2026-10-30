#include "EditorLoadedRegions.hpp"

#include <Common/Core/Constants.hpp>
#include <Common/Json/Json.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <vector>

namespace Desert::Editor::EditorLoadedRegions
{
    namespace
    {
        struct WorldRegions
        {
            std::string                                    World; // scene path, relative to the project folder
            std::vector<::Desert::Core::Rules::CellBounds> Regions;
            bool                                           Whole = false;
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
                held.Regions = world.Regions;
                held.Whole   = world.Whole;
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
        file.Worlds.push_back( WorldRegions{ key, held.Regions, held.Whole } );

        std::error_code dirError;
        std::filesystem::create_directories( FilePath().parent_path(), dirError );
        if ( dirError )
            return Common::MakeError<bool>( std::format( "the folder of '{}' could not be created: {}",
                                                         FilePath().string(), dirError.message() ) );
        return Common::Json::WriteFileAtomic( FilePath(), file );
    }
} // namespace Desert::Editor::EditorLoadedRegions
