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
#include <Engine/Assets/Serialization/FoliageType.hpp>
#include <Engine/Assets/CloudNoiseVolume.hpp>
#include <Engine/Assets/MaterialFormat.hpp>
#include <Engine/Assets/CloudLayout.hpp>
#include <Engine/Assets/CloudModellingVolume.hpp>
#include <Engine/Assets/MeshSourceAsset.hpp>
#include <Engine/World/Landscape/LandscapeData.hpp>
#include <Engine/World/Landscape/LandscapeTileFiles.hpp>
#include "MigratorMain.hpp"
#include "SceneMigration.hpp"
#include <Engine/Core/Serialize/ExternalEntities.hpp>
#include "SettingsCanonical.hpp"

#include <Common/Content/ShaderAssetHeader.hpp>
#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/CanonicalText.hpp>
#include <Common/Content/MeshBinaryHeader.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Utilities/Crc32c.hpp>
#include <Common/Utilities/FileSystem.hpp>

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

    // CLIPS ARE COLLECTED TOO, since A5: a second binary over a second corpus is a command somebody forgets.
    // A `.anim` does NOT share the scene's version integers - it states its own ANIM generation, because a
    // scene names a clip by NAME - and is gated on that number alone.
    constexpr const char* kClipExtension = ".anim";
    constexpr const char* kShaderExtension = ".shader";

    // THE OTHER TEXT ASSETS ARE COLLECTED FOR THEIR LAYOUT ONLY (AF6e). Their content has no step in this
    // tool - each is versioned by its own loader - but their text is written by the canonical writer, so a
    // file in the tree that predates it is re-laid-out here, version untouched, and the next save of it
    // diffs only in what the save changed. `.dclayout` is not here: it is binary, and has its own pass
    // (IsCloudLayout).
    // `.defoliage` rides with them: its one content step (FOLT 1 -> 2, FO-3) runs in the same loop.
    constexpr std::array kLayoutOnlyExtensions{ ".danimgraph", ".dgraph",   ".decloudtype",
                                                ".destrings",  ".detheme",  ".derig",
                                                ".retarget",   ".skeleton", ".defoliage" };

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

    // CLOUD LAYOUTS ARE COLLECTED TOO: only the enveloped DCLY 2 is read; a bare "DCLY" 1 is refused.
    bool IsCloudLayout( const std::filesystem::path& path )
    {
        return path.extension() == Desert::Assets::kCloudLayoutExtension;
    }

    // CLOUD NOISE VOLUMES ARE COLLECTED TOO: only the enveloped DCNV 3 is read; a bare "DCNV" 1/2 container is
    // refused by its number.
    bool IsCloudNoiseVolume( const std::filesystem::path& path )
    {
        return path.extension() == Desert::Assets::kCloudNoiseVolumeExtension;
    }

    // SCULPTED CLOUD VOLUMES (.dcmv) LIKEWISE: only the enveloped DCMV 3 is read; a bare "DCMV" 2 is refused.
    bool IsCloudModellingVolume( const std::filesystem::path& path )
    {
        return path.extension() == Desert::Assets::kCloudModellingVolumeExtension;
    }

    // LANDSCAPE HEIGHT TILES (.dlht) ARE COLLECTED TOO: binary 'DLHT' blobs next to their scene
    // (LandscapeTileFiles.hpp). Only the current container version is read by the engine.
    bool IsLandscapeTile( const std::filesystem::path& path )
    {
        return path.extension() == Desert::World::Landscape::kLandscapeTileExtension;
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
                  std::vector<std::filesystem::path>& shaders, std::vector<std::filesystem::path>& tiles,
                  std::ostream& out )
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
                else if ( IsLandscapeTile( entry.path() ) )
                    tiles.push_back( entry.path() );
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
        else if ( IsLandscapeTile( root ) )
            tiles.push_back( root );
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

    // A SCENE goes through the engine's one scene writer (ExternalEntities::WriteSceneFile), partitioned or not,
    // so a migrated scene and a scene the editor saved are laid out alike, file for file: a partitioned world as
    // its header plus one file per entity, any other scene whole and canonical.
    bool WriteScene( const std::filesystem::path& path, const std::string& json, std::ostream& err )
    {
        const auto written = Desert::Core::ExternalEntities::WriteSceneText( path, json );
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

    // THE TEXT KINDS WITH A HEADER VERSION, each read at its current generation only. The steps that raised
    // older generations (headerless text, RTGT 2 -> 3, CLTY 4 -> 5, the clip conversions) were deleted with
    // the other legacy steps (LEG1): a file stating anything else is refused by its number, never raised.
    struct TextHeaderGate
    {
        const char* Extension;
        const char* Tag; // the key the header's Versions map states the generation under
        uint32_t    Current;
    };

    constexpr std::array kTextHeaderGates{
         TextHeaderGate{ ".decloudtype", "CLTY", Desert::Assets::kCloudTypeSchemaVersion },
         TextHeaderGate{ ".destrings", "STRT", Desert::Assets::kStringTableSchemaVersion },
         TextHeaderGate{ ".detheme", "UITH", Desert::Assets::kUIThemeSchemaVersion },
         TextHeaderGate{ ".derig", "CRIG", Desert::Assets::kControlRigSchemaVersion },
         TextHeaderGate{ ".retarget", "RTGT", Desert::Assets::kRetargetSchemaVersion },
         TextHeaderGate{ ".danimgraph", "ANGR", Desert::Assets::kAnimGraphSchemaVersion },
         TextHeaderGate{ ".skeleton", "SKEL", Desert::Assets::kSkeletonSchemaVersion },
         TextHeaderGate{ ".anim", "ANIM", Desert::Assets::kAnimationSchemaVersion },
    };

    const TextHeaderGate* TextHeaderGateFor( const std::filesystem::path& path )
    {
        const std::string ext = path.extension().string();
        for ( const TextHeaderGate& row : kTextHeaderGates )
            if ( ext == row.Extension )
                return &row;
        return nullptr;
    }

    // The generation `text` states under `tag`, or the reason it states none. Headerless text is an older
    // generation this tool no longer reads, and is refused as such.
    Common::ResultStr<uint32_t> ReadStatedVersion( const std::filesystem::path& path, const std::string& text,
                                                   const char* tag )
    {
        const std::string  where = "'" + path.string() + "'";
        std::istringstream in( text );
        const auto         header = Common::Content::ReadTextHeaderObject( in );
        if ( !header )
            return Common::MakeError<uint32_t>( where + " states no readable text header (" + header.GetError() +
                                                "); files older than their first headed generation are not "
                                                "supported" );
        const auto parsed = rfl::json::read<Common::Content::TextAssetHeaderSerialized>( header.GetValue() );
        if ( !parsed )
            return Common::MakeError<uint32_t>( where + ": unreadable header: " + parsed.error().what() );
        const auto stated = parsed.value().Versions.find( tag );
        if ( stated == parsed.value().Versions.end() )
            return Common::MakeError<uint32_t>( where + ": the header states no " + tag + " version" );
        return Common::MakeSuccess( stated->second );
    }

    // Refuses (false, reason on `err`) a file of a gated kind whose stated generation is not the current one.
    bool PassesTextHeaderGate( const TextHeaderGate& row, const std::filesystem::path& path,
                               const std::string& text, std::ostream& err )
    {
        const auto stated = ReadStatedVersion( path, text, row.Tag );
        if ( !stated )
        {
            err << "FAIL   " << stated.GetError() << "\n";
            return false;
        }
        if ( stated.GetValue() != row.Current )
        {
            err << "FAIL   " << path.string() << " — states " << row.Tag << " v" << stated.GetValue()
                << ", and this tool reads v" << row.Current << " only; older files are not supported\n";
            return false;
        }
        return true;
    }

    using TextWriter = bool ( * )( const std::filesystem::path&, const std::string&, std::ostream& );

    // `write` is WriteScene for a scene (the one scene writer) and WriteText for every other text asset.
    Layout RelayOutIfNeeded( const std::filesystem::path& path, const std::string& source, bool check,
                             std::ostream& out, std::ostream& err, TextWriter write = WriteText )
    {
        if ( Common::Content::IsCanonicalJsonText( source ) )
            return Layout::Canonical;
        if ( check )
        {
            out << "WOULD  " << path.string() << " — text layout only (canonical text), version unchanged\n";
            return Layout::Relaid;
        }
        if ( !write( path, source, err ) )
        {
            err << "FAIL   " << path.string() << " — the layout could not be written; the original file is "
                << "untouched\n";
            return Layout::Failed;
        }
        out << "relaid " << path.string() << " — text layout only (canonical text), version unchanged\n";
        return Layout::Relaid;
    }

    // THE COOKED MESH GENERATION this tool reads: only the current one. The signature is checked before the
    // version - bytes 12..15 of any file read as a number, and a JSON mesh's `":fal"` reads as 1818322490.
    Common::ResultStr<uint32_t> CookedMeshVersion( const std::string_view source, const std::string_view bytes )
    {
        namespace Content = Common::Content;
        const auto Fail   = [&]( const std::string& why )
        { return Common::MakeFormattedError<uint32_t>( "'{}' {}", std::string( source ), why ); };

        if ( bytes.size() < sizeof( Content::kMeshBinaryMagic ) ||
             std::memcmp( bytes.data(), Content::kMeshBinaryMagic, sizeof( Content::kMeshBinaryMagic ) ) != 0 )
        {
            if ( !bytes.empty() && bytes.front() == '{' )
                return Fail( "is a JSON mesh (pre-binary format) — re-import it from its source" );
            return Fail( "is an unknown mesh format: it does not open with the cooked-mesh signature DESTMESH" );
        }
        Content::MeshBinaryFileHeader header{};
        if ( bytes.size() < sizeof( header ) )
            return Fail( "is " + std::to_string( bytes.size() ) + " bytes, shorter than a mesh header" );
        std::memcpy( &header, bytes.data(), sizeof( header ) );
        if ( header.ByteOrder != Content::kMeshBinaryByteOrderTag )
            return Fail( "is a cooked mesh of the other byte order; re-cook it on this host" );
        if ( header.Version != Content::kMeshBinaryVersion )
            return Fail( "is mesh version " + std::to_string( header.Version ) + "; this tool reads only v" +
                         std::to_string( Content::kMeshBinaryVersion ) +
                         " (older cooked meshes are not supported - re-cook it from its source)" );
        return Common::MakeSuccess( header.Version );
    }

    // The MATL generation a `.demat` states in its text header. A file with no header, or a header naming no
    // MATL version, predates MATL v1 and is refused by name rather than read as generation 0.
    Common::ResultStr<uint32_t> ReadMaterialSchemaVersion( const std::filesystem::path& path,
                                                           const std::string&           text )
    {
        return ReadStatedVersion( path, text, "MATL" );
    }

    void PrintSteps( std::ostream& out, const Desert::Migration::FileMigrationReport& report )
    {
        if ( report.ExternalEntitiesRaised )
        {
            out << " scene ->v" << Desert::Migration::kSceneVersionExternalEntities;
            if ( report.EntitiesMovedOut > 0 )
                out << " (" << report.EntitiesMovedOut << " entit"
                    << ( report.EntitiesMovedOut == 1 ? "y" : "ies" ) << " moved to one file each)";
        }
        if ( report.PathOnlyMeshGuidsRaised )
            out << " scene v" << Desert::Migration::kSceneVersionShaderGuids << "->v"
                << Desert::Migration::kSceneVersionPathOnlyMeshGuids << " (" << report.PathOnlyMeshGuids.Rewritten
                << " path-only mesh reference(s) now state the mesh header GUID)";
        if ( report.LandscapeLayerRefsRaised )
            out << " scene v" << Desert::Migration::kSceneVersionFoliageTypes << "->v"
                << Desert::Migration::kSceneVersionLandscapeLayerRefs
                << " (landscape layers are .delayerinfo references; no inline layer in this file)";
        if ( report.FoliageTypesRaised )
            out << " scene v" << Desert::Migration::kSceneVersionPathOnlyMeshGuids << "->v"
                << Desert::Migration::kSceneVersionFoliageTypes << " (" << report.FoliageTypes.Rewritten
                << " Foliage block(s) now name a .defoliage; " << report.FoliageTypes.NewTypes.size()
                << " type file(s) written)";
    }

    // The `.defoliage` files the v33 step minted, written BEFORE the scene that names them, so a scene never
    // lands on disk naming a type that is not there.
    bool WriteNewFoliageTypes( const Desert::Migration::FileMigrationReport& report, std::ostream& err )
    {
        for ( const auto& [file, text] : report.FoliageTypes.NewTypes )
        {
            std::error_code ec;
            std::filesystem::create_directories( file.parent_path(), ec );
            if ( !WriteText( file, text, err ) )
            {
                err << "FAIL   " << file.string() << " — the foliage type could not be written\n";
                return false;
            }
        }
        return true;
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

    // One pass of the tool over `args`. `dryRun` forces --check whatever `args` say; `failedOut` receives the
    // number of files the pass refused, which RunSceneMigrator's all-or-nothing verdict reads.
    static int RunSceneMigratorPass( const std::vector<std::string>& args, bool dryRun, std::ostream& out,
                                     std::ostream& err, int& failedOut )
    {
        failedOut                                = 0;
        bool                               check = dryRun;
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
        std::vector<std::filesystem::path> tiles;
        for ( const auto& root : roots )
            Collect( root, scenes, materials, prefabs, clips, texts, meshes, layouts, noises, models, shaders,
                     tiles, out );

        if ( scenes.empty() && materials.empty() && prefabs.empty() && clips.empty() && texts.empty() &&
             meshes.empty() && layouts.empty() && noises.empty() && models.empty() && shaders.empty() &&
             tiles.empty() )
        {
            err << "SceneMigrator: no " << kSceneExtension << ", " << kMaterialExtension << ", "
                << kPrefabExtension << ", " << kClipExtension
                << ", cooked mesh, cloud layout, cloud noise volume, sculpted cloud volume, landscape tile, "
                   "shader "
                   "or other text "
                   "asset "
                   "files found\n";
            return 2;
        }

        int changed = 0;
        int failed  = 0;
        int relaid  = 0;

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
            const auto version = CookedMeshVersion( path.string(), bytes );
            if ( !version )
            {
                err << "FAIL   " << version.GetError() << "\n";
                ++failed;
                continue;
            }
            out << "ok     " << path.string() << " — already at mesh v" << version.GetValue() << "\n";
        }

        // THE CLOUD LAYOUTS: only the enveloped generation (container 2) is read. A bare version-1 'DCLY'
        // container is refused by its number - its step was deleted with the other legacy steps (LEG1).
        for ( const auto& path : layouts )
        {
            namespace CC                        = Common::Content;
            const CC::SubsystemVersion kKnown[] = {
                 { Desert::Assets::kCloudLayoutSubsystemTag, Desert::Assets::kCloudLayoutContainerVersion } };
            const std::string bytes  = ReadAll( path );
            const auto        header = CC::ReadEnvelopeHeader( std::as_bytes( std::span( bytes ) ),
                                                               CC::AssetHeaderReadContext{ kKnown } );
            if ( !header || header.GetValue().Asset.Kind != CC::ContentKind::CloudLayout )
            {
                err << "FAIL   " << path.string() << " — not a cloud layout envelope at v"
                    << Desert::Assets::kCloudLayoutContainerVersion << " (older layouts are not supported): "
                    << ( header ? "the envelope's kind is not CloudLayout" : header.GetError() ) << "\n";
                ++failed;
                continue;
            }
            out << "ok     " << path.string() << " — already at layout v"
                << Desert::Assets::kCloudLayoutContainerVersion << "\n";
        }

        // THE CLOUD NOISE VOLUMES and THE SCULPTED CLOUD VOLUMES: only the enveloped generation (container 3)
        // is read. A bare 'DCNV' 1/2 or 'DCMV' 2 container is refused by its number - their wrapping steps were
        // deleted with the other legacy steps (LEG1), and the committed corpus holds none.
        for ( const auto& path : noises )
        {
            namespace CC                        = Common::Content;
            const CC::SubsystemVersion kKnown[] = {
                 { Desert::Assets::kCloudNoiseSubsystemTag, Desert::Assets::kCloudNoiseContainerVersion } };
            const std::string bytes = ReadAll( path );
            const auto        all   = std::as_bytes( std::span( bytes ) );
            const bool        bare  = bytes.size() >= sizeof( Desert::Assets::kCloudNoiseMagic ) + 4u &&
                              std::memcmp( bytes.data(), Desert::Assets::kCloudNoiseMagic,
                                           sizeof( Desert::Assets::kCloudNoiseMagic ) ) == 0;
            if ( bare )
            {
                uint32_t version = 0;
                std::memcpy( &version, bytes.data() + sizeof( Desert::Assets::kCloudNoiseMagic ),
                             sizeof( version ) );
                err << "FAIL   " << path.string() << " — a bare 'DCNV' container version " << version
                    << "; only the enveloped v" << Desert::Assets::kCloudNoiseContainerVersion
                    << " is read (older noise volumes are not supported)\n";
                ++failed;
                continue;
            }
            const auto header = CC::ReadEnvelopeHeader( all, CC::AssetHeaderReadContext{ kKnown } );
            if ( !header || header.GetValue().Asset.Kind != CC::ContentKind::CloudNoiseVolume )
            {
                err << "FAIL   " << path.string() << " — not a noise volume envelope at v"
                    << Desert::Assets::kCloudNoiseContainerVersion << ": "
                    << ( header ? "the envelope's kind is not CloudNoiseVolume" : header.GetError() ) << "\n";
                ++failed;
                continue;
            }
            out << "ok     " << path.string() << " — already at noise volume v"
                << Desert::Assets::kCloudNoiseContainerVersion << "\n";
        }

        for ( const auto& path : models )
        {
            namespace CC                        = Common::Content;
            const CC::SubsystemVersion kKnown[] = { { Desert::Assets::kCloudModellingSubsystemTag,
                                                      Desert::Assets::kCloudModellingContainerVersion } };
            const std::string          bytes    = ReadAll( path );
            const auto                 all      = std::as_bytes( std::span( bytes ) );
            const bool bare = bytes.size() >= sizeof( Desert::Assets::kCloudModellingMagic ) + 4u &&
                              std::memcmp( bytes.data(), Desert::Assets::kCloudModellingMagic,
                                           sizeof( Desert::Assets::kCloudModellingMagic ) ) == 0;
            if ( bare )
            {
                uint32_t version = 0;
                std::memcpy( &version, bytes.data() + sizeof( Desert::Assets::kCloudModellingMagic ),
                             sizeof( version ) );
                err << "FAIL   " << path.string() << " — a bare 'DCMV' container version " << version
                    << "; only the enveloped v" << Desert::Assets::kCloudModellingContainerVersion
                    << " is read (older sculpted cloud volumes are not supported)\n";
                ++failed;
                continue;
            }
            const auto header = CC::ReadEnvelopeHeader( all, CC::AssetHeaderReadContext{ kKnown } );
            if ( !header || header.GetValue().Asset.Kind != CC::ContentKind::CloudModellingVolume )
            {
                err << "FAIL   " << path.string() << " — not a sculpted cloud volume envelope at v"
                    << Desert::Assets::kCloudModellingContainerVersion << ": "
                    << ( header ? "the envelope's kind is not CloudModellingVolume" : header.GetError() ) << "\n";
                ++failed;
                continue;
            }
            out << "ok     " << path.string() << " — already at sculpted cloud volume v"
                << Desert::Assets::kCloudModellingContainerVersion << "\n";
        }

        // THE LANDSCAPE TILES: only the current 'DLHT' container (v3) is read, by the engine's own decoder, so
        // a v1 or v2 tile FAILS by path and number - each raising step was deleted once it had raised the corpus
        // (LS-15 v1 -> v2, L10a2 v2 -> v3). A tile with no edit layers FAILS by path too: every landscape has at
        // least its Base layer, and the step that gave the corpus one (L10b3) is deleted. Every line carries the
        // heights' CRC-32C, so two runs over the corpus compare heights bit for bit.
        for ( const auto& path : tiles )
        {
            const std::string bytes = ReadAll( path );
            const auto        tile  = Desert::World::Landscape::DecodeLandscapeTile(
                 std::span( reinterpret_cast<const unsigned char*>( bytes.data() ), bytes.size() ) );
            if ( !tile )
            {
                err << "FAIL   " << path.string() << " — " << tile.GetError() << "\n";
                ++failed;
                continue;
            }
            if ( tile.GetValue().EditLayers().empty() )
            {
                err << "FAIL   " << path.string()
                    << " — the tile has no edit layers; every landscape tile carries at least the Base layer\n";
                ++failed;
                continue;
            }
            const std::vector<uint16_t>& samples = tile.GetValue().Samples();
            out << "ok     " << path.string() << " — already at tile v"
                << Desert::World::Landscape::kLandscapeTileContainerVersion << ", heights crc " << std::hex
                << Common::Utils::Crc32c( samples.data(), samples.size() * sizeof( uint16_t ) ) << std::dec
                << "\n";
        }

        // THE SHADERS: only SHDR 1 (the comment header line, ShaderAssetHeader.hpp) is read. A file stating no
        // header is generation 0 and is refused by number - its stamping step was deleted with the other legacy
        // steps (LEG1); a headed file is left byte for byte once its header is checked.
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
            if ( !source.starts_with( CC::kShaderHeaderPrefix ) )
            {
                err << "FAIL   " << path.string() << " — states no shader header (SHDR 0); only SHDR "
                    << Desert::Assets::kShaderSchemaVersion << " is read (older shaders are not supported)\n";
                ++failed;
                continue;
            }
            const auto header = CC::ReadShaderHeader( source );
            const int  stated =
                 header ? Desert::Assets::StatedVersion( header.GetValue(), Desert::Assets::kShaderSchemaTag )
                         : -1;
            if ( stated != static_cast<int>( Desert::Assets::kShaderSchemaVersion ) )
            {
                err << "FAIL   " << path.string() << " — "
                    << ( header ? "states SHDR " + std::to_string( stated ) + "; this tool reads SHDR " +
                                       std::to_string( Desert::Assets::kShaderSchemaVersion )
                                : header.GetError() )
                    << "\n";
                ++failed;
                continue;
            }
            out << "ok     " << path.string() << " — already at shader v" << Desert::Assets::kShaderSchemaVersion
                << "\n";
        }

        for ( const auto& path : scenes )
        {
            std::string source = ReadAll( path );
            if ( source.empty() )
            {
                err << "FAIL   " << path.string() << " — unreadable or empty\n";
                ++failed;
                continue;
            }
            // A partitioned world's header (v35) is joined with its entity files first, by the engine's own
            // reader, so every step below sees the one scene document it always has.
            const auto document = Common::Json::TextDocument::Parse( source );
            const bool isHeader = document && Desert::Core::ExternalEntities::IsHeader( document.GetValue() );
            if ( isHeader )
            {
                auto joined = Desert::Core::ExternalEntities::ReadSceneFileText( path );
                if ( !joined )
                {
                    err << "FAIL   " << path.string() << " — " << joined.GetError() << "\n";
                    ++failed;
                    continue;
                }
                source = joined.ExtractValue();
            }

            auto parsed = rfl::json::read<Desert::Migration::SceneSerialized>( source );
            if ( !parsed )
            {
                err << "FAIL   " << path.string() << " — " << parsed.error().what() << "\n";
                ++failed;
                continue;
            }

            // ONE root for this scene, and it is the scene's own (see SceneOutputRoot). It is handed to
            // the migration as well as used for the write below, so the root the step resolves asset paths
            // against and the root the scene is written under are the same root by construction.
            const std::filesystem::path                  assetsRoot = SceneOutputRoot( path );
            Desert::Migration::FileMigrationReport       report =
                 Desert::Migration::MigrateScene( parsed.value(), assetsRoot, path );

            // A scene from a LATER build: nothing ran and nothing was stamped, so this is a FAILED file
            // and not an "ok". It used to be neither — the tree fell through every gate and was stamped
            // DOWN to this tool's head, then reported as already current.
            if ( !report.Refused.empty() )
            {
                err << "FAIL   " << path.string() << " — " << report.Refused << "\n";
                ++failed;
                continue;
            }

            // THE CANONICAL PASS, after the steps: every reflected block (Settings and the rows of
            // ReflectedComponentBlocks.hpp) restated in the saver's own text, so a scene opened and saved with
            // no edit is byte-identical to its file. No version moves - see SettingsCanonical.hpp.
            const Desert::Migration::SceneCanonicalisationReport canonical =
                 Desert::Migration::CanonicaliseScene( parsed.value() );
            if ( !canonical.Refused.empty() )
            {
                err << "FAIL   " << path.string() << " — canonical pass: " << canonical.Refused
                    << "; the file is untouched\n";
                ++failed;
                continue;
            }
            const auto printCanonical = [&out, &canonical]
            {
                out << " canonical text of " << canonical.BlocksRestated << " reflected block(s) ("
                    << canonical.KeysAdded << " key(s) added, " << canonical.ValuesRestated
                    << " value(s) restated), version unchanged";
            };

            // A partitioned world at the head whose records are still INLINE (written by a tool that
            // predates v35's layout, or by hand) is moved out now: that is the whole of the v35 step.
            const bool partitioned = parsed.value().WorldPartition.has_value();
            if ( !report.Changed() && partitioned && !isHeader )
            {
                report.ExternalEntitiesRaised = true;
                report.EntitiesMovedOut       = parsed.value().Entities.size();
            }

            if ( !report.Changed() && canonical.Changed() )
            {
                out << ( check ? "WOULD  " : "canon  " ) << path.string() << " —";
                printCanonical();
                out << "\n";
                if ( !check && !WriteScene( path, rfl::json::write( parsed.value() ), err ) )
                {
                    err << "FAIL   " << path.string() << " — the canonical text could not be written; the "
                        << "original file is untouched\n";
                    ++failed;
                    continue;
                }
                ++changed;
                continue;
            }

            if ( !report.Changed() )
            {
                // A header is the engine writer's canonical output piece by piece; `source` is the joined
                // text, which is not the file, so there is no layout to compare it with.
                if ( isHeader )
                {
                    out << "ok     " << path.string() << " — already at scene v"
                        << Desert::Migration::kSceneVersion << " (one file per entity)\n";
                    continue;
                }
                if ( const Layout layout = RelayOutIfNeeded( path, source, check, out, err, WriteScene );
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
            PrintSteps( out, report );
            if ( canonical.Changed() )
                printCanonical();
            out << "\n";

            if ( check )
            {
                ++changed;
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
            if ( !WriteNewFoliageTypes( report, err ) )
            {
                ++failed;
                continue;
            }
            if ( !WriteScene( path, rfl::json::write( parsed.value() ), err ) )
            {
                err << "FAIL   " << path.string() << " — the raise could not be written; the original file "
                    << "is untouched\n";
                ++failed;
                continue;
            }
            ++changed;
        }

        // THE MATERIALS. Every committed `.demat` is MATL v4, and this tool no longer raises an older one (the
        // legacy steps, their material-number register and the path-derived asset table were retired with
        // LEG1): an older or newer generation FAILS by its number, a current one is only re-laid-out if its text
        // layout is not canonical.
        for ( const auto& path : materials )
        {
            const std::string source = ReadAll( path );
            if ( source.empty() )
            {
                err << "FAIL   " << path.string() << " — unreadable or empty\n";
                ++failed;
                continue;
            }
            const auto stated = ReadMaterialSchemaVersion( path, source );
            if ( !stated )
            {
                err << "FAIL   " << stated.GetError() << "\n";
                ++failed;
                continue;
            }
            if ( stated.GetValue() != Desert::Assets::kMaterialSchemaVersion )
            {
                err << "FAIL   " << path.string() << " — states MATL v" << stated.GetValue()
                    << "; this tool reads " << "only MATL v" << Desert::Assets::kMaterialSchemaVersion
                    << ( stated.GetValue() < Desert::Assets::kMaterialSchemaVersion
                              ? " (older materials are not supported)"
                              : " (written by a later build)" )
                    << "\n";
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
        }
        // ---- THE CLIPS ----------------------------------------------------------------------------
        // Their own generation sequence, their own gate: only the current ANIM generation is read.
        for ( const auto& path : clips )
        {
            const std::string source = ReadAll( path );
            if ( source.empty() )
            {
                err << "FAIL   " << path.string() << " — unreadable or empty\n";
                ++failed;
                continue;
            }
            if ( !PassesTextHeaderGate( *TextHeaderGateFor( path ), path, source, err ) )
            {
                ++failed;
                continue;
            }
            if ( const Layout layout = RelayOutIfNeeded( path, source, check, out, err );
                 layout != Layout::Canonical )
            {
                ++( layout == Layout::Failed ? failed : relaid );
                continue;
            }
            out << "ok     " << path.string() << " — ANIM v" << Desert::Assets::kAnimationSchemaVersion << "\n";
        }

        // THE PREFABS, through the SAME chain the scenes went through (И11).
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

            const Desert::Migration::PrefabMigrationOutcome outcome =
                 Desert::Migration::MigratePrefab( parsed.value(), assetsRoot, path );

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
            out << " from scene v" << outcome.FoundSceneVersion << ":";
            PrintSteps( out, outcome.Steps );
            out << "\n";

            if ( check )
            {
                ++prefabsChanged;
                continue;
            }

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
            if ( !WriteNewFoliageTypes( outcome.Steps, err ) )
            {
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

        int foliageRaised = 0;
        for ( const auto& path : texts )
        {
            const std::string text = ReadAll( path );
            if ( path.extension() == Desert::Assets::Serialization::kFoliageTypeExtension )
            {
                const auto stated = ReadStatedVersion( path, text, "FOLT" );
                if ( !stated )
                {
                    err << "FAIL   " << stated.GetError() << "\n";
                    ++failed;
                    continue;
                }
                if ( stated.GetValue() >= 1u && stated.GetValue() <= 5u )
                {
                    // The chain: each generation below the engine's takes every step from its own on.
                    using Step             = Common::ResultStr<std::string> ( * )( const std::string& );
                    const Step  steps[]    = { &Desert::Migration::MigrateFoliageTypeV1ToV2,
                                               &Desert::Migration::MigrateFoliageTypeV2ToV3,
                                               &Desert::Migration::MigrateFoliageTypeV3ToV4,
                                               &Desert::Migration::MigrateFoliageTypeV4ToV5,
                                               &Desert::Migration::MigrateFoliageTypeV5ToV6 };
                    std::string raisedText = text;
                    bool        stepFailed = false;
                    for ( uint32_t from = stated.GetValue(); from <= 5u; ++from )
                    {
                        const auto raised = steps[from - 1u]( raisedText );
                        if ( !raised )
                        {
                            err << "FAIL   " << path.string() << " — FOLT " << from << " -> " << ( from + 1u )
                                << ": " << raised.GetError() << "\n";
                            stepFailed = true;
                            break;
                        }
                        raisedText = raised.GetValue();
                    }
                    if ( stepFailed )
                    {
                        ++failed;
                        continue;
                    }
                    out << ( check ? "would raise " : "raised " ) << path.string() << " FOLT " << stated.GetValue()
                        << " -> " << Desert::Assets::kFoliageTypeSchemaVersion << "\n";
                    if ( !check && !WriteText( path, raisedText, err ) )
                    {
                        ++failed;
                        continue;
                    }
                    ++foliageRaised;
                    continue;
                }
                if ( stated.GetValue() != Desert::Assets::kFoliageTypeSchemaVersion )
                {
                    err << "FAIL   " << path.string() << " — states FOLT v" << stated.GetValue()
                        << ", and this tool reads v1 to v3 (raised) and v"
                        << Desert::Assets::kFoliageTypeSchemaVersion << " only\n";
                    ++failed;
                    continue;
                }
            }
            if ( const TextHeaderGate* row = TextHeaderGateFor( path );
                 row != nullptr && !PassesTextHeaderGate( *row, path, text, err ) )
            {
                ++failed;
                continue;
            }
            if ( const Layout layout = RelayOutIfNeeded( path, text, check, out, err );
                 layout != Layout::Canonical )
                ++( layout == Layout::Failed ? failed : relaid );
        }

        out << "SceneMigrator: " << scenes.size() << " scene(s), " << changed
            << ( check ? " would change, " : " raised, " ) << clips.size() << " clip(s), " << materials.size()
            << " material(s), " << prefabs.size() << " prefab(s), " << prefabsChanged
            << ( check ? " would change, " : " raised, " ) << texts.size() << " other text asset(s), " << relaid
            << ( check ? " would be re-laid-out, " : " re-laid-out, " ) << foliageRaised
            << ( check ? " foliage type(s) would be raised, " : " foliage type(s) raised, " ) << tiles.size()
            << " landscape tile(s), " << failed << " failed\n";

        failedOut = failed;
        if ( failed > 0 )
            return 1;
        return ( check && ( changed > 0 || prefabsChanged > 0 || relaid > 0 || foliageRaised > 0 ) ) ? 1 : 0;
    }

    // ALL OR NOTHING. A write run used to raise file after file and let one refusal fail only itself: over
    // Editor/Resources/Assets that rewrote 147 files around the one it refused, so the tree held two
    // generations at once and the refusal had to be fixed against content already half-moved. Now the
    // whole set is migrated in memory first (the --check pass computes every step without writing), and
    // any refusal there writes NOTHING: every refusal is printed and the exit code is 1. Only a set with
    // no refusal reaches the writing pass. A write that fails after that is an I/O failure of one file,
    // which the atomic write-then-rename already leaves byte-identical.
    int RunSceneMigrator( const std::vector<std::string>& args, std::ostream& out, std::ostream& err )
    {
        int failed = 0;
        if ( std::ranges::find( args, std::string( "--check" ) ) != args.end() )
            return RunSceneMigratorPass( args, false, out, err, failed );

        std::ostringstream dryOut;
        std::ostringstream dryErr;
        const int          dry = RunSceneMigratorPass( args, true, dryOut, dryErr, failed );
        if ( dry == 2 || failed > 0 )
        {
            out << dryOut.str();
            err << dryErr.str();
            if ( failed > 0 )
                err << "SceneMigrator: " << failed
                    << " file(s) refused in the in-memory pass - NOTHING was written; fix every refusal above "
                       "and run again\n";
            return dry == 2 ? 2 : 1;
        }
        return RunSceneMigratorPass( args, false, out, err, failed );
    }
} // namespace Desert::Migration
