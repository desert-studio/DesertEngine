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
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>

#include <Engine/Core/SceneSettings.hpp>
#include <Engine/ECS/Components.hpp>
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

    namespace
    {
        // The enumerator names a file may state for a light's Falloff, at the numbers the reader expects.
        // Pinned against the enum so a reorder breaks the build here instead of renumbering a light.
        constexpr std::array<const char*, 3> kLightFalloffNames = { "Linear", "Quadratic", "InverseSquare" };
        static_assert( static_cast<int>( ECS::LightFalloff::Linear ) == 0 &&
                            static_cast<int>( ECS::LightFalloff::Quadratic ) == 1 &&
                            static_cast<int>( ECS::LightFalloff::InverseSquare ) == 2,
                       "LightFalloff moved: kLightFalloffNames states the numbers a Falloff name becomes" );
        constexpr int kSendEventAction = static_cast<int>( ECS::UIButtonAction::SendEvent );

        // Applies @p edit to the object block @p component of @p components, if it is stated as an object.
        template <typename Edit>
        void EditBlock( rfl::ExtraFields<rfl::Generic>& components, const char* component, Edit&& edit )
        {
            const auto payload = components.get( component );
            if ( !payload.has_value() )
                return;
            auto fields = payload.value().to_object();
            if ( !fields.has_value() )
                return;
            rfl::Generic::Object block = std::move( fields.value() );
            if ( edit( block ) )
                components[component] = rfl::Generic( std::move( block ) );
        }

        // @p block without @p key; true when the key was there.
        bool DropKey( rfl::Generic::Object& block, const std::string& key )
        {
            if ( !block.get( key ).has_value() )
                return false;
            rfl::Generic::Object kept;
            for ( const auto& [name, value] : block )
                if ( name != key )
                    kept[name] = value;
            block = std::move( kept );
            return true;
        }
    } // namespace

    UndeclaredKeysReport MigrateUndeclaredKeysV38ToV39( std::vector<Assets::EntityData>& entities )
    {
        UndeclaredKeysReport report;
        const auto           settle = [&]( rfl::ExtraFields<rfl::Generic>& components, const std::string& who )
        {
            EditBlock( components, "UIToggle",
                       [&]( rfl::Generic::Object& block )
                       {
                           const bool dropped = DropKey( block, "On" );
                           report.TogglesOnDropped += dropped ? 1 : 0;
                           return dropped;
                       } );
            EditBlock( components, "UIButton",
                       [&]( rfl::Generic::Object& block )
                       {
                           const bool changed = DropKey( block, "CornerRadius" );
                           report.ButtonCornerRadiiDropped += changed ? 1 : 0;
                           const auto action = block.get( "Action" );
                           if ( !action.has_value() )
                               return changed;
                           const auto name = action.value().to_string();
                           if ( !name.has_value() )
                               return changed;
                           std::string target;
                           if ( const auto stated = block.get( "OnClickMessage" ); stated.has_value() )
                               target = stated.value().to_string().value_or( std::string() );
                           if ( !target.empty() && target != name.value() )
                           {
                               report.Refused.push_back( who + " / UIButton states Action '" + name.value() +
                                                         "' as a message name and OnClickMessage '" + target +
                                                         "' as another; which one the button sends is not in "
                                                         "the file" );
                               return changed;
                           }
                           block["Action"]         = rfl::Generic( kSendEventAction );
                           block["OnClickMessage"] = rfl::Generic( name.value() );
                           ++report.ButtonActionNamesMoved;
                           return true;
                       } );
            for ( const char* light : { "PointLight", "SpotLight" } )
                EditBlock( components, light,
                           [&]( rfl::Generic::Object& block )
                           {
                               const auto falloff = block.get( "Falloff" );
                               if ( !falloff.has_value() )
                                   return false;
                               const auto name = falloff.value().to_string();
                               if ( !name.has_value() )
                                   return false;
                               // An index rather than the iterator: an iterator's spelling differs between
                               // standard libraries (a pointer in libc++, a class in MSVC's), an index does not.
                               const auto index = static_cast<std::size_t>( std::ranges::distance(
                                    kLightFalloffNames.begin(),
                                    std::ranges::find( kLightFalloffNames, name.value() ) ) );
                               if ( index == kLightFalloffNames.size() )
                               {
                                   report.Refused.push_back( who + " / " + light + " states Falloff '" +
                                                             name.value() +
                                                             "', which is no LightFalloff enumerator (Linear, "
                                                             "Quadratic, InverseSquare)" );
                                   return false;
                               }
                               block["Falloff"] = rfl::Generic( static_cast<int>( index ) );
                               ++report.FalloffNamesNumbered;
                               return true;
                           } );
        };
        for ( std::size_t i = 0; i < entities.size(); ++i )
        {
            auto&             entity = entities[i];
            const std::string who    = "entity #" + std::to_string( i ) + " '" + entity.Tag.value_or( "" ) + "'";
            settle( entity.Components, who );
            if ( !entity.PrefabOverrides )
                continue;
            for ( auto& override_ : *entity.PrefabOverrides )
                settle( override_.Components, who + " (prefab override)" );
        }
        return report;
    }

    PlayerViewFlagReport MigratePlayerViewFlagV39ToV40( std::vector<Assets::EntityData>& entities )
    {
        constexpr const char* kOld = "IsMainCamera";
        constexpr const char* kNew = "AutoActivateForPlayer";
        PlayerViewFlagReport  report;
        for ( const auto& entity : entities )
            if ( const auto camera = entity.Components.get( "Camera" );
                 camera.has_value() && camera.value().to_object().has_value() )
                ++report.Cameras;
        report.KeptOne = report.Cameras == 1;

        for ( auto& entity : entities )
        {
            EditBlock( entity.Components, "Camera",
                       [&]( rfl::Generic::Object& block )
                       {
                           // A missing key was the old default, true: the one camera of a scene that never
                           // stated the flag is the view it has always played from.
                           bool stated = true;
                           if ( const auto old = block.get( kOld ); old.has_value() )
                               stated = old.value().to_bool().value_or( true );
                           DropKey( block, kOld );
                           block[kNew] = rfl::Generic( report.KeptOne && stated );
                           return true;
                       } );
            if ( !entity.PrefabOverrides )
                continue;
            for ( auto& override_ : *entity.PrefabOverrides )
                EditBlock( override_.Components, "Camera",
                           [&]( rfl::Generic::Object& block )
                           {
                               const bool dropped = DropKey( block, kOld );
                               report.OverridesDropped += dropped ? 1 : 0;
                               return dropped;
                           } );
        }
        return report;
    }

    std::size_t MigrateLandscapeLayerModesV37ToV38( std::vector<Assets::EntityData>& entities )
    {
        std::size_t dropped = 0;
        const auto  strip   = [&]( rfl::ExtraFields<rfl::Generic>& components )
        {
            const auto payload = components.get( "LandscapeMaterial" );
            if ( !payload.has_value() )
                return;
            const auto fields = payload.value().to_object();
            if ( !fields.has_value() )
                return;
            rfl::Generic::Object kept;
            for ( const auto& [key, field] : fields.value() )
            {
                if ( std::ranges::find_if( kRetiredLandscapeLayerModeKeys, [&]( const char* retired )
                                           { return key == retired; } ) != kRetiredLandscapeLayerModeKeys.end() )
                {
                    ++dropped;
                    continue;
                }
                kept[key] = field;
            }
            components["LandscapeMaterial"] = rfl::Generic( std::move( kept ) );
        };
        for ( auto& entity : entities )
        {
            strip( entity.Components );
            if ( !entity.PrefabOverrides )
                continue;
            for ( auto& override_ : *entity.PrefabOverrides )
                strip( override_.Components );
        }
        return dropped;
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
            const auto& overrides = *entity.PrefabOverrides;
            for ( std::size_t i = 0; i < overrides.size(); ++i )
                scan( overrides[i].Components, tag + " > PrefabOverrides[" + std::to_string( i ) + "]" );
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
        // FOLT 2's body, member for member: v3 added CullDistance, and the engine's struct is v4.
        struct FoliageTypeDataV2
        {
            std::optional<Common::Content::TextAssetHeaderSerialized> Header;
            Assets::AssetGuidRef                                      Mesh;
            float                                                     Density = 100.0f;
            Assets::Serialization::FoliageFloatInterval               ScaleX{ 0.8f, 1.3f };
            Assets::Serialization::FoliageFloatInterval               ZOffset{ 0.0f, 0.0f };
            bool                                                      AlignToNormal    = true;
            bool                                                      RandomYaw        = true;
            float                                                     RandomPitchAngle = 0.0f;
            Assets::Serialization::FoliageFloatInterval               GroundSlopeAngle{ 0.0f, 90.0f };
            Assets::Serialization::FoliageFloatInterval               Height{ -262144.0f, 262144.0f };
            std::vector<Assets::AssetGuidRef>                         LandscapeLayers;
            float                                                     MinimumLayerWeight = 0.0f;
        };

        // FOLT 3's body, member for member: the engine's struct is v4 and refuses a file without Wind.
        struct FoliageTypeDataV3
        {
            std::optional<Common::Content::TextAssetHeaderSerialized> Header;
            Assets::AssetGuidRef                                      Mesh;
            float                                                     Density = 100.0f;
            Assets::Serialization::FoliageFloatInterval               ScaleX{ 0.8f, 1.3f };
            Assets::Serialization::FoliageFloatInterval               ZOffset{ 0.0f, 0.0f };
            bool                                                      AlignToNormal    = true;
            bool                                                      RandomYaw        = true;
            float                                                     RandomPitchAngle = 0.0f;
            Assets::Serialization::FoliageFloatInterval               GroundSlopeAngle{ 0.0f, 90.0f };
            Assets::Serialization::FoliageFloatInterval               Height{ -262144.0f, 262144.0f };
            std::vector<Assets::AssetGuidRef>                         LandscapeLayers;
            float                                                     MinimumLayerWeight = 0.0f;
            Assets::Serialization::FoliageFloatInterval               CullDistance{ 0.0f, 0.0f };
        };

        // FOLT 4's body: v3 and Wind. The engine's struct is v6.
        struct FoliageTypeDataV4
        {
            std::optional<Common::Content::TextAssetHeaderSerialized> Header;
            Assets::AssetGuidRef                                      Mesh;
            float                                                     Density = 100.0f;
            Assets::Serialization::FoliageFloatInterval               ScaleX{ 0.8f, 1.3f };
            Assets::Serialization::FoliageFloatInterval               ZOffset{ 0.0f, 0.0f };
            bool                                                      AlignToNormal    = true;
            bool                                                      RandomYaw        = true;
            float                                                     RandomPitchAngle = 0.0f;
            Assets::Serialization::FoliageFloatInterval               GroundSlopeAngle{ 0.0f, 90.0f };
            Assets::Serialization::FoliageFloatInterval               Height{ -262144.0f, 262144.0f };
            std::vector<Assets::AssetGuidRef>                         LandscapeLayers;
            float                                                     MinimumLayerWeight = 0.0f;
            Assets::Serialization::FoliageFloatInterval               CullDistance{ 0.0f, 0.0f };
            Assets::Serialization::FoliageWind                        Wind;
        };

        // FOLT 5's body: v4 and IncludeInHLOD. The engine's struct is v6.
        struct FoliageTypeDataV5
        {
            std::optional<Common::Content::TextAssetHeaderSerialized> Header;
            Assets::AssetGuidRef                                      Mesh;
            float                                                     Density = 100.0f;
            Assets::Serialization::FoliageFloatInterval               ScaleX{ 0.8f, 1.3f };
            Assets::Serialization::FoliageFloatInterval               ZOffset{ 0.0f, 0.0f };
            bool                                                      AlignToNormal    = true;
            bool                                                      RandomYaw        = true;
            float                                                     RandomPitchAngle = 0.0f;
            Assets::Serialization::FoliageFloatInterval               GroundSlopeAngle{ 0.0f, 90.0f };
            Assets::Serialization::FoliageFloatInterval               Height{ -262144.0f, 262144.0f };
            std::vector<Assets::AssetGuidRef>                         LandscapeLayers;
            float                                                     MinimumLayerWeight = 0.0f;
            Assets::Serialization::FoliageFloatInterval               CullDistance{ 0.0f, 0.0f };
            Assets::Serialization::FoliageWind                        Wind;
            bool                                                      IncludeInHLOD = true;
        };

        // FOLT 1's body, member for member: the engine's struct is v3 and cannot read what v1 meant.
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

    namespace
    {
        // SKEL 1 as it was written: the v2 struct without PreviewMesh / CompatibleSkeletons.
        struct SkeletonAssetDataV1
        {
            std::optional<Common::Content::TextAssetHeaderSerialized> Header;
            uint64_t                                                  Signature = 0;
            std::vector<Desert::Animation::BoneInfo>                  Bones;
            std::optional<Assets::Serialization::SkeletonImportInfo>  Import;
        };
    } // namespace

    Common::ResultStr<std::string> MigrateSkeletonV1ToV2( const std::string& text )
    {
        const auto v1 = Common::Json::Read<SkeletonAssetDataV1>( text );
        if ( !v1 )
            return Common::MakeFormattedError<std::string>( "SKEL 1 body does not read: {}", v1.GetError() );
        const SkeletonAssetDataV1& old = v1.GetValue();
        if ( !old.Header )
            return Common::MakeFormattedError<std::string>( "the file states no header" );
        const auto stated = old.Header->Versions.find( "SKEL" );
        if ( stated == old.Header->Versions.end() || stated->second != 1u )
            return Common::MakeFormattedError<std::string>(
                 "the header states SKEL {}, and this step raises SKEL 1 only",
                 stated == old.Header->Versions.end() ? std::string( "nothing" )
                                                      : std::to_string( stated->second ) );

        Assets::Serialization::SkeletonAssetData data;
        data.Header                   = old.Header;
        data.Header->Versions["SKEL"] = 2u;
        data.Signature                = old.Signature;
        data.Bones                    = old.Bones;
        data.Import                   = old.Import;
        std::string written           = Common::Json::Write( data );
        // What the step writes, the engine's reader must read.
        if ( auto back = Assets::Serialization::ReadSkeletonJson( written ); !back )
            return Common::MakeFormattedError<std::string>( "the raised file does not read as SKEL 2: {}",
                                                            back.GetError() );
        return Common::MakeSuccess( std::move( written ) );
    }

    Common::ResultStr<Animation::SkeletonCandidate> ReadSkeletonCandidate( const std::filesystem::path& path,
                                                                           const std::string&           text )
    {
        const auto read = Assets::Serialization::ReadSkeletonJson( text );
        if ( !read )
            return Common::MakeFormattedError<Animation::SkeletonCandidate>( "'{}' is not a skeleton candidate: {}",
                                                                             path.string(), read.GetError() );
        const auto& data = read.GetValue();
        if ( !data.Header )
            return Common::MakeFormattedError<Animation::SkeletonCandidate>( "'{}' states no header", path.string() );
        const auto guid = Common::Content::AssetGuidFromText( data.Header->Guid );
        if ( !guid )
            return Common::MakeFormattedError<Animation::SkeletonCandidate>( "'{}' header GUID: {}", path.string(),
                                                                             guid.GetError() );
        std::filesystem::path stated = path.filename();
        for ( auto dir = path.parent_path(); !dir.empty() && dir != dir.parent_path(); dir = dir.parent_path() )
        {
            if ( dir.filename() == "Assets" )
            {
                stated = path.lexically_relative( dir );
                break;
            }
        }
        return Common::MakeSuccess(
             Animation::SkeletonCandidate{ guid.GetValue(), data.Signature, stated.generic_string() } );
    }

    namespace
    {
        // ANIM 4 as it stood: the live clip plus the bone hash ANIM 5 replaced.
        struct AnimationAssetDataV4
        {
            uint64_t                                               SkeletonSignature = 0;
            rfl::Flatten<Assets::Serialization::AnimationAssetData> Rest;
        };
    } // namespace

    Common::ResultStr<std::string> MigrateAnimationV4ToV5( const std::string_view path, const std::string& text,
                                                           const std::span<const Animation::SkeletonCandidate> skeletons )
    {
        const auto v4 = Common::Json::Read<AnimationAssetDataV4>( text );
        if ( !v4 )
            return Common::MakeFormattedError<std::string>( "'{}': ANIM 4 body does not read: {}", path, v4.GetError() );
        Assets::Serialization::AnimationAssetData data = v4.GetValue().Rest.get();
        if ( !data.Header )
            return Common::MakeFormattedError<std::string>( "'{}' states no header", path );
        const auto stated = data.Header->Versions.find( "ANIM" );
        if ( stated == data.Header->Versions.end() || stated->second != 4u )
            return Common::MakeFormattedError<std::string>( "'{}': this step raises ANIM 4 only", path );

        const auto guid = Animation::MigrateSkeletonReference( path, v4.GetValue().SkeletonSignature, skeletons );
        if ( !guid )
            return Common::MakeError<std::string>( guid.GetError() );
        std::string rigPath;
        for ( const auto& candidate : skeletons )
            if ( candidate.Guid == guid.GetValue() )
                rigPath = candidate.Path;
        data.Skeleton = Assets::AssetGuidRef{ Common::Content::AssetGuidToText( guid.GetValue() ), rigPath };

        const auto canonical =
             Common::Content::CanonicalJsonTextOfWriterOutput( Assets::Serialization::WriteAnimationJson( data ) );
        if ( !canonical )
            return Common::MakeError<std::string>( canonical.GetError() );
        // What the step writes, the engine's reader must read.
        if ( auto back = Assets::Serialization::ReadAnimationJson( canonical.GetValue() ); !back )
            return Common::MakeFormattedError<std::string>( "'{}': the raised file does not read as ANIM 5: {}", path,
                                                            back.GetError() );
        return Common::MakeSuccess( canonical.GetValue() );
    }

    Common::ResultStr<std::string> MigrateMeshBinaryToV5( const std::string_view path, const std::string_view bytes,
                                                          const std::span<const Animation::SkeletonCandidate> skeletons )
    {
        namespace C = Common::Content;
        // The v3/v4 layout: a 64-byte header {Magic 8, ByteOrder 4, Version 4, FileSize 8, SectionCount 4,
        // Flags 4, SkeletonSignature 8, BoundsMin 12, BoundsMax 12}, the mesh GUID at 64, the table at 80.
        constexpr std::size_t kOldHeader       = 64;
        constexpr std::size_t kOldPrefix       = kOldHeader + 16;
        constexpr uint32_t    kOldSignatureBit = 1u << 1; // v1-v4 kMeshFlagHasSkeletonSignature
        constexpr uint32_t    kRows            = 12;      // v4 and v5: through Colors (11) and UV1 (12)
        struct Row
        {
            uint32_t Id;
            uint32_t ElementSize;
            uint64_t Offset;
            uint64_t Count;
        };
        static_assert( sizeof( Row ) == C::kMeshBinarySectionRowSize );

        if ( bytes.size() < kOldPrefix || std::memcmp( bytes.data(), C::kMeshBinaryMagic, 8 ) != 0 )
            return Common::MakeFormattedError<std::string>( "'{}' is not a cooked mesh", path );
        uint32_t version = 0, sectionCount = 0, flags = 0;
        uint64_t fileSize = 0, signature = 0;
        std::memcpy( &version, bytes.data() + 12, 4 );
        std::memcpy( &fileSize, bytes.data() + 16, 8 );
        std::memcpy( &sectionCount, bytes.data() + 24, 4 );
        std::memcpy( &flags, bytes.data() + 28, 4 );
        std::memcpy( &signature, bytes.data() + 32, 8 );
        if ( version != 3u && version != 4u )
            return Common::MakeFormattedError<std::string>( "'{}' is mesh version {}; this step raises 3 and 4", path,
                                                            version );
        const uint32_t oldRows = version == 3u ? 10u : kRows;
        const uint64_t oldEnd  = kOldPrefix + sizeof( Row ) * oldRows;
        if ( fileSize != bytes.size() || sectionCount != oldRows || bytes.size() < oldEnd )
            return Common::MakeFormattedError<std::string>(
                 "'{}' declares {} bytes and {} sections ({} present, v{} has {})", path, fileSize, sectionCount,
                 bytes.size(), version, oldRows );

        C::AssetGuid skeleton;
        if ( ( flags & kOldSignatureBit ) != 0 )
        {
            const auto guid = Animation::MigrateSkeletonReference( path, signature, skeletons );
            if ( !guid )
                return Common::MakeError<std::string>( guid.GetError() );
            skeleton = guid.GetValue();
        }

        const uint64_t          delta = C::kMeshBinaryPrefixV3 + sizeof( Row ) * kRows - oldEnd;
        C::MeshBinaryFileHeader header{};
        std::memcpy( header.Magic, bytes.data(), 8 );
        std::memcpy( &header.ByteOrder, bytes.data() + 8, 4 );
        header.Version      = C::kMeshBinaryVersion;
        header.FileSize     = fileSize + delta;
        header.SectionCount = kRows;
        header.Flags        = flags & ~kOldSignatureBit;
        header.SkeletonGuid = skeleton;
        std::memcpy( header.BoundsMin, bytes.data() + 40, 12 );
        std::memcpy( header.BoundsMax, bytes.data() + 52, 12 );

        std::string raised;
        raised.reserve( static_cast<std::size_t>( header.FileSize ) );
        raised.append( reinterpret_cast<const char*>( &header ), sizeof( header ) );
        raised.append( bytes.data() + kOldHeader, 16 ); // the mesh GUID
        for ( uint32_t i = 0; i < kRows; ++i )
        {
            Row row{};
            if ( i < oldRows )
            {
                std::memcpy( &row, bytes.data() + kOldPrefix + sizeof( Row ) * i, sizeof( Row ) );
                row.Offset += delta;
            }
            else // v3 had no Colors / UV1: empty sections at the end of the file
                row = Row{ i + 1, i + 1 == 11 ? 4u : 8u, header.FileSize, 0 };
            raised.append( reinterpret_cast<const char*>( &row ), sizeof( row ) );
        }
        raised.append( bytes.data() + oldEnd, bytes.size() - oldEnd );

        // THE ENGINE JUDGES THE RESULT, and its writer states it: a raise that shifted one byte wrong is refused
        // here by the same reader the editor uses, not discovered as a torn mesh later.
        auto decoded = Assets::Serialization::DecodeMeshBinary( raised, path );
        if ( !decoded )
            return Common::MakeFormattedError<std::string>( "'{}': the raised v{} does not read: {}", path,
                                                            C::kMeshBinaryVersion, decoded.GetError() );
        return Common::MakeSuccess( Assets::Serialization::EncodeMeshBinary( decoded.GetValue() ) );
    }

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
                 stated == old.Header->Versions.end() ? std::string( "nothing" )
                                                      : std::to_string( stated->second ) );

        // v2 text, not the engine's struct: the engine is v4, and the chain raises each generation in turn.
        // v1 names no landscape layer, so its Dependencies (the mesh's GUID) are v2's as they stand.
        FoliageTypeDataV2 data;
        data.Header                   = old.Header;
        data.Header->Versions["FOLT"] = 2u;
        data.Mesh                     = old.Mesh;
        data.Density                  = FoliageDensityFromPerDab( old.Density );
        data.ScaleX                   = old.ScaleX;
        data.ZOffset                  = old.ZOffset;
        data.AlignToNormal            = old.AlignToNormal;
        data.RandomYaw                = old.RandomYaw;
        data.RandomPitchAngle         = old.RandomPitchAngle;
        data.GroundSlopeAngle         = old.GroundSlopeAngle;
        std::string written           = Common::Json::Write( data );
        // What the step writes, the next step must read: a v1 number v2 refuses fails HERE, naming the field.
        if ( auto next = MigrateFoliageTypeV2ToV3( written ); !next )
            return Common::MakeFormattedError<std::string>( "the raised file does not read as FOLT 2: {}",
                                                            next.GetError() );
        return Common::MakeSuccess( std::move( written ) );
    }

    Common::ResultStr<std::string> MigrateFoliageTypeV2ToV3( const std::string& text )
    {
        // The generation first: a v1 or v3 body would otherwise be named by its fields, not by what it is.
        if ( const auto stated = Assets::Serialization::StatedFoliageTypeGeneration( text ); stated != 2u )
            return Common::MakeFormattedError<std::string>(
                 "the header states FOLT {}, and this step raises FOLT 2 only",
                 stated ? std::to_string( *stated ) : std::string( "nothing" ) );
        const auto v2 = Common::Json::Read<FoliageTypeDataV2>( text );
        if ( !v2 )
            return Common::MakeFormattedError<std::string>( "FOLT 2 body does not read: {}", v2.GetError() );
        const FoliageTypeDataV2& old = v2.GetValue();
        if ( !old.Header )
            return Common::MakeFormattedError<std::string>( "the file states no header" );

        // v3 text, not the engine's struct: the engine is v4, and v3 -> v4 is the chain's next step.
        FoliageTypeDataV3 data;
        data.Header                   = old.Header;
        data.Header->Versions["FOLT"] = 3u;
        data.Mesh                     = old.Mesh;
        data.Density                  = old.Density;
        data.ScaleX                   = old.ScaleX;
        data.ZOffset                  = old.ZOffset;
        data.AlignToNormal            = old.AlignToNormal;
        data.RandomYaw                = old.RandomYaw;
        data.RandomPitchAngle         = old.RandomPitchAngle;
        data.GroundSlopeAngle         = old.GroundSlopeAngle;
        data.Height                   = old.Height;
        data.LandscapeLayers          = old.LandscapeLayers;
        data.MinimumLayerWeight       = old.MinimumLayerWeight;
        // UE's default CullDistance {0, 0}: never culled, which is what every v2 field drew.
        data.CullDistance   = { 0.0f, 0.0f };
        std::string written = Common::Json::Write( data );
        if ( auto next = MigrateFoliageTypeV3ToV4( written ); !next )
            return Common::MakeFormattedError<std::string>( "the raised file does not read as FOLT 3: {}",
                                                            next.GetError() );
        return Common::MakeSuccess( std::move( written ) );
    }

    Common::ResultStr<std::string> MigrateFoliageTypeV3ToV4( const std::string& text )
    {
        if ( const auto stated = Assets::Serialization::StatedFoliageTypeGeneration( text ); stated != 3u )
            return Common::MakeFormattedError<std::string>(
                 "the header states FOLT {}, and this step raises FOLT 3 only",
                 stated ? std::to_string( *stated ) : std::string( "nothing" ) );
        const auto v3 = Common::Json::Read<FoliageTypeDataV3>( text );
        if ( !v3 )
            return Common::MakeFormattedError<std::string>( "FOLT 3 body does not read: {}", v3.GetError() );
        const FoliageTypeDataV3& old = v3.GetValue();
        if ( !old.Header )
            return Common::MakeFormattedError<std::string>( "the file states no header" );

        // v4 text, not the engine's struct: the engine is v6, and v4 -> v5 is the chain's next step.
        FoliageTypeDataV4 data;
        data.Header                   = old.Header;
        data.Header->Versions["FOLT"] = 4u;
        data.Mesh               = old.Mesh;
        data.Density            = old.Density;
        data.ScaleX             = old.ScaleX;
        data.ZOffset            = old.ZOffset;
        data.AlignToNormal      = old.AlignToNormal;
        data.RandomYaw          = old.RandomYaw;
        data.RandomPitchAngle   = old.RandomPitchAngle;
        data.GroundSlopeAngle   = old.GroundSlopeAngle;
        data.Height             = old.Height;
        data.LandscapeLayers    = old.LandscapeLayers;
        data.MinimumLayerWeight = old.MinimumLayerWeight;
        data.CullDistance       = old.CullDistance;
        // Strength 0: the instances stand still, which is what every v3 field drew.
        data.Wind = Assets::Serialization::FoliageWind{};

        std::string written = Common::Json::Write( data );
        if ( auto next = MigrateFoliageTypeV4ToV5( written ); !next )
            return Common::MakeFormattedError<std::string>( "the raised file does not read as FOLT 4: {}",
                                                            next.GetError() );
        return Common::MakeSuccess( std::move( written ) );
    }

    Common::ResultStr<std::string> MigrateFoliageTypeV4ToV5( const std::string& text )
    {
        if ( const auto stated = Assets::Serialization::StatedFoliageTypeGeneration( text ); stated != 4u )
            return Common::MakeFormattedError<std::string>(
                 "the header states FOLT {}, and this step raises FOLT 4 only",
                 stated ? std::to_string( *stated ) : std::string( "nothing" ) );
        const auto v4 = Common::Json::Read<FoliageTypeDataV4>( text );
        if ( !v4 )
            return Common::MakeFormattedError<std::string>( "FOLT 4 body does not read: {}", v4.GetError() );
        const FoliageTypeDataV4& old = v4.GetValue();
        if ( !old.Header )
            return Common::MakeFormattedError<std::string>( "the file states no header" );

        // v5 text, not the engine's struct: the engine is v6, and v5 -> v6 is the chain's next step.
        FoliageTypeDataV5 data;
        data.Header                   = old.Header;
        data.Header->Versions["FOLT"] = 5u;
        data.Mesh                     = old.Mesh;
        data.Density                  = old.Density;
        data.ScaleX                   = old.ScaleX;
        data.ZOffset                  = old.ZOffset;
        data.AlignToNormal            = old.AlignToNormal;
        data.RandomYaw                = old.RandomYaw;
        data.RandomPitchAngle         = old.RandomPitchAngle;
        data.GroundSlopeAngle         = old.GroundSlopeAngle;
        data.Height                   = old.Height;
        data.LandscapeLayers          = old.LandscapeLayers;
        data.MinimumLayerWeight       = old.MinimumLayerWeight;
        data.CullDistance             = old.CullDistance;
        data.Wind                     = old.Wind;
        // UE's default bIncludeInHLOD: every v4 field stood in its cell's HLOD.
        data.IncludeInHLOD = true;

        std::string written = Common::Json::Write( data );
        if ( auto next = MigrateFoliageTypeV5ToV6( written ); !next )
            return Common::MakeFormattedError<std::string>( "the raised file does not read as FOLT 5: {}",
                                                            next.GetError() );
        return Common::MakeSuccess( std::move( written ) );
    }

    Common::ResultStr<std::string> MigrateFoliageTypeV5ToV6( const std::string& text )
    {
        if ( const auto stated = Assets::Serialization::StatedFoliageTypeGeneration( text ); stated != 5u )
            return Common::MakeFormattedError<std::string>(
                 "the header states FOLT {}, and this step raises FOLT 5 only",
                 stated ? std::to_string( *stated ) : std::string( "nothing" ) );
        const auto v5 = Common::Json::Read<FoliageTypeDataV5>( text );
        if ( !v5 )
            return Common::MakeFormattedError<std::string>( "FOLT 5 body does not read: {}", v5.GetError() );
        const FoliageTypeDataV5& old = v5.GetValue();

        Assets::Serialization::FoliageTypeData data;
        data.Header = old.Header;
        // Every v5 type drew a mesh: FOLT 5 had no other kind.
        data.Kind               = Assets::Serialization::FoliageTypeKind::Mesh;
        data.Mesh               = old.Mesh;
        data.Density            = old.Density;
        data.ScaleX             = old.ScaleX;
        data.ZOffset            = old.ZOffset;
        data.AlignToNormal      = old.AlignToNormal;
        data.RandomYaw          = old.RandomYaw;
        data.RandomPitchAngle   = old.RandomPitchAngle;
        data.GroundSlopeAngle   = old.GroundSlopeAngle;
        data.Height             = old.Height;
        data.LandscapeLayers    = old.LandscapeLayers;
        data.MinimumLayerWeight = old.MinimumLayerWeight;
        data.CullDistance       = old.CullDistance;
        data.Wind               = old.Wind;
        data.IncludeInHLOD      = old.IncludeInHLOD;

        std::string written = Assets::Serialization::WriteFoliageType( data );
        if ( auto reread = Assets::Serialization::ParseFoliageType( written ); !reread )
            return Common::MakeFormattedError<std::string>( "the raised file does not read as FOLT 6: {}",
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
            // v35 moves no key: it is the file layout of a partitioned world, which the caller writes.
            if ( statedSceneVersion < kSceneVersionExternalEntities )
                report.ExternalEntitiesRaised = true;

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

            // The built-in grass/rock/snow switches leave the landscape (LS-16); nothing reads them any more.
            if ( statedSceneVersion < kSceneVersionNoLandscapeLayerModes )
            {
                report.LandscapeLayerModesRaised  = true;
                report.LandscapeLayerModesDropped = MigrateLandscapeLayerModesV37ToV38( entities );
            }

            // Keys the build never declared, or stated in a type it never read (SAVE1): settled in the file so
            // the canonical pass after the steps finds only declared keys in their declared types.
            if ( statedSceneVersion < kSceneVersionNoUndeclaredKeys )
            {
                report.UndeclaredKeysRaised = true;
                report.UndeclaredKeys       = MigrateUndeclaredKeysV38ToV39( entities );
                if ( !report.UndeclaredKeys.Refused.empty() )
                {
                    std::string lines;
                    for ( const auto& line : report.UndeclaredKeys.Refused )
                        lines += ( lines.empty() ? "" : "; " ) + line;
                    report.Refused = "'" + name + "': " + lines + ". Nothing was written.";
                    return;
                }
            }

            // The player's view is chosen, not defaulted (SPAWN1): IsMainCamera becomes AutoActivateForPlayer.
            if ( statedSceneVersion < kSceneVersionPlayerViewFlag )
            {
                report.PlayerViewFlagRaised = true;
                report.PlayerViewFlag       = MigratePlayerViewFlagV39ToV40( entities );
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

    namespace
    {
        // The fields of Core::PostProcessSettings, spelled as the Settings block stated them before v36.
        constexpr std::array<std::string_view, 35> kGradeKeys = {
             "EnableSSAO",
             "GlobalIllumination",
             "GIIntensity",
             "EnableSSR",
             "SSRIntensity",
             "SSRMaxDistance",
             "Tonemapper",
             "Exposure",
             "Gamma",
             "WhitePoint",
             "AutoExposure",
             "AutoExposureKey",
             "AutoExposureSpeed",
             "AutoExposureMin",
             "AutoExposureMax",
             "EnableBloom",
             "BloomThreshold",
             "BloomIntensity",
             "LensDispersion",
             "EnableLensFlare",
             "LensFlareIntensity",
             "LensFlareTint",
             "LensFlareThreshold",
             "LensFlareGhostCount",
             "LensFlareGhostSpacing",
             "LensFlareGhostSizeNear",
             "LensFlareGhostSizeFar",
             "LensFlareGhostTintInner",
             "LensFlareGhostTintOuter",
             "LensFlareHaloIntensity",
             "LensFlareHaloRadius",
             "LensFlareStreakIntensity",
             "LensFlareStreakLength",
             "LensFlareStreakAngle",
             "LensFlareChromaShift",
        };

        // Settings key -> DirectionalLightData key.
        constexpr std::array<std::pair<std::string_view, std::string_view>, 3> kShadowKeys = { {
             { "EnableShadows", "CastShadows" },
             { "ShadowBias", "ShadowBias" },
             { "CascadeSplitLambda", "CascadeSplitLambda" },
        } };

        bool IsGradeKey( std::string_view key )
        {
            return std::find( kGradeKeys.begin(), kGradeKeys.end(), key ) != kGradeKeys.end();
        }

        int StampLight( Common::Json::KeyedValues& components, const rfl::Generic::Object& shadow )
        {
            const auto payload = components.get( "DirectionLight" );
            if ( !payload.has_value() )
                return 0;
            const auto fields = payload.value().to_object();
            if ( !fields.has_value() )
                return 0;
            rfl::Generic::Object light;
            for ( const auto& [key, field] : fields.value() )
                if ( !shadow.get( key ).has_value() )
                    light[key] = field;
            for ( const auto& [key, field] : shadow )
                light[key] = field;
            components["DirectionLight"] = rfl::Generic( std::move( light ) );
            return 1;
        }
    } // namespace

    SceneSettingsHomesReport MigrateSceneSettingsHomesV35ToV36( SceneSerialized& scene )
    {
        SceneSettingsHomesReport report;
        if ( !scene.Settings.has_value() )
            return report;
        const auto stated = scene.Settings.value().to_object();
        if ( !stated.has_value() )
            return report;

        rfl::Generic::Object kept;
        rfl::Generic::Object grade;
        rfl::Generic::Object shadow;
        for ( const auto& [key, field] : stated.value() )
        {
            if ( IsGradeKey( key ) )
            {
                grade[key] = field;
                ++report.PostKeysMoved;
                continue;
            }
            const auto shadowKey = std::find_if( kShadowKeys.begin(), kShadowKeys.end(),
                                                 [&key]( const auto& pair ) { return pair.first == key; } );
            if ( shadowKey != kShadowKeys.end() )
            {
                shadow[std::string( shadowKey->second )] = field;
                ++report.ShadowKeysFound;
                continue;
            }
            kept[key] = field;
        }
        scene.Settings = rfl::Generic( std::move( kept ) );

        if ( report.ShadowKeysFound > 0 )
            for ( auto& entity : scene.Entities )
            {
                report.LightsStamped += StampLight( entity.Components, shadow );
                if ( entity.PrefabOverrides )
                    for ( auto& overrideRecord : *entity.PrefabOverrides )
                        report.LightsStamped += StampLight( overrideRecord.Components, shadow );
            }

        if ( report.PostKeysMoved == 0 )
            return report;

        uint32_t nextSibling = 0;
        for ( const auto& entity : scene.Entities )
            if ( !entity.parent && entity.siblingIndex )
                nextSibling = std::max( nextSibling, *entity.siblingIndex + 1 );

        const std::string  seed = ( scene.Header ? scene.Header->Guid : scene.SceneName ) + "/PostProcessVolume";
        Assets::EntityData volume;
        volume.id           = Common::UUID( Fnv1a64( seed, 0xcbf29ce484222325ull ) );
        volume.siblingIndex = nextSibling;
        volume.Tag          = "PostProcessVolume";
        volume.Translation  = glm::vec3( 0.0f );
        volume.Rotation     = glm::vec3( 0.0f );
        volume.Scale        = glm::vec3( 1.0f );
        rfl::Generic::Object data;
        data["Unbound"]                        = rfl::Generic( true );
        data["Settings"]                       = rfl::Generic( std::move( grade ) );
        volume.Components["PostProcessVolume"] = rfl::Generic( std::move( data ) );
        scene.Entities.push_back( std::move( volume ) );
        report.VolumeCreated = true;
        return report;
    }

    InstanceTransformsReport MigrateInstanceTransformsV36ToV37( std::vector<Assets::EntityData>& entities,
                                                                const std::filesystem::path&     assetsRoot )
    {
        InstanceTransformsReport report;
        for ( Assets::EntityData& record : entities )
        {
            if ( !record.PrefabPath.has_value() || Assets::MissingInstanceTransform( record ).empty() )
                continue; // not an instance, or already states all three
            const std::string& prefabPath = *record.PrefabPath;

            const std::string site =
                 ( record.id.has_value()
                        ? "Entities[id=" + std::to_string( static_cast<uint64_t>( *record.id ) ) + "]"
                        : std::string( "Entities[?]" ) ) +
                 " > '" + prefabPath + "'";

            const auto located = LocateMeshFile( prefabPath, assetsRoot );
            if ( !located )
            {
                report.UnknownNames.push_back( site + ": " + located.GetError() );
                continue;
            }
            const std::ifstream in( located.GetValue().File, std::ios::binary );
            std::ostringstream  text;
            text << in.rdbuf();
            const auto prefab = rfl::json::read<Assets::PrefabData>( text.str() );
            if ( !prefab )
            {
                report.UnknownNames.push_back( site +
                                               ": the prefab file does not read: " + prefab.error().what() );
                continue;
            }
            const auto root = std::find_if( prefab->Entities.begin(), prefab->Entities.end(),
                                            [&]( const Assets::EntityData& e ) { return e.id == prefab->Root; } );
            if ( root == prefab->Entities.end() )
            {
                report.UnknownNames.push_back( site + ": the prefab states no record for its Root " +
                                               std::to_string( static_cast<uint64_t>( prefab->Root ) ) );
                continue;
            }

            Assets::RootTransformOverride taken;
            if ( record.PrefabOverrides.has_value() )
            {
                taken = Assets::TakeRootTransformOverride( *record.PrefabOverrides, { prefab->Root } );
                if ( record.PrefabOverrides->empty() )
                    record.PrefabOverrides.reset();
            }
            // The order the loader resolved it in before v37: the override over the prefab's root over the
            // component's own default (TransformComponent: no offset, no turn, unit scale).
            if ( !record.Translation.has_value() )
                record.Translation = taken.Translation.value_or( root->Translation.value_or( glm::vec3( 0.0f ) ) );
            if ( !record.Rotation.has_value() )
                record.Rotation = taken.Rotation.value_or( root->Rotation.value_or( glm::vec3( 0.0f ) ) );
            if ( !record.Scale.has_value() )
                record.Scale = taken.Scale.value_or( root->Scale.value_or( glm::vec3( 1.0f ) ) );
            ++report.Stated;
        }
        return report;
    }

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
        if ( report.ExternalEntitiesRaised && scene.WorldPartition.has_value() )
            report.EntitiesMovedOut = scene.Entities.size();

        // Scene-only (it reads the Settings block a prefab does not have), so it runs here rather than in
        // RunSteps, after every entity step of the chain.
        if ( statedSceneVersion < kSceneVersionSceneSettingsHomes )
        {
            report.SceneSettingsHomesRaised = true;
            report.SceneSettingsHomes       = MigrateSceneSettingsHomesV35ToV36( scene );
        }

        // Scene-only as well: a `.deprefab`'s nested instance keeps its root transform in its override, which
        // its own file resolves; only a scene is read by a planner that sees one file.
        if ( statedSceneVersion < kSceneVersionInstanceTransforms )
        {
            report.InstanceTransformsRaised = true;
            report.InstanceTransforms       = MigrateInstanceTransformsV36ToV37( scene.Entities, assetsRoot );
            if ( !report.InstanceTransforms.UnknownNames.empty() )
            {
                report.Refused = "'" + scene.SceneName +
                                 "': " + std::to_string( report.InstanceTransforms.UnknownNames.size() ) +
                                 " prefab instance(s) whose root transform cannot be stated: " +
                                 report.InstanceTransforms.UnknownNames.front();
                return report; // unstamped, as every refusal
            }
        }

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
