#include <Engine/Assets/Prefab/PrefabFormat.hpp>
#include <Common/Content/CanonicalText.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>

#include <Common/Json/Json.hpp>
#include <Common/Json/ReadWrite.hpp>
#include <Common/Core/Logger.hpp>
#include <Engine/Core/Serialize/SceneUpgrade.hpp>

namespace Desert::Assets
{
    std::string RefusePrefabVersion( std::string_view source, int foundSceneVersion, int foundUnitVersion,
                                     int foundWorldPartitionVersion, int foundAssetRefVersion,
                                     int foundLightVersion )
    {
        return fmt::format(
             "[PrefabAsset] '{0}' is at scene schema v{1} / world units v{2} / world partition v{3} / asset references "
             "v{8} / lights v{10}, and this engine loads scene schema v{4} / world units v{5} / world partition v{6} / "
             "asset references v{9} / lights v{11}, and upgrades in memory from scene v{7} / world partition "
             "v{12} up. NOTHING WAS LOADED - no entity was taken from this file, and the scene is exactly as it "
             "was. A prefab carries the same entity payloads a scene does and moves through the same generations "
             "(SceneUpgrade.hpp): a NEWER file was written by a later build; an older one is converted once by "
             "Tools/SceneMigrator.",
             source, foundSceneVersion, foundUnitVersion, foundWorldPartitionVersion, Core::kSceneVersion,
             Core::kUnitVersion, Core::kWorldPartitionVersion, Core::kSceneUpgradeFloor, foundAssetRefVersion,
             Core::kAssetRefVersion, foundLightVersion, Core::kLightVersion, Core::kWorldPartitionUpgradeFloor );
    }

    std::string RefusePrefabVersion( std::string_view source, const PrefabData& prefab )
    {
        return RefusePrefabVersion( source, StatedVersion( prefab.Header, kSceneSchemaTag ),
                                    StatedVersion( prefab.Header, kUnitSchemaTag ),
                                    StatedVersion( prefab.Header, kWorldPartitionSchemaTag ),
                                    StatedVersion( prefab.Header, kAssetRefSchemaTag ),
                                    StatedVersion( prefab.Header, kLightSchemaTag ) );
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
        // An absent Header states no generation; refused by its numbers (all 0).
        if ( !data.Header.has_value() )
            return Common::MakeError<PrefabData>( RefusePrefabVersion( source, data ) );

        // UPGRADE ON LOAD (Core/Serialize/SceneUpgrade.hpp): the scene's chain over the prefab's records - an
        // older generation is raised in memory and logged once, a newer one (or one below the floor) refused.
        if ( !PrefabIsAtCurrentVersion( data ) )
        {
            const Core::SceneUpgradeReport upgrade = Core::UpgradeSceneRecords(
                 data.Entities, Core::StatedGenerations( data.Header ), Core::UpgradeTarget::Prefab, source,
                 Core::ContentMaterialGuidLookup() );
            if ( !upgrade.Refused.empty() )
                return Common::MakeError<PrefabData>( fmt::format(
                     "[PrefabAsset] {0} NOTHING WAS LOADED - no entity was taken from this file.", upgrade.Refused ) );
            Core::RestateCurrentGenerations( *data.Header );
            LOG_INFO( "[PrefabAsset] {0}", Core::DescribeUpgrade( source, upgrade ) );
        }

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

    Common::ResultStr<std::string> WritePrefabJson( PrefabData prefab, const Common::Content::AssetGuid& identity )
    {
        if ( !identity.IsNull() )
        {
            // Kind, Versions and Dependencies are the stamp's to write (WritePrefabJson below); only the
            // identity is carried in.
            prefab.Header =
                 Common::Content::TextAssetHeaderSerialized{ .Kind = {},
                                                             .Guid = Common::Content::AssetGuidToText( identity ),
                                                             .Versions     = {},
                                                             .Dependencies = {} };
        }
        return WritePrefabJson( std::move( prefab ) );
    }

    Common::Content::AssetGuid PrefabStatedGuid( const PrefabData& prefab )
    {
        if ( !prefab.Header )
            return {};
        const auto guid = Common::Content::AssetGuidFromText( prefab.Header->Guid );
        return guid ? guid.GetValue() : Common::Content::AssetGuid{};
    }

    // The content key ("assets:Prefabs/P.deprefab") StableKeyForPath mints for the file; a file outside every
    // root comes back absolute, which is that function's own explicit form.
    std::string StatePrefabPath( const std::filesystem::path& file )
    {
        return Common::AssetHandle::StableKeyForPath( Common::Constants::Path::FullPath( file ) );
    }

    // The exact inverse: a bare relative spelling is refused by name (PathForStableKey) and answers an empty
    // path, which no FindByPath matches; SCNE v68 restated every stored one as its key.
    std::filesystem::path ResolvePrefabPath( std::string_view stated )
    {
        return Common::AssetHandle::PathForStableKey( stated );
    }
} // namespace Desert::Assets
