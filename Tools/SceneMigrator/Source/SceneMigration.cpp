#include "SceneMigration.hpp"
#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <fstream>
#include <sstream>
#include <map>
#include <unordered_map>
#include <unordered_set>

// The graph model and its JSON round trip, for the v20 -> v21 step: the blob it moves out of the entity
// IS this type serialized, so reading it with anything else would be a second statement of the format.
#include <Engine/Animation/Graph/AnimGraph.hpp>

#include <Engine/Core/SceneSettings.hpp>
#include <Engine/Geometry/EditMeshConversion.hpp>
#include <Engine/Geometry/EditMeshSerialization.hpp>
#include <Engine/Core/Serialize/AuthoredComponentIO.hpp>
#include <Engine/World/Landscape/LandscapeData.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>
#include <Engine/World/Landscape/LandscapeTileFiles.hpp>
// For the shipped presets' names and the directory they live in, and nothing else. The v4 -> v5 migration
// turns the species integer a v4 file carries into the PATH of the preset that holds the same twelve
// numbers, and spelling that path here as a literal would be a second statement of it — the exact
// duplication that agrees with itself until somebody moves the library. The header carries no renderer and
// no asset manager, so it does not bring another layer into this one.
#include <Engine/Assets/CloudTypeData.hpp>

#include <Engine/Assets/MaterialData.hpp>
#include <Engine/Assets/MaterialFormat.hpp>
#include <Engine/Assets/Serialization/FoliageType.hpp>
#include <Common/Content/ShaderAssetHeader.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>

#include <Common/Content/MeshBinaryHeader.hpp>
#include <Common/Content/CanonicalText.hpp>
#include <Common/Json/Json.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Units.hpp>

#include <glm/trigonometric.hpp>

// The cloud material step is the only migration that PRODUCES a file rather than only rewriting the
// property tree it was handed, so it is the only one that needs the serializer here. It still writes
// nothing itself: it returns the JSON text in the report and MigratorMain owns the atomic write, which
// is what keeps this function pure and testable (contract Section 4.4).
#include <rflcpp/rfl/DefaultIfMissing.hpp>
#include <rflcpp/rfl/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <optional>
#include <string>

namespace Desert::Migration
{
    namespace
    {
        // The offending value, spelled out. A warning that says "wrong type" without saying WHAT was in
        // the file sends the next reader back to the file anyway.
        std::string Describe( const rfl::Generic& g )
        {
            if ( const auto b = g.to_bool(); b.has_value() )
                return b.value() ? "true" : "false";
            if ( const auto i = g.to_int64(); i.has_value() )
                return std::to_string( i.value() );
            if ( const auto d = g.to_double(); d.has_value() )
                return std::to_string( d.value() );
            if ( const auto s = g.to_string(); s.has_value() )
                return "\"" + s.value() + "\"";
            if ( const auto a = g.to_array(); a.has_value() )
                return "an array of " + std::to_string( a.value().size() ) + " element(s)";
            if ( g.to_object().has_value() )
                return "an object";
            return "null";
        }

        // The project a mesh block's `meshPath` is relative to (the nearest ancestor of `assetsRoot` under
        // which that file exists) and the file itself. An error string names why there is none.
        struct MeshFileLocation
        {
            std::filesystem::path Start; // `assetsRoot`, absolute and normalised
            std::filesystem::path Project;
            std::filesystem::path File;
        };

        Common::ResultStr<MeshFileLocation> LocateMeshFile( const std::string&           meshPath,
                                                            const std::filesystem::path& assetsRoot )
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path  start = fs::absolute( assetsRoot, ec ).lexically_normal();
            fs::path        project;
            for ( fs::path at = start; !at.empty(); at = at.parent_path() )
            {
                if ( fs::is_regular_file( at / meshPath, ec ) )
                {
                    project = at;
                    break;
                }
                if ( at == at.parent_path() )
                    break;
            }
            if ( project.empty() )
                return Common::MakeFormattedError<MeshFileLocation>(
                     "{}", "no file '" + meshPath + "' under any ancestor of " + start.generic_string() );
            return Common::MakeSuccess(
                 MeshFileLocation{ start, project, ( project / meshPath ).lexically_normal() } );
        }

