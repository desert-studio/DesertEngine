#include "SceneMigration.hpp"
#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <format>
#include <fstream>
#include <sstream>
#include <tuple>
#include <map>
#include <unordered_map>
#include <unordered_set>

// The graph model and its JSON round trip, for the v20 -> v21 step: the blob it moves out of the entity
// IS this type serialized, so reading it with anything else would be a second statement of the format.
#include <Engine/Animation/Graph/AnimGraph.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/MeshSourceAsset.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>
#include <Engine/Assets/Serialization/ControlRig.hpp>
#include <Engine/Assets/Serialization/Retarget.hpp>

#include <Engine/Core/SceneSettings.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Animation/Timeline/Hosts.hpp>
#include "UILift.hpp"
#include "ClipInterpShift.hpp"
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
#include <Common/Core/ByteText.hpp>
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
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>

namespace Desert::Migration
{
    namespace
    {
        // A step that refused entities refuses the whole scene: one line naming the scene and every refusal.
        std::string RefusedWhole( const std::string& scene, const std::vector<std::string>& refusals )
        {
            std::string text = std::format( "'{}': ", scene );
            for ( size_t i = 0; i < refusals.size(); ++i )
                std::format_to( std::back_inserter( text ), "{}{}", i == 0 ? "" : "; ", refusals[i] );
            text += ". Nothing was written.";
            return text;
        }

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
                std::string   prefix( Common::Content::kMeshBinaryPrefixSize, '\0' );
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

    WindSourceReport MigrateWindSourceV41ToV42( std::vector<Assets::EntityData>& entities,
                                                const std::string& fileName, bool createSource )
    {
        constexpr const char* kDirection = "WindDirection";
        constexpr const char* kSpeed     = "WindSpeed";
        WindSourceReport      report;

        // The wind one layer stated: [x, y, z] and cm/s, the v41 defaults where a key was missing.
        struct LayerWind
        {
            std::vector<double> Direction{ 1.0, 0.0, 0.0 };
            double              Speed = 3000.0;
            bool                Blows = false;
        };
        std::optional<LayerWind> kept;

        for ( auto& entity : entities )
        {
            EditBlock( entity.Components, "VolumetricCloud",
                       [&]( rfl::Generic::Object& block )
                       {
                           LayerWind wind;
                           if ( const auto stated = block.get( kDirection ); stated.has_value() )
                               if ( const auto array = stated.value().to_array();
                                    array.has_value() && array.value().size() == 3 )
                                   for ( std::size_t i = 0; i < 3; ++i )
                                       wind.Direction[i] = array.value()[i].to_double().value_or( 0.0 );
                           if ( const auto stated = block.get( kSpeed ); stated.has_value() )
                               wind.Speed = stated.value().to_double().value_or( 0.0 );
                           bool enabled = true;
                           if ( const auto stated = block.get( "Enabled" ); stated.has_value() )
                               enabled = stated.value().to_bool().value_or( true );
                           const double lengthSquared = wind.Direction[0] * wind.Direction[0] +
                                                        wind.Direction[1] * wind.Direction[1] +
                                                        wind.Direction[2] * wind.Direction[2];
                           wind.Blows = enabled && wind.Speed > 0.0 && lengthSquared > 1e-12;

                           const bool droppedDirection = DropKey( block, kDirection );
                           const bool droppedSpeed     = DropKey( block, kSpeed );
                           report.CloudWinds += ( droppedDirection || droppedSpeed ) ? 1 : 0;
                           if ( wind.Blows )
                           {
                               if ( !kept )
                                   kept = wind;
                               else if ( kept->Direction != wind.Direction || kept->Speed != wind.Speed )
                                   ++report.Disagreeing;
                           }
                           return droppedDirection || droppedSpeed;
                       } );
            if ( !entity.PrefabOverrides )
                continue;
            for ( auto& override_ : *entity.PrefabOverrides )
                EditBlock( override_.Components, "VolumetricCloud",
                           [&]( rfl::Generic::Object& block )
                           {
                               const int dropped = ( DropKey( block, kDirection ) ? 1 : 0 ) +
                                                   ( DropKey( block, kSpeed ) ? 1 : 0 );
                               report.OverridesDropped += static_cast<std::size_t>( dropped );
                               return dropped != 0;
                           } );
        }

        if ( !createSource || !kept )
            return report;

        // FNV-1a over the file's name: the same scene migrated twice, on any machine, names the same record.
        uint64_t id = 14695981039346656037ull;
        for ( const char c : "WindSource:" + fileName )
            id = ( id ^ static_cast<uint8_t>( c ) ) * 1099511628211ull;

        rfl::Generic::Array direction;
        for ( const double component : kept->Direction )
            direction.push_back( rfl::Generic( component ) );
        rfl::Generic::Object block;
        block["Direction"] = rfl::Generic( std::move( direction ) );
        block["Speed"]     = rfl::Generic( kept->Speed );
        block["PointWind"] = rfl::Generic( false );
        block["Radius"]    = rfl::Generic( 1000.0 );

        Assets::EntityData source;
        source.id                       = Common::UUID( id == 0 ? 1 : id );
        source.Tag                      = std::string( "Wind Source" );
        source.Components["WindSource"] = rfl::Generic( std::move( block ) );
        entities.push_back( std::move( source ) );
        report.Created = true;
        return report;
    }

