#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <Common/Content/AssetRedirector.hpp>

#include <spdlog/fmt/fmt.h>

namespace Desert::Core
{
    namespace
    {
        // Where the build this message came from actually put the tool. NDEBUG is the only configuration
        // marker the workspace defines (Desert/Configurations.lua), so this is the whole choice - and
        // naming the wrong directory would send the reader to a binary that is not there, which is the same
        // dead end as naming no command at all.
#ifdef NDEBUG
        constexpr const char* kMigratorPath = "build/Bin/Release/SceneMigrator";
#else
        constexpr const char* kMigratorPath = "build/Bin/Debug/SceneMigrator";
#endif
    } // namespace

    std::string RefuseSceneVersion( std::string_view source, int foundSceneVersion, int foundUnitVersion )
    {
        return fmt::format(
             "[SceneSerializer] '{0}' is at scene schema v{1} / world units v{2}, and this engine loads "
             "scene schema v{3} / world units v{4} only. NOTHING WAS LOADED - no entity, no setting and no "
             "scene name was taken from this file, and the scene is exactly as it was. The engine does not "
             "convert old scenes any more; Tools/SceneMigrator does, once, and writes the file back. Run:  "
             "{5} \"{0}\"",
             source, foundSceneVersion, foundUnitVersion, kSceneVersion, kUnitVersion, kMigratorPath );
    }

    std::string RefuseSceneVersion( std::string_view source, const SceneSerialized& scene )
    {
        return RefuseSceneVersion( source, Assets::StatedVersion( scene.Header, Assets::kSceneSchemaTag ),
                                   Assets::StatedVersion( scene.Header, Assets::kUnitSchemaTag ) );
    }

    Common::ResultStr<LoadableScene> ParseLoadableScene( std::string_view source, const std::string& json )
    {
        // Opened by path, the old path of a moved scene holds a redirector: name it, not "unreadable JSON".
        // By GUID here (this gate links no registry); the editor's open names the new path before calling.
        if ( auto moved = Common::Content::RefuseRedirectorBytes( source, json ); !moved )
            return Common::MakeError<LoadableScene>(
                 fmt::format( "[SceneSerializer] {0}. Nothing was loaded.", moved.GetError() ) );

        auto document = Common::Json::Parse( json );
        if ( !document )
            return Common::MakeError<LoadableScene>(
                 fmt::format( "[SceneSerializer] '{0}' is not a readable scene file: {1}. Nothing was loaded.",
                              source, document.GetError() ) );

        auto parsed = Common::Json::Root( document.GetValue() ).AsDocument<SceneSerialized>();
        if ( !parsed )
            return Common::MakeError<LoadableScene>(
                 fmt::format( "[SceneSerializer] '{0}' is not a readable scene file: {1}. Nothing was loaded.",
                              source, parsed.GetError() ) );

        const SceneSerialized& scene = parsed.GetValue();
        if ( !SceneIsAtCurrentVersion( scene ) )
            return Common::MakeError<LoadableScene>( RefuseSceneVersion( source, scene ) );

        // The header is the file's identity as well as its versions: a malformed one, or one naming another
        // kind, is refused here rather than half-believed (the AF1 gate: header kind == kind of the extension).
        const Common::Content::AssetHeaderReadContext context{ SceneTextSubsystems() };
        const auto header = Common::Content::TextHeaderToAssetHeader( *scene.Header, context );
        if ( !header )
            return Common::MakeError<LoadableScene>(
                 fmt::format( "[SceneSerializer] '{0}': {1}. Nothing was loaded.", source, header.GetError() ) );
        if ( header.GetValue().Kind != Common::Content::ContentKind::Scene )
            return Common::MakeError<LoadableScene>(
                 fmt::format( "[SceneSerializer] '{0}': the header says kind '{1}', not 'Scene'. Nothing was loaded.",
                              source, scene.Header->Kind ) );

        return Common::MakeSuccess( LoadableScene{ document.ExtractValue(), parsed.ExtractValue() } );
    }

    Common::Json::Value ComposeSceneDocument( const SceneSerialized&                    fresh,
                                              const std::optional<Common::Json::Value>& loaded,
                                              const Serialize::KeyIsOurs&               entityKeyIsOurs )
    {
        Common::Json::Value written = Common::Json::FromStruct( fresh );
        if ( !loaded.has_value() )
            return written;
        return Common::Json::Value( Serialize::MergeSceneDocument( Common::Json::Root( written ),
                                                                   Common::Json::Root( *loaded ), entityKeyIsOurs ) );
    }

} // namespace Desert::Core