        // The GUID the mesh file `file` states. Two headers carry one: the cooked v3 mesh binary, and the
        // DAST envelope a source mesh asset (.stmesh) is written in. A file stating neither, a file of
        // another kind, or the null GUID is an error.
        Common::ResultStr<Common::Content::AssetGuid> MeshFileHeaderGuid( const std::filesystem::path& file )
        {
            {
                std::ifstream in( file, std::ios::binary );
                std::string   prefix( Common::Content::kMeshBinaryPrefixV3, '\0' );
                in.read( prefix.data(), static_cast<std::streamsize>( prefix.size() ) );
                prefix.resize( static_cast<std::size_t>( in.gcount() ) );
                const auto guid = Common::Content::ReadMeshHeaderGuid( prefix );
                if ( guid && !guid->IsNull() )
                    return Common::MakeSuccess( Common::Content::AssetGuid( *guid ) );
            }
            // Record-only: the migrator links no subsystem table, it only reads what the header states.
            const auto header =
                 Common::Content::ReadAssetHeader( file, Common::Content::AssetHeaderReadContext{ {}, true } );
            if ( !header )
                return Common::MakeFormattedError<Common::Content::AssetGuid>(
                     "{}", "'" + file.generic_string() +
                                "' states no mesh GUID (not a v3 mesh; envelope: " + header.GetError() + ")" );
            const auto& asset = header.GetValue();
            if ( asset.Kind != Common::Content::ContentKind::StaticMesh &&
                 asset.Kind != Common::Content::ContentKind::SkinnedMesh )
                return Common::MakeFormattedError<Common::Content::AssetGuid>(
                     "{}", "'" + file.generic_string() + "' is not a mesh (kind " +
                                std::string( Common::Content::KindName( asset.Kind ) ) + ")" );
            if ( asset.Guid.IsNull() )
                return Common::MakeFormattedError<Common::Content::AssetGuid>(
                     "{}", "'" + file.generic_string() + "' states the null GUID" );
            return Common::MakeSuccess( asset.Guid );
        }

    } // namespace

    namespace
    {
        void RaisePathOnlyMeshGuids( rfl::ExtraFields<rfl::Generic>& components, const std::string& tag,
                                     const std::filesystem::path& assetsRoot, MeshGuidsMigrationReport& report )
        {
            static constexpr auto kMeshComponents =
                 std::to_array<const char*>( { "StaticMesh", "SkinnedMesh", "InstancedStaticMesh" } );
            for ( const char* component : kMeshComponents )
            {
                const auto payload = components.get( component );
                if ( !payload.has_value() )
                    continue;
                const auto fields = payload.value().to_object();
                if ( !fields.has_value() )
                    continue;
                const auto        path = fields.value().get( "MeshPath" );
                const std::string text = path.has_value() ? path.value().to_string().value_or( "" ) : "";
                if ( text.empty() )
                    continue;
                const std::string site  = tag + " > " + component + ".MeshPath = '" + text + "'";
                const auto        value = fields.value().get( "MeshGuid" );
                if ( value.has_value() )
                {
                    const auto stated = value.value().to_string();
                    if ( !stated.has_value() )
                    {
                        report.UnknownNames.push_back( site + ": MeshGuid is " + Describe( value.value() ) +
                                                       ", not GUID text" );
                        continue;
                    }
                    if ( !stated.value().empty() )
                        continue; // already named by its GUID
                }
                const auto located = LocateMeshFile( text, assetsRoot );
                if ( !located )
                {
                    report.UnknownNames.push_back( site + ": " + located.GetError() );
                    continue;
                }
                const auto guid = MeshFileHeaderGuid( located.GetValue().File );
                if ( !guid )
                {
                    report.UnknownNames.push_back( site + ": " + guid.GetError() );
                    continue;
                }
                ++report.Rewritten;
                // MeshGuid follows MeshPath, the order the component's writer states them in.
                rfl::Generic::Object raised;
                for ( const auto& [key, field] : fields.value() )
                {
                    if ( key == "MeshGuid" )
                        continue;
                    raised[key] = field;
                    if ( key == "MeshPath" )
                        raised["MeshGuid"] = rfl::Generic( Common::Content::AssetGuidToText( guid.GetValue() ) );
                }
                components[component] = rfl::Generic( std::move( raised ) );
            }
        }
    } // namespace

    MeshGuidsMigrationReport MigratePathOnlyMeshGuidsV31ToV32( std::vector<Assets::EntityData>& entities,
                                                               const std::filesystem::path&     assetsRoot )
    {
        MeshGuidsMigrationReport report;
        for ( auto& entity : entities )
        {
            const std::string tag = entity.Tag.value_or( "Entity" );
            RaisePathOnlyMeshGuids( entity.Components, tag, assetsRoot, report );
            if ( !entity.PrefabOverrides )
                continue;
            auto& overrides = *entity.PrefabOverrides;
            for ( std::size_t i = 0; i < overrides.size(); ++i )
                RaisePathOnlyMeshGuids( overrides[i].Components,
                                        tag + " > PrefabOverrides[" + std::to_string( i ) + "]", assetsRoot,
                                        report );
        }
        return report;
    }

    std::vector<std::string> MigrateLandscapeLayerRefsV33ToV34( const std::vector<Assets::EntityData>& entities )
    {
        std::vector<std::string> inline_layers;
        const auto scan = [&]( const rfl::ExtraFields<rfl::Generic>& components, const std::string& tag )
        {
            const auto payload = components.get( "Landscape" );
            if ( !payload.has_value() )
                return;
            const auto fields = payload.value().to_object();
            if ( !fields.has_value() )
                return;
            const auto layers = fields.value().get( "Layers" );
            if ( !layers.has_value() )
                return;
            const auto list = layers.value().to_array();
            if ( !list.has_value() )
                return;
            for ( std::size_t i = 0; i < list.value().size(); ++i )
            {
                const auto layer = list.value()[i].to_object();
                if ( !layer.has_value() || layer.value().get( "Guid" ).has_value() )
                    continue;
                const auto name = layer.value().get( "Name" );
                inline_layers.push_back( tag + " > Landscape.Layers[" + std::to_string( i ) + "] = '" +
                                         ( name.has_value() ? name.value().to_string().value_or( "?" ) : "?" ) +
                                         "'" );
            }
        };
        for ( const auto& entity : entities )
        {
            const std::string tag = entity.Tag.value_or( "Entity" );
            scan( entity.Components, tag );
            if ( !entity.PrefabOverrides )
                continue;
            for ( std::size_t i = 0; i < entity.PrefabOverrides->size(); ++i )
                scan( ( *entity.PrefabOverrides )[i].Components,
                      tag + " > PrefabOverrides[" + std::to_string( i ) + "]" );
        }
        return inline_layers;
    }

    namespace
    {
        // The v32 reader's rule: an absent key kept the struct default, so an absent key here means that
        // default (the numbers ECS::FoliageComponent carried before FO-1).
        Common::BoolResultStr ReadInlineFloat( const rfl::Generic::Object& fields, const char* key, float& out )
        {
            const auto value = fields.get( key );
            if ( !value.has_value() )
                return BOOLSUCCESS;
            if ( const auto real = value.value().to_double(); real.has_value() )
            {
                out = static_cast<float>( real.value() );
                return BOOLSUCCESS;
            }
            if ( const auto whole = value.value().to_int(); whole.has_value() )
            {
                out = static_cast<float>( whole.value() );
                return BOOLSUCCESS;
            }
            return Common::MakeFormattedError<bool>( "{} is {}, not a number", key, Describe( value.value() ) );
        }

        Common::BoolResultStr ReadInlineBool( const rfl::Generic::Object& fields, const char* key, bool& out )
        {
            const auto value = fields.get( key );
            if ( !value.has_value() )
                return BOOLSUCCESS;
            if ( const auto flag = value.value().to_bool(); flag.has_value() )
            {
                out = flag.value();
                return BOOLSUCCESS;
            }
            return Common::MakeFormattedError<bool>( "{} is {}, not a bool", key, Describe( value.value() ) );
        }

        std::string SafeStem( std::string text )
        {
            for ( char& ch : text )
                if ( !std::isalnum( static_cast<unsigned char>( ch ) ) && ch != '_' && ch != '-' )
                    ch = '_';
            return text.empty() ? std::string( "Foliage" ) : text;
        }

        struct FoliageTypeWriter
        {
            const std::string&           OwnerName;
            const std::filesystem::path& AssetsRoot;
            FoliageTypesMigrationReport& Report;
            // Canonical payload (header aside) -> the relative path already minted for it in this file.
            std::map<std::string, std::string> Minted;

            void Raise( rfl::ExtraFields<rfl::Generic>& components, const std::string& tag )
            {
                const auto payload = components.get( "Foliage" );
                if ( !payload.has_value() )
                    return;
                const auto fields = payload.value().to_object();
                if ( !fields.has_value() )
                {
                    Report.UnknownNames.push_back( tag + " > Foliage is " + Describe( payload.value() ) +
                                                   ", not an object" );
                    return;
                }
                const auto& f = fields.value();

                Assets::Serialization::FoliageTypeData data;
                float scaleMin = 0.8f, scaleMax = 1.3f, zMin = 0.0f, zMax = 0.0f, slopeMin = 0.0f,
                      slopeMax = 90.0f;
                for ( const auto& check :
                      { ReadInlineFloat( f, "Density", data.Density ), ReadInlineFloat( f, "ScaleMin", scaleMin ),
                        ReadInlineFloat( f, "ScaleMax", scaleMax ), ReadInlineFloat( f, "ZOffsetMin", zMin ),
                        ReadInlineFloat( f, "ZOffsetMax", zMax ),
                        ReadInlineFloat( f, "MaxPitchDeg", data.RandomPitchAngle ),
                        ReadInlineFloat( f, "SlopeMinDeg", slopeMin ),
                        ReadInlineFloat( f, "SlopeMaxDeg", slopeMax ),
                        ReadInlineBool( f, "AlignToNormal", data.AlignToNormal ),
                        ReadInlineBool( f, "RandomYaw", data.RandomYaw ) } )
                {
                    if ( !check )
                    {
                        Report.UnknownNames.push_back( tag + " > Foliage." + check.GetError() );
                        return;
                    }
                }
                // The inline number was per dab, as v1 of the type file was: the file this step writes is the
                // current generation, so it states UE's areal density (the FOLT 1 -> 2 conversion).
                data.Density          = FoliageDensityFromPerDab( data.Density );
                data.ScaleX           = { scaleMin, scaleMax };
                data.ZOffset          = { zMin, zMax };
                data.GroundSlopeAngle = { slopeMin, slopeMax };

                // The field's mesh was the ISM beside it; the type now names it (UE: FoliageType::Mesh).
                if ( const auto ism = components.get( "InstancedStaticMesh" ); ism.has_value() )
                    if ( const auto ismFields = ism.value().to_object(); ismFields.has_value() )
                    {
                        const auto guid = ismFields.value().get( "MeshGuid" );
                        const auto path = ismFields.value().get( "MeshPath" );
                        if ( guid.has_value() && path.has_value() )
                        {
                            data.Mesh.Guid = guid.value().to_string().value_or( "" );
                            data.Mesh.Path = path.value().to_string().value_or( "" );
                        }
                    }

                if ( auto valid = Assets::Serialization::ValidateFoliageTypeData( data ); !valid )
                {
                    Report.UnknownNames.push_back( tag + " > Foliage: " + valid.GetError() );
                    return;
                }

                std::string relative;
                // One file per distinct set of numbers and mesh within this scene.
                std::string dataKey = std::to_string( data.Density ) + "|" + std::to_string( scaleMin ) + "|" +
                                      std::to_string( scaleMax ) + "|" + std::to_string( zMin ) + "|" +
                                      std::to_string( zMax ) + "|" + std::to_string( data.RandomPitchAngle ) +
                                      "|" + std::to_string( slopeMin ) + "|" + std::to_string( slopeMax ) + "|" +
                                      ( data.AlignToNormal ? "1" : "0" ) + ( data.RandomYaw ? "1" : "0" ) + "|" +
                                      data.Mesh.Guid;
                if ( const auto it = Minted.find( dataKey ); it != Minted.end() )
                    relative = it->second;
                else
                {
                    const std::string stem = SafeStem( OwnerName ) + "_" + SafeStem( tag );
                    relative               = "Foliage/" + stem + Assets::Serialization::kFoliageTypeExtension;
                    Common::Content::TextAssetHeaderSerialized header;
                    header.Guid = Common::Content::AssetGuidToText( MigrationGuidForPath( relative ) );
                    data.Header = header;
                    Report.NewTypes.emplace_back( ( AssetsRoot / relative ).lexically_normal(),
                                                  Assets::Serialization::WriteFoliageType( data ) );
                    Minted.emplace( dataKey, relative );
                }

                rfl::Generic::Object raised;
                raised["FoliageTypeGuid"] =
                     rfl::Generic( Common::Content::AssetGuidToText( MigrationGuidForPath( relative ) ) );
                raised["FoliageTypePath"] = rfl::Generic( relative );
                components["Foliage"]     = rfl::Generic( std::move( raised ) );
                ++Report.Rewritten;
            }
        };
    } // namespace

    float FoliageDensityFromPerDab( float perDab )
    {
        constexpr float kPi   = 3.14159265358979f;
        const float     discA = kPi * kFoliageV1ReferenceBrushRadiusCm * kFoliageV1ReferenceBrushRadiusCm;
        return perDab * ( 1000.0f * 1000.0f ) / discA;
    }

    namespace
    {
        // FOLT 1's body, member for member: the engine's struct is v2 and cannot read what v1 meant.
        struct FoliageTypeDataV1
        {
            std::optional<Common::Content::TextAssetHeaderSerialized> Header;
            Assets::AssetGuidRef                                      Mesh;
            float                                                     Density = 6.0f;
            Assets::Serialization::FoliageFloatInterval               ScaleX{ 0.8f, 1.3f };
            Assets::Serialization::FoliageFloatInterval               ZOffset{ 0.0f, 0.0f };
            bool                                                      AlignToNormal    = true;
            bool                                                      RandomYaw        = true;
            float                                                     RandomPitchAngle = 0.0f;
            Assets::Serialization::FoliageFloatInterval               GroundSlopeAngle{ 0.0f, 90.0f };
        };
    } // namespace

    Common::ResultStr<std::string> MigrateFoliageTypeV1ToV2( const std::string& text )
    {
        const auto v1 = Common::Json::Read<FoliageTypeDataV1>( text );
        if ( !v1 )
            return Common::MakeFormattedError<std::string>( "FOLT 1 body does not read: {}", v1.GetError() );
        const FoliageTypeDataV1& old = v1.GetValue();
        if ( !old.Header )
            return Common::MakeFormattedError<std::string>( "the file states no header" );
        const auto stated = old.Header->Versions.find( "FOLT" );
        if ( stated == old.Header->Versions.end() || stated->second != 1u )
            return Common::MakeFormattedError<std::string>(
                 "the header states FOLT {}, and this step raises FOLT 1 only",
                 stated == old.Header->Versions.end() ? std::string( "nothing" ) : std::to_string( stated->second ) );

        Assets::Serialization::FoliageTypeData data;
        data.Header           = old.Header;
        data.Mesh             = old.Mesh;
        data.Density          = FoliageDensityFromPerDab( old.Density );
        data.ScaleX           = old.ScaleX;
        data.ZOffset          = old.ZOffset;
        data.AlignToNormal    = old.AlignToNormal;
        data.RandomYaw        = old.RandomYaw;
        data.RandomPitchAngle = old.RandomPitchAngle;
        data.GroundSlopeAngle = old.GroundSlopeAngle;

        std::string written = Assets::Serialization::WriteFoliageType( data );
        // What the step writes, the engine must read: a v1 number v2 refuses fails HERE, naming the field.
        if ( auto reread = Assets::Serialization::ParseFoliageType( written ); !reread )
            return Common::MakeFormattedError<std::string>( "the raised file does not read as FOLT 2: {}",
                                                            reread.GetError() );
        return Common::MakeSuccess( std::move( written ) );
    }

    FoliageTypesMigrationReport MigrateInlineFoliageV32ToV33( std::vector<Assets::EntityData>& entities,
                                                              const std::string&               ownerName,
                                                              const std::filesystem::path&     assetsRoot )
    {
        FoliageTypesMigrationReport report;
        FoliageTypeWriter           writer{ ownerName, assetsRoot, report, {} };
        for ( auto& entity : entities )
        {
            const std::string tag = entity.Tag.value_or( "Entity" );
            writer.Raise( entity.Components, tag );
            if ( !entity.PrefabOverrides )
                continue;
            auto& overrides = *entity.PrefabOverrides;
            for ( std::size_t i = 0; i < overrides.size(); ++i )
                writer.Raise( overrides[i].Components, tag + "_Override" + std::to_string( i ) );
        }
        return report;
    }

    namespace
    {
        // THE STEP CHAIN. LEG1 deleted every step below kSceneVersionShaderGuids along with the legacy
        // material-id register it depended on; the one step that remains is gated the same way every
        // deleted one was, so a step added above this one needs no new machinery.
        void RunSteps( std::vector<Assets::EntityData>& entities, const std::string& name, int statedSceneVersion,
                       const std::filesystem::path& assetsRoot, FileMigrationReport& report )
        {
            // Adds MeshGuid where a StaticMesh/SkinnedMesh/InstancedStaticMesh block names a MeshPath but
            // states no MeshGuid; no step above writes one it lacks.
            if ( statedSceneVersion < kSceneVersionPathOnlyMeshGuids )
            {
                report.PathOnlyMeshGuidsRaised = true;
                report.PathOnlyMeshGuids       = MigratePathOnlyMeshGuidsV31ToV32( entities, assetsRoot );
                if ( !report.PathOnlyMeshGuids.UnknownNames.empty() )
                {
                    std::string names;
                    for ( const auto& unknown : report.PathOnlyMeshGuids.UnknownNames )
                        names += ( names.empty() ? "" : "; " ) + unknown;
                    report.Refused = "'" + name +
                                     "': " + std::to_string( report.PathOnlyMeshGuids.UnknownNames.size() ) +
                                     " path-only mesh reference(s) cannot be given a header GUID: " + names +
                                     ". Nothing was written.";
                    return;
                }
            }

            // Moves inline Foliage numbers into `.defoliage` files; every file below v33 is restamped.
            if ( statedSceneVersion < kSceneVersionFoliageTypes )
            {
                report.FoliageTypesRaised = true;
                report.FoliageTypes       = MigrateInlineFoliageV32ToV33( entities, name, assetsRoot );
                if ( !report.FoliageTypes.UnknownNames.empty() )
                {
                    std::string names;
                    for ( const auto& unknown : report.FoliageTypes.UnknownNames )
                        names += ( names.empty() ? "" : "; " ) + unknown;
                    report.Refused =
                         "'" + name + "': " + std::to_string( report.FoliageTypes.UnknownNames.size() ) +
                         " Foliage block(s) cannot become a foliage type: " + names + ". Nothing was written.";
                    return;
                }
            }
            // A landscape's inline layers become `.delayerinfo` references (LS-12b). Nothing to rewrite in
            // the corpus; an inline layer refuses the file.
            if ( statedSceneVersion < kSceneVersionLandscapeLayerRefs )
            {
                report.LandscapeLayerRefsRaised = true;
                const auto inline_layers        = MigrateLandscapeLayerRefsV33ToV34( entities );
                if ( !inline_layers.empty() )
                {
                    std::string names;
                    for ( const auto& layer : inline_layers )
                        names += ( names.empty() ? "" : "; " ) + layer;
                    report.Refused = "'" + name + "': " + std::to_string( inline_layers.size() ) +
                                     " inline landscape layer(s) must become .delayerinfo assets first (create "
                                     "them in the Landscape panel and re-link): " +
                                     names + ". Nothing was written.";
                    return;
                }
            }
        }

        // The refusal a file gets when its stated pair is one this tool has no route from: the numbers it
        // states, the numbers this tool knows, and what to do instead. Built here rather than at each
        // site so a scene and a prefab are refused in the same words.
        // Older generations are not supported (owner decision, LEG1): their steps were deleted, and no route
        // back to them is promised. The refusal names the numbers and stops there.
        constexpr const char* kOlderThanSupported =
             "The oldest this tool reads is v31/v1 - older files are not supported.";

        std::string RefuseGeneration( const char* what, int statedSceneVersion, int statedUnitVersion,
                                      const char* why )
        {
            return std::string( "states " ) + what + " schema v" + std::to_string( statedSceneVersion ) +
                   " / world units v" + std::to_string( statedUnitVersion ) + ", and this tool raises files to v" +
                   std::to_string( kSceneVersion ) + "/v" + std::to_string( kUnitVersion ) + ". " + why;
        }
    } // namespace

    namespace
    {
        // FNV-1a, 64 bit. Chosen for being specified in four lines and identical on every platform and
        // compiler - the GUID it feeds is written into files and must not depend on a library's version.
        uint64_t Fnv1a64( std::string_view bytes, uint64_t basis )
        {
            uint64_t hash = basis;
            for ( const char c : bytes )
            {
                hash ^= static_cast<uint8_t>( c );
                hash *= 0x100000001b3ull;
            }
            return hash;
        }
    } // namespace

    Common::Content::AssetGuid MigrationGuidForPath( const std::filesystem::path& relativeToContentRoot )
    {
        const std::string          key = relativeToContentRoot.generic_string();
        Common::Content::AssetGuid guid;
        guid.Hi = Fnv1a64( key, 0xcbf29ce484222325ull );
        guid.Lo = Fnv1a64( key, 0x84222325cbf29ce4ull );
        if ( guid.IsNull() )
            guid.Lo = 1;
        return guid;
    }

    namespace
    {
        // The v26 header a migrated file leaves with: its own GUID when it already states one, else the
        // migration's (MigrationGuidForPath, keyed on the path under the content root). A tree built in
        // memory has no path to key on, so it is a new asset and gets a fresh GUID, as a new file would.
        Common::Content::TextAssetHeaderSerialized
        MigrationHeader( const std::optional<Common::Content::TextAssetHeaderSerialized>& stated,
                         Common::Content::ContentKind kind, const std::filesystem::path& assetsRoot,
                         const std::filesystem::path& sourceFile )
        {
            Common::Content::AssetGuid guid;
            if ( stated )
                if ( const auto parsed = Common::Content::AssetGuidFromText( stated->Guid ); parsed )
                    guid = parsed.GetValue();
            if ( guid.IsNull() )
                guid = sourceFile.empty() ? Common::Content::AssetGuid::Generate()
                                          : MigrationGuidForPath( sourceFile.lexically_relative( assetsRoot ) );
            return Common::Content::MakeTextHeader( kind, guid, Core::SceneTextSubsystems() );
        }
    } // namespace

    FileMigrationReport MigrateScene( SceneSerialized& scene, const std::filesystem::path& assetsRoot,
                                      const std::filesystem::path& sourceFile )
    {
        FileMigrationReport report;

        // Since v26 the header states the generations; before it, the two top-level integers did.
        const int statedSceneVersion = scene.Header
                                            ? Assets::StatedVersion( scene.Header, Assets::kSceneSchemaTag )
                                            : scene.SceneVersion.value_or( 0 );
        const int statedUnitVersion  = scene.Header ? Assets::StatedVersion( scene.Header, Assets::kUnitSchemaTag )
                                                    : scene.UnitVersion.value_or( 0 );

        if ( statedSceneVersion > kSceneVersion || statedUnitVersion > kUnitVersion )
        {
            report.Refused = RefuseGeneration( "scene", statedSceneVersion, statedUnitVersion,
                                               "A file at a LATER generation was written by a build this "
                                               "tool predates - convert it with THAT build's SceneMigrator." );
            return report;
        }

        // LEG1: legacy formats are not supported. The oldest this tool reads is v31/v1
        // (kSceneVersionShaderGuids / kUnitVersion) and every file must already carry a text header.
        if ( !scene.Header || statedSceneVersion < kSceneVersionShaderGuids || statedUnitVersion != kUnitVersion )
        {
            report.Refused =
                 RefuseGeneration( "scene", statedSceneVersion, statedUnitVersion, kOlderThanSupported );
            return report;
        }

        RunSteps( scene.Entities, scene.SceneName, statedSceneVersion, assetsRoot, report );
        if ( !report.Refused.empty() )
            return report; // unstamped: the file is FAILED by every caller and written by none

        // Stamped whether or not anything moved: an already-current scene is still stamped, idempotently
        // (MigrationHeader keeps an existing GUID) - leaving it unstamped is how a load would re-run this.
        scene.Header =
             MigrationHeader( scene.Header, Common::Content::ContentKind::Scene, assetsRoot, sourceFile );
        scene.SceneVersion = std::nullopt;
        scene.UnitVersion  = std::nullopt;

        return report;
    }

    PrefabMigrationOutcome MigratePrefab( PrefabData& prefab, const std::filesystem::path& assetsRoot,
                                          const std::filesystem::path& sourceFile )
    {
        PrefabMigrationOutcome outcome;
        outcome.FoundSceneVersion = prefab.Header ? Assets::StatedVersion( prefab.Header, Assets::kSceneSchemaTag )
                                                  : prefab.SceneVersion.value_or( 0 );
        outcome.FoundUnitVersion  = prefab.Header ? Assets::StatedVersion( prefab.Header, Assets::kUnitSchemaTag )
                                                  : prefab.UnitVersion.value_or( 0 );

        if ( outcome.FoundSceneVersion == kSceneVersion && outcome.FoundUnitVersion == kUnitVersion )
        {
            outcome.AlreadyCurrent = true;
            return outcome;
        }

        if ( outcome.FoundSceneVersion > kSceneVersion || outcome.FoundUnitVersion > kUnitVersion )
        {
            outcome.Refused = RefuseGeneration( "prefab", outcome.FoundSceneVersion, outcome.FoundUnitVersion,
                                                "A file at a LATER generation was written by a build this "
                                                "tool predates - convert it with THAT build's SceneMigrator." );
            return outcome;
        }

        // LEG1: legacy formats are not supported, including the pre-Д28 unstamped (0,0) case this tool used
        // to stamp without migrating - that was already a legacy accommodation and is refused now like
        // every other generation below the minimum.
        if ( !prefab.Header || outcome.FoundSceneVersion < kSceneVersionShaderGuids ||
             outcome.FoundUnitVersion != kUnitVersion )
        {
            outcome.Refused = RefuseGeneration( "prefab", outcome.FoundSceneVersion, outcome.FoundUnitVersion,
                                                kOlderThanSupported );
            return outcome;
        }

        RunSteps( prefab.Entities, prefab.Name, outcome.FoundSceneVersion, assetsRoot, outcome.Steps );
        if ( !outcome.Steps.Refused.empty() )
        {
            outcome.Refused = outcome.Steps.Refused;
            return outcome;
        }

        prefab.Header =
             MigrationHeader( prefab.Header, Common::Content::ContentKind::Prefab, assetsRoot, sourceFile );
        prefab.SceneVersion = std::nullopt;
        prefab.UnitVersion  = std::nullopt;

        return outcome;
    }

} // namespace Desert::Migration