    TimeOfDayComponentReport MigrateTimeOfDayComponentV42ToV43( std::vector<Assets::EntityData>& entities )
    {
        constexpr std::array<const char*, 5> kClockKeys = { "DriveSunFromTimeOfDay", "TimeOfDay",
                                                            "DayLengthSeconds", "Latitude", "NorthOffset" };
        TimeOfDayComponentReport             report;
        for ( auto& entity : entities )
        {
            const std::string who = entity.id ? entity.id->ToString() : std::string( "<record without id>" );
            // Collected first and written only when the sky block let go of its keys: a refusal leaves the
            // record exactly as it was read.
            rfl::Generic::Object clock;
            EditBlock( entity.Components, "SkyAtmosphere",
                       [&]( rfl::Generic::Object& block )
                       {
                           for ( const char* key : kClockKeys )
                               if ( const auto value = block.get( key ); value.has_value() )
                                   clock[key] = value.value();
                           if ( clock.size() == 0 )
                               return false;
                           if ( entity.Components.get( "TimeOfDay" ).has_value() )
                           {
                               report.Refused.push_back( std::format(
                                    "entity {}: its SkyAtmosphere states clock keys and the record already has "
                                    "a TimeOfDay block; which clock is meant is not the migrator's to guess",
                                    who ) );
                               clock = rfl::Generic::Object{};
                               return false;
                           }
                           for ( const char* key : kClockKeys )
                               report.KeysMoved += DropKey( block, key ) ? 1 : 0;
                           return true;
                       } );
            if ( clock.size() != 0 )
            {
                entity.Components["TimeOfDay"] = rfl::Generic( std::move( clock ) );
                ++report.Clocks;
            }
            if ( !entity.PrefabOverrides )
                continue;
            for ( auto& override_ : *entity.PrefabOverrides )
                EditBlock( override_.Components, "SkyAtmosphere",
                           [&]( rfl::Generic::Object& block )
                           {
                               for ( const char* key : kClockKeys )
                                   if ( block.get( key ).has_value() )
                                       report.Refused.push_back(
                                            std::format( "entity {} (prefab override): SkyAtmosphere states {}, "
                                                         "which is a TimeOfDay key now; restate it on the "
                                                         "prefab's own TimeOfDay block",
                                                         who, key ) );
                               return false;
                           } );
        }
        return report;
    }

    UIAnimationsReport MigrateUIAnimationsV40ToV41( std::vector<Assets::EntityData>& entities )
    {
        namespace TL = Animation::Timeline;
        UIAnimationsReport report;
        for ( auto& entity : entities )
        {
            const std::string who = entity.id ? entity.id->ToString() : std::string( "<record without id>" );
            EditBlock(
                 entity.Components, "UIAnim",
                 [&]( rfl::Generic::Object& block )
                 {
                     const auto v40 =
                          rfl::json::read<TL::UIAnimationV40, rfl::DefaultIfMissing>( rfl::json::write( block ) );
                     if ( !v40 )
                     {
                         report.Refused.push_back( std::format(
                              "entity {}: its UIAnim block is not a v40 clip: {}", who, v40.error().what() ) );
                         return false;
                     }
                     auto lifted = TL::LiftUIAnimation( v40.value(), who, Animation::PROJECT_TICK_RATE,
                                                        Animation::DEFAULT_DISPLAY_RATE );
                     if ( !lifted )
                     {
                         report.Refused.push_back( std::format( "entity {}: {}", who, lifted.GetError() ) );
                         return false;
                     }
                     auto written = TL::WriteSequence( lifted.GetValue().Lifted );
                     if ( !written )
                     {
                         report.Refused.push_back(
                              std::format( "entity {}: the TMLN writer refused: {}", who, written.GetError() ) );
                         return false;
                     }
                     const std::vector<uint8_t> bytes = written.ExtractValue();
                     const auto sequence = rfl::json::read<rfl::Generic>( std::string( Common::TextOf( bytes ) ) );
                     if ( !sequence )
                     {
                         report.Refused.push_back(
                              std::format( "entity {}: the TMLN writer's text does not read: {}", who,
                                           sequence.error().what() ) );
                         return false;
                     }
                     ++report.Clips;
                     report.RoundedKeys += lifted.GetValue().Report.RoundedKeys;
                     rfl::Generic::Object next;
                     next["Sequence"] = sequence.value();
                     next["Loop"]     = rfl::Generic(
                          static_cast<int64_t>( v40.value().Loop ? TL::LoopMode::Loop : TL::LoopMode::Once ) );
                     next["AutoPlay"] = rfl::Generic( v40.value().Playing );
                     block            = std::move( next );
                     return true;
                 } );
            if ( !entity.PrefabOverrides )
                continue;
            for ( const auto& override_ : *entity.PrefabOverrides )
                if ( override_.Components.get( "UIAnim" ).has_value() )
                    report.Refused.push_back( std::format(
                         "entity {}: a prefab override restates UIAnim, which has no v40 whole to lift "
                         "- move the clip onto the prefab's own record",
                         who ) );
        }
        return report;
    }

