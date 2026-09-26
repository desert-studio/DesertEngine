// SceneMigrator — raises every .desce it is pointed at to the current scene generation and writes it back.
//
// WHY THIS EXISTS. The engine has always migrated old scenes on LOAD, and never once written the result
// down. Nothing in the repository carried a UnitVersion, so every load of every scene re-ran the
// metres-to-centimetres migration, the files stayed permanently authored in metres, and a scene authored
// correctly in world units was silently multiplied by a hundred the first time anyone opened it. The
// contract's migration clause (DEV_CONTRACT §4.3/§4.5) says data migrates once and is written back in the
// new form, and that "the scenes in the repository are converted by the same task". This is the thing that
// converts them.
//
// AND IT IS NOW THE ONLY THING THAT MIGRATES. The migrations used to ALSO live in the engine and run on
// every scene load; they are Source/SceneMigration.cpp beside this file now, and Core::kSceneVersion is a
// requirement the loader enforces rather than a target it drags files towards. So this tool is not a
// convenience any more — it is the conversion, and the loader's refusal names it by command line.
//
// It parses into Core::SceneSerialized, the engine's own struct for the current on-disk shape, and writes
// the same tree back out, so the file this produces is the file the engine reads. There is no second
// statement of the format to disagree with the first.
//
// It needs no GPU, no asset manager and no scene graph, because the migrations are pure functions over the
// parsed tree. That is what makes running it over a whole repository safe: a scene whose meshes or
// materials cannot be resolved on this machine still round-trips exactly, because nothing here resolves
// them.
//
// AND IT RAISES `.demat` FILES TOO, since O-4. The cloud LOOK has lived in a material rather than in the
// scene since v12, so a tool that migrated only scenes would leave every cloud material behind — carrying,
// in that case, a layout slot the shader no longer declares. A `.demat` has no version field, so those
// steps are content-detected and idempotent rather than version-gated; see
// MigrateCloudMaterialLayoutInputs and MigrateCloudMaterialAlbedoToColour.
//
// TWO STEPS NOW, AND NEITHER GATES THE OTHER. The second raises a scalar `ScatteringAlbedo` to a neutral
// colour, which is DATA LOSS if it is skipped rather than a missing feature: a scalar stored as
// (x, 0, 0, 0) and read as a colour is a medium that scatters red and absorbs green and blue outright.
// A material can need it without ever having had a layout binding, so both reports are consulted before
// the file is called clean.
//
// AND `.deprefab` FILES, since И11, and this is why there is no second tool. A prefab carries the scene's
// own EntityData and SHARES the scene's two version integers, so raising the head moves prefabs too — but
// the conversion used to live in a separate binary with a separate corpus, so raising the head and running
// only this one left every prefab in the tree at the old number, refused by the loader and by its own
// migrator alike. One number, one chain (Source/SceneMigration.cpp), one command.
//
//   SceneMigrator <path>...          .desce, .demat, .deprefab and .anim files (and, for their layout
//                                    only, the other text assets), or directories searched recursively
//   SceneMigrator --check <path>...  report what would change and write nothing (exit 1 if any would)

#include <Engine/Assets/TextAssetHeaderStamp.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>
#include <Engine/Assets/Serialization/Retarget.hpp>
#include <Engine/Assets/CloudTypeData.hpp>
#include <Engine/Assets/CloudNoiseVolume.hpp>
#include <Engine/Assets/MaterialFormat.hpp>
#include <Engine/Assets/CloudLayout.hpp>
#include <Engine/Assets/CloudModellingVolume.hpp>
#include <Engine/Assets/MeshSourceAsset.hpp>
#include "LegacyMaterialIds.hpp"
#include "MigratorMain.hpp"
#include "SceneMigration.hpp"
#include "SettingsCanonical.hpp"

#include <Common/Content/ShaderAssetHeader.hpp>
#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/CanonicalText.hpp>
#include <Common/Content/MeshBinaryHeader.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <Engine/Animation/TimeModel.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/AnimationClipMigrate.hpp>

