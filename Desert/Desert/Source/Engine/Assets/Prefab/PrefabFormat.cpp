#include <Engine/Assets/Prefab/PrefabFormat.hpp>
#include <Common/Content/CanonicalText.hpp>

#include <Common/Json/Json.hpp>

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
        return RefusePrefabVersion( source, StatedVersion( prefab.Header, kSceneSchemaTag ),
                                    StatedVersion( prefab.Header, kUnitSchemaTag ) );
    }

    Common::ResultStr<PrefabData> ParseLoadablePrefab( std::string_view source, const std::string& json )
    {
        auto parsed = Common::Json::Read<PrefabData>( json );
        if ( !parsed )
        {
            return Common::MakeError<PrefabData>(
                 fmt::format( "[PrefabAsset] '{0}' is not a readable prefab file: {1}. Nothing was loaded.",
                              source, parsed.GetError() ) );
        }

        PrefabData data = parsed.ExtractValue();
        // An absent Header states no generation, so it is never current; naming it here as well lets the
        // reads below dereference the header without a second, unreachable refusal.
        if ( !PrefabIsAtCurrentVersion( data ) || !data.Header.has_value() )
            return Common::MakeError<PrefabData>( RefusePrefabVersion( source, data ) );

        const Common::Content::AssetHeaderReadContext context{ Core::SceneTextSubsystems() };
        const auto header = Common::Content::TextHeaderToAssetHeader( *data.Header, context );
        if ( !header )
            return Common::MakeError<PrefabData>(
                 fmt::format( "[PrefabAsset] '{0}': {1}. Nothing was loaded.", source, header.GetError() ) );
        if ( header.GetValue().Kind != Common::Content::ContentKind::Prefab )
            return Common::MakeError<PrefabData>(
                 fmt::format( "[PrefabAsset] '{0}': the header says kind '{1}', not 'Prefab'. Nothing was loaded.",
                              source, data.Header->Kind ) );

        return Common::MakeSuccess( std::move( data ) );
    }

    Common::ResultStr<std::string> WritePrefabJson( PrefabData prefab )
    {
        prefab.Header =
             StampTextHeader( prefab.Header, Common::Content::ContentKind::Prefab, Core::SceneTextSubsystems() );
        return Common::Content::CanonicalJsonTextOfWriterOutput( Common::Json::Write( prefab ) );
    }

} // namespace Desert::Assets