    UIAnimationTimelinesReport MigrateUIAnimationTimelinesV1ToV2( std::vector<Assets::EntityData>& entities )
    {
        UIAnimationTimelinesReport report;
        const auto shift = [&]( rfl::ExtraFields<rfl::Generic>& components, const std::string& who )
        {
            EditBlock(
                 components, "UIAnim",
                 [&]( rfl::Generic::Object& block )
                 {
                     const auto sequence = block.get( "Sequence" );
                     if ( !sequence )
                         return false; // an override restating Loop/AutoPlay only
                     const std::string text   = rfl::json::write( sequence.value() );
                     const auto        stated = StatedTimelineVersion( text );
                     if ( !stated )
                     {
                         report.Refused.push_back(
                              std::format( "entity {}: UIAnim: {}", who, stated.GetError() ) );
                         return false;
                     }
                     if ( stated.GetValue() != Animation::Timeline::kTimelineLastArrivingInterpVersion )
                         return false;
                     auto shifted = ShiftTimelineV1( text );
                     if ( !shifted )
                     {
                         report.Refused.push_back(
                              std::format( "entity {}: UIAnim: {}", who, shifted.GetError() ) );
                         return false;
                     }
                     auto written = Animation::Timeline::WriteSequence( shifted.GetValue().Shifted );
                     if ( !written )
                     {
                         report.Refused.push_back(
                              std::format( "entity {}: the TMLN writer refused: {}", who, written.GetError() ) );
                         return false;
                     }
                     const std::vector<uint8_t> bytes = written.ExtractValue();
                     const auto next = rfl::json::read<rfl::Generic>( std::string( Common::TextOf( bytes ) ) );
                     if ( !next )
                     {
                         report.Refused.push_back( std::format(
                              "entity {}: the TMLN writer's text does not read: {}", who, next.error().what() ) );
                         return false;
                     }
                     block["Sequence"] = next.value();
                     ++report.Clips;
                     report.SamplesProved += shifted.GetValue().SamplesProved;
                     return true;
                 } );
        };
        for ( auto& entity : entities )
        {
            const std::string who = entity.id ? entity.id->ToString() : std::string( "<record without id>" );
            shift( entity.Components, who );
            if ( entity.PrefabOverrides )
                for ( auto& override_ : *entity.PrefabOverrides )
                    shift( override_.Components, std::format( "{} (prefab override)", who ) );
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

        // FOLT 4..7's Wind, member for member: the engine's FoliageWind is v8 and has no direction (the scene's
        // WindSource gives it).
        struct FoliageWindV7
        {
            float Strength         = 0.0f;
            float Speed            = 0.5f;
            float Height           = 100.0f;
            float DirectionDegrees = 0.0f;
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
            FoliageWindV7                                             Wind;
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
            FoliageWindV7                                             Wind;
            bool                                                      IncludeInHLOD = true;
        };

        // FOLT 6's body: v5, Kind and Prefab. The engine's struct is v7.
        struct FoliageTypeDataV6
        {
            std::optional<Common::Content::TextAssetHeaderSerialized> Header;
            Assets::Serialization::FoliageTypeKind      Kind = Assets::Serialization::FoliageTypeKind::Mesh;
            Assets::AssetGuidRef                        Mesh;
            Assets::AssetGuidRef                        Prefab;
            float                                       Density = 100.0f;
            Assets::Serialization::FoliageFloatInterval ScaleX{ 0.8f, 1.3f };
            Assets::Serialization::FoliageFloatInterval ZOffset{ 0.0f, 0.0f };
            bool                                        AlignToNormal    = true;
            bool                                        RandomYaw        = true;
            float                                       RandomPitchAngle = 0.0f;
            Assets::Serialization::FoliageFloatInterval GroundSlopeAngle{ 0.0f, 90.0f };
            Assets::Serialization::FoliageFloatInterval Height{ -262144.0f, 262144.0f };
            std::vector<Assets::AssetGuidRef>           LandscapeLayers;
            float                                       MinimumLayerWeight = 0.0f;
            Assets::Serialization::FoliageFloatInterval CullDistance{ 0.0f, 0.0f };
            FoliageWindV7                               Wind;
            bool                                        IncludeInHLOD = true;
        };

        // FOLT 7's body: v6 and Procedural. The engine's struct is v8 (its Wind has no DirectionDegrees).
        struct FoliageTypeDataV7
        {
            std::optional<Common::Content::TextAssetHeaderSerialized> Header;
            Assets::Serialization::FoliageTypeKind      Kind = Assets::Serialization::FoliageTypeKind::Mesh;
            Assets::AssetGuidRef                        Mesh;
            Assets::AssetGuidRef                        Prefab;
            float                                       Density = 100.0f;
            Assets::Serialization::FoliageFloatInterval ScaleX{ 0.8f, 1.3f };
            Assets::Serialization::FoliageFloatInterval ZOffset{ 0.0f, 0.0f };
            bool                                        AlignToNormal    = true;
            bool                                        RandomYaw        = true;
            float                                       RandomPitchAngle = 0.0f;
            Assets::Serialization::FoliageFloatInterval GroundSlopeAngle{ 0.0f, 90.0f };
            Assets::Serialization::FoliageFloatInterval Height{ -262144.0f, 262144.0f };
            std::vector<Assets::AssetGuidRef>           LandscapeLayers;
            float                                       MinimumLayerWeight = 0.0f;
            Assets::Serialization::FoliageFloatInterval CullDistance{ 0.0f, 0.0f };
            FoliageWindV7                               Wind;
            bool                                        IncludeInHLOD = true;
            Assets::Serialization::FoliageProcedural    Procedural;
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

    // Named, not anonymous: rfl reflects these by aggregate conversion, which needs types with linkage.
    namespace SkeletonLegacy
    {
        // Where SKEL 1-2 said an imported rig came from; SKEL 3 dropped it (read here, never written).
        struct SkeletonImportInfoV2
        {
            std::string Source;
            uint64_t    SourceHash = 0;
        };
        // SKEL 1 and 2 as they were written: SKEL 1 lacks PreviewMesh / CompatibleSkeletons, both carry Import.
        struct SkeletonAssetDataV1V2
        {
            std::optional<Common::Content::TextAssetHeaderSerialized> Header;
            uint64_t                                                  Signature = 0;
            std::vector<Desert::Animation::BoneInfo>                  Bones;
            std::optional<SkeletonImportInfoV2>                       Import;
            std::optional<Assets::AssetGuidRef>                       PreviewMesh;
            std::optional<std::vector<Assets::AssetGuidRef>>          CompatibleSkeletons;
        };

        // The rig as ANY generation this tool raises states it (SKEL 1, 2 or the current one), with the
        // version it states; an unreadable body or a missing header is an error naming why.
        // The header is handed out on its own, as a value: the reader is what proves it is there.
        struct AnySkeleton
        {
            SkeletonAssetDataV1V2                      Data;
            Common::Content::TextAssetHeaderSerialized Header;
            uint32_t                                   Version = 0;
        };
        static Common::ResultStr<AnySkeleton> ReadAnySkeleton( const std::string& text )
        {
            const auto read = Common::Json::Read<SkeletonAssetDataV1V2>( text );
            if ( !read )
                return Common::MakeFormattedError<AnySkeleton>( "the skeleton body does not read: {}",
                                                                read.GetError() );
            const auto& header = read.GetValue().Header;
            if ( !header.has_value() )
                return Common::MakeFormattedError<AnySkeleton>( "the file states no header" );
            const auto stated = header->Versions.find( "SKEL" );
            if ( stated == header->Versions.end() )
                return Common::MakeFormattedError<AnySkeleton>( "the header states no SKEL version" );
            return Common::MakeSuccess( AnySkeleton{ read.GetValue(), *header, stated->second } );
        }
    } // namespace SkeletonLegacy
    using SkeletonLegacy::ReadAnySkeleton;

    Common::ResultStr<std::string> MigrateSkeletonToV3( const std::string& text )
    {
        const auto any = ReadAnySkeleton( text );
        if ( !any )
            return Common::MakeError<std::string>( any.GetError() );
        const auto& [old, header, version] = any.GetValue();
        if ( version != 1u && version != 2u )
            return Common::MakeFormattedError<std::string>(
                 "the header states SKEL {}, and this step raises SKEL 1 and 2 only", version );

        Assets::Serialization::SkeletonAssetData   data;
        Common::Content::TextAssetHeaderSerialized stamped = header;
        stamped.Versions["SKEL"]                           = Assets::kSkeletonSchemaVersion;
        data.Header                                        = std::move( stamped );
        data.Signature                                     = old.Signature;
        data.Bones                                         = old.Bones;
        data.PreviewMesh                                   = old.PreviewMesh;
        data.CompatibleSkeletons = old.CompatibleSkeletons.value_or( std::vector<Assets::AssetGuidRef>{} );
        std::string written      = Common::Json::Write( data );
        // What the step writes, the engine's reader must read.
        if ( auto back = Assets::Serialization::ReadSkeletonJson( written ); !back )
            return Common::MakeFormattedError<std::string>( "the raised file does not read as SKEL {}: {}",
                                                            Assets::kSkeletonSchemaVersion, back.GetError() );
        return Common::MakeSuccess( std::move( written ) );
    }

    namespace AnimGraphLegacy
    {
        namespace G = Animation::Graph;
        // ANGR 2: the current layout without the Output Pose node's position.
        struct AnimLayerGraphV2
        {
            std::string              Interface;
            std::string              Layer;
            std::vector<G::PoseNode> Nodes;
            std::string              OutputPose;
        };
        struct AnimGraphLayersV2
        {
            std::vector<G::AnimLayerInterface> Interfaces;
            std::vector<AnimLayerGraphV2>      Implemented;
        };
        struct AnimGraphV2
        {
            std::optional<Common::Content::TextAssetHeaderSerialized> Header;
            std::string                                               Name;
            std::vector<G::Parameter>                                 Parameters;
            std::vector<G::PoseNode>                                  Nodes;
            std::string                                               OutputPose;
            std::optional<AnimGraphLayersV2>                          Layers;
        };
    } // namespace AnimGraphLegacy

    Common::ResultStr<std::string> MigrateAnimGraphV2ToV3( const std::string& text )
    {
        namespace G     = Animation::Graph;
        const auto read = Common::Json::Read<AnimGraphLegacy::AnimGraphV2>( text );
        if ( !read )
            return Common::MakeFormattedError<std::string>( "ANGR 2 body does not read: {}", read.GetError() );
        const AnimGraphLegacy::AnimGraphV2& old = read.GetValue();
        if ( !old.Header )
            return Common::MakeFormattedError<std::string>( "the file states no header" );
        const auto stated = old.Header->Versions.find( "ANGR" );
        if ( stated == old.Header->Versions.end() || stated->second != 2u )
            return Common::MakeFormattedError<std::string>(
                 "the header states ANGR {}, and this step raises ANGR 2 only",
                 stated == old.Header->Versions.end() ? std::string( "nothing" )
                                                      : std::to_string( stated->second ) );

        G::AnimGraph graph;
        graph.Header                   = old.Header;
        graph.Header->Versions["ANGR"] = 3u;
        graph.Name                     = old.Name;
        graph.Parameters               = old.Parameters;
        graph.Nodes                    = old.Nodes;
        graph.OutputPose               = old.OutputPose;
        std::tie( graph.OutputPoseX, graph.OutputPoseY ) =
             G::DefaultOutputPosePosition( old.Nodes, old.OutputPose );
        if ( old.Layers )
        {
            G::AnimGraphLayers layers;
            layers.Interfaces = old.Layers->Interfaces;
            for ( const AnimGraphLegacy::AnimLayerGraphV2& was : old.Layers->Implemented )
            {
                G::AnimLayerGraph layer;
                layer.Interface  = was.Interface;
                layer.Layer      = was.Layer;
                layer.Nodes      = was.Nodes;
                layer.OutputPose = was.OutputPose;
                std::tie( layer.OutputPoseX, layer.OutputPoseY ) =
                     G::DefaultOutputPosePosition( was.Nodes, was.OutputPose );
                layers.Implemented.push_back( std::move( layer ) );
            }
            graph.Layers = std::move( layers );
        }

        // ANGR 3 IS NOT THE CURRENT GENERATION (ANGR 4 added TargetSkeleton): the engine's writer stamps the
        // current one and an empty TargetSkeleton, so the text is put back to ANGR 3's shape; the next run's
        // StateTargetSkeleton raises it on and re-reads it with the engine's reader.
        auto raised = rfl::json::read<rfl::Generic::Object>( G::Serialize( graph ) );
        if ( !raised )
            return Common::MakeFormattedError<std::string>( "the raised graph does not re-read: {}",
                                                            raised.error().what() );
        rfl::Generic::Object v3;
        for ( const auto& [key, field] : raised.value() )
            if ( key != "TargetSkeleton" )
                v3[key] = field;
        auto header   = v3.get( "Header" ).value_or( rfl::Generic() ).to_object();
        auto versions = header ? header.value().get( "Versions" ).value_or( rfl::Generic() ).to_object()
                               : rfl::Result<rfl::Generic::Object>( rfl::Error( "no header" ) );
        if ( !header || !versions )
            return Common::MakeFormattedError<std::string>( "the raised graph states no header versions" );
        versions.value()["ANGR"]   = rfl::Generic( 3 );
        header.value()["Versions"] = rfl::Generic( std::move( versions.value() ) );
        v3["Header"]               = rfl::Generic( std::move( header.value() ) );
        return Common::Content::CanonicalJsonText( rfl::json::write( v3 ) );
    }

    Common::ResultStr<Animation::SkeletonCandidate> ReadSkeletonCandidate( const std::filesystem::path& path,
                                                                           const std::string&           text )
    {
        // Any generation this tool raises: the candidates are gathered BEFORE the rigs themselves are raised.
        const auto read = ReadAnySkeleton( text );
        if ( !read )
            return Common::MakeFormattedError<Animation::SkeletonCandidate>(
                 "'{}' is not a skeleton candidate: {}", path.string(), read.GetError() );
        const auto& data = read.GetValue().Data;
        const auto  guid = Common::Content::AssetGuidFromText( read.GetValue().Header.Guid );
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

    Common::ResultStr<TargetSkeletonRig> ReadTargetSkeletonRig( const std::filesystem::path& path,
                                                                const std::string&           text )
    {
        const auto candidate = ReadSkeletonCandidate( path, text );
        if ( !candidate )
            return Common::MakeError<TargetSkeletonRig>( candidate.GetError() );
        TargetSkeletonRig rig{
             Common::Content::AssetGuidToText( candidate.GetValue().Guid ), candidate.GetValue().Path, {} };
        const auto skeleton = ReadAnySkeleton( text );
        if ( !skeleton )
            return Common::MakeError<TargetSkeletonRig>( skeleton.GetError() );
        for ( const auto& bone : skeleton.GetValue().Data.Bones )
            rig.Bones.insert( bone.Name );
        return Common::MakeSuccess( std::move( rig ) );
    }

    namespace TargetSkeletonStep
    {
        struct Evidence
        {
            std::unordered_set<std::string> Bones;
            std::unordered_set<std::string> Clips;
        };

        void Collect( const rfl::Generic& node, Evidence& out )
        {
            if ( const auto array = node.to_array(); array.has_value() )
            {
                for ( const auto& item : array.value() )
                    Collect( item, out );
                return;
            }
            const auto object = node.to_object();
            if ( !object.has_value() )
                return;
            bool boneSpace = false;
            if ( const auto kind = object.value().get( "Kind" ); kind.has_value() )
                boneSpace = kind.value().to_string().value_or( "" ) == "Bone";
            for ( const auto& [key, field] : object.value() )
            {
                if ( key.starts_with( "Source" ) || key == "Header" )
                    continue;
                if ( const auto value = field.to_string(); value.has_value() )
                {
                    const bool bone = key == "Bone" || key == "BoneName" || key.ends_with( "Bone" ) ||
                                      ( boneSpace && key == "Target" );
                    if ( bone && !value.value().empty() )
                        out.Bones.insert( value.value() );
                    else if ( key == "Clip" && !value.value().empty() )
                        out.Clips.insert( value.value() );
                    continue;
                }
                Collect( field, out );
            }
        }

        // The engine's reader and writer of the kind: what the step writes, the engine must read.
        Common::ResultStr<std::string> Canonical( const std::string& tag, const std::string& text )
        {
            namespace S = Assets::Serialization;
            if ( tag == "ANGR" )
            {
                const auto read = Animation::Graph::Deserialize( text );
                return read ? Common::MakeSuccess( Animation::Graph::Serialize( read.GetValue() ) )
                            : Common::MakeError<std::string>( read.GetError() );
            }
            if ( tag == "CRIG" )
            {
                const auto read = S::ParseControlRig( text );
                return read ? Common::MakeSuccess( S::WriteControlRig( read.GetValue() ) )
                            : Common::MakeError<std::string>( read.GetError() );
            }
            const auto read = S::ParseRetarget( text );
            return read ? Common::MakeSuccess( S::WriteRetarget( read.GetValue() ) )
                        : Common::MakeError<std::string>( read.GetError() );
        }
    } // namespace TargetSkeletonStep

    Common::ResultStr<std::string>
    StateTargetSkeleton( const std::string& text, const std::string& tag,
                         const std::vector<TargetSkeletonRig>&               rigs,
                         const std::unordered_map<std::string, std::string>& clipRigs )
    {
        const uint32_t from = tag == "ANGR" ? 3u : tag == "CRIG" ? 2u : 3u;
        const uint32_t to   = tag == "ANGR"   ? Assets::kAnimGraphSchemaVersion
                              : tag == "CRIG" ? Assets::kControlRigSchemaVersion
                                              : Assets::kRetargetSchemaVersion;
        auto           read = rfl::json::read<rfl::Generic::Object>( text );
        if ( !read )
            return Common::MakeFormattedError<std::string>( "{} {} body does not read: {}", tag, from,
                                                            read.error().what() );
        rfl::Generic::Object document = std::move( read.value() );
        auto                 header   = document.get( "Header" ).value_or( rfl::Generic() ).to_object();
        if ( !header.has_value() )
            return Common::MakeFormattedError<std::string>( "the file states no header" );
        auto versions = header.value().get( "Versions" ).value_or( rfl::Generic() ).to_object();
        if ( !versions.has_value() ||
             versions.value().get( tag ).value_or( rfl::Generic() ).to_int().value_or( -1 ) !=
                  static_cast<int>( from ) )
            return Common::MakeFormattedError<std::string>(
                 "the header does not state {} {}, and this step raises "
                 "{} {} only",
                 tag, from, tag, from );

        TargetSkeletonStep::Evidence evidence;
        TargetSkeletonStep::Collect( rfl::Generic( document ), evidence );
        std::unordered_set<std::string> clipSkeletons;
        for ( const std::string& clip : evidence.Clips )
        {
            const auto rig = clipRigs.find( clip );
            if ( rig == clipRigs.end() )
                return Common::MakeFormattedError<std::string>( "plays clip '{}', which no .anim of the corpus is "
                                                                "named; its skeleton cannot be stated",
                                                                clip );
            clipSkeletons.insert( rig->second );
        }
        if ( evidence.Bones.empty() && clipSkeletons.empty() )
            return Common::MakeFormattedError<std::string>( "names no bone and plays no clip: nothing states its "
                                                            "skeleton; author TargetSkeleton by hand" );
        std::vector<const TargetSkeletonRig*> fits;
        for ( const TargetSkeletonRig& rig : rigs )
        {
            bool fit =
                 clipSkeletons.empty() || ( clipSkeletons.size() == 1 && clipSkeletons.contains( rig.Guid ) );
            for ( const std::string& bone : evidence.Bones )
                fit = fit && rig.Bones.contains( bone );
            if ( fit )
                fits.push_back( &rig );
        }
        if ( fits.size() != 1 )
        {
            std::string named;
            for ( const TargetSkeletonRig* rig : fits )
                named += ( named.empty() ? "" : ", " ) + rig->Path;
            return Common::MakeFormattedError<std::string>(
                 "{} skeletons fit its {} bone name(s) and {} clip rig(s) ({}); exactly one must - author "
                 "TargetSkeleton by hand",
                 fits.size(), evidence.Bones.size(), clipSkeletons.size(), named.empty() ? "none" : named );
        }

        versions.value()[tag]      = rfl::Generic( static_cast<int>( to ) );
        header.value()["Versions"] = rfl::Generic( std::move( versions.value() ) );
        document["Header"]         = rfl::Generic( std::move( header.value() ) );
        rfl::Generic::Object target;
        target["Guid"]             = rfl::Generic( fits.front()->Guid );
        target["Path"]             = rfl::Generic( fits.front()->Path );
        document["TargetSkeleton"] = rfl::Generic( std::move( target ) );

        const auto written = TargetSkeletonStep::Canonical( tag, rfl::json::write( document ) );
        if ( !written )
            return Common::MakeFormattedError<std::string>( "the raised file does not read as {} {}: {}", tag, to,
                                                            written.GetError() );
        return written;
    }

    Common::ResultStr<std::string>
    MigrateMeshBinaryToV5( const std::string_view path, const std::string_view bytes,
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
        uint32_t version      = 0;
        uint32_t sectionCount = 0;
        uint32_t flags        = 0;
        uint64_t fileSize     = 0;
        uint64_t signature    = 0;
        std::memcpy( &version, bytes.data() + 12, 4 );
        std::memcpy( &fileSize, bytes.data() + 16, 8 );
        std::memcpy( &sectionCount, bytes.data() + 24, 4 );
        std::memcpy( &flags, bytes.data() + 28, 4 );
        std::memcpy( &signature, bytes.data() + 32, 8 );
        if ( version != 3u && version != 4u )
            return Common::MakeFormattedError<std::string>( "'{}' is mesh version {}; this step raises 3 and 4",
                                                            path, version );
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

        const uint64_t          delta = C::kMeshBinaryPrefixSize + sizeof( Row ) * kRows - oldEnd;
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
        const auto headerBytes = std::bit_cast<std::array<char, sizeof( header )>>( header );
        raised.append( headerBytes.data(), headerBytes.size() );
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
            const auto rowBytes = std::bit_cast<std::array<char, sizeof( row )>>( row );
            raised.append( rowBytes.data(), rowBytes.size() );
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

    Common::ResultStr<std::string>
    MigrateMeshSourceToV3( const std::string_view path, const std::string_view bytes,
                           const std::span<const Animation::SkeletonCandidate> skeletons )
    {
        namespace C   = Common::Content;
        auto envelope = C::ReadAssetEnvelope( std::as_bytes( std::span( bytes.data(), bytes.size() ) ),
                                              Assets::MeshAssetHeaderReadContext() );
        if ( !envelope )
            return Common::MakeFormattedError<std::string>( "'{}': {}", path, envelope.GetError() );
        C::AssetEnvelope e      = envelope.ExtractValue();
        const auto       source = std::find_if( e.Sections.begin(), e.Sections.end(), []( const auto& section )
                                                { return section.Tag == C::EnvelopeSection::Source; } );
        if ( source == e.Sections.end() )
            return Common::MakeFormattedError<std::string>( "'{}' has no SRCE section", path );

        // THE SRCE 2 LAYOUT (MeshSourceAsset.cpp Reader/EncodeSource at version 2), walked without decoding:
        // U32 version; models {Floats, Ints x3, 3 optional overlays, UV overlays}; slots {String, U64, U64};
        // U8 skinned; skin {U64 signature, bone names, influences}. Little-endian, counts are U32.
        const std::vector<std::byte>& old = source->Bytes;
        std::size_t                   at  = 0;
        bool                          ok  = true;
        const auto                    u32 = [&]() -> uint32_t
        {
            if ( !ok || old.size() - at < 4 )
            {
                ok = false;
                return 0;
            }
            uint32_t v = 0;
            std::memcpy( &v, old.data() + at, 4 );
            at += 4;
            return v;
        };
        const auto skip = [&]( const uint64_t n )
        {
            if ( !ok || old.size() - at < n )
                ok = false;
            else
                at += static_cast<std::size_t>( n );
        };
        const auto array   = [&]() { skip( uint64_t{ 4 } * u32() ); }; // Floats / Ints
        const auto overlay = [&]()
        {
            array();
            array();
        };
        const uint32_t version = u32();
        if ( !ok || version != 2u )
            return Common::MakeFormattedError<std::string>(
                 "'{}' states mesh Source version {}; this step raises 2", path, version );
        const uint32_t models = u32();
        for ( uint32_t m = 0; ok && m < models; ++m )
        {
            for ( int i = 0; i < 4; ++i )
                array();
            for ( int i = 0; ok && i < 3; ++i )
            {
                skip( 1 );
                if ( ok && std::to_integer<uint8_t>( old[at - 1] ) == 1 )
                    overlay();
            }
            const uint32_t uvs = u32();
            for ( uint32_t i = 0; ok && i < uvs; ++i )
                overlay();
        }
        const uint32_t slots = u32();
        for ( uint32_t i = 0; ok && i < slots; ++i )
        {
            skip( u32() );
            skip( 16 );
        }
        skip( 1 );
        if ( !ok )
            return Common::MakeFormattedError<std::string>( "'{}': its SRCE 2 section is truncated", path );
        const bool skinned = std::to_integer<uint8_t>( old[at - 1] ) == 1;

        std::vector<std::byte> raised( old.begin(), old.begin() + static_cast<std::ptrdiff_t>( at ) );
        const uint32_t         three = 3u;
        std::memcpy( raised.data(), &three, 4 );
        if ( skinned )
        {
            uint64_t signature = 0;
            if ( old.size() - at < 8 )
                return Common::MakeFormattedError<std::string>( "'{}': its SRCE 2 skin is truncated", path );
            std::memcpy( &signature, old.data() + at, 8 );
            at += 8;
            const auto guid = Animation::MigrateSkeletonReference( path, signature, skeletons );
            if ( !guid )
                return Common::MakeError<std::string>( guid.GetError() );
            const C::AssetGuid skeleton = guid.GetValue();
            for ( const uint64_t half : { skeleton.Hi, skeleton.Lo } )
                for ( int i = 0; i < 8; ++i )
                    raised.push_back( static_cast<std::byte>( ( half >> ( 8 * i ) ) & 0xFFu ) );
            if ( std::find( e.Asset.Dependencies.begin(), e.Asset.Dependencies.end(), skeleton ) ==
                 e.Asset.Dependencies.end() )
                e.Asset.Dependencies.push_back( skeleton );
        }
        raised.insert( raised.end(), old.begin() + static_cast<std::ptrdiff_t>( at ), old.end() );
        source->Bytes = std::move( raised );

        const auto written = C::WriteAssetEnvelope( e );
        if ( !written )
            return Common::MakeFormattedError<std::string>( "'{}': {}", path, written.GetError() );
        // THE ENGINE JUDGES THE RESULT: the raised file must read as the asset the editor opens, and its writer
        // states it (canonical bytes).
        auto decoded = Assets::DecodeMeshSourceAsset( written.GetValue() );
        if ( !decoded )
            return Common::MakeFormattedError<std::string>( "'{}': the raised SRCE 3 does not read: {}", path,
                                                            decoded.GetError() );
        const auto encoded = Assets::EncodeMeshSourceAsset( decoded.GetValue() );
        if ( !encoded )
            return Common::MakeFormattedError<std::string>( "'{}': {}", path, encoded.GetError() );
        const auto& canonical = encoded.GetValue();
        std::string text( canonical.size(), '\0' );
        std::ranges::transform( canonical, text.begin(),
                                []( const std::byte b ) { return static_cast<char>( b ); } );
        return Common::MakeSuccess( std::move( text ) );
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
        data.Wind = FoliageWindV7{};

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
        if ( !old.Header )
            return Common::MakeFormattedError<std::string>( "the file states no header" );

        // v6 text, not the engine's struct: the engine is v7, and v6 -> v7 is the chain's next step.
        FoliageTypeDataV6 data;
        data.Header                   = old.Header;
        data.Header->Versions["FOLT"] = 6u;
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

        std::string written = Common::Json::Write( data );
        if ( auto next = MigrateFoliageTypeV6ToV7( written ); !next )
            return Common::MakeFormattedError<std::string>( "the raised file does not read as FOLT 6: {}",
                                                            next.GetError() );
        return Common::MakeSuccess( std::move( written ) );
    }

    Common::ResultStr<std::string> MigrateFoliageTypeV6ToV7( const std::string& text )
    {
        if ( const auto stated = Assets::Serialization::StatedFoliageTypeGeneration( text ); stated != 6u )
            return Common::MakeFormattedError<std::string>(
                 "the header states FOLT {}, and this step raises FOLT 6 only",
                 stated ? std::to_string( *stated ) : std::string( "nothing" ) );
        const auto v6 = Common::Json::Read<FoliageTypeDataV6>( text );
        if ( !v6 )
            return Common::MakeFormattedError<std::string>( "FOLT 6 body does not read: {}", v6.GetError() );
        const FoliageTypeDataV6& old = v6.GetValue();
        if ( !old.Header )
            return Common::MakeFormattedError<std::string>( "the file states no header" );

        // v7 text, not the engine's struct: the engine is v8, and v7 -> v8 is the chain's next step.
        FoliageTypeDataV7 data;
        data.Header                   = old.Header;
        data.Header->Versions["FOLT"] = 7u;
        data.Kind                     = old.Kind;
        data.Mesh                     = old.Mesh;
        data.Prefab                   = old.Prefab;
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
        data.IncludeInHLOD            = old.IncludeInHLOD;
        // UE UFoliageType's procedural defaults: FOLT 6 had no procedural simulation to state them for.
        data.Procedural = Assets::Serialization::FoliageProcedural{};

        std::string written = Common::Json::Write( data );
        if ( auto next = MigrateFoliageTypeV7ToV8( written ); !next )
            return Common::MakeFormattedError<std::string>( "the raised file does not read as FOLT 7: {}",
                                                            next.GetError() );
        return Common::MakeSuccess( std::move( written ) );
    }

    Common::ResultStr<std::string> MigrateFoliageTypeV7ToV8( const std::string& text )
    {
        if ( const auto stated = Assets::Serialization::StatedFoliageTypeGeneration( text ); stated != 7u )
            return Common::MakeFormattedError<std::string>(
                 "the header states FOLT {}, and this step raises FOLT 7 only",
                 stated ? std::to_string( *stated ) : std::string( "nothing" ) );
        const auto v7 = Common::Json::Read<FoliageTypeDataV7>( text );
        if ( !v7 )
            return Common::MakeFormattedError<std::string>( "FOLT 7 body does not read: {}", v7.GetError() );
        const FoliageTypeDataV7& old = v7.GetValue();

        Assets::Serialization::FoliageTypeData data;
        data.Header             = old.Header;
        data.Kind               = old.Kind;
        data.Mesh               = old.Mesh;
        data.Prefab             = old.Prefab;
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
        // The response stays the type's; DirectionDegrees is dropped: the direction is the scene's WindSource,
        // read through ECS::WindAt (a per-type direction cannot become a scene-wide one without choosing
        // between types).
        data.Wind.Strength = old.Wind.Strength;
        data.Wind.Speed    = old.Wind.Speed;
        data.Wind.Height   = old.Wind.Height;
        data.IncludeInHLOD = old.IncludeInHLOD;
        data.Procedural    = old.Procedural;

        std::string written = Assets::Serialization::WriteFoliageType( data );
        if ( auto reread = Assets::Serialization::ParseFoliageType( written ); !reread )
            return Common::MakeFormattedError<std::string>( "the raised file does not read as FOLT 8: {}",
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
                    report.Refused = RefusedWhole( name, report.UndeclaredKeys.Refused );
                    return;
                }
            }

            // The player's view is chosen, not defaulted (SPAWN1): IsMainCamera becomes AutoActivateForPlayer.
            if ( statedSceneVersion < kSceneVersionPlayerViewFlag )
            {
                report.PlayerViewFlagRaised = true;
                report.PlayerViewFlag       = MigratePlayerViewFlagV39ToV40( entities );
            }

            // UI animation is a Timeline sequence (ANIM-I9): the v40 key model is lifted once, here.
            if ( statedSceneVersion < kSceneVersionUIAnimationSequences )
            {
                report.UIAnimationsRaised = true;
                report.UIAnimations       = MigrateUIAnimationsV40ToV41( entities );
                if ( !report.UIAnimations.Refused.empty() )
                {
                    report.Refused = RefusedWhole( name, report.UIAnimations.Refused );
                    return;
                }
            }

            // The clock leaves the sky (TOD-SPLIT): its five keys become a TimeOfDay block on the sky's record.
            if ( statedSceneVersion < kSceneVersionTimeOfDayComponent )
            {
                report.TimeOfDayComponentRaised = true;
                report.TimeOfDayComponent       = MigrateTimeOfDayComponentV42ToV43( entities );
                if ( !report.TimeOfDayComponent.Refused.empty() )
                {
                    report.Refused = RefusedWhole( name, report.TimeOfDayComponent.Refused );
                    return;
                }
            }

            // TMLN v1 -> v2 (ANIM-FMT): after the v40 lift (which writes v2 itself); keyed on each block's number.
            report.UIAnimationTimelines       = MigrateUIAnimationTimelinesV1ToV2( entities );
            report.UIAnimationTimelinesRaised = report.UIAnimationTimelines.Clips != 0;
            if ( !report.UIAnimationTimelines.Refused.empty() )
            {
                report.Refused = RefusedWhole( name, report.UIAnimationTimelines.Refused );
                return;
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

    int MigrateGameModeSettingsV43ToV44( SceneSerialized& scene )
    {
        if ( !scene.Settings.has_value() )
            return 0;
        const auto stated = scene.Settings.value().to_object();
        if ( !stated.has_value() )
            return 0;
        bool hasController = false;
        bool hasDelay      = false;
        for ( const auto& [key, field] : stated.value() )
        {
            hasController = hasController || key == "PlayerController";
            hasDelay      = hasDelay || key == "RespawnDelay";
        }
        if ( hasController && hasDelay )
            return 0;

        rfl::Generic::Object unsetPrefab;
        unsetPrefab["Guid"] = rfl::Generic( std::string() );
        unsetPrefab["Path"] = rfl::Generic( std::string() );
        const rfl::Generic delay( static_cast<double>( Core::SceneSettings{}.RespawnDelay ) );

        int                  added  = 0;
        bool                 placed = false;
        rfl::Generic::Object out;
        auto                 addMissing = [&]()
        {
            if ( !hasController )
            {
                out["PlayerController"] = rfl::Generic( unsetPrefab );
                ++added;
            }
            if ( !hasDelay )
            {
                out["RespawnDelay"] = delay;
                ++added;
            }
            placed = true;
        };
        for ( const auto& [key, field] : stated.value() )
        {
            out[key] = field;
            if ( key == "DefaultPawn" )
                addMissing();
        }
        if ( !placed )
            addMissing();
        scene.Settings = rfl::Generic( std::move( out ) );
        return added;
    }

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

        // The cloud layer's wind becomes the scene's WindSource (WIND-SRC). Scene-side so the record is added
        // to a scene and never to a prefab; MigratePrefab runs the same step with createSource = false.
        if ( statedSceneVersion < kSceneVersionWindSource )
        {
            report.WindSourceRaised = true;
            report.WindSource       = MigrateWindSourceV41ToV42( scene.Entities, scene.SceneName, true );
        }
        if ( report.ExternalEntitiesRaised && scene.WorldPartition.has_value() )
            report.EntitiesMovedOut = scene.Entities.size();

        // Scene-only (it reads the Settings block a prefab does not have), so it runs here rather than in
        // RunSteps, after every entity step of the chain.
        if ( statedSceneVersion < kSceneVersionSceneSettingsHomes )
        {
            report.SceneSettingsHomesRaised = true;
            report.SceneSettingsHomes       = MigrateSceneSettingsHomesV35ToV36( scene );
        }
        if ( statedSceneVersion < kSceneVersionGameModeSettings )
        {
            report.GameModeSettingsRaised = true;
            report.GameModeKeysAdded      = MigrateGameModeSettingsV43ToV44( scene );
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
        if ( outcome.Steps.Refused.empty() && outcome.FoundSceneVersion < kSceneVersionGameModeSettings )
            outcome.Steps.GameModeSettingsRaised = true; // the stamp only: a prefab has no settings block
        if ( outcome.Steps.Refused.empty() && outcome.FoundSceneVersion < kSceneVersionWindSource )
        {
            outcome.Steps.WindSourceRaised = true;
            outcome.Steps.WindSource       = MigrateWindSourceV41ToV42( prefab.Entities, prefab.Name, false );
        }
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
