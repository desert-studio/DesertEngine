#include <Engine/Assets/Prefab/PrefabFormat.hpp>
#include <Common/Content/CanonicalText.hpp>

#include <rflcpp/rfl/json.hpp>

#include <spdlog/fmt/fmt.h>

namespace Desert::Assets
{
    namespace
    {
        // Where the build this message came from actually put the tool — same NDEBUG choice, and for the
        // same reason, as SceneFormat.cpp: naming a directory the binary is not in is the same dead end
        // as naming no command at all.
        //
        // IT IS THE SCENE'S TOOL, and that is the point rather than an accident of naming: a prefab
        // shares the scene's two version integers, so it moves through the same generations and is
        // raised by the same step chain. Naming a separate PrefabMigrator here was naming a second
        // command over a second corpus that a version bump had to remember (И11).
#ifdef NDEBUG
        constexpr const char* kMigratorPath = "build/Bin/Release/SceneMigrator";
#else
        constexpr const char* kMigratorPath = "build/Bin/Debug/SceneMigrator";
#endif
    } // namespace

    std::string RefusePrefabVersion( std::string_view source, int foundSceneVersion, int foundUnitVersion )
    {
        return fmt::format(
             "[PrefabAsset] '{0}' is at scene schema v{1} / world units v{2}, and this engine loads "
             "scene schema v{3} / world units v{4} only. NOTHING WAS LOADED - no entity was taken from "
             "this file, and the scene is exactly as it was. A prefab carries the same entity payloads a "
             "scene does, so it moves through the same generations; Tools/SceneMigrator converts it, "
             "once, and writes the file back. Run:  {5} \"{0}\"",
             source, foundSceneVersion, foundUnitVersion, Core::kSceneVersion, Core::kUnitVersion, kMigratorPath );
    }

    std::string RefusePrefabVersion( std::string_view source, const PrefabData& prefab )
    {
        return RefusePrefabVersion( source, prefab.SceneVersion.value_or( 0 ), prefab.UnitVersion.value_or( 0 ) );
    }

    Common::ResultStr<PrefabData> ParseLoadablePrefab( std::string_view source, const std::string& json )
    {
        auto parsed = rfl::json::read<PrefabData>( json );
        if ( !parsed )
        {
            return Common::MakeError<PrefabData>(
                 fmt::format( "[PrefabAsset] '{0}' is not a readable prefab file: {1}. Nothing was loaded.",
                              source, parsed.error().what() ) );
        }

        if ( !PrefabIsAtCurrentVersion( parsed.value() ) )
            return Common::MakeError<PrefabData>( RefusePrefabVersion( source, parsed.value() ) );

        return Common::MakeSuccess( std::move( parsed.value() ) );
    }

    Common::ResultStr<std::string> WritePrefabJson( PrefabData prefab )
    {
        prefab.SceneVersion = Core::kSceneVersion;
        prefab.UnitVersion  = Core::kUnitVersion;
        return Common::Content::CanonicalJsonTextOfWriterOutput( rfl::json::write( prefab ) );
    }

} // namespace Desert::Assets
