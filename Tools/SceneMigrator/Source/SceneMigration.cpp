#include "SceneMigration.hpp"
#include <Engine/Core/Serialize/SceneDependencies.hpp>
#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Settings/RenderMethods.hpp>
#include <Engine/Scripting/ScriptProperty.hpp>
#include <regex>
#include <cmath>
#include <optional>
#include <array>
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
#include <Engine/ECS/System/SystemRules.hpp>
#include <Engine/Animation/Timeline/Hosts.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>
#include <Engine/Assets/SoundAsset.hpp>
#include <functional>
#include "UILift.hpp"
#include "ClipInterpShift.hpp"
#include <Engine/Core/Serialize/AuthoredComponentIO.hpp>
#include <Engine/Core/Serialize/WorldPartitionConversion.hpp>
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
        constexpr int kSendEventAction = static_cast<int>( UI::UIButtonAction::SendEvent );

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

    WindSourceReport MigrateWindSourceV42ToV43( std::vector<Assets::EntityData>& entities,
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
        for ( const char c : std::format( "WindSource:{}", fileName ) )
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

    TimeOfDayComponentReport MigrateTimeOfDayComponentV43ToV44( std::vector<Assets::EntityData>& entities )
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

    ParticleSpriteMaterialsReport
    MigrateParticleSpriteMaterialsV46ToV47( std::vector<Assets::EntityData>& entities )
    {
        constexpr const char*         kBlend    = "Blend";
        constexpr const char*         kMaterial = "Material";
        constexpr int64_t             kAdditive = 0; // ParticleBlendMode::Additive, the first enumerator
        ParticleSpriteMaterialsReport report;

        // Whether the block stated Additive; the key leaves either way. A missing key was AlphaBlend.
        const auto takeAdditive = [&]( rfl::Generic::Object& block )
        {
            bool additive = false;
            if ( const auto stated = block.get( kBlend ); stated.has_value() )
                additive = stated.value().to_int64().value_or( 1 ) == kAdditive;
            DropKey( block, kBlend );
            return additive;
        };
        const auto additiveMaterial = []
        {
            rfl::Generic::Object ref;
            ref["Guid"] = rfl::Generic( std::string( kParticleAdditiveMaterialGuid ) );
            ref["Path"] = rfl::Generic( std::string( kParticleAdditiveMaterialPath ) );
            return rfl::Generic( std::move( ref ) );
        };

        for ( auto& entity : entities )
        {
            EditBlock( entity.Components, "ParticleEmitter",
                       [&]( rfl::Generic::Object& block )
                       {
                           ++report.Emitters;
                           if ( takeAdditive( block ) && !block.get( kMaterial ).has_value() )
                           {
                               block[kMaterial] = additiveMaterial();
                               ++report.MovedToAdditive;
                           }
                           return true;
                       } );
            if ( !entity.PrefabOverrides )
                continue;
            for ( auto& override_ : *entity.PrefabOverrides )
                EditBlock( override_.Components, "ParticleEmitter",
                           [&]( rfl::Generic::Object& block )
                           {
                               if ( !block.get( kBlend ).has_value() )
                                   return false;
                               if ( takeAdditive( block ) && !block.get( kMaterial ).has_value() )
                               {
                                   block[kMaterial] = additiveMaterial();
                                   ++report.OverridesAdditive;
                               }
                               else
                                   ++report.OverridesDropped;
                               return true;
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

    CollisionProfilesReport MigrateCollisionProfilesV41ToV42( std::vector<Assets::EntityData>& entities )
    {
        static_assert( static_cast<int>( Physics::BodyType::Static ) == 0 &&
                            static_cast<int>( Physics::BodyType::Dynamic ) == 1 &&
                            static_cast<int>( Physics::BodyType::Kinematic ) == 2,
                       "BodyType moved: the v41 numbers this step reads are 0 Static, 1 Dynamic, 2 Kinematic" );
        constexpr const char*   kKey = "CollisionProfile";
        CollisionProfilesReport report;

        // The profile a v41 body of @p block's Type had in effect: UE's presets for a static, a code-moved and a
        // simulated body. False (with a refusal) when Type is stated but is not a BodyType number.
        const auto profileOf = [&]( const rfl::Generic::Object& block, const std::string& who,
                                    const char*& profile ) -> bool
        {
            const auto type = block.get( "Type" );
            if ( !type.has_value() )
            {
                profile = "PhysicsActor"; // the v41 default Type, Dynamic
                return true;
            }
            const auto number = type.value().to_int64();
            if ( number.has_value() && number.value() == static_cast<int64_t>( Physics::BodyType::Static ) )
                profile = "BlockAll";
            else if ( number.has_value() &&
                      number.value() == static_cast<int64_t>( Physics::BodyType::Kinematic ) )
                profile = "BlockAllDynamic";
            else if ( number.has_value() && number.value() == static_cast<int64_t>( Physics::BodyType::Dynamic ) )
                profile = "PhysicsActor";
            else
            {
                report.Refused.push_back(
                     std::format( "{}: RigidBody.Type {} is not a body type (0 Static, 1 Dynamic, 2 Kinematic)",
                                  who, rfl::json::write( type.value() ) ) );
                return false;
            }
            return true;
        };

        for ( auto& entity : entities )
        {
            const std::string who = entity.id ? std::format( "entity {}", entity.id->ToString() )
                                              : std::string( "<record without id>" );
            EditBlock( entity.Components, "RigidBody",
                       [&]( rfl::Generic::Object& block )
                       {
                           if ( block.get( kKey ).has_value() )
                               return false;
                           const char* profile = nullptr;
                           if ( !profileOf( block, who, profile ) )
                               return false;
                           block[kKey] = rfl::Generic( std::string( profile ) );
                           ++report.Bodies;
                           return true;
                       } );
            EditBlock( entity.Components, "CharacterController",
                       [&]( rfl::Generic::Object& block )
                       {
                           if ( block.get( kKey ).has_value() )
                               return false;
                           block[kKey] = rfl::Generic( std::string( "Pawn" ) );
                           ++report.Characters;
                           return true;
                       } );
            if ( !entity.PrefabOverrides )
                continue;
            // An override restating Type changes the body's kind; the profile follows it. One that does not
            // restate Type inherits the prefab record's profile, which the prefab's own migration states.
            for ( auto& override_ : *entity.PrefabOverrides )
                EditBlock( override_.Components, "RigidBody",
                           [&]( rfl::Generic::Object& block )
                           {
                               if ( !block.get( "Type" ).has_value() || block.get( kKey ).has_value() )
                                   return false;
                               const char* profile = nullptr;
                               if ( !profileOf( block, std::format( "{} (prefab override)", who ), profile ) )
                                   return false;
                               block[kKey] = rfl::Generic( std::string( profile ) );
                               ++report.Bodies;
                               return true;
                           } );
        }
        return report;
    }

    Common::ResultStr<std::pair<Common::Content::AssetGuid, std::filesystem::path>>
    SoundForAudioFile( const std::filesystem::path& audioFile )
    {
        using Found = std::pair<Common::Content::AssetGuid, std::filesystem::path>;
        std::error_code    ec;
        const auto         dir = audioFile.parent_path();
        std::vector<Found> found;
        for ( const auto& entry : std::filesystem::directory_iterator( dir, ec ) )
        {
            if ( entry.path().extension() != Assets::kSoundExtension )
                continue;
            std::ifstream      in( entry.path(), std::ios::binary );
            std::ostringstream text;
            text << in.rdbuf();
            const auto parsed = Assets::SoundAsset::Parse( text.str() );
            if ( !parsed )
                return Common::MakeFormattedError<Found>( "'{}': {}", entry.path().string(), parsed.GetError() );
            if ( ( dir / parsed.GetValue().Source ).lexically_normal() == audioFile.lexically_normal() )
                found.emplace_back( parsed.GetValue().Guid, entry.path() );
        }
        if ( ec )
            return Common::MakeFormattedError<Found>( "'{}' cannot be listed: {}", dir.string(), ec.message() );
        if ( found.size() != 1 )
            return Common::MakeFormattedError<Found>(
                 "{} `.desound` beside '{}' name it as their Source (exactly one must): create the Sound asset",
                 found.size(), audioFile.string() );
        return Common::MakeSuccess( found.front() );
    }

    SoundRefsReport MigrateSoundRefsV45ToV46( std::vector<Assets::EntityData>& entities,
                                              const std::filesystem::path&     assetsRoot )
    {
        SoundRefsReport report;
        // A v41 path is relative to its project (the content root), which is an ancestor of the scene's
        // directory: the same rule LocateMeshFile applies to `meshPath`. The project is kept for the Path the
        // Sound reference states. A file found under no ancestor is named under `assetsRoot` and refused.
        std::filesystem::path project  = assetsRoot;
        const auto            absolute = [&]( const std::string& path )
        {
            const std::filesystem::path named( path );
            if ( named.is_absolute() )
                return named;
            if ( const auto located = LocateMeshFile( path, assetsRoot ) )
            {
                project = located.GetValue().Project;
                return located.GetValue().File;
            }
            project = assetsRoot;
            return ( assetsRoot / named ).lexically_normal();
        };
        // Every Audio section of a sequence is an object {"Audio": {"Sound": ...}} somewhere under it.
        const std::function<void( rfl::Generic&, const std::string& )> walk =
             [&]( rfl::Generic& node, const std::string& who )
        {
            if ( auto array = node.to_array(); array.has_value() )
            {
                auto items = std::move( array.value() );
                for ( auto& item : items )
                    walk( item, who );
                node = rfl::Generic( std::move( items ) );
                return;
            }
            auto object = node.to_object();
            if ( !object.has_value() )
                return;
            rfl::Generic::Object fields = std::move( object.value() );
            for ( auto& [name, value] : fields )
            {
                if ( name == "Audio" )
                {
                    auto audio = value.to_object();
                    if ( !audio.has_value() )
                        continue;
                    rfl::Generic::Object block = std::move( audio.value() );
                    const auto           sound = block.get( "Sound" );
                    const std::string    text =
                         sound.has_value() ? sound.value().to_string().value_or( std::string() ) : std::string();
                    if ( !text.empty() && !Common::Content::AssetGuidFromText( text ) )
                    {
                        const auto asset = SoundForAudioFile( absolute( text ) );
                        if ( !asset )
                            report.Refused.push_back(
                                 std::format( "entity {}: Audio section: {}", who, asset.GetError() ) );
                        else
                        {
                            block["Sound"] =
                                 rfl::Generic( Common::Content::AssetGuidToText( asset.GetValue().first ) );
                            ++report.Sections;
                        }
                    }
                    value = rfl::Generic( std::move( block ) );
                    continue;
                }
                walk( value, who );
            }
            node = rfl::Generic( std::move( fields ) );
        };
        const auto raise = [&]( rfl::ExtraFields<rfl::Generic>& components, const std::string& who )
        {
            EditBlock( components, "AudioSource",
                       [&]( rfl::Generic::Object& block )
                       {
                           const auto clip = block.get( "Clip" );
                           if ( !clip.has_value() )
                               return false;
                           const std::string path = clip.value().to_string().value_or( "" );
                           DropKey( block, "Clip" );
                           ++report.Sources;
                           if ( path.empty() )
                               return true; // no clip stated: the slot is unset
                           const auto asset = SoundForAudioFile( absolute( path ) );
                           if ( !asset )
                           {
                               report.Refused.push_back(
                                    std::format( "entity {}: AudioSource.Clip: {}", who, asset.GetError() ) );
                               return false;
                           }
                           rfl::Generic::Object ref;
                           ref["Guid"] =
                                rfl::Generic( Common::Content::AssetGuidToText( asset.GetValue().first ) );
                           ref["Path"] = rfl::Generic(
                                asset.GetValue().second.lexically_relative( project ).generic_string() );
                           block["Sound"] = rfl::Generic( std::move( ref ) );
                           return true;
                       } );
            EditBlock( components, "UIAnim",
                       [&]( rfl::Generic::Object& block )
                       {
                           const auto sequence = block.get( "Sequence" );
                           if ( !sequence )
                               return false;
                           const std::size_t before = report.Sections;
                           rfl::Generic      tree   = sequence.value();
                           walk( tree, who );
                           if ( report.Sections == before )
                               return false;
                           // A current-generation sequence is rewritten by the one writer, which lists the sound
                           // in its header's Dependencies; a TMLN v1 one gets that from the v1 -> v2 shift after.
                           const std::string text = rfl::json::write( tree );
                           if ( const auto stated = StatedTimelineVersion( text );
                                stated && stated.GetValue() == Animation::Timeline::kTimelineFormatVersion )
                           {
                               const auto read = Animation::Timeline::ReadSequence( Common::BytesOf( text ) );
                               if ( !read )
                               {
                                   report.Refused.push_back( std::format( "entity {}: the TMLN reader refused: {}",
                                                                          who, read.GetError() ) );
                                   return false;
                               }
                               auto written = Animation::Timeline::WriteSequence( read.GetValue() );
                               if ( !written )
                               {
                                   report.Refused.push_back( std::format( "entity {}: the TMLN writer refused: {}",
                                                                          who, written.GetError() ) );
                                   return false;
                               }
                               const auto next = rfl::json::read<rfl::Generic>(
                                    std::string( Common::TextOf( written.GetValue() ) ) );
                               if ( !next )
                               {
                                   report.Refused.push_back(
                                        std::format( "entity {}: the TMLN writer's text does not read: {}", who,
                                                     next.error().what() ) );
                                   return false;
                               }
                               tree = next.value();
                           }
                           block["Sequence"] = std::move( tree );
                           return true;
                       } );
        };
        for ( auto& entity : entities )
        {
            const std::string who = entity.id ? entity.id->ToString() : std::string( "<record without id>" );
            raise( entity.Components, who );
            if ( entity.PrefabOverrides )
                for ( auto& override_ : *entity.PrefabOverrides )
                    raise( override_.Components, who );
        }
        return report;
    }

    SSRDefaultOnReport MigrateSSRDefaultOnV47ToV48( std::vector<Assets::EntityData>& entities )
    {
        constexpr const char* kKey = "EnableSSR";
        SSRDefaultOnReport    report;
        for ( auto& entity : entities )
        {
            const std::string who = entity.id ? std::format( "entity {}", entity.id->ToString() )
                                              : std::string( "<record without id>" );
            // Only the record's own volume: an override's EnableSSR is a delta against its prefab record,
            // which the prefab's own migration raises.
            EditBlock( entity.Components, "PostProcessVolume",
                       [&]( rfl::Generic::Object& block )
                       {
                           rfl::Generic::Object settings;
                           if ( const auto stated = block.get( "Settings" ); stated.has_value() )
                           {
                               auto fields = stated.value().to_object();
                               if ( !fields.has_value() )
                               {
                                   report.Refused.push_back(
                                        std::format( "{}: PostProcessVolume.Settings {} is not an object", who,
                                                     rfl::json::write( stated.value() ) ) );
                                   return false;
                               }
                               settings = std::move( fields.value() );
                           }
                           if ( const auto enabled = settings.get( kKey ); enabled.has_value() )
                           {
                               const auto flag = enabled.value().to_bool();
                               if ( !flag.has_value() )
                               {
                                   report.Refused.push_back(
                                        std::format( "{}: PostProcessVolume.Settings.EnableSSR {} is not a bool",
                                                     who, rfl::json::write( enabled.value() ) ) );
                                   return false;
                               }
                               if ( flag.value() )
                                   return false;
                               DropKey( settings, kKey );
                           }
                           settings[kKey] = rfl::Generic( true );
                           DropKey( block, "Settings" );
                           block["Settings"] = rfl::Generic( std::move( settings ) );
                           ++report.Volumes;
                           return true;
                       } );
        }
        return report;
    }

    RenderMethodsReport MigrateRenderMethodsV52ToV53( std::vector<Assets::EntityData>& entities )
    {
        using Common::Scalability::GIMethod;
        using Common::Scalability::ReflectionMethod;
        RenderMethodsReport report;

        // The old GIMode numbers, in their order (0 Off, 1 ScreenSpace, 2 RSM), as the methods they name.
        constexpr std::array<GIMethod, 3> kFromGIMode{ GIMethod::Off, GIMethod::ScreenSpace,
                                                       GIMethod::ReflectiveShadowMaps };

        const auto restate = [&]( rfl::ExtraFields<rfl::Generic>& components, const std::string& who )
        {
            EditBlock( components, "PostProcessVolume",
                       [&]( rfl::Generic::Object& block )
                       {
                           const auto stated = block.get( "Settings" );
                           if ( !stated.has_value() )
                               return false;
                           auto fields = stated.value().to_object();
                           if ( !fields.has_value() )
                           {
                               report.Refused.push_back(
                                    std::format( "{}: PostProcessVolume.Settings {} is not an object", who,
                                                 rfl::json::write( stated.value() ) ) );
                               return false;
                           }
                           rfl::Generic::Object settings = std::move( fields.value() );
                           bool                 changed  = false;
                           if ( const auto gi = settings.get( "GlobalIllumination" ); gi.has_value() )
                           {
                               const auto mode = gi.value().to_int();
                               if ( !mode.has_value() || mode.value() < 0 ||
                                    mode.value() >= static_cast<int>( kFromGIMode.size() ) )
                               {
                                   report.Refused.push_back( std::format(
                                        "{}: PostProcessVolume.Settings.GlobalIllumination {} is not a GIMode "
                                        "(0 Off, 1 ScreenSpace, 2 RSM)",
                                        who, rfl::json::write( gi.value() ) ) );
                                   return false;
                               }
                               DropKey( settings, "GlobalIllumination" );
                               settings["GlobalIlluminationMethod"] = rfl::Generic(
                                    static_cast<int>( kFromGIMode[static_cast<std::size_t>( mode.value() )] ) );
                               changed = true;
                           }
                           if ( const auto ssr = settings.get( "EnableSSR" ); ssr.has_value() )
                           {
                               const auto flag = ssr.value().to_bool();
                               if ( !flag.has_value() )
                               {
                                   report.Refused.push_back(
                                        std::format( "{}: PostProcessVolume.Settings.EnableSSR {} is not a bool",
                                                     who, rfl::json::write( ssr.value() ) ) );
                                   return false;
                               }
                               DropKey( settings, "EnableSSR" );
                               settings["ReflectionsMethod"] = rfl::Generic( static_cast<int>(
                                    flag.value() ? ReflectionMethod::ScreenSpace : ReflectionMethod::Off ) );
                               changed                       = true;
                           }
                           if ( !changed )
                               return false;
                           DropKey( block, "Settings" );
                           block["Settings"] = rfl::Generic( std::move( settings ) );
                           ++report.Rewritten;
                           return true;
                       } );
        };

        for ( auto& entity : entities )
        {
            const std::string who = entity.id ? std::format( "entity {}", entity.id->ToString() )
                                              : std::string( "<record without id>" );
            restate( entity.Components, who );
            // An override's Settings is a delta against its prefab record, but the keys it states are gone from
            // the type all the same: a delta stating EnableSSR states ReflectionsMethod now.
            if ( entity.PrefabOverrides )
                for ( auto& override_ : *entity.PrefabOverrides )
                    restate( override_.Components, who );
        }
        return report;
    }

    LightUnitsReport MigrateLightUnitsV55ToV56( std::vector<Assets::EntityData>& entities )
    {
        LightUnitsReport report;

        // What the pre-v56 type defaulted to: a block that stated nothing kept these, so they are what it meant.
        constexpr int   kOldFalloffQuadratic = static_cast<int>( ECS::LightFalloff::Quadratic );
        constexpr float kOldIntensity        = 1.0f;
        constexpr float kOldMinLuminance     = 0.02f;
        constexpr float kOldMaxLuminance     = 8.0f;
        static_assert( static_cast<int>( ECS::LightUnits::Unitless ) == 0,
                       "LightUnits moved: the migration states Unitless as 0" );

        const auto number = []( const rfl::Generic& g ) -> std::optional<double>
        {
            if ( const auto d = g.to_double(); d.has_value() )
                return d.value();
            if ( const auto i = g.to_int64(); i.has_value() )
                return static_cast<double>( i.value() );
            return std::nullopt;
        };

        // A record states every key a pre-v56 build would have defaulted; an override is a delta against its
        // prefab record (which its own migration raises), so it only gains the units every older light had.
        const auto lights = [&]( rfl::ExtraFields<rfl::Generic>& components, bool record )
        {
            for ( const char* light : { "PointLight", "SpotLight" } )
                EditBlock( components, light,
                           [&]( rfl::Generic::Object& block )
                           {
                               if ( block.get( "IntensityUnits" ).has_value() )
                                   return false;
                               block["IntensityUnits"] = rfl::Generic( 0 );
                               if ( record && !block.get( "Falloff" ).has_value() )
                                   block["Falloff"] = rfl::Generic( kOldFalloffQuadratic );
                               if ( record && !block.get( "Intensity" ).has_value() )
                                   block["Intensity"] = rfl::Generic( static_cast<double>( kOldIntensity ) );
                               ++report.Lights;
                               return true;
                           } );
        };

        // AutoExposureMin / Max (luminance) -> AutoExposureMin / MaxEV100, the same clamp.
        const auto exposure =
             [&]( rfl::ExtraFields<rfl::Generic>& components, bool record, const std::string& who )
        {
            EditBlock( components, "PostProcessVolume",
                       [&]( rfl::Generic::Object& block )
                       {
                           const auto stated = block.get( "Settings" );
                           if ( !stated.has_value() )
                               return false;
                           auto fields = stated.value().to_object();
                           if ( !fields.has_value() )
                           {
                               report.Refused.push_back(
                                    std::format( "{}: PostProcessVolume.Settings {} is not an object", who,
                                                 Describe( stated.value() ) ) );
                               return false;
                           }
                           rfl::Generic::Object settings = std::move( fields.value() );
                           bool                 changed  = false;
                           const auto           restate  = [&]( const char* oldKey, const char* newKey,
                                                     float oldDefault ) -> bool
                           {
                               if ( settings.get( newKey ).has_value() )
                                   return true;
                               double luminance = oldDefault;
                               if ( const auto value = settings.get( oldKey ); value.has_value() )
                               {
                                   const auto n = number( value.value() );
                                   if ( !n.has_value() || !( n.value() > 0.0 ) )
                                   {
                                       report.Refused.push_back( std::format(
                                            "{}: PostProcessVolume.Settings.{} {} is not a positive luminance",
                                            who, oldKey, Describe( value.value() ) ) );
                                       return false;
                                   }
                                   luminance = n.value();
                                   DropKey( settings, oldKey );
                               }
                               else if ( !record )
                                   return true; // a delta that does not state it inherits its prefab's
                               settings[newKey] = rfl::Generic( std::log2( luminance ) + 3.0 );
                               changed          = true;
                               return true;
                           };
                           if ( !restate( "AutoExposureMin", "AutoExposureMinEV100", kOldMinLuminance ) ||
                                !restate( "AutoExposureMax", "AutoExposureMaxEV100", kOldMaxLuminance ) )
                               return false;
                           if ( !changed )
                               return false;
                           DropKey( block, "Settings" );
                           block["Settings"] = rfl::Generic( std::move( settings ) );
                           ++report.Volumes;
                           return true;
                       } );
        };

        for ( auto& entity : entities )
        {
            const std::string who = entity.id ? std::format( "entity {}", entity.id->ToString() )
                                              : std::string( "<record without id>" );
            lights( entity.Components, true );
            exposure( entity.Components, true, who );
            if ( entity.PrefabOverrides )
                for ( auto& override_ : *entity.PrefabOverrides )
                {
                    lights( override_.Components, false );
                    exposure( override_.Components, false, who );
                }
        }
        return report;
    }

    SunCalendarReport MigrateSunCalendarV57ToV58( std::vector<Assets::EntityData>& entities )
    {
        constexpr std::array<const char*, 6> kCalendarKeys = { "Longitude", "TimeZone", "DaylightSavingTime",
                                                               "Year",      "Month",    "Day" };
        SunCalendarReport                    report;
        for ( auto& entity : entities )
            EditBlock( entity.Components, "TimeOfDay",
                       [&]( rfl::Generic::Object& block )
                       {
                           for ( const char* key : kCalendarKeys )
                               if ( block.get( key ).has_value() )
                                   return false;
                           block["Longitude"]          = rfl::Generic( 0.0 );
                           block["TimeZone"]           = rfl::Generic( 0.0 );
                           block["DaylightSavingTime"] = rfl::Generic( false );
                           block["Year"]               = rfl::Generic( 2026 );
                           block["Month"]              = rfl::Generic( 3 );
                           block["Day"]                = rfl::Generic( 20 );
                           ++report.Clocks;
                           return true;
                       } );
        return report;
    }

    SunRotationReport MigrateSunRotationV58ToV61( std::vector<Assets::EntityData>& entities )
    {
        // The old engine's gate (Rules::kSunDirectionEpsilon before v61): below it a Translation was no
        // direction and the light emitted nothing.
        constexpr float kOldDirectionEpsilon = 1e-4f;

        std::unordered_map<Common::UUID, const Assets::EntityData*> byId;
        for ( const auto& entity : entities )
            if ( entity.id.has_value() )
                byId.emplace( *entity.id, &entity );

        // The world rotation of a record as the file states it: its own Rotation under its parents'. Bounded
        // by the record count, so a parent cycle in a damaged file ends instead of spinning.
        const auto worldRotation = [&]( std::optional<Common::UUID> id )
        {
            glm::quat world = glm::quat( 1.0f, glm::vec3( 0.0f ) );
            for ( std::size_t depth = 0; id.has_value() && depth <= entities.size(); ++depth )
            {
                const auto found = byId.find( *id );
                if ( found == byId.end() )
                    break;
                world = glm::quat( found->second->Rotation.value_or( glm::vec3( 0.0f ) ) ) * world;
                id    = found->second->parent;
            }
            return world;
        };

        const auto isLight = []( Common::Json::KeyedValues& components )
        { return components.get( "DirectionLight" ).has_value(); };

        SunRotationReport report;
        for ( auto& entity : entities )
        {
            if ( isLight( entity.Components ) )
            {
                const glm::vec3 travel = entity.Translation.value_or( glm::vec3( 0.0f ) );
                if ( glm::length( travel ) > kOldDirectionEpsilon )
                {
                    const glm::quat parent = worldRotation( entity.parent );
                    entity.Rotation =
                         glm::eulerAngles( glm::inverse( parent ) * ECS::Rules::RotationForTravel( travel ) );
                    entity.Translation = glm::vec3( 0.0f );
                    ++report.Aimed;
                }
                else
                {
                    ++report.Undirected;
                }
            }

            if ( !entity.PrefabOverrides )
                continue;
            for ( auto& override_ : *entity.PrefabOverrides )
            {
                if ( !isLight( override_.Components ) || !override_.Translation.has_value() ||
                     glm::length( *override_.Translation ) <= kOldDirectionEpsilon )
                    continue;
                override_.Rotation = glm::eulerAngles( ECS::Rules::RotationForTravel( *override_.Translation ) );
                override_.Translation = glm::vec3( 0.0f );
                ++report.Overrides;
            }
        }
        return report;
    }

    SkySunFromLightReport MigrateSkySunFromLightV56ToV57( std::vector<Assets::EntityData>& entities )
    {
        SkySunFromLightReport report;

        // What the v56 types defaulted to: a record states every key, so these only answer for a key it lacks.
        const ECS::SkyAtmosphereData    kOldSky;
        const ECS::DirectionalLightData kOldLight;
        constexpr double                kOldExposure = 1.0;
        constexpr double                kOldKey      = 0.18;
        static_assert( static_cast<int>( ECS::SkyModel::PhysicalAtmosphere ) == 1,
                       "SkyModel moved: the migration reads PhysicalAtmosphere as 1" );

        const auto number = []( const rfl::Generic& g ) -> std::optional<double>
        {
            if ( const auto d = g.to_double(); d.has_value() )
                return d.value();
            if ( const auto i = g.to_int64(); i.has_value() )
                return static_cast<double>( i.value() );
            return std::nullopt;
        };
        // A stated vec3 (a three-number array), or @p fallback when the key is not stated; nullopt when it is
        // stated in another shape.
        const auto vec3 = [&]( const rfl::Generic::Object& block, const char* key,
                               const glm::vec3& fallback ) -> std::optional<glm::dvec3>
        {
            const auto stated = block.get( key );
            if ( !stated.has_value() )
                return glm::dvec3( fallback );
            const auto array = stated.value().to_array();
            if ( !array.has_value() || array.value().size() != 3 )
                return std::nullopt;
            glm::dvec3 v;
            for ( int i = 0; i < 3; ++i )
            {
                const auto n = number( array.value()[static_cast<std::size_t>( i )] );
                if ( !n.has_value() )
                    return std::nullopt;
                v[i] = n.value();
            }
            return v;
        };
        const auto scalar = [&]( const rfl::Generic::Object& block, const char* key,
                                 double fallback ) -> std::optional<double>
        {
            const auto stated = block.get( key );
            return stated.has_value() ? number( stated.value() ) : std::optional<double>( fallback );
        };
        const auto flag = []( const rfl::Generic::Object& block, const char* key, bool fallback )
        {
            const auto stated = block.get( key );
            return stated.has_value() ? stated.value().to_bool().value_or( fallback ) : fallback;
        };
        const auto put3 = []( rfl::Generic::Object& block, const char* key, const glm::dvec3& v )
        {
            DropKey( block, key );
            block[key] = rfl::Generic(
                 rfl::Generic::Array{ rfl::Generic( v.x ), rfl::Generic( v.y ), rfl::Generic( v.z ) } );
        };
        const auto object = []( rfl::ExtraFields<rfl::Generic>& components,
                                const char*                     component ) -> std::optional<rfl::Generic::Object>
        {
            const auto payload = components.get( component );
            if ( !payload.has_value() )
                return std::nullopt;
            auto fields = payload.value().to_object();
            if ( !fields.has_value() )
                return std::nullopt;
            return std::move( fields.value() );
        };

        // ---- Manual exposure: multiplier -> EV100 at the volume's key. ----
        const auto exposure =
             [&]( rfl::ExtraFields<rfl::Generic>& components, bool record, const std::string& who )
        {
            EditBlock(
                 components, "PostProcessVolume",
                 [&]( rfl::Generic::Object& block )
                 {
                     const auto stated = block.get( "Settings" );
                     if ( !stated.has_value() )
                         return false;
                     auto fields = stated.value().to_object();
                     if ( !fields.has_value() )
                     {
                         report.Refused.push_back(
                              std::format( "{}: PostProcessVolume.Settings {} is not an object", who,
                                           Describe( stated.value() ) ) );
                         return false;
                     }
                     rfl::Generic::Object settings = std::move( fields.value() );
                     if ( settings.get( "ManualExposureEV100" ).has_value() )
                         return false;
                     const bool statesExposure = settings.get( "Exposure" ).has_value();
                     if ( !record && !statesExposure )
                         return false; // a delta that does not state it inherits its prefab's
                     const auto m   = scalar( settings, "Exposure", kOldExposure );
                     const auto key = scalar( settings, "AutoExposureKey", kOldKey );
                     if ( !m.has_value() || !( m.value() > 0.0 ) || !key.has_value() || !( key.value() > 0.0 ) )
                     {
                         report.Refused.push_back( std::format(
                              "{}: PostProcessVolume.Settings Exposure / AutoExposureKey is not a positive "
                              "number",
                              who ) );
                         return false;
                     }
                     DropKey( settings, "Exposure" );
                     settings["ManualExposureEV100"] = rfl::Generic( std::log2( key.value() / m.value() ) + 3.0 );
                     DropKey( block, "Settings" );
                     block["Settings"] = rfl::Generic( std::move( settings ) );
                     ++report.Volumes;
                     return true;
                 } );
        };

        for ( auto& entity : entities )
        {
            const std::string who = entity.id ? std::format( "entity {}", entity.id->ToString() )
                                              : std::string( "<record without id>" );
            exposure( entity.Components, true, who );
            if ( entity.PrefabOverrides )
                for ( auto& override_ : *entity.PrefabOverrides )
                    exposure( override_.Components, false, who );
        }

        // ---- The physical sky's sun: fold the sky's own sun into UE's factors against the atmosphere light. ----
        Assets::EntityData* skyEntity = nullptr;
        for ( auto& entity : entities )
        {
            const auto sky = object( entity.Components, "SkyAtmosphere" );
            if ( !sky.has_value() )
                continue;
            const auto model =
                 scalar( sky.value(), "Model", static_cast<double>( static_cast<int>( kOldSky.Model ) ) );
            if ( model.has_value() && static_cast<int>( model.value() ) == 1 &&
                 flag( sky.value(), "Enabled", kOldSky.Enabled ) )
            {
                if ( skyEntity != nullptr )
                {
                    report.Refused.push_back( "two enabled Physical Atmosphere skies: the engine draws one" );
                    return report;
                }
                skyEntity = &entity;
            }
        }
        if ( skyEntity == nullptr )
            return report;

        Assets::EntityData* lightEntity = nullptr;
        for ( auto& entity : entities )
        {
            const auto light = object( entity.Components, "DirectionLight" );
            if ( light.has_value() && flag( light.value(), "AtmosphereSunLight", kOldLight.AtmosphereSunLight ) )
            {
                lightEntity = &entity; // the engine renders exactly one directional light
                break;
            }
        }

        rfl::Generic::Object sky          = object( skyEntity->Components, "SkyAtmosphere" ).value();
        const auto           sunColor     = vec3( sky, "SunColor", kOldSky.SunColor );
        const auto           sunIntensity = scalar( sky, "SunIntensity", kOldSky.SunIntensity );
        const auto           apFactor =
             vec3( sky, "SkyAndAerialPerspectiveLuminanceFactor", kOldSky.SkyAndAerialPerspectiveLuminanceFactor );
        if ( !sunColor.has_value() || !sunIntensity.has_value() || !apFactor.has_value() )
        {
            report.Refused.push_back(
                 "SkyAtmosphere SunColor / SunIntensity / SkyAndAerialPerspectiveLuminanceFactor "
                 "is not the v56 shape" );
            return report;
        }
        const glm::dvec3 skySun = sunColor.value() * sunIntensity.value();
        if ( lightEntity == nullptr )
        {
            if ( glm::all( glm::equal( skySun, glm::dvec3( 0.0 ) ) ) )
                return report;
            report.Refused.push_back( "a Physical Atmosphere sky with no atmosphere sun light: from v57 the sky "
                                      "scatters the light's illuminance, and there is none to keep this picture" );
            return report;
        }
        rfl::Generic::Object light      = object( lightEntity->Components, "DirectionLight" ).value();
        const auto           lightColor = vec3( light, "Color", kOldLight.Color );
        const auto           lightLux   = scalar( light, "Intensity", kOldLight.Intensity );
        const auto           diskScale  = vec3( light, "AtmosphereSunDiskColorScale", glm::vec3( 1.0f ) );
        if ( !lightColor.has_value() || !lightLux.has_value() || !diskScale.has_value() )
        {
            report.Refused.push_back( "DirectionLight Color / Intensity is not the v56 shape" );
            return report;
        }
        const glm::dvec3 lightSun = lightColor.value() * lightLux.value();
        glm::dvec3       ratio( 1.0 );
        for ( int i = 0; i < 3; ++i )
        {
            if ( lightSun[i] > 0.0 )
                ratio[i] = skySun[i] / lightSun[i];
            else if ( skySun[i] > 0.0 )
            {
                report.Refused.push_back( std::format(
                     "the atmosphere sun light's illuminance is 0 in channel {} under a sky sun of {}: no factor "
                     "keeps that picture",
                     i, skySun[i] ) );
                return report;
            }
        }
        if ( glm::all( glm::lessThan( glm::abs( ratio - glm::dvec3( 1.0 ) ), glm::dvec3( 1e-6 ) ) ) )
            return report; // the sky's sun already was the light's
        put3( sky, "SkyAndAerialPerspectiveLuminanceFactor", apFactor.value() * ratio );
        skyEntity->Components["SkyAtmosphere"] = rfl::Generic( std::move( sky ) );
        put3( light, "AtmosphereSunDiskColorScale", diskScale.value() * ratio );
        lightEntity->Components["DirectionLight"] = rfl::Generic( std::move( light ) );
        ++report.Skies;
        return report;
    }

    namespace
    {
        // A literal default a script declares: what `Name = Property.<Kind>( <literal>` states.
        struct DeclaredDefault
        {
            Scripting::PropertyType Type   = Scripting::PropertyType::Number;
            double                  Number = 0.0;
            bool                    Bool   = false;
            std::string             Str;
        };
        struct ScriptDefaults
        {
            std::map<std::string, DeclaredDefault> Literal; // declarations with a literal default
            std::map<std::string, std::string>     Other;   // declarations without one, as their constructor
        };

        // The `Properties` table of @p source, comments removed. The ONE declaration form (Property.<Kind>(...),
        // LuauProperties::Settle refuses anything else) is read for its first argument: a number, a bool or a
        // quoted string with no escapes. Any other first argument - an expression, a Vector, an Enum list - is
        // `Other`, and the migration keeps that value rather than evaluate the script.
        std::optional<ScriptDefaults> ReadScriptDefaults( const std::string& source )
        {
            std::string code;
            code.reserve( source.size() );
            char quote = 0;
            for ( std::size_t i = 0; i < source.size(); ++i )
            {
                const char c = source[i];
                if ( quote == 0 && c == '-' && i + 1 < source.size() && source[i + 1] == '-' )
                {
                    while ( i < source.size() && source[i] != '\n' )
                        ++i;
                    code += '\n';
                    continue;
                }
                if ( quote == 0 && ( c == '"' || c == '\'' ) )
                    quote = c;
                else if ( quote != 0 && c == quote )
                    quote = 0;
                code += c;
            }
            static const std::regex kTable( R"((^|\n)Properties\s*=\s*\{)" );
            std::smatch             table;
            if ( !std::regex_search( code, table, kTable ) )
                return std::nullopt;
            // The table's own closing brace: the depth returns to zero.
            std::size_t begin = static_cast<std::size_t>( table.position( 0 ) + table.length( 0 ) );
            std::size_t end   = begin;
            for ( int depth = 1; end < code.size() && depth > 0; ++end )
                depth += code[end] == '{' ? 1 : code[end] == '}' ? -1 : 0;
            const std::string body = code.substr( begin, end - begin );

            static const std::regex kEntry( R"((\w+)\s*=\s*Property\.(\w+)\s*\(\s*)" );
            static const std::regex kNumber( R"(^-?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?\s*[,)])" );
            static const std::regex kBool( R"(^(true|false)\s*[,)])" );
            static const std::regex kString( R"re(^"([^"\\]*)"\s*[,)])re" );
            ScriptDefaults          defaults;
            for ( auto it = std::sregex_iterator( body.begin(), body.end(), kEntry ); it != std::sregex_iterator();
                  ++it )
            {
                const std::string name = ( *it )[1];
                const std::string kind = ( *it )[2];
                const std::string rest =
                     body.substr( static_cast<std::size_t>( it->position( 0 ) + it->length( 0 ) ) );
                std::smatch     literal;
                DeclaredDefault value;
                if ( ( kind == "Float" || kind == "Int" ) && std::regex_search( rest, literal, kNumber ) )
                {
                    value.Type   = kind == "Int" ? Scripting::PropertyType::Int : Scripting::PropertyType::Number;
                    value.Number = std::stod( literal[0].str().substr( 0, literal[0].length() - 1 ) );
                }
                else if ( kind == "Bool" && std::regex_search( rest, literal, kBool ) )
                {
                    value.Type = Scripting::PropertyType::Bool;
                    value.Bool = literal[1] == "true";
                }
                else if ( kind == "String" && std::regex_search( rest, literal, kString ) )
                {
                    value.Type = Scripting::PropertyType::String;
                    value.Str  = literal[1];
                }
                else
                {
                    defaults.Other[name] = "Property." + kind;
                    continue;
                }
                defaults.Literal[name] = value;
            }
            return defaults;
        }
    } // namespace

    ScriptDefaultsReport MigrateScriptDefaultsV53ToV54( std::vector<Assets::EntityData>& entities,
                                                        const std::filesystem::path&     assetsRoot )
    {
        using Scripting::PropertyType;
        ScriptDefaultsReport                                 report;
        std::map<std::string, std::optional<ScriptDefaults>> read; // by script key, read once
        const auto defaultsOf = [&]( const std::string& key ) -> const std::optional<ScriptDefaults>&
        {
            auto found = read.find( key );
            if ( found != read.end() )
                return found->second;
            constexpr std::string_view    kMount = "assets:";
            std::optional<ScriptDefaults> defaults;
            if ( key.starts_with( kMount ) )
            {
                const std::string     relative = key.substr( kMount.size() );
                std::filesystem::path file     = ( assetsRoot / relative ).lexically_normal();
                if ( const auto located = LocateMeshFile( relative, assetsRoot ); located.IsSuccess() )
                    file = located.GetValue().File;
                if ( std::ifstream in( file ); in )
                {
                    std::stringstream text;
                    text << in.rdbuf();
                    defaults = ReadScriptDefaults( text.str() );
                }
            }
            return read.emplace( key, std::move( defaults ) ).first->second;
        };

        const auto restate = [&]( rfl::ExtraFields<rfl::Generic>& components, const std::string& who )
        {
            EditBlock(
                 components, "Script",
                 [&]( rfl::Generic::Object& block )
                 {
                     const auto stated = block.get( "Scripts" );
                     if ( !stated.has_value() )
                         return false;
                     auto slots = stated.value().to_array();
                     if ( !slots.has_value() )
                     {
                         report.Refused.push_back( std::format( "{}: Script.Scripts is not an array", who ) );
                         return false;
                     }
                     rfl::Generic::Array raised;
                     bool                changed = false;
                     for ( const rfl::Generic& slotValue : slots.value() )
                     {
                         auto slotFields = slotValue.to_object();
                         if ( !slotFields.has_value() )
                         {
                             report.Refused.push_back( std::format( "{}: a Script slot is not an object", who ) );
                             return false;
                         }
                         rfl::Generic::Object slot  = std::move( slotFields.value() );
                         const auto           key   = slot.get( "ScriptKey" );
                         const auto           props = slot.get( "Props" );
                         const std::string    script =
                              key.has_value() ? key.value().to_string().value_or( std::string() ) : std::string();
                         // A local: a range-for over `props.value().to_array().value()` walks a destroyed
                         // temporary.
                         rfl::Generic::Array statedProps;
                         if ( props.has_value() )
                             if ( auto array = props.value().to_array(); array.has_value() )
                                 statedProps = std::move( array.value() );
                         if ( statedProps.empty() )
                         {
                             raised.push_back( rfl::Generic( std::move( slot ) ) );
                             continue;
                         }
                         const std::optional<ScriptDefaults>& defaults = defaultsOf( script );
                         rfl::Generic::Array                  kept;
                         for ( const rfl::Generic& propValue : statedProps )
                         {
                             auto propFields = propValue.to_object();
                             if ( !propFields.has_value() )
                             {
                                 report.Refused.push_back(
                                      std::format( "{}: a property of {} is not an object", who, script ) );
                                 return false;
                             }
                             rfl::Generic::Object prop      = std::move( propFields.value() );
                             const auto           nameValue = prop.get( "Name" );
                             const auto           typeValue = prop.get( "Type" );
                             const std::string    name =
                                  nameValue.has_value() ? nameValue.value().to_string().value_or( "" ) : "";
                             const int type =
                                  typeValue.has_value() ? typeValue.value().to_int().value_or( -1 ) : -1;
                             const auto keep = [&]( const std::string& why )
                             {
                                 report.Kept.push_back( std::format( "{} > {}.{}: {}", who, script, name, why ) );
                                 kept.push_back( rfl::Generic( prop ) );
                             };
                             if ( !defaults.has_value() )
                             {
                                 keep( "the script is not found or declares no Properties table" );
                                 continue;
                             }
                             const auto literal = defaults->Literal.find( name );
                             if ( literal == defaults->Literal.end() )
                             {
                                 const auto other = defaults->Other.find( name );
                                 keep( other != defaults->Other.end()
                                            ? std::format( "{} states no literal default", other->second )
                                            : std::string( "the script no longer declares it" ) );
                                 continue;
                             }
                             const DeclaredDefault& declared = literal->second;
                             const auto             number   = [&]( const char* field )
                             {
                                 const auto v = prop.get( field );
                                 return v.has_value() ? v.value().to_double().value_or( 0.0 ) : 0.0;
                             };
                             bool equal = false;
                             switch ( declared.Type )
                             {
                                 case PropertyType::Number:
                                 case PropertyType::Int:
                                     if ( type != static_cast<int>( PropertyType::Number ) &&
                                          type != static_cast<int>( PropertyType::Int ) )
                                     {
                                         keep( "stated as another kind than the script declares" );
                                         continue;
                                     }
                                     equal = number( "Number" ) == declared.Number;
                                     if ( !equal && declared.Type == PropertyType::Int &&
                                          type == static_cast<int>( PropertyType::Number ) )
                                     {
                                         prop["Type"] = rfl::Generic( static_cast<int>( PropertyType::Int ) );
                                         ++report.Retyped;
                                         changed = true;
                                     }
                                     break;
                                 case PropertyType::Bool:
                                 {
                                     const auto v = prop.get( "Bool" );
                                     equal = type == static_cast<int>( PropertyType::Bool ) && v.has_value() &&
                                             v.value().to_bool().value_or( !declared.Bool ) == declared.Bool;
                                     break;
                                 }
                                 default:
                                 {
                                     const auto v = prop.get( "Str" );
                                     equal = type == static_cast<int>( PropertyType::String ) && v.has_value() &&
                                             v.value().to_string().value_or( "" ) == declared.Str;
                                     break;
                                 }
                             }
                             if ( equal )
                             {
                                 ++report.Dropped;
                                 changed = true;
                                 continue;
                             }
                             kept.push_back( rfl::Generic( std::move( prop ) ) );
                         }
                         DropKey( slot, "Props" );
                         slot["Props"] = rfl::Generic( std::move( kept ) );
                         raised.push_back( rfl::Generic( std::move( slot ) ) );
                     }
                     if ( !changed )
                         return false;
                     DropKey( block, "Scripts" );
                     block["Scripts"] = rfl::Generic( std::move( raised ) );
                     return true;
                 } );
        };

        for ( auto& entity : entities )
        {
            const std::string who = entity.id ? std::format( "entity {}", entity.id->ToString() )
                                              : std::string( "<record without id>" );
            restate( entity.Components, who );
            if ( entity.PrefabOverrides )
                for ( auto& override_ : *entity.PrefabOverrides )
                    restate( override_.Components, who );
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

    std::size_t MigrateShadowBiasCentimetresV48ToV49( std::vector<Assets::EntityData>& entities )
    {
        std::size_t restated = 0;
        const auto  restate  = [&]( rfl::ExtraFields<rfl::Generic>& components )
        {
            const auto payload = components.get( "DirectionLight" );
            if ( !payload.has_value() )
                return;
            auto fields = payload.value().to_object();
            if ( !fields.has_value() || !fields.value().get( "ShadowBias" ).has_value() )
                return;
            fields.value()["ShadowBias"] = rfl::Generic( static_cast<double>( kShadowBiasCentimetresDefault ) );
            components["DirectionLight"] = rfl::Generic( std::move( fields.value() ) );
            ++restated;
        };
        for ( auto& entity : entities )
        {
            restate( entity.Components );
            if ( !entity.PrefabOverrides )
                continue;
            for ( auto& override_ : *entity.PrefabOverrides )
                restate( override_.Components );
        }
        return restated;
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

    namespace
    {
        Common::BoolResultStr ReadEmitterVec3( const rfl::Generic::Object& fields, const char* key,
                                               glm::vec3& out )
        {
            const auto value = fields.get( key );
            if ( !value.has_value() )
                return BOOLSUCCESS;
            const auto items = value.value().to_array();
            if ( !items.has_value() || items.value().size() != 3 )
                return Common::MakeFormattedError<bool>( "{} is {}, not three numbers", key,
                                                         Describe( value.value() ) );
            for ( int c = 0; c < 3; ++c )
            {
                const auto& item = items.value()[static_cast<std::size_t>( c )];
                if ( const auto real = item.to_double(); real.has_value() )
                    out[c] = static_cast<float>( real.value() );
                else if ( const auto whole = item.to_int(); whole.has_value() )
                    out[c] = static_cast<float>( whole.value() );
                else
                    return Common::MakeFormattedError<bool>( "{}[{}] is {}, not a number", key, c,
                                                             Describe( item ) );
            }
            return BOOLSUCCESS;
        }

        namespace S = Assets::Serialization;

        S::VFXModuleInput ValueInput( const char* name, S::VFXValueType type, const glm::vec4& value )
        {
            S::VFXModuleInput in;
            in.Name   = name;
            in.Type   = type;
            in.Source = S::VFXInputSource::Value;
            in.Value  = value;
            return in;
        }

        S::VFXModuleInput RandomInput( const char* name, float min, float max )
        {
            S::VFXModuleInput in;
            in.Name   = name;
            in.Type   = S::VFXValueType::Float;
            in.Source = S::VFXInputSource::Random;
            in.Random =
                 S::VFXRandomRange{ glm::vec4( min, 0.0f, 0.0f, 0.0f ), glm::vec4( max, 0.0f, 0.0f, 0.0f ) };
            return in;
        }

        // One channel per component, Linear keys at the given times.
        S::VFXModuleInput CurveInput( const char* name, S::VFXValueType type,
                                      const std::vector<std::vector<S::VFXCurveKey>>& channels )
        {
            S::VFXModuleInput in;
            in.Name   = name;
            in.Type   = type;
            in.Source = S::VFXInputSource::Curve;
            in.Curve  = channels;
            return in;
        }

        S::VFXCurveKey LinearKey( float time, float value )
        {
            S::VFXCurveKey key;
            key.Time   = time;
            key.Value  = value;
            key.Interp = Animation::KeyInterp::Linear;
            return key;
        }

        S::VFXModuleUse Use( const char* module, std::vector<S::VFXModuleInput> inputs )
        {
            S::VFXModuleUse use;
            use.Module = std::string( S::kVFXEnginePrefix ) + module;
            use.Inputs = std::move( inputs );
            return use;
        }
    } // namespace

    Common::ResultStr<ParticleEmitterV43> ReadParticleEmitterV43( const rfl::Generic::Object& f )
    {
        ParticleEmitterV43 e;
        float              maxParticles = static_cast<float>( e.MaxParticles );
        for ( const auto& check :
              { ReadInlineBool( f, "Enabled", e.Enabled ), ReadInlineFloat( f, "MaxParticles", maxParticles ),
                ReadInlineFloat( f, "SpawnRate", e.SpawnRate ), ReadInlineBool( f, "Looping", e.Looping ),
                ReadInlineBool( f, "WorldSpace", e.WorldSpace ), ReadInlineFloat( f, "Lifetime", e.Lifetime ),
                ReadInlineFloat( f, "LifetimeVariance", e.LifetimeVariance ),
                ReadInlineFloat( f, "StartSpeed", e.StartSpeed ),
                ReadInlineFloat( f, "SpeedVariance", e.SpeedVariance ),
                ReadEmitterVec3( f, "Direction", e.Direction ), ReadInlineFloat( f, "ConeAngle", e.ConeAngle ),
                ReadEmitterVec3( f, "Gravity", e.Gravity ), ReadInlineFloat( f, "StartSize", e.StartSize ),
                ReadInlineFloat( f, "SizeCurvePower", e.SizeCurvePower ),
                ReadInlineFloat( f, "EndSize", e.EndSize ), ReadEmitterVec3( f, "StartColor", e.StartColor ),
                ReadEmitterVec3( f, "EndColor", e.EndColor ), ReadInlineFloat( f, "StartAlpha", e.StartAlpha ),
                ReadInlineFloat( f, "EndAlpha", e.EndAlpha ) } )
            if ( !check )
                return Common::MakeFormattedError<ParticleEmitterV43>( "ParticleEmitter.{}", check.GetError() );
        e.MaxParticles = static_cast<int>( maxParticles );
        if ( const auto material = f.get( "Material" ); material.has_value() )
            if ( const auto ref = material.value().to_object(); ref.has_value() )
            {
                if ( const auto guid = ref.value().get( "Guid" ); guid.has_value() )
                    e.Material.Guid = guid.value().to_string().value_or( "" );
                if ( const auto path = ref.value().get( "Path" ); path.has_value() )
                    e.Material.Path = path.value().to_string().value_or( "" );
            }
        return Common::MakeSuccess( std::move( e ) );
    }

    Assets::Serialization::VFXSystemData VFXSystemFromParticleEmitter( const ParticleEmitterV43& d )
    {
        using T = S::VFXValueType;

        // The old simulate shader: speed = StartSpeed * (1 - SpeedVariance * r), life = Lifetime * (1 -
        // LifetimeVariance * r), r uniform in [0, 1) - a uniform range ending at the stated number.
        const float lifeMax  = std::max( d.Lifetime, 0.0f );
        const float lifeMin  = lifeMax * ( 1.0f - std::clamp( d.LifetimeVariance, 0.0f, 1.0f ) );
        const float speedMax = d.StartSpeed;
        const float speedMin = speedMax * ( 1.0f - std::clamp( d.SpeedVariance, 0.0f, 1.0f ) );
        glm::vec3   axis     = glm::dot( d.Direction, d.Direction ) < 1e-6f ? glm::vec3( 0.0f, 1.0f, 0.0f )
                                                                            : glm::normalize( d.Direction );

        S::VFXEmitterData emitter;
        emitter.Name     = "Emitter";
        emitter.Enabled  = d.Enabled;
        emitter.Space    = d.WorldSpace ? S::VFXSimulationSpace::World : S::VFXSimulationSpace::Local;
        emitter.Capacity = static_cast<uint32_t>( std::max( d.MaxParticles, 1 ) );
        // The component ran forever at its rate; a non-looping one spawned nothing (VFXWorld v43).
        emitter.Lifecycle = S::VFXEmitterLifecycle{ 0.0f, 1.0f, S::VFXLoopBehavior::Infinite, 1 };
        emitter.Stack.EmitterUpdate.push_back(
             Use( "SpawnRate",
                  { ValueInput( "SpawnRate", T::Float,
                                glm::vec4( d.Looping ? std::max( d.SpawnRate, 0.0f ) : 0.0f, 0, 0, 0 ) ) } ) );

        // Size and colour: the initial value is 1 and the over-life Scale curve carries Start -> End, so a zero
        // start channel still reaches its end (a Start x ratio scale could not).
        emitter.Stack.ParticleSpawn = {
             Use( "ShapePoint", { ValueInput( "Offset", T::Vec3, glm::vec4( 0.0f ) ) } ),
             Use( "InitializeLifetime", { RandomInput( "Lifetime", lifeMin, lifeMax ) } ),
             Use( "AddVelocityInCone", { RandomInput( "Speed", speedMin, speedMax ),
                                         ValueInput( "Angle", T::Float, glm::vec4( d.ConeAngle, 0, 0, 0 ) ),
                                         ValueInput( "Falloff", T::Float, glm::vec4( 0.0f ) ),
                                         ValueInput( "Axis", T::Vec3, glm::vec4( axis, 0.0f ) ) } ),
             Use( "InitializeSpriteSize", { ValueInput( "Size", T::Vec2, glm::vec4( 1.0f, 1.0f, 0.0f, 0.0f ) ) } ),
             Use( "InitializeColor", { ValueInput( "Color", T::Vec4, glm::vec4( 1.0f ) ) } ),
        };

        std::vector<S::VFXCurveKey> size;
        const bool                  linear   = std::abs( d.SizeCurvePower - 1.0f ) < 1e-6f;
        const int                   segments = linear ? 1 : kVFXConvertedSizeSegments;
        const float                 power    = d.SizeCurvePower > 0.0f ? d.SizeCurvePower : 1.0f;
        for ( int k = 0; k <= segments; ++k )
        {
            const float t = static_cast<float>( k ) / static_cast<float>( segments );
            size.push_back( LinearKey( t, d.StartSize + ( d.EndSize - d.StartSize ) * std::pow( t, power ) ) );
        }
        const glm::vec4                          c0( d.StartColor, d.StartAlpha );
        const glm::vec4                          c1( d.EndColor, d.EndAlpha );
        std::vector<std::vector<S::VFXCurveKey>> colour;
        for ( int c = 0; c < 4; ++c )
            colour.push_back( { LinearKey( 0.0f, c0[c] ), LinearKey( 1.0f, c1[c] ) } );

        emitter.Stack.ParticleUpdate = {
             Use( "UpdateAge", {} ),
             Use( "Gravity", { ValueInput( "Gravity", T::Vec3, glm::vec4( d.Gravity, 0.0f ) ) } ),
             Use( "SolveForcesAndVelocity", { ValueInput( "SpeedLimit", T::Float, glm::vec4( 0.0f ) ) } ),
             Use( "SizeOverLife", { CurveInput( "Scale", T::Vec2, { size, size } ) } ),
             Use( "ColorOverLife", { CurveInput( "Scale", T::Vec4, colour ) } ),
        };
        emitter.Renderers.push_back( S::VFXRendererData{ S::VFXRendererKind::Sprite, true } );

        S::VFXSystemData system;
        system.Category = kVFXConvertedCategory;
        system.Duration = 1.0f;
        system.Loop     = true;
        system.Seed     = 0;
        // Fixed bounds that hold every particle the old emitter could make: the farthest a particle travels in
        // its longest life at the top speed under gravity, plus the larger sprite.
        const float reach = std::abs( speedMax ) * lifeMax + 0.5f * glm::length( d.Gravity ) * lifeMax * lifeMax +
                            std::max( std::abs( d.StartSize ), std::abs( d.EndSize ) );
        system.Bounds = S::VFXBounds{ glm::vec3( -reach ), glm::vec3( reach ) };
        system.Emitters.push_back( std::move( emitter ) );
        return system;
    }

    ParticleEmittersToVFXReport MigrateParticleEmittersToVFX( std::vector<Assets::EntityData>& entities,
                                                              const std::string&               ownerName,
                                                              const std::filesystem::path&     assetsRoot )
    {
        ParticleEmittersToVFXReport        report;
        std::map<std::string, std::string> minted; // canonical body -> the relative path minted for it

        for ( auto& entity : entities )
        {
            const std::string who = entity.id ? entity.id->ToString() : std::string( "<record without id>" );
            if ( entity.PrefabOverrides )
                for ( const auto& override_ : *entity.PrefabOverrides )
                    if ( override_.Components.get( "ParticleEmitter" ).has_value() )
                        report.Refused.push_back(
                             std::format( "{}: a prefab override states ParticleEmitter keys; "
                                          "raise the prefab first and re-state the override "
                                          "on its VFX block",
                                          who ) );

            const auto block = entity.Components.get( "ParticleEmitter" );
            if ( !block.has_value() )
                continue;
            const auto fields = block.value().to_object();
            if ( !fields.has_value() )
            {
                report.Refused.push_back(
                     std::format( "{}: ParticleEmitter is {}, not an object", who, Describe( block.value() ) ) );
                continue;
            }
            const auto read = ReadParticleEmitterV43( fields.value() );
            if ( !read )
            {
                report.Refused.push_back( std::format( "{}: {}", who, read.GetError() ) );
                continue;
            }

            S::VFXSystemData  system = VFXSystemFromParticleEmitter( read.GetValue() );
            const std::string body   = S::WriteVFXSystem( system ); // header minted fresh: compared without it
            const std::string key    = body.substr( body.find( "\"Category\"" ) );

            std::string relative;
            if ( const auto it = minted.find( key ); it != minted.end() )
            {
                relative = it->second;
                ++report.Shared;
            }
            else
            {
                relative =
                     std::format( "VFX/{}_{}{}", SafeStem( ownerName ), SafeStem( who ), S::kVFXSystemExtension );
                Common::Content::TextAssetHeaderSerialized header;
                header.Guid   = Common::Content::AssetGuidToText( MigrationGuidForPath( relative ) );
                system.Header = header;
                report.NewSystems.emplace_back( ( assetsRoot / relative ).lexically_normal(),
                                                S::WriteVFXSystem( system ) );
                minted.emplace( key, relative );
            }

            // An asset slot states its file as a path under the assets root (the reader's form for AssetHandle).
            rfl::Generic::Object vfx;
            vfx["System"]       = rfl::Generic( relative );
            vfx["AutoActivate"] = rfl::Generic( true );

            Common::Json::KeyedValues kept;
            for ( const auto& [name, value] : entity.Components )
                if ( name != "ParticleEmitter" )
                    kept[name] = value;
            kept["VFX"]       = rfl::Generic( std::move( vfx ) );
            entity.Components = std::move( kept );
            ++report.Emitters;
        }
        return report;
    }

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
        // SKEL 1-4 as they were written: SKEL 1 lacks PreviewMesh / CompatibleSkeletons, 1 and 2 carry Import,
        // only 4 states Sockets / BoneMasks.
        struct SkeletonAssetDataV1V2
        {
            std::optional<Common::Content::TextAssetHeaderSerialized> Header;
            uint64_t                                                  Signature = 0;
            std::vector<Desert::Animation::BoneInfo>                  Bones;
            std::optional<SkeletonImportInfoV2>                       Import;
            std::optional<Assets::AssetGuidRef>                       PreviewMesh;
            std::optional<std::vector<Assets::AssetGuidRef>>          CompatibleSkeletons;
            std::optional<std::vector<Animation::SkeletonSocket>>     Sockets;
            std::optional<std::vector<Animation::BoneMask>>           BoneMasks;
        };

        // The rig as ANY generation this tool raises states it (SKEL 1, 2, 3 or the current one), with the
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

    Common::ResultStr<std::string> MigrateSkeletonToV4( const std::string& text )
    {
        const auto any = ReadAnySkeleton( text );
        if ( !any )
            return Common::MakeError<std::string>( any.GetError() );
        const auto& [old, header, version] = any.GetValue();
        if ( version < 1u || version > 3u )
            return Common::MakeFormattedError<std::string>(
                 "the header states SKEL {}, and this step raises SKEL 1, 2 and 3 only", version );

        Assets::Serialization::SkeletonAssetData   data;
        Common::Content::TextAssetHeaderSerialized stamped = header;
        stamped.Versions["SKEL"]                           = Assets::kSkeletonSchemaVersion;
        data.Header                                        = std::move( stamped );
        data.Signature                                     = old.Signature;
        data.Bones                                         = old.Bones;
        data.PreviewMesh                                   = old.PreviewMesh;
        data.CompatibleSkeletons = old.CompatibleSkeletons.value_or( std::vector<Assets::AssetGuidRef>{} );
        // SKEL 4: no generation before it had sockets or bone masks - both lists are stated, empty.
        data.Sockets.clear();
        data.BoneMasks.clear();
        std::string written = Common::Json::Write( data );
        // What the step writes, the engine's reader must read.
        if ( auto back = Assets::Serialization::ReadSkeletonJson( written ); !back )
            return Common::MakeFormattedError<std::string>( "the raised file does not read as SKEL {}: {}",
                                                            Assets::kSkeletonSchemaVersion, back.GetError() );
        return Common::MakeSuccess( std::move( written ) );
    }

    namespace AnimGraphLegacy
    {
        namespace G = Animation::Graph;
        // ANGR 2: the current layout without the Output Pose node's position and without comment boxes (the
        // canvases' `Comments`, which the files gained in place and an ANGR 2 file never states).
        struct StateMachineV2
        {
            std::string           Entry;
            std::vector<G::State> States;
        };
        struct PoseNodeV2
        {
            std::string                               Name;
            int                                       Kind = 0;
            std::vector<std::string>                  PoseInputs;
            std::vector<G::ParameterPin>              ParameterInputs;
            std::optional<StateMachineV2>             Machine;
            std::optional<G::LayeredBlendPerBoneNode> LayeredBlend;
            std::optional<G::SequencePlayerNode>      Sequence;
            std::optional<G::LinkedAnimLayerNode>     LinkedLayer;
            float                                     X = 0.0f;
            float                                     Y = 0.0f;
        };
        struct AnimLayerGraphV2
        {
            std::string             Interface;
            std::string             Layer;
            std::vector<PoseNodeV2> Nodes;
            std::string             OutputPose;
        };

        std::vector<G::PoseNode> RaisePoseNodes( const std::vector<PoseNodeV2>& old )
        {
            std::vector<G::PoseNode> raised;
            raised.reserve( old.size() );
            for ( const PoseNodeV2& was : old )
            {
                G::PoseNode node;
                node.Name            = was.Name;
                node.Kind            = was.Kind;
                node.PoseInputs      = was.PoseInputs;
                node.ParameterInputs = was.ParameterInputs;
                if ( was.Machine )
                    node.Machine = G::StateMachine{ was.Machine->Entry, was.Machine->States, {} };
                node.LayeredBlend = was.LayeredBlend;
                node.Sequence     = was.Sequence;
                node.LinkedLayer  = was.LinkedLayer;
                node.X            = was.X;
                node.Y            = was.Y;
                raised.push_back( std::move( node ) );
            }
            return raised;
        }
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
            std::vector<PoseNodeV2>                                   Nodes;
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
        graph.Nodes                    = AnimGraphLegacy::RaisePoseNodes( old.Nodes );
        graph.OutputPose               = old.OutputPose;
        std::tie( graph.OutputPoseX, graph.OutputPoseY ) =
             G::DefaultOutputPosePosition( graph.Nodes, old.OutputPose );
        if ( old.Layers )
        {
            G::AnimGraphLayers layers;
            layers.Interfaces = old.Layers->Interfaces;
            for ( const AnimGraphLegacy::AnimLayerGraphV2& was : old.Layers->Implemented )
            {
                G::AnimLayerGraph layer;
                layer.Interface  = was.Interface;
                layer.Layer      = was.Layer;
                layer.Nodes      = AnimGraphLegacy::RaisePoseNodes( was.Nodes );
                layer.OutputPose = was.OutputPose;
                std::tie( layer.OutputPoseX, layer.OutputPoseY ) =
                     G::DefaultOutputPosePosition( layer.Nodes, was.OutputPose );
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
                return MigrateControlRigToV4( text ); // CRIG 3 is not this build's; the chain ends at its reader
            const auto read = S::ParseRetarget( text );
            return read ? Common::MakeSuccess( S::WriteRetarget( read.GetValue() ) )
                        : Common::MakeError<std::string>( read.GetError() );
        }
    } // namespace TargetSkeletonStep

    Common::ResultStr<std::string> MigrateControlRigToV4( const std::string& text )
    {
        auto read = rfl::json::read<rfl::Generic::Object>( text );
        if ( !read )
            return Common::MakeFormattedError<std::string>( "CRIG 3 body does not read: {}", read.error().what() );
        rfl::Generic::Object document = std::move( read.value() );
        auto                 header   = document.get( "Header" ).value_or( rfl::Generic() ).to_object();
        if ( !header.has_value() )
            return Common::MakeFormattedError<std::string>( "the file states no header" );
        auto versions = header.value().get( "Versions" ).value_or( rfl::Generic() ).to_object();
        if ( !versions.has_value() ||
             versions.value().get( "CRIG" ).value_or( rfl::Generic() ).to_int().value_or( -1 ) != 3 )
            return Common::MakeFormattedError<std::string>(
                 "the header does not state CRIG 3, and this step raises CRIG 3 only" );

        if ( auto controls = document.get( "Controls" ).value_or( rfl::Generic() ).to_array() )
        {
            rfl::Generic::Array raised;
            for ( const rfl::Generic& item : controls.value() )
            {
                auto control = item.to_object();
                if ( !control.has_value() )
                    return Common::MakeFormattedError<std::string>( "a Controls entry is not an object" );
                if ( !control.value().get( "Limits" ).has_value() )
                    control.value()["Limits"] = rfl::Generic( rfl::Generic::Array{} );
                raised.push_back( rfl::Generic( std::move( control.value() ) ) );
            }
            document["Controls"] = rfl::Generic( std::move( raised ) );
        }

        rfl::Generic::Array graphs;
        if ( const auto graph = document.get( "Graph" ); graph.has_value() )
        {
            const auto old = graph.value().to_object();
            if ( !old.has_value() )
                return Common::MakeFormattedError<std::string>( "Graph is not an object" );
            const auto nodes = old.value().get( "Nodes" ).value_or( rfl::Generic() ).to_array();
            if ( !nodes.has_value() )
                return Common::MakeFormattedError<std::string>( "Graph states no Nodes array" );
            rfl::Generic::Array placed;
            for ( size_t i = 0; i < nodes.value().size(); ++i )
            {
                auto node = nodes.value()[i].to_object();
                if ( !node.has_value() )
                    return Common::MakeFormattedError<std::string>( "graph node {} is not an object", i );
                rfl::Generic::Object position;
                position["X"]            = rfl::Generic( 40.0 + 260.0 * double( i % 4 ) );
                position["Y"]            = rfl::Generic( 40.0 + 180.0 * double( i / 4 ) );
                node.value()["Position"] = rfl::Generic( std::move( position ) );
                placed.push_back( rfl::Generic( std::move( node.value() ) ) );
            }
            rfl::Generic::Object forwards;
            forwards["Event"] = rfl::Generic( std::string( "Forwards" ) );
            forwards["Nodes"] = rfl::Generic( std::move( placed ) );
            graphs.push_back( rfl::Generic( std::move( forwards ) ) );
            rfl::Generic::Object without;
            for ( const auto& [key, field] : document )
                if ( key != "Graph" )
                    without[key] = field;
            document = std::move( without );
        }
        document["Graphs"] = rfl::Generic( std::move( graphs ) );

        versions.value()["CRIG"]   = rfl::Generic( static_cast<int>( Assets::kControlRigSchemaVersion ) );
        header.value()["Versions"] = rfl::Generic( std::move( versions.value() ) );
        document["Header"]         = rfl::Generic( std::move( header.value() ) );

        namespace S     = Assets::Serialization;
        const auto back = S::ParseControlRig( rfl::json::write( document ) );
        if ( !back )
            return Common::MakeFormattedError<std::string>( "the raised file does not read as CRIG {}: {}",
                                                            Assets::kControlRigSchemaVersion, back.GetError() );
        return Common::MakeSuccess( S::WriteControlRig( back.GetValue() ) );
    }

    Common::ResultStr<std::string>
    StateTargetSkeleton( const std::string& text, const std::string& tag,
                         const std::vector<TargetSkeletonRig>&               rigs,
                         const std::unordered_map<std::string, std::string>& clipRigs )
    {
        const uint32_t from = tag == "ANGR" ? 3u : tag == "CRIG" ? 2u : 3u;
        const uint32_t to   = tag == "ANGR"   ? Assets::kAnimGraphSchemaVersion
                              : tag == "CRIG" ? 3u
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

    namespace
    {
        // CLTY 5 as it was written: the shape with a silhouette and no altitude density.
        struct CloudVerticalProfileV5
        {
            std::array<float, Graphic::kCloudProfileSamples> HalfWidth;
        };
        struct CloudTypeShapeV5
        {
            float                  BaseAltitudeKm;
            float                  TopAltitudeKm;
            float                  EdgeTopFraction;
            float                  BaseRampFraction;
            CloudVerticalProfileV5 Profile;
            float                  AnvilAltitudeKm;
            float                  AnvilThicknessKm;
            float                  AnvilStrength;
            float                  DetailCharacter;
            float                  DetailFactor;
            float                  DensityFactor;
            float                  ExtinctionFactor;
            float                  PlacementScale;
            float                  PlacementAnisotropy;
        };
        struct CloudTypeDataV5
        {
            std::optional<Common::Content::TextAssetHeaderSerialized> Header;
            std::optional<std::string>                                DisplayName;
            std::optional<std::string>                                Notes;
            std::optional<Assets::AssetGuidRef>                       NoiseVolume;
            CloudTypeShapeV5                                          Shape;
        };
    } // namespace

    Common::ResultStr<std::string> MigrateCloudTypeV5ToV6( const std::string& text )
    {
        const auto v5 = Common::Json::Read<CloudTypeDataV5>( text );
        if ( !v5 )
            return Common::MakeFormattedError<std::string>( "CLTY 5 body does not read: {}", v5.GetError() );
        const CloudTypeDataV5& old = v5.GetValue();
        if ( !old.Header )
            return Common::MakeFormattedError<std::string>(
                 "the file states no header, and this step raises CLTY 5 only" );
        const auto stated = old.Header->Versions.find( "CLTY" );
        if ( stated == old.Header->Versions.end() || stated->second != 5u )
            return Common::MakeFormattedError<std::string>(
                 "the header states CLTY {}, and this step raises CLTY 5 only",
                 stated == old.Header->Versions.end() ? std::string( "nothing" )
                                                      : std::to_string( stated->second ) );

        Assets::CloudTypeData data;
        data.Header      = old.Header;
        data.DisplayName = old.DisplayName;
        data.Notes       = old.Notes;
        data.NoiseVolume = old.NoiseVolume;

        const CloudTypeShapeV5&  from = old.Shape;
        Graphic::CloudTypeShape& to   = data.Shape;
        to.BaseAltitudeKm             = from.BaseAltitudeKm;
        to.TopAltitudeKm              = from.TopAltitudeKm;
        to.EdgeTopFraction            = from.EdgeTopFraction;
        to.BaseRampFraction           = from.BaseRampFraction;
        to.Profile.HalfWidth          = from.Profile.HalfWidth;
        to.Profile.Density            = Graphic::CloudProfileDensityBaseRamp();
        to.AnvilAltitudeKm            = from.AnvilAltitudeKm;
        to.AnvilThicknessKm           = from.AnvilThicknessKm;
        to.AnvilStrength              = from.AnvilStrength;
        to.DetailCharacter            = from.DetailCharacter;
        to.DetailFactor               = from.DetailFactor;
        to.DensityFactor              = from.DensityFactor;
        to.ExtinctionFactor           = from.ExtinctionFactor;
        to.PlacementScale             = from.PlacementScale;
        to.PlacementAnisotropy        = from.PlacementAnisotropy;

        // WriteCloudType stamps the header at the engine's CLTY and keeps the loaded GUID.
        std::string written = Assets::WriteCloudType( data );
        if ( auto reread = Assets::ParseCloudType( written ); !reread )
            return Common::MakeFormattedError<std::string>( "the raised file does not read as CLTY 6: {}",
                                                            reread.GetError() );
        return Common::MakeSuccess( std::move( written ) );
    }

    Common::ResultStr<std::string> MigrateCloudTypeV6ToV7( const std::string& text )
    {
        // CLTY 7 IS CLTY 6 PLUS ONE KEY, so the v6 body is the v7 one once `Shape.BodyDiameterKm` is there:
        // it goes in after `PlacementAnisotropy`, the shape's last v6 key, and the body then reads as the
        // current type — no frozen v6 copy of fifteen fields to keep in step with the struct.
        const std::string key = "\"PlacementAnisotropy\"";
        const size_t      at  = text.find( key );
        if ( at == std::string::npos )
            return Common::MakeFormattedError<std::string>( "CLTY 6 body has no Shape.PlacementAnisotropy" );
        if ( text.find( "\"BodyDiameterKm\"" ) != std::string::npos )
            return Common::MakeFormattedError<std::string>( "the body already states BodyDiameterKm" );
        const size_t end = text.find_first_of( ",}\n", text.find( ':', at ) + 1u );
        if ( end == std::string::npos )
            return Common::MakeFormattedError<std::string>( "CLTY 6 Shape.PlacementAnisotropy has no value end" );

        // ONE KILOMETRE, the default type's (Assets::CloudTypeDefaultShape): a v6 type had no body size, its
        // bodies were its cells; the shipped library states its own after the raise (FIELD-GRAIN).
        std::string patched = text;
        patched.insert( end,
                        ", \"BodyDiameterKm\": " + std::format( "{}", Graphic::CloudTypeShape{}.BodyDiameterKm ) );

        const auto v6 = Common::Json::Read<Assets::CloudTypeData>( patched );
        if ( !v6 )
            return Common::MakeFormattedError<std::string>( "CLTY 6 body does not read: {}", v6.GetError() );
        const Assets::CloudTypeData& data = v6.GetValue();
        if ( !data.Header )
            return Common::MakeFormattedError<std::string>(
                 "the file states no header, and this step raises CLTY 6 only" );
        const auto stated = data.Header->Versions.find( "CLTY" );
        if ( stated == data.Header->Versions.end() || stated->second != 6u )
            return Common::MakeFormattedError<std::string>(
                 "the header states CLTY {}, and this step raises CLTY 6 only",
                 stated == data.Header->Versions.end() ? std::string( "nothing" )
                                                       : std::to_string( stated->second ) );

        std::string written = Assets::WriteCloudType( data );
        if ( auto reread = Assets::ParseCloudType( written ); !reread )
            return Common::MakeFormattedError<std::string>( "the raised file does not read as CLTY 7: {}",
                                                            reread.GetError() );
        return Common::MakeSuccess( std::move( written ) );
    }

    float MigratePatchStrengthV4ToV5( float strength )
    {
        const float s = std::clamp( strength, 0.0f, 1.0f );
        // At s <= 1/2 the v4 ramp never reached zero: no sky was clear, which is v5's zero.
        return s <= 0.5f ? 0.0f : 1.0f - 1.0f / ( 2.0f * s );
    }

    Common::ResultStr<std::string> MigrateMaterialV4ToV5( const std::string& text )
    {
        const auto parsed = Common::Json::Read<Assets::MaterialData>( text );
        if ( !parsed )
            return Common::MakeFormattedError<std::string>( "MATL 4 body does not read: {}", parsed.GetError() );
        Assets::MaterialData data = parsed.GetValue();
        if ( !data.Header )
            return Common::MakeFormattedError<std::string>(
                 "the file states no header, and this step raises MATL 4 only" );
        const auto stated = data.Header->Versions.find( "MATL" );
        if ( stated == data.Header->Versions.end() || stated->second != 4u )
            return Common::MakeFormattedError<std::string>(
                 "the header states MATL {}, and this step raises MATL 4 only",
                 stated == data.Header->Versions.end() ? std::string( "nothing" )
                                                       : std::to_string( stated->second ) );

        // BY NAME, NOT BY SHADER: an instance of the cloud material states no Shader of its own and may still
        // override PatchStrength, and no other schema has a parameter of that name.
        for ( Assets::MaterialShaderParam& param : data.Params )
            if ( param.Name == "PatchStrength" )
                param.Value.x = MigratePatchStrengthV4ToV5( param.Value.x );

        auto written = Assets::WriteMaterialJson( data );
        if ( !written )
            return Common::MakeFormattedError<std::string>( "the raised material does not write: {}",
                                                            written.GetError() );
        if ( auto reread = Assets::ParseMaterialJson( "MATL 4 -> 5", written.GetValue() ); !reread )
            return Common::MakeFormattedError<std::string>( "the raised file does not read as MATL 5: {}",
                                                            reread.GetError() );
        return Common::MakeSuccess( std::string( written.GetValue() ) );
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

            // Collision is a profile (PHYS-A1): every body and character states the profile its v41 kind had.
            if ( statedSceneVersion < kSceneVersionCollisionProfiles )
            {
                report.CollisionProfilesRaised = true;
                report.CollisionProfiles       = MigrateCollisionProfilesV41ToV42( entities );
                if ( !report.CollisionProfiles.Refused.empty() )
                {
                    report.Refused = RefusedWhole( name, report.CollisionProfiles.Refused );
                    return;
                }
            }

            // The clock leaves the sky (TOD-SPLIT): its five keys become a TimeOfDay block on the sky's record.
            if ( statedSceneVersion < kSceneVersionTimeOfDayComponent )
            {
                report.TimeOfDayComponentRaised = true;
                report.TimeOfDayComponent       = MigrateTimeOfDayComponentV43ToV44( entities );
                if ( !report.TimeOfDayComponent.Refused.empty() )
                {
                    report.Refused = RefusedWhole( name, report.TimeOfDayComponent.Refused );
                    return;
                }
            }

            // A sound is a `.desound` asset named by GUID (SOUND-ASSET): before the v1 -> v2 shift, whose reader
            // refuses an Audio section naming a file.
            if ( statedSceneVersion < kSceneVersionSoundAssets )
            {
                report.SoundRefsRaised = true;
                report.SoundRefs       = MigrateSoundRefsV45ToV46( entities, assetsRoot );
                if ( !report.SoundRefs.Refused.empty() )
                {
                    report.Refused = RefusedWhole( name, report.SoundRefs.Refused );
                    return;
                }
            }

            // A particle sprite composites by its material (VFX-08): ParticleEmitter.Blend moves onto Material.
            if ( statedSceneVersion < kSceneVersionParticleSpriteMaterial )
            {
                report.ParticleSpriteMaterialsRaised = true;
                report.ParticleSpriteMaterials       = MigrateParticleSpriteMaterialsV46ToV47( entities );
            }

            // SSR is on by default (SSR1B): every volume states true where v47 stated the old default false.
            if ( statedSceneVersion < kSceneVersionSSRDefaultOn )
            {
                report.SSRDefaultOnRaised = true;
                report.SSRDefaultOn       = MigrateSSRDefaultOnV47ToV48( entities );
                if ( !report.SSRDefaultOn.Refused.empty() )
                {
                    report.Refused = RefusedWhole( name, report.SSRDefaultOn.Refused );
                    return;
                }
            }

            // The shadow bias is centimetres at the caster (SHADOW-BIAS): a stated NDC fraction is re-stated.
            if ( statedSceneVersion < kSceneVersionShadowBiasCentimetres )
            {
                report.ShadowBiasCentimetresRaised = true;
                report.ShadowBiasesRestated        = MigrateShadowBiasCentimetresV48ToV49( entities );
            }

            // The particle emitter is a VFX system (VFX-HOST): each block becomes a `.dfx` and a VFX block.
            if ( statedSceneVersion < kSceneVersionParticleEmittersToVFX )
            {
                report.ParticleEmittersToVFXRaised = true;
                report.ParticleEmittersToVFX       = MigrateParticleEmittersToVFX( entities, name, assetsRoot );
                if ( !report.ParticleEmittersToVFX.Refused.empty() )
                {
                    report.Refused = RefusedWhole( name, report.ParticleEmittersToVFX.Refused );
                    return;
                }
            }

            // GIMode / EnableSSR are the render methods (RMODE-IFACE).
            if ( statedSceneVersion < kSceneVersionRenderMethods )
            {
                report.RenderMethodsRaised = true;
                report.RenderMethods       = MigrateRenderMethodsV52ToV53( entities );
                if ( !report.RenderMethods.Refused.empty() )
                {
                    report.Refused = RefusedWhole( name, report.RenderMethods.Refused );
                    return;
                }
            }

            // A script slot holds only what differs from its script (SCR-DETAILS).
            if ( statedSceneVersion < kSceneVersionScriptDefaults )
            {
                report.ScriptDefaultsRaised = true;
                report.ScriptDefaults       = MigrateScriptDefaultsV53ToV54( entities, assetsRoot );
                if ( !report.ScriptDefaults.Refused.empty() )
                {
                    report.Refused = RefusedWhole( name, report.ScriptDefaults.Refused );
                    return;
                }
            }

            // Lights state their units, the exposure range is EV100 (LIGHT-UNITS).
            if ( statedSceneVersion < kSceneVersionLightUnits )
            {
                report.LightUnitsRaised = true;
                report.LightUnits       = MigrateLightUnitsV55ToV56( entities );
                if ( !report.LightUnits.Refused.empty() )
                {
                    report.Refused = RefusedWhole( name, report.LightUnits.Refused );
                    return;
                }
            }

            // The physical sky's sun is the light; manual exposure is EV100 (LIGHT-UNITS-2).
            if ( statedSceneVersion < kSceneVersionSkySunFromLight )
            {
                report.SkySunFromLightRaised = true;
                report.SkySunFromLight       = MigrateSkySunFromLightV56ToV57( entities );
                if ( !report.SkySunFromLight.Refused.empty() )
                {
                    report.Refused = RefusedWhole( name, report.SkySunFromLight.Refused );
                    return;
                }
            }

            // The clock has a calendar (TOD1).
            if ( statedSceneVersion < kSceneVersionSunCalendar )
            {
                report.SunCalendarRaised = true;
                report.SunCalendar       = MigrateSunCalendarV57ToV58( entities );
            }

            // A directional light is aimed by its Rotation (LEG-SUN-ROTATION).
            if ( statedSceneVersion < kSceneVersionSunRotation )
            {
                report.SunRotationRaised = true;
                report.SunRotation       = MigrateSunRotationV58ToV61( entities );
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
            Common::Content::TextAssetHeaderSerialized header =
                 Common::Content::MakeTextHeader( kind, guid, Core::SceneTextSubsystems() );
            // What the file stated it depends on stays stated: only the v50 -> v51 step (SCENE-DEPS) gathers the
            // list, and it runs once, so a restated header without it would empty every later migration's list.
            if ( stated )
                header.Dependencies = stated->Dependencies;
            return header;
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

    int MigrateGameModeSettingsV44ToV45( SceneSerialized& scene )
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

    bool MigrateHLODLayersV49ToV50( SceneSerialized& scene )
    {
        if ( !scene.WorldPartition.has_value() || scene.WorldPartition->HLODLayers.has_value() ||
             scene.WorldPartition->DefaultHLODLayer.has_value() )
            return false;
        Core::Rules::StateInstancingHLODLayer( *scene.WorldPartition );
        return true;
    }

    SceneDependenciesReport MigrateSceneDependenciesV51ToV52( SceneSerialized&             scene,
                                                              const std::filesystem::path& assetsRoot )
    {
        // The save's own rule (Core::GatherSceneDependencies), so a migrated scene and the same scene saved
        // again state the same list.
        SceneDependenciesReport     report;
        Core::SceneDependencyGather gather = Core::GatherSceneDependencies(
             scene.Entities, scene.Settings.has_value() ? &*scene.Settings : nullptr, assetsRoot );
        for ( const auto& refusal : gather.Refused )
            report.Refused.push_back( scene.SceneName + " > " + refusal );
        report.Stated = gather.Guids.size();
        if ( report.Refused.empty() && scene.Header.has_value() )
            scene.Header->Dependencies = std::move( gather.Guids );
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
            report.WindSource       = MigrateWindSourceV42ToV43( scene.Entities, scene.SceneName, true );
        }
        if ( report.ExternalEntitiesRaised && scene.WorldPartition.has_value() )
            report.EntitiesMovedOut = scene.Entities.size();

        // Scene-only (it reads the Settings block a prefab does not have), so it runs here rather than in
        // RunSteps, after every entity step of the chain.
        if ( statedSceneVersion < kSceneVersionSceneSettingsHomes )
        {
            report.SceneSettingsHomesRaised = true;
            report.SceneSettingsHomes       = MigrateSceneSettingsHomesV35ToV36( scene );
            // The Settings block's ShadowBias it just stamped onto the lights is the same pre-v49 NDC fraction
            // RunSteps' v49 step re-stated before it was there: re-state again (idempotent), the count is total.
            if ( report.ShadowBiasCentimetresRaised && report.SceneSettingsHomes.ShadowKeysFound > 0 )
                report.ShadowBiasesRestated = MigrateShadowBiasCentimetresV48ToV49( scene.Entities );
            // Likewise the volume it just made states the grade's pre-v53 GlobalIllumination / EnableSSR: re-state
            // them as methods (idempotent — a re-stated key is gone).
            if ( report.RenderMethodsRaised )
            {
                const RenderMethodsReport lifted = MigrateRenderMethodsV52ToV53( scene.Entities );
                report.RenderMethods.Rewritten += lifted.Rewritten;
                if ( !lifted.Refused.empty() )
                {
                    report.Refused = RefusedWhole( scene.SceneName, lifted.Refused );
                    return report;
                }
            }
            // And its luminance exposure clamps as EV100 (idempotent - a volume stating the EV100 keys is
            // skipped).
            if ( report.LightUnitsRaised )
            {
                const LightUnitsReport lifted = MigrateLightUnitsV55ToV56( scene.Entities );
                report.LightUnits.Volumes += lifted.Volumes;
                if ( !lifted.Refused.empty() )
                {
                    report.Refused = RefusedWhole( scene.SceneName, lifted.Refused );
                    return report;
                }
            }
            // And its manual exposure as EV100 (idempotent - a volume stating ManualExposureEV100 is skipped).
            if ( report.SkySunFromLightRaised )
            {
                const SkySunFromLightReport lifted = MigrateSkySunFromLightV56ToV57( scene.Entities );
                report.SkySunFromLight.Volumes += lifted.Volumes;
                if ( !lifted.Refused.empty() )
                {
                    report.Refused = RefusedWhole( scene.SceneName, lifted.Refused );
                    return report;
                }
            }
        }
        if ( statedSceneVersion < kSceneVersionGameModeSettings )
        {
            report.GameModeSettingsRaised = true;
            report.GameModeKeysAdded      = MigrateGameModeSettingsV44ToV45( scene );
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

        // Scene-only: the WorldPartition block is a scene's (WP-FAR-7).
        if ( statedSceneVersion < kSceneVersionHLODLayers )
        {
            report.HLODLayersRaised = true;
            report.HLODLayerStated  = MigrateHLODLayersV49ToV50( scene );
        }

        // Stamped whether or not anything moved: an already-current scene is still stamped, idempotently
        // (MigrationHeader keeps an existing GUID) - leaving it unstamped is how a load would re-run this.
        scene.Header =
             MigrationHeader( scene.Header, Common::Content::ContentKind::Scene, assetsRoot, sourceFile );
        scene.SceneVersion = std::nullopt;
        scene.UnitVersion  = std::nullopt;

        // Scene-only, after the stamp (MigrationHeader restates the header): it reads what every step left stated.
        if ( statedSceneVersion < kSceneVersionSceneDependencies )
        {
            report.SceneDependenciesRaised = true;
            report.SceneDependencies       = MigrateSceneDependenciesV51ToV52( scene, assetsRoot );
            if ( !report.SceneDependencies.Refused.empty() )
            {
                report.Refused = RefusedWhole( scene.SceneName, report.SceneDependencies.Refused );
                return report;
            }
        }

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
            outcome.Steps.WindSource       = MigrateWindSourceV42ToV43( prefab.Entities, prefab.Name, false );
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
