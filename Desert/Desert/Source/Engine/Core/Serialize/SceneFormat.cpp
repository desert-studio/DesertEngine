#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/SceneUpgrade.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/AssetRedirector.hpp>
#include <Common/Content/ContentKinds.hpp>
#include <Common/Json/ReadWrite.hpp>

namespace Desert::Core
{
    std::string RefuseSceneVersion( std::string_view source, int foundSceneVersion, int foundUnitVersion,
                                    int foundWorldPartitionVersion, int foundAssetRefVersion,
                                    int foundLightVersion )
    {
        return fmt::format(
             "[SceneSerializer] '{0}' is at scene schema v{1} / world units v{2} / world partition v{3} / asset references "
             "v{8} / lights v{10}, and this engine loads scene schema v{4} / world units v{5} / world partition v{6} / "
             "asset references v{9} / lights v{11}, and upgrades in memory from scene v{7} / world partition "
             "v{12} up. NOTHING WAS LOADED - no entity, no setting and no scene name was taken from this file, and "
             "the scene is exactly as it was. A NEWER file was written by a later build; an older one is "
             "converted once by Tools/SceneMigrator.",
             source, foundSceneVersion, foundUnitVersion, foundWorldPartitionVersion, kSceneVersion, kUnitVersion,
             kWorldPartitionVersion, kSceneUpgradeFloor, foundAssetRefVersion, kAssetRefVersion, foundLightVersion,
             kLightVersion, kWorldPartitionUpgradeFloor );
    }

    std::string RefuseSceneVersion( std::string_view source, const SceneSerialized& scene )
    {
        return RefuseSceneVersion( source, Assets::StatedVersion( scene.Header, Assets::kSceneSchemaTag ),
                                   Assets::StatedVersion( scene.Header, Assets::kUnitSchemaTag ),
                                   Assets::StatedVersion( scene.Header, Assets::kWorldPartitionSchemaTag ),
                                   Assets::StatedVersion( scene.Header, Assets::kAssetRefSchemaTag ),
                                   Assets::StatedVersion( scene.Header, Assets::kLightSchemaTag ) );
    }

    Common::ResultStr<LoadableScene> ParseLoadableScene( std::string_view source, const std::string& json )
    {
        // Opened by path, the old path of a moved scene holds a redirector: name it, not "unreadable JSON".
        // By GUID here (this gate links no registry); the editor's open names the new path before calling.
        if ( auto moved = Common::Content::RefuseRedirectorBytes( source, json ); !moved )
            return Common::MakeError<LoadableScene>(
                 fmt::format( "[SceneSerializer] {0}. Nothing was loaded.", moved.GetError() ) );

        auto document = Common::Json::TextDocument::Parse( json );
        if ( !document )
            return Common::MakeError<LoadableScene>(
                 fmt::format( "[SceneSerializer] '{0}' is not a readable scene file: {1}. Nothing was loaded.",
                              source, document.GetError() ) );

        // A partitioned world's header holds ids where its records were. Parsed as it is it would be a world
        // with no entities - a load that "succeeds" with the world missing - so it is refused here and read
        // through ExternalEntities::ReadSceneFileText, which joins the records back in first.
        for ( const std::string& key : document.GetValue().KeysAt() )
            if ( key == "ExternalEntities" )
                return Common::MakeError<LoadableScene>( fmt::format(
                     "[SceneSerializer] '{0}' is the header of a partitioned world: its entities are in their own "
                     "files, and it is read through ExternalEntities::ReadSceneFileText. Nothing was loaded.",
                     source ) );

        auto parsed = document.GetValue().AsDocument<SceneSerialized>();
        if ( !parsed )
            return Common::MakeError<LoadableScene>(
                 fmt::format( "[SceneSerializer] '{0}' is not a readable scene file: {1}. Nothing was loaded.",
                              source, parsed.GetError() ) );

        SceneSerialized scene = parsed.ExtractValue();
        // The header is the file's identity as well as its versions: a malformed one, or one naming another
        // kind, is refused here rather than half-believed (the AF1 gate: header kind == kind of the extension).
        if ( !scene.Header.has_value() )
            return Common::MakeError<LoadableScene>(
                 fmt::format( "[SceneSerializer] '{0}' states no header. Nothing was loaded.", source ) );

        // UPGRADE ON LOAD (SceneUpgrade.hpp): an older generation is raised in memory, a newer one (or one below
        // the floor) is refused and nothing is loaded.
        const SceneUpgradeReport upgrade = UpgradeScene( scene );
        if ( !upgrade.Refused.empty() )
            return Common::MakeError<LoadableScene>(
                 fmt::format( "[SceneSerializer] {0} NOTHING WAS LOADED - the scene is exactly as it was.",
                              upgrade.Refused ) );
        const Common::Content::AssetHeaderReadContext context{ SceneTextSubsystems() };
        const auto header = Common::Content::TextHeaderToAssetHeader( *scene.Header, context );
        if ( !header )
            return Common::MakeError<LoadableScene>(
                 fmt::format( "[SceneSerializer] '{0}': {1}. Nothing was loaded.", source, header.GetError() ) );
        if ( header.GetValue().Kind != Common::Content::ContentKind::Scene )
            return Common::MakeError<LoadableScene>( fmt::format(
                 "[SceneSerializer] '{0}': the header says kind '{1}', not 'Scene'. Nothing was loaded.", source,
                 scene.Header->Kind ) );

        if ( upgrade.From == CurrentGenerations() )
            return Common::MakeSuccess( LoadableScene{ document.ExtractValue(), std::move( scene ), {} } );

        // An upgraded tree: the document the saver merges foreign keys back from is the UPGRADED one, written by
        // the typed writer - the parsed one still states every key a step dropped or renamed, and a save would
        // bring them back as "keys this build does not declare". This is what Tools/SceneMigrator writes.
        auto upgraded = Common::Json::TextDocument::Parse( Common::Json::Write( scene ) );
        if ( !upgraded )
            return Common::MakeError<LoadableScene>(
                 fmt::format( "[SceneSerializer] '{0}': the upgraded tree is not JSON ({1}). Nothing was loaded.",
                              source, upgraded.GetError() ) );
        std::string note = DescribeUpgrade( source, upgrade );
        return Common::MakeSuccess( LoadableScene{ upgraded.ExtractValue(), std::move( scene ), std::move( note ) } );
    }

    Common::ResultStr<Common::Json::TextDocument>
    ComposeSceneDocument( const SceneSerialized& fresh, const std::optional<Common::Json::TextDocument>& loaded,
                          const Serialize::KeyIsOurs&          entityKeyIsOurs,
                          const Common::Json::MemberKeyIsOurs& componentKeyIsOurs )
    {
        auto written = Common::Json::TextDocument::Parse( Common::Json::Write( fresh ) );
        if ( !written )
            return Common::MakeError<Common::Json::TextDocument>(
                 fmt::format( "[SceneSerializer] '{0}': the typed writer's output is not JSON ({1})",
                              fresh.SceneName, written.GetError() ) );
        if ( !loaded.has_value() )
            return written;
        return Common::MakeSuccess(
             Serialize::MergeSceneDocument( written.GetValue(), *loaded, entityKeyIsOurs, componentKeyIsOurs ) );
    }

} // namespace Desert::Core