#include <rflcpp/rfl/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace
{
    constexpr const char* kSceneExtension = ".desce";

    // MATERIALS ARE COLLECTED TOO, since O-4. A `.demat` has no version field to gate on, so the
    // material step is content-detected (see MigrateCloudMaterialLayoutInputs) — but it still has to be
    // REACHED, and the cloud look has lived in `.demat` files rather than in scenes since v12. A tool
    // that migrated only the scenes would leave every cloud material behind with a slot the shader no
    // longer declares.
    constexpr const char* kMaterialExtension = ".demat";

    // PREFABS ARE COLLECTED TOO, since И11, and by THIS tool rather than a second one. A `.deprefab`
    // carries the scene's own EntityData and shares the scene's two version integers, so it moves
    // through the same generations and is raised by the same chain — and the two-binaries arrangement it
    // replaces meant that raising the head required remembering to run a SECOND command over a SECOND
    // corpus. Forgetting it is precisely the data loss the shared version number creates: prefabs left
    // at the old number, refused by the loader and by their own migrator alike.
    constexpr const char* kPrefabExtension = ".deprefab";

    // CLIPS ARE COLLECTED TOO, since A5, and by THIS tool for the reason the prefabs gave: a second
    // binary over a second corpus is a command somebody forgets, and forgetting it leaves files the
    // loader refuses. A `.anim` does NOT share the scene's version integers — it has its own sequence,
    // because a scene names a clip by NAME and nothing in a `.desce` changes when a clip's time model
    // does. Its step is gated on its own number and is content-detected the same way: a file with no
    // version field is generation 0, never "already current".
    constexpr const char* kClipExtension = ".anim";
    constexpr const char* kShaderExtension = ".shader";

    // THE OTHER TEXT ASSETS ARE COLLECTED FOR THEIR LAYOUT ONLY (AF6e). Their content has no step in this
    // tool - each is versioned by its own loader - but their text is written by the canonical writer, so a
    // file in the tree that predates it is re-laid-out here, version untouched, and the next save of it
    // diffs only in what the save changed. `.dclayout` is not here: it is binary, and has its own pass
    // (container 1 -> 2, IsCloudLayout).
    constexpr std::array kLayoutOnlyExtensions{ ".danimgraph", ".dgraph", ".decloudtype", ".destrings",
                                                ".detheme",    ".derig",  ".retarget",    ".skeleton" };

    bool IsLayoutOnly( const std::filesystem::path& path )
    {
        const std::string ext = path.extension().string();
        return std::find( kLayoutOnlyExtensions.begin(), kLayoutOnlyExtensions.end(), ext ) !=
               kLayoutOnlyExtensions.end();
    }

    // COOKED MESHES ARE COLLECTED TOO, since AF7q: MeshBinary v3 states the mesh's GUID and names each
    // submesh's material by GUID, and SCNE 28 reads that GUID from the mesh file - so the mesh pass runs
    // BEFORE the scenes, whatever order the paths were given in.
    bool IsCookedMesh( const std::filesystem::path& path )
    {
        const std::string ext = path.extension().string();
        return ext == Common::Constants::Extensions::STATIC_MESH ||
               ext == Common::Constants::Extensions::SKINNED_MESH;
    }

    // CLOUD LAYOUTS ARE COLLECTED TOO, since AF7y (T6b2): version 1 was a bare "DCLY" container with no
    // identity; version 2 is the same bytes inside the AF1 binary envelope, with a GUID minted HERE, once.
    bool IsCloudLayout( const std::filesystem::path& path )
    {
        return path.extension() == Desert::Assets::kCloudLayoutExtension;
    }

    // CLOUD NOISE VOLUMES ARE COLLECTED TOO, since T7g: versions 1 and 2 were a bare "DCNV" container with no
    // identity; version 3 is the version-2 bytes after magic and version inside the AF1 binary envelope, with
    // a GUID minted HERE, once.
    bool IsCloudNoiseVolume( const std::filesystem::path& path )
    {
        return path.extension() == Desert::Assets::kCloudNoiseVolumeExtension;
    }

    // SCULPTED CLOUD VOLUMES (.dcmv) LIKEWISE, since T7g: version 2 was a bare "DCMV" container with no
    // identity; version 3 is the version-2 bytes after magic and version inside the AF1 binary envelope.
    bool IsCloudModellingVolume( const std::filesystem::path& path )
    {
        return path.extension() == Desert::Assets::kCloudModellingVolumeExtension;
    }

    // The exclusion `path` falls under, by its trailing components (MigratorMain.hpp, ScanExclusions).
    const Desert::Migration::ScanExclusion* ExclusionFor( const std::filesystem::path& path )
    {
        const std::vector<std::filesystem::path> components( path.begin(), path.end() );
        for ( const Desert::Migration::ScanExclusion& exclusion : Desert::Migration::ScanExclusions() )
        {
            const std::filesystem::path              stated( exclusion.Path );
            const std::vector<std::filesystem::path> tail( stated.begin(), stated.end() );
            if ( tail.size() <= components.size() &&
                 std::equal( tail.rbegin(), tail.rend(), components.rbegin() ) )
                return &exclusion;
        }
        return nullptr;
    }

    void Collect( const std::filesystem::path& root, std::vector<std::filesystem::path>& scenes,
                  std::vector<std::filesystem::path>& materials, std::vector<std::filesystem::path>& prefabs,
                  std::vector<std::filesystem::path>& clips, std::vector<std::filesystem::path>& texts,
                  std::vector<std::filesystem::path>& meshes, std::vector<std::filesystem::path>& layouts,
                  std::vector<std::filesystem::path>& noises, std::vector<std::filesystem::path>& models,
                  std::vector<std::filesystem::path>& shaders, std::ostream& out )
    {
        std::error_code ec;
        if ( std::filesystem::is_directory( root, ec ) )
        {
            for ( auto it = std::filesystem::recursive_directory_iterator( root, ec );
                  it != std::filesystem::recursive_directory_iterator(); ++it )
            {
                const auto& entry = *it;
                if ( const Desert::Migration::ScanExclusion* excluded = ExclusionFor( entry.path() ) )
                {
                    out << "skip   " << entry.path().string() << " — " << excluded->Reason << "\n";
                    if ( entry.is_directory() )
                        it.disable_recursion_pending();
                    continue;
                }
                if ( !entry.is_regular_file() )
                    continue;
                if ( entry.path().extension() == kSceneExtension )
                    scenes.push_back( entry.path() );
                else if ( entry.path().extension() == kMaterialExtension )
                    materials.push_back( entry.path() );
                else if ( entry.path().extension() == kPrefabExtension )
                    prefabs.push_back( entry.path() );
                else if ( entry.path().extension() == kClipExtension )
                {
                    clips.push_back( entry.path() );
                }
                else if ( IsLayoutOnly( entry.path() ) )
                    texts.push_back( entry.path() );
                else if ( IsCookedMesh( entry.path() ) )
                    meshes.push_back( entry.path() );
                else if ( IsCloudLayout( entry.path() ) )
                    layouts.push_back( entry.path() );
                else if ( IsCloudNoiseVolume( entry.path() ) )
                    noises.push_back( entry.path() );
                else if ( IsCloudModellingVolume( entry.path() ) )
                    models.push_back( entry.path() );
                else if ( entry.path().extension() == kShaderExtension )
                    shaders.push_back( entry.path() );
            }
            return;
        }

        if ( root.extension() == kMaterialExtension )
            materials.push_back( root );
        else if ( root.extension() == kPrefabExtension )
            prefabs.push_back( root );
        else if ( root.extension() == kClipExtension )
        {
            clips.push_back( root );
        }
        else if ( IsLayoutOnly( root ) )
            texts.push_back( root );
        else if ( IsCookedMesh( root ) )
            meshes.push_back( root );
        else if ( IsCloudLayout( root ) )
            layouts.push_back( root );
        else if ( IsCloudNoiseVolume( root ) )
            noises.push_back( root );
        else if ( IsCloudModellingVolume( root ) )
            models.push_back( root );
        else if ( root.extension() == kShaderExtension )
            shaders.push_back( root );
        else
            scenes.push_back( root );
    }

    std::string ReadAll( const std::filesystem::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // Every text asset this tool writes goes through the one canonical writer (AF6), so a migrated file and a
    // file saved by the engine are laid out alike and a migration's diff is only the lines it changed.
    // `object` (JSON text of an object without a "Header" member) with "Header": `header` as its FIRST member,
    // every other byte left as written; WriteText lays the result out canonically. A text that does not open
    // an object is returned unchanged, and WriteText's parse then refuses it by name.
    std::string PrependHeaderMember( const std::string& object, const std::string& header )
    {
        const std::size_t open = object.find_first_not_of( " \t\r\n" );
        if ( open == std::string::npos || object[open] != '{' )
            return object;
        const std::size_t next  = object.find_first_not_of( " \t\r\n", open + 1 );
        const bool        empty = next != std::string::npos && object[next] == '}';
        return object.substr( 0, open + 1 ) + "\"Header\":" + header + ( empty ? "" : "," ) +
               object.substr( open + 1 );
    }

    // `object` without its TOP-LEVEL member `key` and the one comma that separated it from a neighbour, every
    // other byte kept; nullopt when the top level does not state `key` exactly once. Text, not rfl::Generic:
    // the generic tree reads every integer as int64, and a uint64 above INT64_MAX came back negative (T7e2).
    std::optional<std::string> EraseTopLevelMember( const std::string& object, std::string_view key )
    {
        // The end of the string literal opening at `quote`, one past its closing quote.
        const auto stringEnd = [&]( std::size_t quote )
        {
            std::size_t at = quote + 1;
            while ( at < object.size() && object[at] != '"' )
                at += object[at] == '\\' ? 2 : 1;
            return at + 1;
        };
        int                                                depth = 0;
        std::optional<std::pair<std::size_t, std::size_t>> found;
        for ( std::size_t at = 0; at < object.size(); ++at )
        {
            const char c = object[at];
            if ( c == '{' || c == '[' )
                ++depth;
            else if ( c == '}' || c == ']' )
                --depth;
            else if ( c == '"' )
            {
                const std::size_t close = stringEnd( at );
                const std::size_t colon = object.find_first_not_of( " \t\r\n", close );
                const bool        isKey = depth == 1 && colon != std::string::npos && object[colon] == ':' &&
                                   std::string_view( object ).substr( at + 1, close - at - 2 ) == key;
                if ( isKey )
                {
                    if ( found )
                        return std::nullopt;
                    // The value ends at the first ',' or '}' back at the member's own depth.
                    int         inner = 0;
                    std::size_t end   = colon + 1;
                    while ( end < object.size() )
                    {
                        const char v = object[end];
                        if ( v == '"' )
                        {
                            end = stringEnd( end );
                            continue;
                        }
                        if ( v == '{' || v == '[' )
                            ++inner;
                        else if ( inner > 0 && ( v == '}' || v == ']' ) )
                            --inner;
                        else if ( inner == 0 && ( v == ',' || v == '}' ) )
                            break;
                        ++end;
                    }
                    while ( end > colon && std::isspace( static_cast<unsigned char>( object[end - 1] ) ) != 0 )
                        --end;
                    found = std::make_pair( at, end );
                    at    = end - 1;
                    continue;
                }
                at = close - 1;
            }
        }
        if ( !found )
            return std::nullopt;
        const auto [begin, end] = *found;
        std::size_t before      = begin;
        while ( before > 0 && std::isspace( static_cast<unsigned char>( object[before - 1] ) ) != 0 )
            --before;
        if ( before > 0 && object[before - 1] == ',' )
            return object.substr( 0, before - 1 ) + object.substr( end );
        std::size_t after = end;
        while ( after < object.size() && std::isspace( static_cast<unsigned char>( object[after] ) ) != 0 )
            ++after;
        if ( after < object.size() && object[after] == ',' )
            return object.substr( 0, begin ) + object.substr( after + 1 );
        return object.substr( 0, begin ) + object.substr( end );
    }

    bool WriteText( const std::filesystem::path& path, const std::string& json, std::ostream& err )
    {
        const auto text = Common::Content::CanonicalJsonText( json );
        if ( !text )
        {
            err << "FAIL   " << path.string() << " — " << text.GetError() << "\n";
            return false;
        }
        const auto written = Common::Utils::FileSystem::WriteContentToFileAtomic( path, text.GetValue() );
        if ( !written )
            err << "FAIL   " << path.string() << " — " << written.GetError() << "\n";
        return static_cast<bool>( written );
    }

    // A file whose DOCUMENT is current but whose TEXT is not canonical is re-laid-out, not raised: its version
    // stays, because the content is unchanged (the CanonicalText suite proves old and new parse to one document).
    enum class Layout
    {
        Canonical,
        Relaid,
        Failed,
    };

    // ONE STEP, MANY TEXT KINDS: "the file gains the text asset header". An old-generation text asset states
    // a top-level version member and no identity; the raised one opens with the text asset header (its Kind, a
    // GUID minted HERE, once, and the format under the kind's tag) and states its version nowhere else. Every
    // other member keeps its order and bytes. Each kind is a row, not a copy of the step: T6b1 wrote it for
    // the cloud type, T7b made it the table the next text kinds join.
    struct TextHeaderRaise
    {
        const char*                  Extension;
        Common::Content::ContentKind Kind;
        uint32_t                     Tag;
        int                          FromVersion;
        uint32_t                     ToVersion;
        // The member the old generation stated its version in; dropped by the raise. nullptr when the old
        // generation stated no version at all (the anim graph): every headerless file of it IS FromVersion.
        const char* VersionMember;
        // Whether a file that left the member out IS FromVersion (the string table and the theme said
        // "absent means 1"), or states nothing the step may assume (the cloud type).
        bool AbsentIsFrom;
    };

    constexpr std::array kTextHeaderRaises{
         // .decloudtype 3 -> 4 (AF7v, T6b1). The literal 4 and not kCloudTypeSchemaVersion: CLTY 5 (T7h) names
         // the noise volume by GUID, which this splice cannot state, so RaiseCloudTypeV4ToV5 takes it on.
         TextHeaderRaise{ ".decloudtype", Common::Content::ContentKind::CloudType,
                          Desert::Assets::kCloudTypeSchemaTag, 3, 4, "FormatVersion", false },
         // .destrings 1 -> 2 (T7b).
         TextHeaderRaise{ ".destrings", Common::Content::ContentKind::StringTable,
                          Desert::Assets::kStringTableSchemaTag, 1, Desert::Assets::kStringTableSchemaVersion,
                          "FormatVersion", true },
         // .detheme 1 -> 2 (T7b).
         TextHeaderRaise{ ".detheme", Common::Content::ContentKind::UITheme, Desert::Assets::kUIThemeSchemaTag, 1,
                          Desert::Assets::kUIThemeSchemaVersion, "FormatVersion", true },
         // .derig 1 -> 2 (T7c).
         TextHeaderRaise{ ".derig", Common::Content::ContentKind::ControlRig, Desert::Assets::kControlRigSchemaTag,
                          1, Desert::Assets::kControlRigSchemaVersion, "FormatVersion", true },
         // .retarget 1 -> 2 (T7c). The literal 2 and not kRetargetSchemaVersion: RTGT 3 (T7f) names the rig by
         // GUID, which this splice cannot state, so a v1 file lands at 2 and RaiseRetargetV2ToV3 takes it on.
         TextHeaderRaise{ ".retarget", Common::Content::ContentKind::Retarget, Desert::Assets::kRetargetSchemaTag,
                          1, 2, "FormatVersion", true },
         // .danimgraph 0 -> 1 (T7d): generation 0 stated no version member at all.
         TextHeaderRaise{ ".danimgraph", Common::Content::ContentKind::AnimGraph,
                          Desert::Assets::kAnimGraphSchemaTag, 0, Desert::Assets::kAnimGraphSchemaVersion, nullptr,
                          true },
         // .skeleton 0 -> 1 (T7e): generation 0 stated no version member at all.
         TextHeaderRaise{ ".skeleton", Common::Content::ContentKind::Skeleton, Desert::Assets::kSkeletonSchemaTag,
                          0, Desert::Assets::kSkeletonSchemaVersion, nullptr, true },
         // .anim 3 -> 4 (T7e): the clip's own `Version` moves into the header. Generations 0-2 reach 3 first
         // through MigrateAnimationJson (the clip loop); a file that left `Version` out is generation 0, never 3.
         TextHeaderRaise{ ".anim", Common::Content::ContentKind::Animation, Desert::Assets::kAnimationSchemaTag,
                          Desert::Assets::Serialization::kAnimationLastVersionMember,
                          Desert::Assets::kAnimationSchemaVersion, "Version", false },
    };

    const TextHeaderRaise* TextHeaderRaiseFor( const std::filesystem::path& path )
    {
        const std::string ext = path.extension().string();
        for ( const TextHeaderRaise& row : kTextHeaderRaises )
            if ( ext == row.Extension )
                return &row;
        return nullptr;
    }

    // A file that already has a header returns nullopt (nothing to raise); any version but the row's
    // FromVersion is refused, the file untouched.
    Common::ResultStr<std::optional<std::string>> RaiseTextToHeader( const TextHeaderRaise&            row,
                                                                     const std::string&                source,
                                                                     const Common::Content::AssetGuid& guid )
    {
        const auto tree = rfl::json::read<rfl::Generic>( source );
        if ( !tree || !tree.value().to_object() )
            return Common::MakeError<std::optional<std::string>>( "not a JSON object" );
        const rfl::Generic::Object fields = tree.value().to_object().value();
        if ( fields.get( std::string( Common::Content::kTextHeaderMember ) ).has_value() )
            return Common::MakeSuccess( std::optional<std::string>{} );
        const std::string member  = row.VersionMember != nullptr ? row.VersionMember : "";
        const auto        stated  = row.VersionMember != nullptr ? fields.get( member )
                                                                 : rfl::Result<rfl::Generic>( rfl::Error( "unstated" ) );
        const auto version = [&]() -> rfl::Result<int>
        {
            if ( stated.has_value() )
                return stated.value().to_int();
            if ( row.AbsentIsFrom )
                return row.FromVersion;
            return { rfl::Error( "absent" ) };
        }();
        if ( !version || version.value() != row.FromVersion )
            return Common::MakeError<std::optional<std::string>>(
                 std::string( Common::Content::KindName( row.Kind ) ) + " format version " +
                 ( version ? std::to_string( version.value() ) : "(unstated)" ) + " has no step to v" +
                 std::to_string( row.ToVersion ) + "; only a version-" + std::to_string( row.FromVersion ) +
                 " file is raised" );
        const std::array<Common::Content::SubsystemVersion, 1> versions = {
             Common::Content::SubsystemVersion{ row.Tag, row.ToVersion } };
        const std::string headerText =
             rfl::json::write( Common::Content::MakeTextHeader( row.Kind, guid, versions ) );
        // THE HEADER IS SPLICED INTO THE SOURCE TEXT and the version member cut out of it, every other byte
        // kept. A round trip through rfl::Generic reads an integer as int64, so a skeleton's Signature and a
        // clip's SkeletonSignature above INT64_MAX came back negative (T7e, T7e2) - a rig no mesh would match.
        std::string body = source;
        if ( stated.has_value() )
        {
            auto cut = EraseTopLevelMember( source, member );
            if ( !cut )
                return Common::MakeError<std::optional<std::string>>( "the top level states '" + member +
                                                                      "' more than once" );
            body = std::move( *cut );
        }
        return Common::MakeSuccess( std::optional<std::string>( PrependHeaderMember( body, headerText ) ) );
    }

    // RTGT 2 AS IT WAS WRITTEN, frozen here: the rig a bare path relative to the cooked meshes root. Every
    // other member is the engine's own row type, which RTGT 3 did not change.
    struct RetargetDataV2
    {
        std::optional<Common::Content::TextAssetHeaderSerialized>          Header;
        std::string                                                        Name;
        std::string                                                        SourceSkeleton;
        std::string                                                        SourcePelvisBone;
        std::string                                                        TargetPelvisBone;
        Desert::Assets::Serialization::RetargetPoseData                    SourceRetargetPose;
        Desert::Assets::Serialization::RetargetPoseData                    TargetRetargetPose;
        std::vector<Desert::Assets::Serialization::RetargetChainData>      Chains;
        std::vector<Desert::Assets::Serialization::RetargetBoneRenameData> BoneRenames;
    };

    // .retarget RTGT 2 -> 3 (T7f): the source rig named by {Guid, Path}, the GUID read out of the named
    // `.skeleton`'s own header. nullopt when the text already states 3; any other version, or a rig that is
    // not there or states no GUID, is refused by name and the caller leaves the file untouched.
    //
    // THE RIG IS LOOKED UP WHERE THE ENGINE LOOKS: the project's Cooked/Meshes, beside the assets root
    // (`<project>/Resources/Assets`) this retarget lies under - the mesh step's reading of the same layout.
    // The text written is the engine's own WriteRetarget (header GUID kept, Dependencies stated) and it
    // must pass the engine's own ParseRetarget before the caller writes a byte.
    Common::ResultStr<std::optional<std::string>> RaiseRetargetV2ToV3( const std::filesystem::path& path,
                                                                       const std::string&           text )
    {
        using Result      = std::optional<std::string>;
        namespace File    = Desert::Assets::Serialization;
        namespace Paths   = Common::Constants::Path;
        const auto parsed = rfl::json::read<RetargetDataV2>( text );
        const auto stated = [&]() -> int
        {
            if ( parsed )
                return Desert::Assets::StatedVersion( parsed.value().Header, Desert::Assets::kRetargetSchemaTag );
            const auto current = rfl::json::read<File::RetargetAssetData>( text );
            return current ? Desert::Assets::StatedVersion( current.value().Header,
                                                            Desert::Assets::kRetargetSchemaTag )
                           : 0;
        }();
        if ( stated == File::kRetargetVersion )
            return Common::MakeSuccess( Result{} );
        if ( !parsed || stated != 2 )
            return Common::MakeError<Result>( "not a readable RTGT 2 retarget (states RTGT " +
                                              std::to_string( stated ) + ")" +
                                              ( parsed ? "" : std::string( ": " ) + parsed.error().what() ) );
        const RetargetDataV2& v2 = parsed.value();

        auto assetsRoot = Paths::RootForContentPath( Paths::ContentDir::Retarget, path );
        if ( assetsRoot && assetsRoot->filename().empty() )
            assetsRoot = assetsRoot->parent_path();
        const std::filesystem::path sandbox( Paths::SANDBOX_ASSETS_ROOT );
        if ( !assetsRoot || assetsRoot->filename() != sandbox.filename() ||
             assetsRoot->parent_path().filename() != sandbox.parent_path().filename() )
            return Common::MakeError<Result>( "lies under no <project>/" + sandbox.generic_string() +
                                              "/Retargets folder, so there is no Cooked/Meshes holding its rig '" +
                                              v2.SourceSkeleton + "'" );
        const std::filesystem::path meshesCooked =
             Paths::CONTENT_DIRS[static_cast<std::size_t>( Paths::ContentDir::MeshCooked )].Rel;
        const std::filesystem::path rig = ( assetsRoot->parent_path().parent_path() / Paths::COOKED_DIR_NAME /
                                            meshesCooked / v2.SourceSkeleton )
                                               .lexically_normal();
        const Common::Content::AssetGuid rigGuid = Desert::Assets::ReadTextHeaderGuid( rig );
        if ( rigGuid.IsNull() )
            return Common::MakeError<Result>( "its source rig " + rig.generic_string() +
                                              " is missing or states no header GUID; raise the rig first" );

        File::RetargetAssetData v3;
        v3.Header             = v2.Header;
        v3.Name               = v2.Name;
        v3.SourceSkeleton     = { Common::Content::AssetGuidToText( rigGuid ), v2.SourceSkeleton };
        v3.SourcePelvisBone   = v2.SourcePelvisBone;
        v3.TargetPelvisBone   = v2.TargetPelvisBone;
        v3.SourceRetargetPose = v2.SourceRetargetPose;
        v3.TargetRetargetPose = v2.TargetRetargetPose;
        v3.Chains             = v2.Chains;
        v3.BoneRenames        = v2.BoneRenames;
        std::string written   = File::WriteRetarget( v3 );
        if ( auto loadable = File::ParseRetarget( written ); !loadable )
            return Common::MakeError<Result>( "the raised text does not pass the engine's own ParseRetarget: " +
                                              loadable.GetError() );
        return Common::MakeSuccess( Result( std::move( written ) ) );
    }

    // The GUID a `.dcnv` envelope header states; null when the file is absent, bare or not a noise volume.
    // The engine's CloudNoiseVolumeAsset::ReadCloudNoiseVolumeGuid, minus the VFS the migrator has no use for.
    Common::Content::AssetGuid ReadNoiseVolumeGuid( const std::filesystem::path& file )
    {
        namespace CC                        = Common::Content;
        const CC::SubsystemVersion kKnown[] = {
             { Desert::Assets::kCloudNoiseSubsystemTag, Desert::Assets::kCloudNoiseContainerVersion } };
        const CC::AssetHeaderReadContext context{ kKnown };
        std::ifstream                    in( file, std::ios::binary );
        if ( !in )
            return {};
        const auto header = CC::ReadEnvelopeHeader( in, context );
        if ( !header || header.GetValue().Asset.Kind != CC::ContentKind::CloudNoiseVolume )
            return {};
        return header.GetValue().Asset.Guid;
    }

    // .decloudtype CLTY 4 -> 5 (T7h): the noise volume named by {Guid, Path}, the GUID read out of the named
    // `.dcnv`'s envelope, and stated as the header's one Dependency. nullopt when the text already states 5;
    // any other version, or a volume that is not there or states no GUID, is refused by name and the caller
    // leaves the file untouched. A type naming no volume only moves its version.
    //
    // THE VOLUME IS LOOKED UP WHERE THE ENGINE LOOKS: relative to the assets root this type lies under (its
    // Clouds/Types folder's root). The text written must pass the engine's own ParseCloudType before the
    // caller writes a byte.
    Common::ResultStr<std::optional<std::string>> RaiseCloudTypeV4ToV5( const std::filesystem::path& path,
                                                                        const std::string&           text )
    {
        using Result    = std::optional<std::string>;
        namespace Paths = Common::Constants::Path;
        struct HeaderOnly
        {
            std::optional<Common::Content::TextAssetHeaderSerialized> Header;
        };
        const auto header = rfl::json::read<HeaderOnly>( text );
        const int  stated =
             header ? Desert::Assets::StatedVersion( header.value().Header, Desert::Assets::kCloudTypeSchemaTag )
                     : 0;
        if ( stated == static_cast<int>( Desert::Assets::kCloudTypeSchemaVersion ) )
            return Common::MakeSuccess( Result{} );
        const auto tree = rfl::json::read<rfl::Generic>( text );
        if ( stated != 4 || !tree || !tree.value().to_object() )
            return Common::MakeError<Result>( "not a readable CLTY 4 cloud type (states CLTY " +
                                              std::to_string( stated ) + ")" );
        rfl::Generic::Object doc = tree.value().to_object().value();

        std::vector<std::string> dependencies;
        if ( const auto named = doc.get( "NoiseVolume" ); named )
        {
            const auto relative = named.value().to_string();
            if ( !relative )
                return Common::MakeError<Result>( "its NoiseVolume is not a CLTY 4 path string" );
            auto assetsRoot = Paths::RootForContentPath( Paths::ContentDir::CloudType, path );
            if ( assetsRoot && assetsRoot->filename().empty() )
                assetsRoot = assetsRoot->parent_path();
            if ( !assetsRoot )
                return Common::MakeError<Result>( "lies under no <assets>/Clouds/Types folder, so there is no "
                                                  "assets root holding its noise volume '" +
                                                  relative.value() + "'" );
            const std::filesystem::path      volume = ( *assetsRoot / relative.value() ).lexically_normal();
            const Common::Content::AssetGuid guid   = ReadNoiseVolumeGuid( volume );
            if ( guid.IsNull() )
                return Common::MakeError<Result>(
                     "its noise volume " + volume.generic_string() +
                     " is missing or states no envelope GUID; raise the volume first" );
            rfl::Generic::Object ref;
            ref["Guid"]        = rfl::Generic( Common::Content::AssetGuidToText( guid ) );
            ref["Path"]        = rfl::Generic( relative.value() );
            doc["NoiseVolume"] = rfl::Generic( ref );
            dependencies.push_back( Common::Content::AssetGuidToText( guid ) );
        }

        // THE DOCUMENT IS EDITED, NOT RE-SERIALISED: only the version, the Dependencies and the volume reference
        // move, so every authored number keeps its spelling (the typed writer would print 9.4 as the float's
        // double expansion). The engine's own ParseCloudType is still the gate the result has to pass.
        const auto headerTree = doc.get( "Header" );
        if ( !headerTree || !headerTree.value().to_object() )
            return Common::MakeError<Result>( "its Header is not an object" );
        rfl::Generic::Object headerDoc = headerTree.value().to_object().value();
        rfl::Generic::Object versions;
        versions["CLTY"] = rfl::Generic( static_cast<int>( Desert::Assets::kCloudTypeSchemaVersion ) );
        rfl::Generic::Array dependencyTexts;
        for ( const auto& guid : dependencies )
            dependencyTexts.emplace_back( guid );
        headerDoc["Versions"]     = rfl::Generic( versions );
        headerDoc["Dependencies"] = rfl::Generic( dependencyTexts );
        doc["Header"]             = rfl::Generic( headerDoc );
        std::string written       = rfl::json::write( rfl::Generic( doc ) );
        if ( auto loadable = Desert::Assets::ParseCloudType( written ); !loadable )
            return Common::MakeError<Result>( "the raised text does not pass the engine's own ParseCloudType: " +
                                              loadable.GetError() );
        return Common::MakeSuccess( Result( std::move( written ) ) );
    }

    Layout RelayOutIfNeeded( const std::filesystem::path& path, const std::string& source, bool check,
                             std::ostream& out, std::ostream& err )
    {
        if ( Common::Content::IsCanonicalJsonText( source ) )
            return Layout::Canonical;
        if ( check )
        {
            out << "WOULD  " << path.string() << " — text layout only (canonical text), version unchanged\n";
            return Layout::Relaid;
        }
        if ( !WriteText( path, source, err ) )
        {
            err << "FAIL   " << path.string() << " — the layout could not be written; the original file is "
                << "untouched\n";
            return Layout::Failed;
        }
        out << "relaid " << path.string() << " — text layout only (canonical text), version unchanged\n";
        return Layout::Relaid;
    }

    // THE `.demat` FILES A v11 -> v12 RAISE PRODUCED, written FIRST and atomically, before the file that
    // names them: a file naming a material which does not exist is worse than one not yet migrated, so if
    // a material cannot be written its source is not either. Returns false on failure, having named it,
    // and the caller then leaves the source file exactly as it found it.
    //
    // Shared by scenes and prefabs since И11 — a prefab can carry a VolumetricCloud entity like any other,
    // and the collision guard below has to see BOTH file classes through ONE `claimed` map, or two files
    // of different classes minting the same material name would overwrite each other unseen.
    // MATL 2 text -> the MATL 4 canonical text: the frozen v2 shape read, its numbers and its shader name
    // translated through `refs` (RaiseMaterialV2ToV4), the header raised and its Dependencies stated by the one
    // .demat writer.
    Common::ResultStr<std::string> RaiseMaterialV2TextToV4( const std::filesystem::path&                path,
                                                            const std::string&                          text,
                                                            const Desert::Migration::LegacyAssetRefMap& refs )
    {
        const auto parsed = rfl::json::read<Desert::Migration::MaterialDataV2>( text );
        if ( !parsed )
            return Common::MakeError<std::string>( "not a readable MATL 2 material: " +
                                                   std::string( parsed.error().what() ) );
        const auto raised = Desert::Migration::RaiseMaterialV2ToV4( path.generic_string(), parsed.value(), refs );
        if ( !raised )
            return Common::MakeError<std::string>( raised.GetError() );
        return Desert::Assets::WriteMaterialJson( raised.GetValue() );
    }

    // MATL 3 -> 4 (T7k): the shader named by its header GUID instead of by name. "ShaderName" becomes
    // "Shader": {Guid, Path} at the same position (Desert::Migration::ShaderRefByName: the one .shader in `refs`
    // with that file stem), "ShaderRefs" (the cloud Medium slot, a shader by path) moves into CloudAssets by the
    // shader's GUID, and the header states MATL 4 with its Dependencies in the engine's order
    // (MaterialData::ReferencedGuidTexts). A shader name no file carries, or a file with no header GUID, is
    // refused by name and the caller leaves the material untouched.
    //
    // THE DOCUMENT IS EDITED, NOT RE-SERIALISED (as RaiseCloudTypeV4ToV5): every authored number keeps its
    // spelling. The engine's own ParseMaterialJson is the gate the result has to pass.
    Common::ResultStr<std::string> RaiseMaterialV3ToV4( const std::filesystem::path& path, const std::string& text,
                                                        const Desert::Migration::LegacyAssetRefMap& refs )
    {
        const std::string source = path.generic_string();
        const auto        fail   = []( const std::string& why ) { return Common::MakeError<std::string>( why ); };
        const auto        tree   = rfl::json::read<rfl::Generic>( text );
        if ( !tree || !tree.value().to_object() )
            return fail( "not a readable MATL 3 material" );
        const rfl::Generic::Object doc = tree.value().to_object().value();

        rfl::Generic::Array mediums;
        if ( const auto shaderRefs = doc.get( "ShaderRefs" ); shaderRefs )
        {
            const auto list = shaderRefs.value().to_array();
            if ( !list )
                return fail( "its ShaderRefs is not a MATL 3 array" );
            for ( const auto& entry : list.value() )
            {
                const auto slot = entry.to_object();
                const auto name = slot ? slot.value().get( "Name" ).and_then( []( const rfl::Generic& g )
                                                                              { return g.to_string(); } )
                                       : rfl::Result<std::string>( rfl::Error( "not an object" ) );
                if ( !name )
                    return fail( "a ShaderRefs entry states no Name" );
                const auto locator =
                     slot.value().get( "Path" ).and_then( []( const rfl::Generic& g ) { return g.to_string(); } );
                rfl::Generic::Object cloud;
                cloud["Name"] = rfl::Generic( name.value() );
                cloud["Guid"] = rfl::Generic( std::string() );
                cloud["Path"] = rfl::Generic( std::string() );
                if ( locator && !locator.value().empty() )
                {
                    const auto found =
                         refs.find( static_cast<uint64_t>( Common::AssetHandle::FromKey( locator.value() ) ) );
                    if ( found == refs.end() || found->second.Kind != Desert::Migration::LegacyAssetKind::Shader ||
                         found->second.Guid.empty() )
                        return fail(
                             "its shader slot '" + name.value() + "' names '" + locator.value() +
                             "', which is no .shader with a header GUID under the content root's Shaders/" );
                    cloud["Guid"] = rfl::Generic( found->second.Guid );
                    cloud["Path"] = rfl::Generic( found->second.Path );
                }
                mediums.emplace_back( cloud );
            }
        }

        rfl::Generic::Object raised;
        bool                 cloudsSeen = false;
        for ( const auto& [key, value] : doc )
        {
            if ( key == "ShaderRefs" )
                continue;
            if ( key == "ShaderName" )
            {
                const auto name = value.to_string();
                if ( !name )
                    return fail( "its ShaderName is not a string" );
                if ( name.value().empty() )
                    continue; // an empty name drew the standard surface, which MATL 4 states by absence
                const auto shader = Desert::Migration::ShaderRefByName( source, name.value(), refs );
                if ( !shader )
                    return fail( shader.GetError() );
                rfl::Generic::Object ref;
                ref["Guid"]      = rfl::Generic( shader.GetValue().Guid );
                ref["Path"]      = rfl::Generic( shader.GetValue().Path );
                raised["Shader"] = rfl::Generic( ref );
                continue;
            }
            if ( key == "CloudAssets" && !mediums.empty() )
            {
                const auto list = value.to_array();
                if ( !list )
                    return fail( "its CloudAssets is not an array" );
                rfl::Generic::Array merged = list.value();
                merged.insert( merged.end(), mediums.begin(), mediums.end() );
                raised[key] = rfl::Generic( merged );
                cloudsSeen  = true;
                continue;
            }
            raised[key] = value;
        }
        if ( !mediums.empty() && !cloudsSeen )
            raised["CloudAssets"] = rfl::Generic( mediums );

        // The Dependencies the engine derives from the raised references, read back through the typed shape so
        // their order is ReferencedGuidTexts' own (parent, shader, textures, cloud assets).
        const auto typed =
             rfl::json::read<Desert::Assets::MaterialData>( rfl::json::write( rfl::Generic( raised ) ) );
        if ( !typed )
            return fail( "the raised material is not readable: " + std::string( typed.error().what() ) );
        const auto headerTree = raised.get( "Header" );
        if ( !headerTree || !headerTree.value().to_object() )
            return fail( "its Header is not an object" );
        rfl::Generic::Object headerDoc = headerTree.value().to_object().value();
        rfl::Generic::Object versions;
        versions["MATL"] = rfl::Generic( static_cast<int>( Desert::Assets::kMaterialSchemaVersion ) );
        rfl::Generic::Array dependencyTexts;
        for ( const auto& guid : typed.value().ReferencedGuidTexts() )
            dependencyTexts.emplace_back( guid );
        headerDoc["Versions"]     = rfl::Generic( versions );
        headerDoc["Dependencies"] = rfl::Generic( dependencyTexts );
        raised["Header"]          = rfl::Generic( headerDoc );

        std::string written = rfl::json::write( rfl::Generic( raised ) );
        if ( auto loadable = Desert::Assets::ParseMaterialJson( source, written ); !loadable )
            return fail( "the raised text does not pass the engine's own ParseMaterialJson: " +
                         loadable.GetError() );
        return Common::MakeSuccess( std::move( written ) );
    }

    bool WriteCloudMaterials( const std::vector<Desert::Migration::CloudMaterialFile>& produced,
                              const std::filesystem::path&                             assetsRoot,
                              const Desert::Migration::LegacyAssetRefMap&              refs,
                              const std::filesystem::path& source, std::map<std::string, std::string>& claimed,
                              std::ostream& out, std::ostream& err )
    {
        for ( const auto& mat : produced )
        {
            // Under the SAME root the migration was measured against: the relative path inside the file
            // and the file on disk must agree about one root, or the source names a material that is not
            // where it says. That root is the SOURCE FILE'S and not the process's working directory —
            // see the header for what the working directory cost.
            const std::filesystem::path matPath = ( assetsRoot / mat.RelativePath ).lexically_normal();

            // TWO FILES MUST NOT LAND ON ONE MATERIAL FILE. The name is derived from the source's own
            // name, which is NOT unique by construction — a .desce copied from another and edited keeps
            // the original's name, and this repository's own verification protocol relies on exactly that
            // copying. Two such files would produce one path here, the second write would take the
            // first's look, and BOTH would then name a file that describes only one of them: a silent
            // whole-sky loss with nothing in the log. The migration function is pure and per-file, so it
            // cannot see the collision; this loop is the only place in the run that can. Named and fatal,
            // never resolved by guessing at a suffix — the fix is to give the file its own name, which is
            // what the operator has to know.
            const auto entry = claimed.emplace( matPath.generic_string(), source.string() );
            if ( !entry.second && entry.first->second != source.string() )
            {
                err << "FAIL   " << source.string() << " — its cloud material would be written to "
                    << matPath.string() << ", which " << entry.first->second
                    << " already claimed in this run: both state the same name. Give one of them its own "
                    << "name and re-run; neither file is modified.\n";
                return false;
            }

            // Born MATL 2 by the pure scene step, raised here where the content root is known, so ONE run
            // leaves a file this build reads.
            const auto raised = RaiseMaterialV2TextToV4( matPath, mat.Json, refs );
            if ( !raised )
            {
                err << "FAIL   " << source.string() << " — its cloud material " << matPath.string()
                    << " cannot be raised to MATL 4: " << raised.GetError() << "; neither file is modified\n";
                return false;
            }

            std::error_code ec;
            std::filesystem::create_directories( matPath.parent_path(), ec );
            if ( !WriteText( matPath, raised.GetValue(), err ) )
            {
                err << "FAIL   " << matPath.string() << " — the cloud material could not be written; "
                    << source.string() << " is left at its old version\n";
                return false;
            }
            // The FULL path, not the relative name it used to print. An operator reading "wrote
            // Materials/M_X.demat" cannot tell which of two trees it landed in, which is precisely the
            // question this defect turned on; the line now answers it.
            out << "        wrote " << matPath.string() << "\n";
        }
        return true;
    }

    // Writes the `.danimgraph` files the v20 -> v21 step produced, BEFORE the source file that names
    // them, on exactly the terms WriteCloudMaterials states. Returns false on failure, having named it.
    //
    // THE COLLISION RULE IS THE OPPOSITE OF THE MATERIAL ONE, AND THAT IS THE WHOLE POINT OF THE STEP.
    // A cloud material is named after its SCENE, so two scenes landing on one path means one sky is about
    // to be lost and the run must stop. A graph is named after ITSELF, so two entities — in one scene or
    // in twenty — landing on one path is the migration doing its job: a walk graph that was copied into
    // four characters as four identical blobs becomes ONE file that all four share, which is the defect
    // §5.1 named. So a repeated path is fine WHEN THE BYTES AGREE, and fatal when they do not: two
    // different state machines that happen to carry one Name would otherwise silently become whichever of
    // them was written last, with every character on the loser pointing at the winner's graph.
    //
    // The map therefore holds the CONTENT, not the source file name: "who claimed it first" cannot answer
    // "is it the same graph", and this is the one place in the run that can see both.
    bool WriteAnimGraphs( const std::vector<Desert::Migration::AnimGraphFile>& produced,
                          const std::filesystem::path& assetsRoot, const std::filesystem::path& source,
                          std::map<std::string, std::string>& claimed, std::ostream& out, std::ostream& err )
    {
        for ( const auto& graph : produced )
        {
            // Under the SAME root the migration was measured against, for WriteCloudMaterials' reason:
            // the relative path inside the file and the file on disk must agree about one root, and that
            // root is the SOURCE FILE'S rather than the process's working directory.
            const std::filesystem::path graphPath = ( assetsRoot / graph.RelativePath ).lexically_normal();

            const auto entry = claimed.emplace( graphPath.generic_string(), graph.Json );
            if ( !entry.second )
            {
                if ( entry.first->second == graph.Json )
                {
                    // The same graph, reached a second time. Written once, shared from here on — say so,
                    // because "one file, four characters" is the outcome an operator is checking for.
                    out << "        shares " << graphPath.string() << "\n";
                    continue;
                }
                err << "FAIL   " << source.string() << " — its anim graph would be written to "
                    << graphPath.string()
                    << ", which a different graph already claimed in this run: two state machines state "
                    << "the same Name and are not the same graph. Rename one and re-run; no file is "
                    << "modified.\n";
                return false;
            }

            // THE STEP WROTE GENERATION 0 (no header, as a v21 scene's graphs were); the file gains its header
            // here, ONCE PER PATH, so graphs shared by bytes above still share one GUID (T7d, ANGR 0 -> 1).
            const auto raised = RaiseTextToHeader( *TextHeaderRaiseFor( graphPath ), graph.Json,
                                                   Common::Content::AssetGuid::Generate() );
            if ( !raised || !raised.GetValue().has_value() )
            {
                err << "FAIL   " << graphPath.string() << " — the anim graph could not be given its header"
                    << ( raised ? std::string() : ": " + raised.GetError() ) << "; " << source.string()
                    << " is left at its old version\n";
                return false;
            }
            std::error_code ec;
            std::filesystem::create_directories( graphPath.parent_path(), ec );
            // value_or, not *: the has_value() above sits behind the Result, where clang-tidy cannot see it.
            if ( !WriteText( graphPath, raised.GetValue().value_or( std::string() ), err ) )
            {
                err << "FAIL   " << graphPath.string() << " — the anim graph could not be written; "
                    << source.string() << " is left at its old version\n";
                return false;
            }
            out << "        wrote " << graphPath.string() << "\n";
        }
        return true;
    }

    // The v22 -> v23 step's tile files. Each is named by a tile id derived from its entity's, so a path is
    // never claimed twice in a run; a failed write stops the file, which then keeps its old version and its
    // Terrain block. Encoded by the step - here they are only written.
    bool WriteLandscapeTiles( const std::vector<Desert::Migration::LandscapeTileFile>& produced,
                              const std::filesystem::path& source, std::ostream& out, std::ostream& err )
    {
        for ( const auto& tile : produced )
        {
            const std::string bytes( tile.Bytes.begin(), tile.Bytes.end() );
            std::error_code   ec;
            std::filesystem::create_directories( tile.Path.parent_path(), ec );
            if ( !Common::Utils::FileSystem::WriteContentToFileAtomic( tile.Path, bytes ) )
            {
                err << "FAIL   " << tile.Path.string() << " — the landscape tile could not be written; "
                    << source.string() << " is left at its old version\n";
                return false;
            }
        }
        if ( !produced.empty() )
            out << "        wrote " << produced.size() << " landscape tile file(s) under "
                << produced.front().Path.parent_path().string() << "\n";
        return true;
    }

    // WHAT THE CHAIN DID, in one line, for a scene OR a prefab: the same report comes back from
    // both entry points, so the same function prints it and no step can be reported in one file class
    // and silently omitted in the other. §4.7 - a migration that says nothing is a migration nobody can
    // check.
    //
    // `canonical` is null for a prefab, which has no scene-wide Settings block to canonicalise - the
    // same structural argument the chain itself makes with its settings pointer.
    void PrintSteps( std::ostream& out, const Desert::Migration::FileMigrationReport& report,
                     const Desert::Migration::SettingsCanonicalisationReport* canonical )
    {
        if ( report.SkyRaised )
            out << " sky v0->v" << Desert::Migration::kSceneVersionSky << " (" << report.Sky.Entities
                << " entity(ies), " << report.Sky.FieldsCarried << " carried, " << report.Sky.FieldsRejected
                << " rejected)";
        if ( report.TonemapperRaised )
            out << " scene v" << Desert::Migration::kSceneVersionSky << "->v"
                << Desert::Migration::kSceneVersionTonemap << " ("
                << ( report.Tonemap.OperatorPinned ? "tonemapper pinned to Reinhard"
                                                   : "tonemapper NOT pinned — see the warning above" )
                << ( report.Tonemap.SettingsCreated ? ", settings block created" : "" ) << ")";
        if ( report.CloudNoiseRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionTonemap << "->v"
                << Desert::Migration::kSceneVersionCloudNoise << " (";
            if ( report.CloudNoise.Entities > 0 )
                out << report.CloudNoise.FieldsDropped << " cloud bake setting(s) dropped from "
                    << report.CloudNoise.Entities << " entity(ies)";
            else
                out << "stamp only — no cloud layer carried a bake setting";
            out << ")";
        }
        // The three cloud steps below went unreported until 2026-09-06: Changed() was true, the file
        // was rewritten, and the report line skipped straight from v3 to v6 — a silent migration,
        // which §4.7 forbids ("log which scene, from which version to which, and how many fields
        // moved"). Their counters existed all along; only the printing was missing.
        if ( report.CloudSpeciesRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionCloudNoise << "->v"
                << Desert::Migration::kSceneVersionCloudSpecies << " (";
            if ( report.CloudSpecies.Entities > 0 )
                out << report.CloudSpecies.FieldsDropped << " authored shell field(s) dropped and "
                    << report.CloudSpecies.SpeciesSet << " scalar type(s) turned into a species on "
                    << report.CloudSpecies.Entities << " entity(ies)";
            else
                out << "stamp only — no cloud layer carried the scalar-type shape";
            out << ")";
        }
        if ( report.CloudTypeRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionCloudSpecies << "->v"
                << Desert::Migration::kSceneVersionCloudType << " (";
            if ( report.CloudType.Entities > 0 )
            {
                out << report.CloudType.TypesSet << " species enumerator(s) became a .decloudtype path on "
                    << report.CloudType.Entities << " entity(ies)";
                // Named loud, not folded into the count: these two are the cases the operator has to
                // act on — a layer that lost its noise volume renders a different sky until re-pointed.
                if ( report.CloudType.VolumesLost > 0 )
                    out << "; " << report.CloudType.VolumesLost
                        << " layer noise volume(s) DROPPED — re-point them on the cloud type";
                if ( report.CloudType.FieldsBroken > 0 )
                    out << "; " << report.CloudType.FieldsBroken
                        << " unreadable species value(s) left at the default";
            }
            else
            {
                out << "stamp only — no cloud layer named a species";
            }
            out << ")";
        }
        if ( report.CloudSetRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionCloudType << "->v"
                << Desert::Migration::kSceneVersionCloudSet << " (";
            if ( report.CloudSet.Entities > 0 )
                out << report.CloudSet.SlotsCarried << " cloud type(s) moved into slot 1 of the set on "
                    << report.CloudSet.Entities << " entity(ies), " << report.CloudSet.SlotsEmpty
                    << " of them the empty handle";
            else
                out << "stamp only — no cloud layer carried a single-type key";
            out << ")";
        }
        if ( report.TerrainMaterialRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionCloudSet << "->v"
                << Desert::Migration::kSceneVersionTerrainMaterial << " (";
            if ( report.TerrainMaterial.Entities > 0 )
            {
                // Named, not counted, and for the same reason the loader names them: this step DROPS the
                // values it finds, so the operator running this tool has to be able to see what left.
                out << "inline terrain material removed from " << report.TerrainMaterial.Entities
                    << " entity(ies), dropping " << report.TerrainMaterial.Params << " param(s) and "
                    << report.TerrainMaterial.Textures << " texture(s):";
                for ( const auto& name : report.TerrainMaterial.DroppedNames )
                    out << " " << name;
            }
            else
            {
                out << "stamp only — no terrain entity carried an inline material";
            }
            out << ")";
        }
        if ( report.MaterialPathRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionTerrainMaterial << "->v"
                << Desert::Migration::kSceneVersionMaterialPath << " (";
            if ( report.MaterialPath.Paths > 0 )
                out << report.MaterialPath.Paths << " material path(s) made relative to the assets "
                    << "root in " << report.MaterialPath.Entities << " entity(ies)";
            else
                out << "stamp only - no entity named a material by an absolute path";
            // Named, not counted, for the reason the terrain step names its drops: these are the ones the
            // step could not fix, and the operator has to be able to see which slot to re-point.
            for ( const auto& name : report.MaterialPath.OutsideNames )
                out << "; OUTSIDE the assets root, left absolute: " << name;
            out << ")";
        }
        if ( report.GravityUnitsRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionMaterialPath << "->v"
                << Desert::Migration::kSceneVersionGravityUnits << " (";
            if ( !report.GravityUnits.Found )
                out << "stamp only - the scene states no gravity";
            else if ( report.GravityUnits.Scaled )
                out << "gravity " << report.GravityUnits.Before << " -> " << report.GravityUnits.After
                    << " cm/s2 (metre-era value, x100)";
            else if ( report.GravityUnits.Unrecognised )
                // Named rather than counted, for the same reason the two steps above name what they could
                // not fix: this is the one case the operator has to look at by hand.
                out << "gravity " << report.GravityUnits.Before
                    << " LEFT UNCHANGED - neither Earth in metres nor in centimetres, so it was not "
                       "guessed at";
            else if ( report.GravityUnits.Tidied )
                out << "gravity " << report.GravityUnits.Before << " -> " << report.GravityUnits.After
                    << " cm/s2 (already centimetres; dropped the earlier pass's rounding)";
            else
                out << "gravity already " << report.GravityUnits.After << " cm/s2, unchanged";
            out << ")";
        }
        if ( report.UIVisibilityRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionGravityUnits << "->v"
                << Desert::Migration::kSceneVersionUIVisibility << " (";
            if ( report.UIVisibility.Entities > 0 )
                out << report.UIVisibility.FlagsDropped << " interaction flag(s) folded into Hit Test on "
                    << report.UIVisibility.Entities << " element(s), " << report.UIVisibility.HitTestSet
                    << " of which stopped being the default";
            else
                out << "stamp only - no UI element stated an interaction flag";
            // Named, not counted, for the reason the two steps above name what they could not carry: an
            // element whose flag was unreadable keeps the default, and the operator has to see which one.
            for ( const auto& name : report.UIVisibility.BrokenNames )
                out << "; NOT a boolean, left at the default Hit Test: " << name;
            out << ")";
        }
        if ( report.SSRUnitsRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionUIVisibility << "->v"
                << Desert::Migration::kSceneVersionSSRUnits << " (";
            if ( report.SSRUnits.Scaled )
                out << "SSR max distance " << report.SSRUnits.Before << " -> " << report.SSRUnits.After
                    << " cm (metre-era slider value, x100)";
            else
                out << "stamp only - the scene states no SSR max distance";
            out << ")";
        }
        if ( report.CloudMaterialRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionSSRUnits << "->v"
                << Desert::Migration::kSceneVersionCloudMaterial << " (";
            if ( report.CloudMaterial.Entities > 0 )
            {
                out << report.CloudMaterial.ValuesMoved << " value(s) and " << report.CloudMaterial.AssetsMoved
                    << " asset slot(s) moved into " << report.CloudMaterial.Materials.size()
                    << " bespoke cloud material(s), " << report.CloudMaterial.DefaultsAssigned
                    << " layer(s) pointed at the shared " << Desert::Migration::kDefaultCloudMaterialRelativePath
                    << " (D-37), " << report.CloudMaterial.Defaulted << " field(s) left at the schema default";
            }
            else
                out << "stamp only - no VolumetricCloud payload in this scene";
            // Named, not counted, like every step above that can refuse a value: a rejected number is
            // an authored one that will now read as the default, and the operator has to see which.
            for ( const auto& name : report.CloudMaterial.RejectedNames )
                out << "; NOT carried, schema default stands: " << name;
            out << ")";
        }
        if ( report.AnimGraphRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionTextKeySigil << "->v"
                << Desert::Migration::kSceneVersionAnimGraphAsset << " (";
            if ( report.AnimGraph.Entities > 0 || report.AnimGraph.Empty > 0 )
            {
                out << report.AnimGraph.Entities << " state machine(s) moved into "
                    << report.AnimGraph.Graphs.size() << " file(s), " << report.AnimGraph.Empty
                    << " empty GraphJson key(s) dropped";
            }
            else
                out << "stamp only - no Animation payload in this file states a graph";
            // Named, not counted, like every step above that can refuse a value: a rejected blob is an
            // authored state machine that did NOT move, and it is still in the file.
            for ( const auto& name : report.AnimGraph.RejectedNames )
                out << "; LEFT IN PLACE, not moved: " << name;
            out << ")";
        }
        if ( report.EditMeshRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionAnimGraphAsset << "->v"
                << Desert::Migration::kSceneVersionEditMesh << " (";
            if ( report.EditMesh.Entities > 0 )
                out << report.EditMesh.Entities << " edited mesh(es) now saved as an EditMesh:";
            else
                out << "stamp only - no StaticMesh payload in this file states CustomVertices";
            // Named, not counted: the conversion is a WELD by distance, and the operator has to be able to
            // see what it made of each mesh; a rejected one is still in the file, under its v21 keys.
            for ( const auto& name : report.EditMesh.ConvertedNames )
                out << " " << name << ";";
            for ( const auto& name : report.EditMesh.RejectedNames )
                out << "; LEFT IN PLACE, not converted: " << name;
            out << ")";
        }
        if ( report.ProceduralTerrainRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionEditMesh << "->v"
                << Desert::Migration::kSceneVersionProceduralTerrain << " (";
            if ( report.ProceduralTerrain.Entities > 0 )
                out << report.ProceduralTerrain.Entities << " procedural terrain(s) baked into "
                    << report.ProceduralTerrain.Tiles << " landscape tile(s):";
            else
                out << "stamp only - no Terrain payload in this file";
            for ( const auto& name : report.ProceduralTerrain.ConvertedNames )
                out << " " << name << ";";
            for ( const auto& name : report.ProceduralTerrain.RejectedNames )
                out << "; LEFT IN PLACE, not baked: " << name;
            out << ")";
        }
        if ( report.TextureAssetRefsRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionProceduralTerrain << "->v"
                << Desert::Migration::kSceneVersionTextureAssetRefs << " (";
            if ( report.TextureAssetRefs.Rewritten > 0 )
                out << report.TextureAssetRefs.Rewritten << " texture reference(s) now name the asset:";
            else
                out << "stamp only - no cooked:Textures/ reference in this file";
            for ( const auto& name : report.TextureAssetRefs.RewrittenNames )
                out << " " << name << ";";
            for ( const auto& name : report.TextureAssetRefs.MissingNames )
                out << "; NO ASSET under the assets root: " << name;
            out << ")";
        }
        if ( report.TextureGuidsRaised )
            out << " scene v" << Desert::Migration::kSceneVersionMeshGuids << "->v"
                << Desert::Migration::kSceneVersionTextureGuids << " (" << report.TextureGuids.Rewritten
                << " texture/skybox reference(s) now state the .detex header GUID)";
        if ( report.SpriteGuidsRaised )
            out << " scene v" << Desert::Migration::kSceneVersionTextureGuids << "->v"
                << Desert::Migration::kSceneVersionSpriteGuids << " (" << report.SpriteGuids.Rewritten
                << " UI sprite / splash reference(s) now state the .detex header GUID)";
        if ( report.ShaderSceneGuidsRaised )
            out << " scene v" << Desert::Migration::kSceneVersionSpriteGuids << "->v"
                << Desert::Migration::kSceneVersionShaderGuids << " (" << report.ShaderSceneGuids.Rewritten
                << " material shader / render-texture scene reference(s) now state the header GUID)";
        if ( report.TextHeaderRaised )
            out << " scene v" << Desert::Migration::kSceneVersionSiblingOrder << "->v"
                << Desert::Migration::kSceneVersionTextHeader << " (text header stated: kind, GUID, SCNE/UNIT)";
        if ( report.SiblingOrderRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionTextureAssetRefs << "->v"
                << Desert::Migration::kSceneVersionSiblingOrder << " (" << report.SiblingOrder.Indexed
                << " sibling index(es) stated, " << report.SiblingOrder.Reordered
                << " record(s) moved when sorted by id)";
        }
        if ( report.DebugViewRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionCloudMaterial << "->v"
                << Desert::Migration::kSceneVersionDebugView << " (";
            if ( report.DebugView.KeysRemoved > 0 )
            {
                // Named with their values, not counted: these were AUTHORED flags, and the operator has
                // to see that (say) the collider wireframes stopped because the file stopped deciding
                // them - not because something broke.
                out << report.DebugView.KeysRemoved << " viewport debug key(s) removed - the view owns "
                    << "them now (editor Show flags / View Mode):";
                for ( const auto& name : report.DebugView.RemovedNames )
                    out << " " << name;
            }
            else
            {
                out << "stamp only - the scene stated no viewport debug flag";
            }
            out << ")";
        }
        if ( report.ScriptRootRaised )
        {
            // v15, not the previous PRINTED step (v13): 14 and 15 are rows of kRetiredKeys rather
            // than steps of their own, so the last line above this one names 13 and a file arriving
            // here is at 15. Printing the previous printed number would report a transition no file
            // made — the same wrong-transition trap the retired-keys line below documents.
            out << " scene v" << Desert::Migration::kSceneVersionMachineQuality << "->v"
                << Desert::Migration::kSceneVersionScriptRoot << " (";
            if ( report.ScriptRoot.Slots > 0 )
                out << report.ScriptRoot.Slots << " script reference(s) root-tagged on "
                    << report.ScriptRoot.Entities << " entity(ies), " << report.ScriptRoot.Empty
                    << " of them an empty slot";
            else if ( report.ScriptRoot.UnrootedNames.empty() )
                out << "stamp only - no entity named a script";
            else
                out << "no reference could be root-tagged";
            // Named, not counted, like every step above that can refuse a value: a reference the
            // census could not place still does not resolve in a packaged game, and the operator has
            // to see which entity to re-point.
            for ( const auto& name : report.ScriptRoot.UnrootedNames )
                out << "; NOT under a Scripts/ folder, carried over untagged: " << name;
            out << ")";
        }
        if ( report.ServiceAssetRootRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionScriptRoot << "->v"
                << Desert::Migration::kSceneVersionServiceAssetRoot << " (";
            if ( report.ServiceAssetRoot.Refs > 0 )
                out << report.ServiceAssetRoot.Refs << " font/icon/video reference(s) root-tagged on "
                    << report.ServiceAssetRoot.Entities << " entity(ies), " << report.ServiceAssetRoot.Empty
                    << " of them an empty slot";
            else if ( report.ServiceAssetRoot.UnrootedNames.empty() )
                out << "stamp only - no entity named a font, an icon or a video";
            else
                out << "no reference could be root-tagged";
            // Named, not counted, like every step above that can refuse a value: a reference neither
            // root can place still does not resolve in a packaged game.
            for ( const auto& name : report.ServiceAssetRoot.UnrootedNames )
                out << "; under NEITHER content root, carried over untagged: " << name;
            out << ")";
        }
        if ( report.GrassGenerationRaised )
        {
            out << " scene v" << Desert::Migration::kSceneVersionServiceAssetRoot << "->v"
                << Desert::Migration::kSceneVersionGrassGeneration << " (";
            if ( report.GrassGeneration.Entities == 0 )
                out << "stamp only - no terrain stated a grass generator key";
            else
                out << report.GrassGeneration.KeysRemoved << " grass generator key(s) removed from "
                    << report.GrassGeneration.Entities << " terrain(s)";
            // Named with their values, because this is the only place the numbers a designer authored
            // for the generator are ever said again.
            for ( const auto& name : report.GrassGeneration.RemovedNames )
                out << " " << name;
            // And the carry, separately: it is the half of the step that changes what is DRAWN.
            for ( const auto& name : report.GrassGeneration.CarriedNames )
                out << "; carried " << name;
            out << ")";
        }
        if ( report.RetiredKeysRaised )
        {
            // NOT a step's own pair of numbers, unlike every line above: the retirement pass is
            // gated on the head and sweeps a file from WHEREVER it was to wherever the head is now
            // (see MigrateRetiredKeys). Printing a fixed 13->14 here would have reported the wrong
            // transition for every file K3 converted, which stood at 14.
            out << " retired keys -> v" << Desert::Migration::kSceneVersion << " (";
            if ( report.RetiredKeys.KeysRemoved > 0 )
            {
                // Named with their values AND the reason, because from v14 on this is the ONLY way a
                // key ever leaves a file: the saver preserves everything it does not declare, so a
                // removal is always a decision somebody made and the operator is entitled to see it.
                out << report.RetiredKeys.KeysRemoved << " retired key(s) removed:";
                for ( const auto& name : report.RetiredKeys.RemovedNames )
                    out << " " << name;
            }
            else
            {
                out << "stamp only - the scene stated no retired key";
            }
            // A prefab has no Settings block, so there is nothing to canonicalise and no clause to
            // print — not an omission from the line, an absence in the file.
            if ( canonical != nullptr )
            {
                if ( canonical->Refused )
                {
                    out << "; Settings NOT canonicalised - see the error above";
                }
                else
                {
                    out << "; Settings canonical (";
                    if ( canonical->BlockCreated )
                        out << "block created, ";
                    out << canonical->KeysAdded << " field(s) the file did not state, "
                        << canonical->ValuesRestated << " restated at float precision)";
                }
            }
            out << ")";
        }
        if ( report.UnitsRaised )
            out << " units v0->v" << Desert::Migration::kUnitVersion << " (" << report.Units.Entities
                << " entity(ies), " << report.Units.Values << " value(s) x100, " << report.Units.Rejected
                << " rejected)";
    }

} // namespace

namespace Desert::Migration
{
    namespace
    {
        // The one sentence both output-root functions are: derive the assets root from the FILE, through
        // the census row for the folder its own class lives in, and never from where the process stands.
        std::filesystem::path OutputRootFor( Common::Constants::Path::ContentDir dir,
                                             const std::filesystem::path&        contentPath )
        {
            if ( const auto root = Common::Constants::Path::RootForContentPath( dir, contentPath ) )
                return *root;

            // Not under the census folder, so it has nothing to say: the file's own directory is the
            // root. `parent_path()` is empty for a bare `x.desce`, and an empty root would resolve the
            // material against the working directory by a different route — the very thing being fixed —
            // so it is spelled as the current directory explicitly.
            const std::filesystem::path directory = contentPath.parent_path();
            return directory.empty() ? std::filesystem::path( "." ) : directory;
        }
    } // namespace

    std::filesystem::path SceneOutputRoot( const std::filesystem::path& scenePath )
    {
        return OutputRootFor( Common::Constants::Path::ContentDir::Scene, scenePath );
    }

    std::filesystem::path PrefabOutputRoot( const std::filesystem::path& prefabPath )
    {
        return OutputRootFor( Common::Constants::Path::ContentDir::Prefab, prefabPath );
    }

    std::filesystem::path MaterialOutputRoot( const std::filesystem::path& materialPath )
    {
        return OutputRootFor( Common::Constants::Path::ContentDir::Material, materialPath );
    }

    std::span<const ScanExclusion> ScanExclusions()
    {
        static constexpr std::array kExclusions{
             ScanExclusion{ "ThirdParty",
                            "vendored third-party code: its files share our extensions but not our formats "
                            "(assimp's Quake 3 .shader scripts, Ogre .skeleton files)" },
             ScanExclusion{ "build", "build outputs and logs, regenerated from the sources, never content" },
        };
        return kExclusions;
    }

    int RunSceneMigrator( const std::vector<std::string>& args, std::ostream& out, std::ostream& err )
    {
        bool                               check = false;
        std::vector<std::filesystem::path> roots;

        for ( const std::string& arg : args )
        {
            if ( arg == "--check" )
                check = true;
            else
                roots.emplace_back( arg );
        }

        if ( roots.empty() )
        {
            err << "usage: SceneMigrator [--check] <scene.desce | material.demat | prefab.deprefab | "
                   "clip.anim | mesh.stmesh | mesh.skmesh | directory>...\n";
            return 2;
        }

        std::vector<std::filesystem::path> scenes;
        std::vector<std::filesystem::path> materials;
        std::vector<std::filesystem::path> prefabs;
        std::vector<std::filesystem::path> clips;
        std::vector<std::filesystem::path> texts;
        std::vector<std::filesystem::path> meshes;
        std::vector<std::filesystem::path> layouts;
        std::vector<std::filesystem::path> noises;
        std::vector<std::filesystem::path> models;
        std::vector<std::filesystem::path> shaders;
        for ( const auto& root : roots )
            Collect( root, scenes, materials, prefabs, clips, texts, meshes, layouts, noises, models, shaders,
                     out );

        if ( scenes.empty() && materials.empty() && prefabs.empty() && clips.empty() && texts.empty() &&
             meshes.empty() && layouts.empty() && noises.empty() && models.empty() && shaders.empty() )
        {
            err << "SceneMigrator: no " << kSceneExtension << ", " << kMaterialExtension << ", "
                << kPrefabExtension << ", " << kClipExtension
                << ", cooked mesh, cloud layout, cloud noise volume, sculpted cloud volume, shader or other text "
                   "asset "
                   "files found\n";
            return 2;
        }

        int changed = 0;
        int failed  = 0;
        int relaid  = 0;

        // Cloud material FILE -> the scene that produced it, for the collision check below. Keyed on the
        // resolved path and not on the relative name any more: the root is per-scene now, so two scenes
        // sharing a SceneName under two different assets roots produce two different files and are not in
        // conflict at all, while the case the guard exists for — one file, two scenes — is exactly a
        // repeated key here.
        std::map<std::string, std::string> writtenMaterials;

        // The same guard for the graphs, and it holds CONTENT rather than the claiming file — see
        // WriteAnimGraphs for why a repeated path is the intended outcome there and a fatal one here.
        // Shared by the scene pass and the prefab pass, for the reason `writtenMaterials` is: two files of
        // different classes minting one graph name have to be seen through ONE map.
        std::map<std::string, std::string> writtenGraphs;

        // THE OLD MATERIAL NUMBERS, per assets root, loaded once (LegacyMaterialIds.hpp) and - outside
        // --check - written to the register BEFORE any material is rewritten: once a file is v2 its old
        // number is stated nowhere else. Loaded ABOVE the scene pass, because the scene step SCNE 27
        // translates `MaterialGuids` through the same map (and so does the prefab pass).
        std::map<std::filesystem::path, Desert::Migration::LegacyMaterialIdMap> legacyIds;
        const auto                                                              LegacyIdsFor =
             [&]( const std::filesystem::path& root ) -> const Desert::Migration::LegacyMaterialIdMap*
        {
            if ( const auto at = legacyIds.find( root ); at != legacyIds.end() )
                return &at->second;
            auto loaded = Desert::Migration::LoadLegacyMaterialIds( root );
            if ( !loaded )
            {
                err << "FAIL   " << root.string() << " — " << loaded.GetError() << "\n";
                return nullptr;
            }
            if ( !check )
                if ( const auto saved = Desert::Migration::SaveLegacyMaterialIds( root, loaded.GetValue() );
                     !saved )
                {
                    err << "FAIL   " << Desert::Migration::LegacyMaterialIdRegisterPath( root ).string() << " — "
                        << saved.GetError() << "\n";
                    return nullptr;
                }
            return &legacyIds.emplace( root, std::move( loaded.GetValue() ) ).first->second;
        };
        // THE PATH-DERIVED ASSET NUMBERS MATL 2 named slots by, per assets root, rebuilt from the files on disk
        // (LoadLegacyAssetRefs) once per run: the material pass and the cloud materials the scene and prefab
        // passes write both translate through it.
        std::map<std::filesystem::path, Desert::Migration::LegacyAssetRefMap> legacyAssetRefs;
        const auto                                                            LegacyAssetRefsFor =
             [&]( const std::filesystem::path& root ) -> const Desert::Migration::LegacyAssetRefMap*
        {
            if ( const auto at = legacyAssetRefs.find( root ); at != legacyAssetRefs.end() )
                return &at->second;
            auto loaded = Desert::Migration::LoadLegacyAssetRefs( root );
            if ( !loaded )
            {
                err << "FAIL   " << root.string() << " — " << loaded.GetError() << "\n";
                return nullptr;
            }
            return &legacyAssetRefs.emplace( root, std::move( loaded.GetValue() ) ).first->second;
        };
        // THE MESHES, BEFORE the scenes (see IsCookedMesh). A v3 file is left byte-for-byte as it is, so a
        // second run changes nothing. A mesh under <project>/Cooked/Meshes translates its material numbers
        // through the register of <project>/Resources/Assets; one under an assets root's Meshes/ through
        // that root's. A file that is not a cooked mesh this build reads (a JSON-era mesh, a foreign file, a
        // later version) FAILS by name and is left untouched - never "ok".
        for ( const auto& path : meshes )
        {
            const std::string bytes = ReadAll( path );
            // THE MESH ASSET (AF4b/AF4d): a `.stmesh`/`.skmesh` that is an AF1 envelope stamped 'MSAS' carries its
            // editable source and has no step in this tool yet. It is judged by the engine's own reader - the
            // whole envelope, every section hash and the SRCE decode - so a torn file FAILS by name and only a
            // file the editor would open is "ok". Anything not opening with DESTMESH that is not such an asset
            // falls through to the cooked-mesh refusal below, which names what it is instead.
            if ( !std::string_view( bytes ).starts_with( std::string_view(
                      Common::Content::kMeshBinaryMagic, sizeof( Common::Content::kMeshBinaryMagic ) ) ) &&
                 ( bytes.empty() || bytes.front() != '{' ) )
            {
                const auto asset = Desert::Assets::DecodeMeshSourceAsset( std::as_bytes( std::span( bytes ) ) );
                if ( !asset )
                {
                    err << "FAIL   " << path.string() << " — neither a cooked DESTMESH mesh nor a readable mesh "
                        << "asset: " << asset.GetError() << "\n";
                    ++failed;
                    continue;
                }
                out << "ok     " << path.string() << " — mesh asset, MSAS v"
                    << Desert::Assets::kMeshAssetSubsystemVersion << ", GUID "
                    << Common::Content::AssetGuidToText( asset.GetValue().Guid ) << "\n";
                continue;
            }
            const auto version = Desert::Migration::CookedMeshVersion( path.string(), bytes );
            if ( !version )
            {
                err << "FAIL   " << version.GetError() << "\n";
                ++failed;
                continue;
            }
            if ( version.GetValue() == Common::Content::kMeshBinaryVersion )
            {
                out << "ok     " << path.string() << " — already at mesh v" << version.GetValue() << "\n";
                continue;
            }
            std::optional<std::filesystem::path> assetsRoot;
            namespace Paths = Common::Constants::Path;
            if ( auto cooked = Paths::RootForContentPath( Paths::ContentDir::MeshCooked, path ) )
            {
                if ( cooked->filename().empty() )
                    cooked = cooked->parent_path();
                assetsRoot = ( cooked->parent_path() / Paths::SANDBOX_ASSETS_ROOT ).lexically_normal();
            }
            else if ( const auto assets = Paths::RootForContentPath( Paths::ContentDir::Mesh, path ) )
                assetsRoot = *assets;
            if ( !assetsRoot )
            {
                err << "FAIL   " << path.string() << " — lies under neither a Cooked/Meshes nor an assets Meshes "
                    << "folder, so no legacy material register applies to it\n";
                ++failed;
                continue;
            }
            const auto* meshIds = LegacyIdsFor( *assetsRoot );
            if ( meshIds == nullptr )
            {
                ++failed;
                continue;
            }
            const Common::Content::AssetGuid guid = Common::Content::AssetGuid::Generate();
            const auto raised = Desert::Migration::UpgradeMeshBytesToV3( path.string(), bytes, guid, *meshIds );
            if ( !raised )
            {
                err << "FAIL   " << raised.GetError() << "\n";
                ++failed;
                continue;
            }
            out << ( check ? "WOULD  " : "raised " ) << path.string() << " — mesh v" << version.GetValue()
                << " -> v" << Common::Content::kMeshBinaryVersion << ", GUID "
                << Common::Content::AssetGuidToText( guid ) << "\n";
            ++changed;
            if ( check )
                continue;
            if ( const auto written =
                      Common::Utils::FileSystem::WriteContentToFileAtomic( path, raised.GetValue() );
                 !written )
            {
                err << "FAIL   " << path.string() << " — " << written.GetError() << "\n";
                ++failed;
                --changed;
            }
        }

        // THE CLOUD LAYOUTS (container 1 -> 2). The version-1 bytes after magic and version ARE the
        // version-2 payload, so the pass only wraps them: kind CloudLayout, a fresh GUID, the layout version
        // under its own tag. The wrapped file is read back and its payload compared before anything is
        // written. A version-2 file is left byte-for-byte as it is, so a second run changes nothing.
        for ( const auto& path : layouts )
        {
            namespace CC                        = Common::Content;
            const CC::SubsystemVersion kKnown[] = {
                 { Desert::Assets::kCloudLayoutSubsystemTag, Desert::Assets::kCloudLayoutContainerVersion } };
            const std::string bytes     = ReadAll( path );
            const auto*       first     = reinterpret_cast<const std::byte*>( bytes.data() );
            constexpr size_t  kV1Prefix = sizeof( Desert::Assets::kCloudLayoutVersion1Magic ) + 4u;
            if ( bytes.size() >= kV1Prefix &&
                 std::memcmp( bytes.data(), Desert::Assets::kCloudLayoutVersion1Magic,
                              sizeof( Desert::Assets::kCloudLayoutVersion1Magic ) ) != 0 )
            {
                const auto header = CC::ReadEnvelopeHeader( std::span( first, bytes.size() ),
                                                            CC::AssetHeaderReadContext{ kKnown } );
                if ( !header || header.GetValue().Asset.Kind != CC::ContentKind::CloudLayout )
                {
                    err << "FAIL   " << path.string() << " — neither a version-1 'DCLY' layout nor a cloud layout "
                        << "envelope: "
                        << ( header ? "the envelope's kind is not CloudLayout" : header.GetError() ) << "\n";
                    ++failed;
                    continue;
                }
                out << "ok     " << path.string() << " — already at layout v"
                    << Desert::Assets::kCloudLayoutContainerVersion << "\n";
                continue;
            }
            if ( bytes.size() < kV1Prefix )
            {
                err << "FAIL   " << path.string() << " — " << bytes.size() << " bytes, too short for any layout\n";
                ++failed;
                continue;
            }
            uint32_t version = 0;
            std::memcpy( &version, bytes.data() + 4, sizeof( version ) );
            if ( version != 1u )
            {
                err << "FAIL   " << path.string() << " — a bare 'DCLY' container version " << version
                    << "; this tool raises version 1 only\n";
                ++failed;
                continue;
            }

            CC::AssetEnvelope envelope;
            envelope.Asset.Kind       = CC::ContentKind::CloudLayout;
            envelope.Asset.Guid       = CC::AssetGuid::Generate();
            envelope.Asset.Subsystems = { kKnown[0] };
            envelope.Sections.push_back( { CC::EnvelopeSection::Payload, CC::EnvelopeCodec::Stored,
                                           std::vector<std::byte>( first + kV1Prefix, first + bytes.size() ) } );
            const auto wrapped = CC::WriteAssetEnvelope( envelope );
            const auto reread =
                 wrapped ? CC::ReadAssetEnvelope( wrapped.GetValue(), CC::AssetHeaderReadContext{ kKnown } )
                         : Common::MakeFormattedError<CC::AssetEnvelope>( "{}", wrapped.GetError() );
            if ( !reread || !( reread.GetValue().Asset == envelope.Asset ) ||
                 reread.GetValue().Sections != envelope.Sections )
            {
                err << "FAIL   " << path.string() << " — the wrapped layout does not read back: "
                    << ( reread ? std::string( "its header or payload differs" ) : reread.GetError() ) << "\n";
                ++failed;
                continue;
            }
            out << ( check ? "WOULD  " : "raised " ) << path.string() << " — layout v1 -> v"
                << Desert::Assets::kCloudLayoutContainerVersion << ", GUID "
                << CC::AssetGuidToText( envelope.Asset.Guid ) << "\n";
            ++changed;
            if ( check )
                continue;
            if ( const auto written =
                      Common::Utils::FileSystem::WriteBytesToFileAtomic( path, wrapped.GetValue() );
                 !written )
            {
                err << "FAIL   " << path.string() << " — " << written.GetError() << "\n";
                ++failed;
                --changed;
            }
        }

        // THE CLOUD NOISE VOLUMES (bare container 1 or 2 -> DCNV 3). The version-2 bytes after magic and
        // version ARE the version-3 payload; a version-1 file lacks only the origin word the version-2 layout
        // put after the recipe, and a version-1 file could only have come from the generator, so the word
        // written for it is Generated (0) with nothing guessed. Wrapped with a fresh GUID, read back through
        // the engine's own decoder before anything is written; an enveloped file is left byte-for-byte.
        for ( const auto& path : noises )
        {
            namespace CC                        = Common::Content;
            const CC::SubsystemVersion kKnown[] = {
                 { Desert::Assets::kCloudNoiseSubsystemTag, Desert::Assets::kCloudNoiseContainerVersion } };
            const std::string bytes     = ReadAll( path );
            const auto        all       = std::as_bytes( std::span( bytes ) );
            constexpr size_t  kPrefix   = sizeof( Desert::Assets::kCloudNoiseMagic ) + 4u;
            constexpr size_t  kOriginAt = 52u; // payload offset of the origin word version 2 added
            if ( bytes.size() < kPrefix || std::memcmp( bytes.data(), Desert::Assets::kCloudNoiseMagic,
                                                        sizeof( Desert::Assets::kCloudNoiseMagic ) ) != 0 )
            {
                const auto header = CC::ReadEnvelopeHeader( all, CC::AssetHeaderReadContext{ kKnown } );
                if ( !header || header.GetValue().Asset.Kind != CC::ContentKind::CloudNoiseVolume )
                {
                    err << "FAIL   " << path.string() << " — neither a bare 'DCNV' container nor a cloud noise "
                        << "volume envelope: "
                        << ( header ? "the envelope's kind is not CloudNoiseVolume" : header.GetError() ) << "\n";
                    ++failed;
                    continue;
                }
                out << "ok     " << path.string() << " — already at noise volume v"
                    << Desert::Assets::kCloudNoiseContainerVersion << "\n";
                continue;
            }
            uint32_t version = 0;
            std::memcpy( &version, bytes.data() + 4, sizeof( version ) );
            if ( version != 1u && version != 2u )
            {
                err << "FAIL   " << path.string() << " — a bare 'DCNV' container version " << version
                    << "; this tool raises versions 1 and 2 only\n";
                ++failed;
                continue;
            }
            std::vector<std::byte> payload( all.begin() + kPrefix, all.end() );
            if ( version == 1u )
            {
                if ( payload.size() < kOriginAt )
                {
                    err << "FAIL   " << path.string() << " — " << bytes.size()
                        << " bytes, too short for a version-1 noise volume header\n";
                    ++failed;
                    continue;
                }
                payload.insert( payload.begin() + static_cast<std::ptrdiff_t>( kOriginAt ), 4u, std::byte{ 0 } );
            }

            CC::AssetEnvelope envelope;
            envelope.Asset.Kind       = CC::ContentKind::CloudNoiseVolume;
            envelope.Asset.Guid       = CC::AssetGuid::Generate();
            envelope.Asset.Subsystems = { kKnown[0] };
            envelope.Sections.push_back( { CC::EnvelopeSection::Payload, CC::EnvelopeCodec::Stored, payload } );
            const auto wrapped = CC::WriteAssetEnvelope( envelope );
            if ( !wrapped )
            {
                err << "FAIL   " << path.string() << " — " << wrapped.GetError() << "\n";
                ++failed;
                continue;
            }
            std::vector<unsigned char> wrappedBytes( wrapped.GetValue().size() );
            std::memcpy( wrappedBytes.data(), wrapped.GetValue().data(), wrapped.GetValue().size() );
            const auto reread = Desert::Assets::DecodeCloudNoiseVolume( wrappedBytes );
            if ( !reread || !( reread.GetValue().Guid == envelope.Asset.Guid ) )
            {
                err << "FAIL   " << path.string() << " — the wrapped noise volume does not read back: "
                    << ( reread ? std::string( "its GUID differs" ) : reread.GetError() ) << "\n";
                ++failed;
                continue;
            }
            out << ( check ? "WOULD  " : "raised " ) << path.string() << " — noise volume v" << version << " -> v"
                << Desert::Assets::kCloudNoiseContainerVersion << ", GUID "
                << CC::AssetGuidToText( envelope.Asset.Guid ) << "\n";
            ++changed;
            if ( check )
                continue;
            if ( const auto written =
                      Common::Utils::FileSystem::WriteBytesToFileAtomic( path, wrapped.GetValue() );
                 !written )
            {
                err << "FAIL   " << path.string() << " — " << written.GetError() << "\n";
                ++failed;
                --changed;
            }
        }

        // THE SCULPTED CLOUD VOLUMES (bare DCMV 2 -> DCMV 3). The version-2 bytes after magic and version ARE
        // the version-3 payload. There is no version-1 reader anywhere in the tree (version 1 was re-baked by
        // the change that introduced version 2), so a bare version 1 is refused by name rather than guessed at.
        // Wrapped with a fresh GUID, read back through the engine's own decoder before anything is written; an
        // enveloped file is left byte-for-byte.
        for ( const auto& path : models )
        {
            namespace CC                        = Common::Content;
            const CC::SubsystemVersion kKnown[] = { { Desert::Assets::kCloudModellingSubsystemTag,
                                                      Desert::Assets::kCloudModellingContainerVersion } };
            const std::string          bytes    = ReadAll( path );
            const auto                 all      = std::as_bytes( std::span( bytes ) );
            constexpr size_t           kPrefix  = sizeof( Desert::Assets::kCloudModellingMagic ) + 4u;
            if ( bytes.size() < kPrefix || std::memcmp( bytes.data(), Desert::Assets::kCloudModellingMagic,
                                                        sizeof( Desert::Assets::kCloudModellingMagic ) ) != 0 )
            {
                const auto header = CC::ReadEnvelopeHeader( all, CC::AssetHeaderReadContext{ kKnown } );
                if ( !header || header.GetValue().Asset.Kind != CC::ContentKind::CloudModellingVolume )
                {
                    err << "FAIL   " << path.string() << " — neither a bare 'DCMV' container nor a sculpted "
                        << "cloud volume envelope: "
                        << ( header ? "the envelope's kind is not CloudModellingVolume" : header.GetError() )
                        << "\n";
                    ++failed;
                    continue;
                }
                out << "ok     " << path.string() << " — already at sculpted cloud volume v"
                    << Desert::Assets::kCloudModellingContainerVersion << "\n";
                continue;
            }
            uint32_t version = 0;
            std::memcpy( &version, bytes.data() + 4, sizeof( version ) );
            if ( version != 2u )
            {
                err << "FAIL   " << path.string() << " — a bare 'DCMV' container version " << version
                    << "; this tool raises version 2 only\n";
                ++failed;
                continue;
            }

            CC::AssetEnvelope envelope;
            envelope.Asset.Kind       = CC::ContentKind::CloudModellingVolume;
            envelope.Asset.Guid       = CC::AssetGuid::Generate();
            envelope.Asset.Subsystems = { kKnown[0] };
            envelope.Sections.push_back( { CC::EnvelopeSection::Payload, CC::EnvelopeCodec::Stored,
                                           std::vector<std::byte>( all.begin() + kPrefix, all.end() ) } );
            const auto wrapped = CC::WriteAssetEnvelope( envelope );
            if ( !wrapped )
            {
                err << "FAIL   " << path.string() << " — " << wrapped.GetError() << "\n";
                ++failed;
                continue;
            }
            std::vector<unsigned char> wrappedBytes( wrapped.GetValue().size() );
            std::memcpy( wrappedBytes.data(), wrapped.GetValue().data(), wrapped.GetValue().size() );
            const auto reread = Desert::Assets::DecodeCloudModellingVolume( wrappedBytes );
            if ( !reread || !( reread.GetValue().Guid == envelope.Asset.Guid ) )
            {
                err << "FAIL   " << path.string() << " — the wrapped sculpted cloud volume does not read back: "
                    << ( reread ? std::string( "its GUID differs" ) : reread.GetError() ) << "\n";
                ++failed;
                continue;
            }
            out << ( check ? "WOULD  " : "raised " ) << path.string() << " — sculpted cloud volume v" << version
                << " -> v" << Desert::Assets::kCloudModellingContainerVersion << ", GUID "
                << CC::AssetGuidToText( envelope.Asset.Guid ) << "\n";
            ++changed;
            if ( check )
                continue;
            if ( const auto written =
                      Common::Utils::FileSystem::WriteBytesToFileAtomic( path, wrapped.GetValue() );
                 !written )
            {
                err << "FAIL   " << path.string() << " — " << written.GetError() << "\n";
                ++failed;
                --changed;
            }
        }

        // THE SHADERS (SHDR 0 -> 1, T7j). Generation 0 stated nothing; the raised file opens with the comment
        // header line (ShaderAssetHeader.hpp) and a GUID minted HERE, once, and every source byte after it is
        // kept. The header is read back through the loader's own reader before a byte is written. A headed file
        // is left byte for byte once its header is checked; one stating another SHDR is refused by name.
        for ( const auto& path : shaders )
        {
            namespace CC             = Common::Content;
            const std::string source = ReadAll( path );
            if ( source.empty() )
            {
                err << "FAIL   " << path.string() << " — unreadable or empty\n";
                ++failed;
                continue;
            }
            if ( source.starts_with( CC::kShaderHeaderPrefix ) )
            {
                const auto header = CC::ReadShaderHeader( source );
                const int  stated =
                     header ? Desert::Assets::StatedVersion( header.GetValue(), Desert::Assets::kShaderSchemaTag )
                             : -1;
                if ( stated != static_cast<int>( Desert::Assets::kShaderSchemaVersion ) )
                {
                    err << "FAIL   " << path.string() << " — "
                        << ( header ? "states SHDR " + std::to_string( stated ) + "; this tool writes SHDR " +
                                           std::to_string( Desert::Assets::kShaderSchemaVersion )
                                    : header.GetError() )
                        << "\n";
                    ++failed;
                    continue;
                }
                out << "ok     " << path.string() << " — already at shader v"
                    << Desert::Assets::kShaderSchemaVersion << "\n";
                continue;
            }
            const std::array<CC::SubsystemVersion, 1> versions = {
                 CC::SubsystemVersion{ Desert::Assets::kShaderSchemaTag, Desert::Assets::kShaderSchemaVersion } };
            const CC::AssetGuid guid = CC::AssetGuid::Generate();
            const std::string   raised =
                 CC::WriteShaderHeaderLine( CC::MakeTextHeader( CC::ContentKind::Shader, guid, versions ) ) +
                 source;
            const auto reread = CC::ReadShaderHeader( raised );
            if ( !reread || reread.GetValue().Guid != CC::AssetGuidToText( guid ) )
            {
                err << "FAIL   " << path.string() << " — the stamped header does not read back: "
                    << ( reread ? std::string( "its GUID differs" ) : reread.GetError() ) << "\n";
                ++failed;
                continue;
            }
            out << ( check ? "WOULD  " : "raised " ) << path.string() << " — shader v0 -> v"
                << Desert::Assets::kShaderSchemaVersion << ", GUID " << CC::AssetGuidToText( guid ) << "\n";
            ++changed;
            if ( check )
                continue;
            if ( const auto written = Common::Utils::FileSystem::WriteContentToFileAtomic( path, raised );
                 !written )
            {
                err << "FAIL   " << path.string() << " — " << written.GetError() << "\n";
                ++failed;
                --changed;
            }
        }

        for ( const auto& path : scenes )
        {
            const std::string source = ReadAll( path );
            if ( source.empty() )
            {
                err << "FAIL   " << path.string() << " — unreadable or empty\n";
                ++failed;
                continue;
            }

            auto parsed = rfl::json::read<Desert::Migration::SceneSerialized>( source );
            if ( !parsed )
            {
                err << "FAIL   " << path.string() << " — " << parsed.error().what() << "\n";
                ++failed;
                continue;
            }

            // ONE root for this scene, and it is the scene's own (see SceneOutputRoot). It is handed to
            // the migration as well as used for the write below, so the root the v7 -> v8 step measures
            // material paths against and the root the v11 -> v12 material is written under are the same
            // root by construction — the two used to be one global read twice, which is how a path
            // written into the scene could name a place the file was not.
            const std::filesystem::path assetsRoot = SceneOutputRoot( path );
            const auto*                 sceneIds   = LegacyIdsFor( assetsRoot );
            if ( sceneIds == nullptr )
            {
                ++failed; // LegacyIdsFor named the register it could not read
                continue;
            }

            const Desert::Migration::FileMigrationReport report =
                 Desert::Migration::MigrateScene( parsed.value(), assetsRoot, path, *sceneIds );

            // A scene from a LATER build: nothing ran and nothing was stamped, so this is a FAILED file
            // and not an "ok". It used to be neither — the tree fell through every gate and was stamped
            // DOWN to this tool's head, then reported as already current.
            if ( !report.Refused.empty() )
            {
                err << "FAIL   " << path.string() << " — " << report.Refused << "\n";
                ++failed;
                continue;
            }

            // CANONICALISATION IS THE TOOL'S, NOT A SCHEMA STEP'S, and the split is structural rather
            // than tidiness. Every function in SceneMigration.hpp is pure over the parsed tree, which is
            // what lets sixteen suites compile that one translation unit and test a step each with no
            // engine linked; this needs the engine's reflection table, so it lives beside main where the
            // table is already paid for. It runs on the same gate as the retirement step and AFTER it —
            // canonical means "the fields the table describes", and a retired key is by definition not
            // one of them, so running it first would drop the key with nothing left to report.
            const Desert::Migration::SettingsCanonicalisationReport canonical =
                 report.RetiredKeysRaised ? Desert::Migration::CanonicaliseSettings( parsed.value().Settings )
                                          : Desert::Migration::SettingsCanonicalisationReport{};

            if ( !report.Changed() )
            {
                if ( const Layout layout = RelayOutIfNeeded( path, source, check, out, err );
                     layout != Layout::Canonical )
                {
                    ++( layout == Layout::Failed ? failed : relaid );
                    continue;
                }
                out << "ok     " << path.string() << " — already at scene v" << Desert::Migration::kSceneVersion
                    << " / units v" << Desert::Migration::kUnitVersion << "\n";
                continue;
            }

            out << ( check ? "WOULD  " : "raised " ) << path.string() << " —";
            PrintSteps( out, report, &canonical );
            out << "\n";

            if ( check )
            {
                ++changed;
                continue;
            }

            const auto* assetRefs = LegacyAssetRefsFor( assetsRoot );
            if ( assetRefs == nullptr )
            {
                ++failed; // LegacyAssetRefsFor named the file it could not read
                continue;
            }
            if ( !WriteCloudMaterials( report.CloudMaterial.Materials, assetsRoot, *assetRefs, path,
                                       writtenMaterials, out, err ) )
            {
                ++failed;
                continue;
            }

            if ( !WriteAnimGraphs( report.AnimGraph.Graphs, assetsRoot, path, writtenGraphs, out, err ) )
            {
                ++failed;
                continue;
            }

            if ( !WriteLandscapeTiles( report.ProceduralTerrain.Files, path, out, err ) )
            {
                ++failed;
                continue;
            }

            // Write-then-rename, and the verdict comes from the WRITE rather than from the open. This
            // used to open the scene ITSELF with trunc and check only that the open worked — so by the
            // time a full disk, dropped permissions or a killed process stopped the write, the scene
            // was already zero bytes, and the tool still printed "raised" and exited 0 over the wreck.
            // The atomic primitive never opens the original at all; a failure at any step leaves it
            // byte-identical, and is a failed FILE here: counted, named, fatal to the exit code like
            // every FAIL above. The "raised" line above then describes work that was NOT kept, which is
            // why this line says so explicitly.
            if ( !WriteText( path, rfl::json::write( parsed.value() ), err ) )
            {
                err << "FAIL   " << path.string() << " — the raise could not be written; the original file "
                    << "is untouched\n";
                ++failed;
                continue;
            }
            ++changed;
        }

        // THE MATERIALS, AFTER the scenes — a scene's v11 -> v12 raise WRITES `.demat` files, and those
        // are already produced with the current slot names (MigrateCloudMaterialV11ToV12 calls the same
        // step), so this pass finds nothing to do in them and says so. Running it first would depend on
        // whether the file existed yet, which is an ordering nobody should have to know about.
        int materialsChanged = 0;
        int clipsChanged     = 0;
        for ( const auto& path : materials )
        {
            const std::string source = ReadAll( path );
            if ( source.empty() )
            {
                err << "FAIL   " << path.string() << " — unreadable or empty\n";
                ++failed;
                continue;
            }

            // MATL 0 -> 1: a file without a header gets one at v1 FIRST, its GUID derived from its path under
            // the content root (MigrationGuidForPath), so the v1 -> v2 step below has one input shape to read.
            const std::filesystem::path root   = MaterialOutputRoot( path );
            const auto                  stated = Desert::Migration::ReadStatedMaterialIds( path.string(), source );
            if ( !stated )
            {
                err << "FAIL   " << path.string() << " — " << stated.GetError() << "\n";
                ++failed;
                continue;
            }
            // MATL 3 -> 4 (T7k): its own step, an in-place edit of the text (RaiseMaterialV3ToV4); every step
            // further below reads the frozen v2 shape and raises straight to 4.
            if ( stated.GetValue().Version == 3 )
            {
                const auto* assetRefs = LegacyAssetRefsFor( root );
                if ( assetRefs == nullptr )
                {
                    ++failed; // LegacyAssetRefsFor named the file it could not read
                    continue;
                }
                const auto raised = RaiseMaterialV3ToV4( path, source, *assetRefs );
                if ( !raised )
                {
                    err << "FAIL   " << path.string() << " — " << raised.GetError() << "\n";
                    ++failed;
                    continue;
                }
                constexpr std::string_view kWhat = " MATL v3 -> v4, the shader named by its header GUID;";
                if ( check )
                {
                    out << "WOULD  " << path.string() << " —" << kWhat << "\n";
                    ++materialsChanged;
                    continue;
                }
                if ( !WriteText( path, raised.GetValue(), err ) )
                {
                    err << "FAIL   " << path.string() << " — the raise could not be written; the original file "
                        << "is untouched. It would have been:" << kWhat << "\n";
                    ++failed;
                    continue;
                }
                out << "raised " << path.string() << " —" << kWhat << "\n";
                ++materialsChanged;
                continue;
            }
            // A MATL 4 file has nothing left to raise: every step below is for an older shape. Only its layout
            // is checked, so a second run changes nothing.
            if ( stated.GetValue().Version >= Desert::Assets::kMaterialSchemaVersion )
            {
                if ( stated.GetValue().Version > Desert::Assets::kMaterialSchemaVersion )
                {
                    err << "FAIL   " << path.string() << " — states MATL v" << stated.GetValue().Version
                        << ", newer than this tool's v" << Desert::Assets::kMaterialSchemaVersion << "\n";
                    ++failed;
                    continue;
                }
                if ( const Layout layout = RelayOutIfNeeded( path, source, check, out, err );
                     layout != Layout::Canonical )
                {
                    ++( layout == Layout::Failed ? failed : relaid );
                    continue;
                }
                out << "ok     " << path.string() << " — MATL v" << Desert::Assets::kMaterialSchemaVersion << "\n";
                continue;
            }

            std::string text         = source;
            const bool  headerRaised = stated.GetValue().Version == 0;
            if ( headerRaised )
            {
                const std::array<Common::Content::SubsystemVersion, 1> v1 = {
                     Common::Content::SubsystemVersion{ Desert::Assets::kMaterialSchemaTag, 1 } };
                text = PrependHeaderMember(
                     text,
                     rfl::json::write( Common::Content::MakeTextHeader(
                          Common::Content::ContentKind::Material,
                          Desert::Migration::MigrationGuidForPath( path.lexically_relative( root ) ), v1 ) ) );
            }

            // MATL 1 -> 2 (AF7c): MaterialId dropped, ParentMaterialId named by GUID. Spliced into the text,
            // not round-tripped through MaterialData, whose v2 shape no longer has either member.
            Desert::Migration::MaterialV2Report identity;
            const bool                          identityRaised = headerRaised || stated.GetValue().Version == 1;
            if ( identityRaised )
            {
                const auto* ids = LegacyIdsFor( root );
                if ( ids == nullptr )
                {
                    ++failed;
                    continue;
                }
                const auto raised =
                     Desert::Migration::RaiseMaterialTextToV2( path.string(), text, *ids, identity );
                if ( !raised )
                {
                    err << "FAIL   " << path.string() << " — " << raised.GetError() << "\n";
                    ++failed;
                    continue;
                }
                text = raised.GetValue();
            }

            // MATL 2 -> 4 (T6c3, T7k) is the LAST material step and every step before it reads the frozen v2
            // shape.
            auto parsed = rfl::json::read<Desert::Migration::MaterialDataV2>( text );
            if ( !parsed )
            {
                err << "FAIL   " << path.string() << " — " << parsed.error().what() << "\n";
                ++failed;
                continue;
            }

            const Desert::Migration::CloudMaterialLayoutReport report =
                 Desert::Migration::MigrateCloudMaterialLayoutInputs( parsed.value() );
            // BOTH STEPS ALWAYS RUN, and neither short-circuits the other: a `.demat` can need the albedo
            // raise without ever having had a `CloudLayout` binding, and returning early on the first
            // report is how a file gets certified "ok" while still carrying a scalar albedo — which
            // renders as a RED sky, not as a missing feature.
            const Desert::Migration::CloudMaterialAlbedoReport albedo =
                 Desert::Migration::MigrateCloudMaterialAlbedoToColour( parsed.value() );

            const auto* assetRefs = LegacyAssetRefsFor( root );
            if ( assetRefs == nullptr )
            {
                ++failed; // LegacyAssetRefsFor named the file it could not read
                continue;
            }
            const auto raised =
                 Desert::Migration::RaiseMaterialV2ToV4( path.generic_string(), parsed.value(), *assetRefs );
            if ( !raised )
            {
                err << "FAIL   " << raised.GetError() << "\n";
                ++failed;
                continue;
            }
            const auto v3 = Desert::Assets::WriteMaterialJson( raised.GetValue() );
            if ( !v3 )
            {
                err << "FAIL   " << path.string() << " — " << v3.GetError() << "\n";
                ++failed;
                continue;
            }

            // WHAT WAS RAISED, phrased ONCE and printed only where it is true. It used to be printed
            // before the write, which meant a refused write reported "raised <file>" and then "FAIL
            // <file>" — a claim of an action that had not happened, beside the correction. That is the
            // Д31 class with its sign flipped, and it was found by making the write refuse on purpose.
            std::ostringstream what;
            if ( report.Changed() )
                what << " " << report.Split
                     << " CloudLayout binding(s) split into LayoutPattern + LayoutMask, both naming the "
                        "same painting;";
            if ( headerRaised )
                what << " text header stated;";
            if ( identityRaised )
            {
                what << " MATL v2";
                if ( identity.DroppedId )
                    what << ", MaterialId dropped (the header GUID is the identity)";
                if ( identity.Parented )
                    what << ", ParentMaterialId -> Parent GUID";
                what << ";";
            }
            if ( albedo.Changed() )
                what << " " << albedo.Broadcast
                     << " scalar ScatteringAlbedo value(s) broadcast to a neutral colour;";
            what << " MATL v" << Desert::Assets::kMaterialSchemaVersion << ", " << parsed.value().Textures.size()
                 << " slot(s) named by header GUID;";

            if ( check )
            {
                out << "WOULD  " << path.string() << " —" << what.str() << "\n";
                ++materialsChanged;
                continue;
            }

            if ( !WriteText( path, v3.GetValue(), err ) )
            {
                err << "FAIL   " << path.string() << " — the raise could not be written; the original file "
                    << "is untouched. It would have been:" << what.str() << "\n";
                ++failed;
                continue;
            }
            out << "raised " << path.string() << " —" << what.str() << "\n";
            ++materialsChanged;
        }

        // ---- THE CLIPS ----------------------------------------------------------------------------
        //
        // Their own generation sequence, their own gate. A `.anim` with no version field is generation 0
        // and is converted; one already at the current generation is REFUSED by the migration function
        // rather than converted again — this step is NOT idempotent, and reading integer ticks as seconds
        // is exactly the doubling the text-sigil step once shipped (`#menu.play` -> `##menu.play`).
        for ( const auto& path : clips )
        {
            const std::string source = ReadAll( path );
            if ( source.empty() )
            {
                err << "FAIL   " << path.string() << " — unreadable or empty\n";
                ++failed;
                continue;
            }

            // Generations 0-2 are converted to 3 first (their keys and sections); a generation-3 text is
            // raised as it is. Either way the header raise is the last step and mints the clip's GUID once.
            Desert::Assets::Serialization::AnimationMigrationReport report;
            const auto migrated = Desert::Assets::Serialization::MigrateAnimationJson( source, report );
            if ( !migrated )
            {
                // A clip that already states its header is the ordinary case on a second run, and it is
                // reported as "ok" rather than as a failure — the refusal is what keeps a second run from
                // doubling the conversion, not a sign that anything is wrong.
                if ( report.FromVersion >= Desert::Assets::Serialization::kAnimationVersion )
                {
                    if ( const Layout layout = RelayOutIfNeeded( path, source, check, out, err );
                         layout != Layout::Canonical )
                    {
                        ++( layout == Layout::Failed ? failed : relaid );
                        continue;
                    }
                    out << "ok     " << path.string() << " — already at `.anim` generation " << report.FromVersion
                        << "\n";
                    continue;
                }
                if ( report.FromVersion != Desert::Assets::Serialization::kAnimationLastVersionMember )
                {
                    err << "FAIL   " << path.string() << " — " << migrated.GetError() << "\n";
                    ++failed;
                    continue;
                }
            }

            std::ostringstream what;
            if ( migrated )
            {
                // WHAT ACTUALLY HAPPENED TO THIS FILE, which is not the same sentence for both steps. A
                // generation-0 file has its key times moved from float seconds onto the tick grid; a
                // generation-1 one keeps every tick it had and gains the SHAPE each key now states. Printing
                // the first sentence for both was true of the corpus on the day it was written and false the
                // day a second step existed.
                std::ostringstream what;
                if ( report.FromVersion < 1 )
                {
                    what << " generation " << report.FromVersion << ": key times moved from float seconds onto "
                         << "the " << Desert::Animation::PROJECT_TICK_RATE.Numerator << "-tick grid; display rate "
                         << report.DisplayRateNumerator << " fps"
                         << ( report.DisplayRateIsAFallback ? " (no standard grid fits its keys, defaulted)" : "" )
                         << "; " << report.KeysMoved << " key time(s) rounded";
                    if ( report.KeysMoved > 0 )
                    {
                        what << ", worst by " << report.WorstMicro << " millionths of a tick";
                    }
                    what << ";";
                }
                else
                {
                    what << " generation " << report.FromVersion << ": every tick kept; display rate "
                         << report.DisplayRateNumerator << " fps carried;";
                }
                what << " " << report.ShapesWritten
                     << " key(s) now STATE their interpolation and tangent mode instead of inheriting a silent "
                        "default;";
                // THE GENERATION-3 SENTENCE, and it is a separate one because it is a separate claim: the
                // step from 2 adds NO key shapes (they were already stated) and adds the section instead, so
                // a line reporting only `ShapesWritten` would say "0" and read as a conversion that did
                // nothing.
                what << " " << report.SectionsWritten
                     << " section(s) now STATE the range, blend type and weight its values are read under;";
            }
            const Common::Content::AssetGuid guid = Common::Content::AssetGuid::Generate();
            const auto                       raised =
                 RaiseTextToHeader( *TextHeaderRaiseFor( path ), migrated ? migrated.GetValue() : source, guid );
            if ( !raised || !raised.GetValue().has_value() )
            {
                err << "FAIL   " << path.string() << " — the header raise "
                    << ( raised ? std::string( "found a header already" ) : raised.GetError() ) << "\n";
                ++failed;
                continue;
            }
            what << " generation " << Desert::Assets::Serialization::kAnimationLastVersionMember << " -> "
                 << Desert::Assets::Serialization::kAnimationVersion << ": the version now lives in the text "
                 << "asset header, GUID " << Common::Content::AssetGuidToText( guid ) << ";";

            if ( check )
            {
                out << "WOULD  " << path.string() << " —" << what.str() << "\n";
                ++clipsChanged;
                continue;
            }

            if ( !WriteText( path, *raised.GetValue(), err ) )
            {
                err << "FAIL   " << path.string() << " — the conversion could not be written; the original "
                    << "file is untouched. It would have been:" << what.str() << "\n";
                ++failed;
                continue;
            }
            out << "raised " << path.string() << " —" << what.str() << "\n";
            ++clipsChanged;
        }

        // THE PREFABS, through the SAME chain the scenes went through (И11). They come last for the
        // reason the materials do: a prefab's v11 -> v12 raise can produce a `.demat` under a name a
        // scene may already have claimed in this run, and `writtenMaterials` is the one map that sees it.
        int prefabsChanged = 0;
        for ( const auto& path : prefabs )
        {
            const std::string source = ReadAll( path );
            if ( source.empty() )
            {
                err << "FAIL   " << path.string() << " — unreadable or empty\n";
                ++failed;
                continue;
            }

            // rfl::json::read and NOT the engine's ParseLoadablePrefab: that gate REFUSES everything but
            // the head, which is precisely the population this tool exists to convert. The gate is
            // applied to the OUTPUT instead, below, where it belongs.
            auto parsed = rfl::json::read<Desert::Migration::PrefabData>( source );
            if ( !parsed )
            {
                err << "FAIL   " << path.string() << " — " << parsed.error().what() << "\n";
                ++failed;
                continue;
            }

            const std::filesystem::path assetsRoot = PrefabOutputRoot( path );

            const auto* prefabIds = LegacyIdsFor( assetsRoot );
            if ( prefabIds == nullptr )
            {
                ++failed; // LegacyIdsFor named the register it could not read
                continue;
            }

            const Desert::Migration::PrefabMigrationOutcome outcome =
                 Desert::Migration::MigratePrefab( parsed.value(), assetsRoot, path, *prefabIds );

            if ( !outcome.Refused.empty() )
            {
                err << "FAIL   " << path.string() << " — " << outcome.Refused << "\n";
                ++failed;
                continue;
            }
            if ( outcome.AlreadyCurrent )
            {
                if ( const Layout layout = RelayOutIfNeeded( path, source, check, out, err );
                     layout != Layout::Canonical )
                {
                    ++( layout == Layout::Failed ? failed : relaid );
                    continue;
                }
                out << "ok     " << path.string() << " — already at scene v" << Desert::Migration::kSceneVersion
                    << " / units v" << Desert::Migration::kUnitVersion << "\n";
                continue;
            }

            out << ( check ? "WOULD  " : "raised " ) << path.string() << " —";
            if ( outcome.StampOnly )
            {
                // The one case with no step behind it, and it says so rather than printing an empty
                // line: an operator whose prefab looks wrong afterwards has to be able to see that this
                // file was stamped on an ASSUMPTION about its generation, not migrated.
                out << " unversioned (v0/v0) stamped to scene v" << Desert::Migration::kSceneVersion
                    << " / units v" << Desert::Migration::kUnitVersion
                    << " (stamp only; entities untouched — an unstamped prefab states no generation to "
                       "migrate FROM)";
            }
            else
            {
                out << " from scene v" << outcome.FoundSceneVersion << ":";
                // No canonicalisation clause: a prefab has no Settings block (see PrintSteps).
                PrintSteps( out, outcome.Steps, nullptr );
            }
            out << "\n";

            if ( check )
            {
                ++prefabsChanged;
                continue;
            }

            const auto* assetRefs = LegacyAssetRefsFor( assetsRoot );
            if ( assetRefs == nullptr )
            {
                ++failed; // LegacyAssetRefsFor named the file it could not read
                continue;
            }
            if ( !WriteCloudMaterials( outcome.Steps.CloudMaterial.Materials, assetsRoot, *assetRefs, path,
                                       writtenMaterials, out, err ) )
            {
                ++failed;
                continue;
            }

            if ( !WriteAnimGraphs( outcome.Steps.AnimGraph.Graphs, assetsRoot, path, writtenGraphs, out, err ) )
            {
                ++failed;
                continue;
            }

            if ( !WriteLandscapeTiles( outcome.Steps.ProceduralTerrain.Files, path, out, err ) )
            {
                ++failed;
                continue;
            }

            // Serialized through the ENGINE'S OWN writer, so the bytes written are the bytes the saver
            // would produce, and then gate-checked with the ENGINE'S OWN loader gate before a byte
            // reaches the target: "the tool wrote it" and "the engine will load it" cannot drift. The
            // write itself is the shared atomic primitive — the original is byte-identical on any
            // failure, which is the guarantee И2 had to add after this tool truncated a file it then
            // reported as raised.
            const auto written =
                 Desert::Assets::WritePrefabJson( Desert::Migration::ToEnginePrefab( parsed.value() ) );
            if ( !written )
            {
                err << "FAIL   " << path.string() << " — " << written.GetError() << " (original untouched)\n";
                ++failed;
                continue;
            }
            const std::string& migrated = written.GetValue();
            if ( auto loadable = Desert::Assets::ParseLoadablePrefab( path.string(), migrated ); !loadable )
            {
                err << "FAIL   " << path.string() << " — the migrated text does not pass the engine's own "
                    << "gate: " << loadable.GetError() << " (original untouched)\n";
                ++failed;
                continue;
            }
            if ( !WriteText( path, migrated, err ) )
            {
                err << "FAIL   " << path.string() << " — the raise could not be written; the original file "
                    << "is untouched\n";
                ++failed;
                continue;
            }
            ++prefabsChanged;
        }

        for ( const auto& path : texts )
        {
            if ( const TextHeaderRaise* row = TextHeaderRaiseFor( path ) )
            {
                std::string                      text   = ReadAll( path );
                const Common::Content::AssetGuid guid   = Common::Content::AssetGuid::Generate();
                const auto                       raised = RaiseTextToHeader( *row, text, guid );
                if ( !raised )
                {
                    err << "FAIL   " << path.string() << " — " << raised.GetError() << "\n";
                    ++failed;
                    continue;
                }
                std::ostringstream steps;
                if ( raised.GetValue().has_value() )
                {
                    steps << Common::Content::KindName( row->Kind ) << " v" << row->FromVersion << " -> v"
                          << row->ToVersion << ", GUID " << Common::Content::AssetGuidToText( guid );
                    text = *raised.GetValue();
                }
                // Chained in the same run, so a v1 retarget lands at RTGT 3 and never waits on disk at 2.
                if ( row->Kind == Common::Content::ContentKind::Retarget )
                {
                    const auto rigged = RaiseRetargetV2ToV3( path, text );
                    if ( !rigged )
                    {
                        err << "FAIL   " << path.string() << " — RTGT 2 -> 3: " << rigged.GetError()
                            << " (original untouched)\n";
                        ++failed;
                        continue;
                    }
                    if ( rigged.GetValue().has_value() )
                    {
                        steps << ( steps.tellp() > 0 ? "; " : "" ) << "RTGT 2 -> 3, source rig by GUID";
                        text = *rigged.GetValue();
                    }
                }
                // Chained the same way, so a CLTY 3 type lands at CLTY 5.
                if ( row->Kind == Common::Content::ContentKind::CloudType )
                {
                    const auto named = RaiseCloudTypeV4ToV5( path, text );
                    if ( !named )
                    {
                        err << "FAIL   " << path.string() << " — CLTY 4 -> 5: " << named.GetError()
                            << " (original untouched)\n";
                        ++failed;
                        continue;
                    }
                    if ( named.GetValue().has_value() )
                    {
                        steps << ( steps.tellp() > 0 ? "; " : "" ) << "CLTY 4 -> 5, noise volume by GUID";
                        text = *named.GetValue();
                    }
                }
                if ( steps.tellp() > 0 )
                {
                    out << ( check ? "WOULD  " : "raised " ) << path.string() << " — " << steps.str() << "\n";
                    ++changed;
                    if ( !check && !WriteText( path, text, err ) )
                        ++failed;
                    continue;
                }
            }
            if ( const Layout layout = RelayOutIfNeeded( path, ReadAll( path ), check, out, err );
                 layout != Layout::Canonical )
                ++( layout == Layout::Failed ? failed : relaid );
        }

        out << "SceneMigrator: " << scenes.size() << " scene(s), " << changed
            << ( check ? " would change, " : " raised, " ) << clips.size() << " clip(s) of which " << clipsChanged
            << ( check ? " would change, " : " raised, " ) << materials.size() << " material(s), "
            << materialsChanged << ( check ? " would change, " : " raised, " ) << prefabs.size() << " prefab(s), "
            << prefabsChanged << ( check ? " would change, " : " raised, " ) << texts.size()
            << " other text asset(s), " << relaid << ( check ? " would be re-laid-out, " : " re-laid-out, " )
            << failed << " failed\n";

        if ( failed > 0 )
            return 1;
        return ( check && ( changed > 0 || materialsChanged > 0 || prefabsChanged > 0 || relaid > 0 ) ) ? 1 : 0;
    }
} // namespace Desert::Migration
