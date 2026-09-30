#include "GamePackager.hpp"
#include "PackageCook.hpp"
#include "PackageTarget.hpp"
#include "PackagedContentTrees.hpp"

#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/ExternalEntities.hpp>
#include <Engine/Core/Serialize/WorldCells.hpp>
#include <Engine/Core/ShaderCompiler/ShaderCacheKey.hpp>
#include <Engine/Project/ProjectContext.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/DevInstruments.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Project/ProjectFormat.hpp>
#include <Common/Settings/ProductName.hpp>
#include <Common/Utilities/ContentManifest.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Content/ContentChunks.hpp>
#include <Common/Content/DerivedDataCache.hpp>
#include <Common/Content/ContentScan.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Utilities/AssetRegistry.hpp>
#include <Common/Utilities/PakFile.hpp>
#include <Common/Utilities/PeImports.hpp>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <system_error>

namespace Desert::Editor
{
    namespace fs = std::filesystem;

    namespace
    {
        // Raw mesh sources are import-time input only — the runtime reads cooked .stmesh/.skmesh.
        bool IsRawMeshSource( const fs::path& p )
        {
            std::string ext = p.extension().string();
            std::transform( ext.begin(), ext.end(), ext.begin(), ::tolower );
            return ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb" ||
                   ext == ".blend" || ext == ".dae";
        }

        struct CopyStats
        {
            size_t   Files = 0;
            uintmax_t Bytes = 0;
        };

        // The pak key the cooked registry ships under — the one path the packager writes rather than stages.
        std::string ShippedRegistryKey()
        {
            return ( fs::path( Common::Constants::Path::COOKED_DIR_NAME ) / "AssetRegistry.dreg" )
                 .generic_string();
        }

        // Copies one census tree into the cooked tree at <cooked>/<PakKey>/<relative>, skipping raw mesh
        // sources and staging texture assets in their cooked form when asked (the project asset tree).
        // `staged` receives ("<PakKey>/<relative>", staged file) for the steps that cook from the staged
        // content (the partitioned worlds). THE SOURCE TREES ARE READ HERE AND NOWHERE ELSE on the way to an
        // archive: the packer walks only Saved/Cooked/<Platform> (CollectCookedTree), as UE's pak step reads
        // only the cooker's output.
        bool StageTree( const PackagedTree& tree, const fs::path& cooked, bool developerInstruments,
                        std::vector<std::pair<std::string, fs::path>>& staged, std::string& error )
        {
            const fs::path& from = *tree.Tree;
            std::error_code ec;
            if ( !fs::exists( from, ec ) )
                return true; // nothing to stage is fine (e.g. no Cooked/ yet)

            const std::string keyPrefix = tree.PakKey;
            for ( auto it = fs::recursive_directory_iterator( from, ec );
                  it != fs::recursive_directory_iterator(); it.increment( ec ) )
            {
                if ( ec )
                {
                    error = "walk failed under " + from.string() + ": " + ec.message();
                    return false;
                }
                const fs::path& src = it->path();
                if ( !it->is_regular_file() )
                    continue;
                if ( tree.StripRawMeshSources && IsRawMeshSource( src ) )
                    continue;

                const fs::path rel = fs::relative( src, from, ec );
                if ( ec )
                {
                    error = "cannot relativize " + src.string() + ": " + ec.message();
                    return false;
                }
                if ( IsLeftOutOfPackage( src, developerInstruments ) )
                    continue;
                const std::string key = keyPrefix + "/" + rel.generic_string();
                // A registry left on the disk from before the registry built itself (AF9) is not staged:
                // the one the pak carries is written from this process's gather, under the same key, and a
                // stale copy would collide with it or, worse, be the one the game read.
                if ( key == ShippedRegistryKey() )
                    continue;
                const fs::path target = cooked / key;
                if ( tree.StripRawMeshSources && src.extension() == Assets::kTextureAssetExtension )
                {
                    if ( auto written = StageCookedTextureAsset( src, target ); !written.IsSuccess() )
                    {
                        error = written.GetError();
                        return false;
                    }
                }
                else
                {
                    fs::create_directories( target.parent_path(), ec );
                    fs::copy_file( src, target, fs::copy_options::overwrite_existing, ec );
                    if ( ec )
                    {
                        error = "cannot stage " + src.string() + " into " + target.string() + ": " + ec.message();
                        return false;
                    }
                }
                staged.emplace_back( key, target );
            }
            return true;
        }

        // A CFBundleIdentifier is a reverse-DNS string: only letters, digits, '-' and '.'. That is a
        // different namespace from a folder name, so it is derived here from the folder name rather than
        // being a second folder-name rule.
        std::string BundleIdentifierComponent( const std::string& inFolderName )
        {
            std::string id = inFolderName;
            for ( char& c : id )
                if ( std::isalnum( static_cast<unsigned char>( c ) ) == 0 && c != '-' && c != '.' )
                    c = '-';
            return id;
        }

        // THE DESCRIPTOR GOES INTO THE ARCHIVE, under the one name the player looks for
        // (Project::kPackagedDescriptorName). That is what makes a package "a binary and one .dpak"
        // and nothing else: no loose file beside the archive for a copy, a zip or an installer to
        // leave behind, and the descriptor travels with the content it describes rather than beside
        // it. Both entry points below go through this, so the archive is a GAME whichever one built
        // it — an archive that describes no game is the thing П5 had to fix.
        //
        // The empty-serialization check is not defensive noise: `WriteProjectFile` answers with a
        // string, so a failure there is an EMPTY SUCCESS, and an empty descriptor packed under the
        // right key produces a game that reaches "could not be read" on the player's machine instead
        // of failing here where somebody can act on it (DC §1.4).
        // A PARTITIONED WORLD SHIPS CUT INTO ITS CELLS (WP9): the runtime reads its index and always-loaded cell
        // at the start and every other cell on a worker when a streaming source wants it (WorldStreamer.hpp), from
        // WorldCells::CookedWorldDirectory of the scene's key. Cooked here, from the scene as it is on disk, with
        // the registry the editor plans with — so the cells are the ones the editor's Play streams. A scene
        // file that states a partition and does not cook fails the package: the game would fail on it too.
        bool CollectCookedWorlds( const std::vector<std::pair<std::string, fs::path>>& staged,
                                  std::vector<std::pair<std::string, std::string>>& baseBlobs, std::string& error )
        {
            for ( const auto& [key, path] : staged )
            {
                if ( path.extension() != ".desce" )
                    continue;
                auto text = Core::ExternalEntities::ReadSceneFileText( path );
                if ( !text )
                {
                    error = "cooking the worlds: " + text.GetError();
                    return false;
                }
                // Only a file that names the block can state one; the rest are not parsed twice.
                if ( text.GetValue().find( "\"WorldPartition\"" ) == std::string::npos )
                    continue;
                auto scene = Core::ParseLoadableScene( path.string(), text.GetValue() );
                if ( !scene )
                {
                    error = "cooking the worlds: " + scene.GetError();
                    return false;
                }
                if ( !scene.GetValue().Scene.WorldPartition.has_value() )
                    continue;
                auto cooked = Core::WorldCells::CookWorld( scene.GetValue().Scene,
                                                           std::span( &Assets::ContentRegistry::Get(), 1 ) );
                if ( !cooked )
                {
                    error = "cooking the world '" + key + "': " + cooked.GetError();
                    return false;
                }
                const std::string directory = Core::WorldCells::CookedWorldDirectory( key );
                for ( const auto& file : cooked.GetValue().Files )
                {
                    baseBlobs.emplace_back( directory + file.Name,
                                            std::string( file.Bytes.begin(), file.Bytes.end() ) );
                }
                LOG_INFO( "[Package] world '{}' cooked into {} file(s) under '{}'", key,
                          cooked.GetValue().Files.size(), directory );
            }
            return true;
        }

        bool CollectDescriptor( const Common::Project::ProjectFile&               project,
                                std::vector<std::pair<std::string, std::string>>& blobs, std::string& error )
        {
            const std::string json = Common::Project::WriteProjectFile( PackagedDescriptor( project ) );
            if ( json.empty() )
            {
                error = "the project descriptor for '" + project.Name + "' serialized to nothing";
                return false;
            }
            blobs.emplace_back( Project::kPackagedDescriptorName, json );
            return true;
        }

        // THE COOK'S SECOND HALF: everything a package ships, written into Saved/Cooked/<Platform> — the census
        // trees (PackagedContentTrees.hpp), the registry this process gathered (AF9a: nothing in git carries
        // one), the partitioned worlds cut into cells, and the regenerated descriptor. CookContentCaches has
        // already wiped the tree and staged the DDC buckets into it. `baseKeys` receives the keys that must
        // land in the base archive whatever the chunk plan says (registry, worlds, descriptor: the game reads
        // them before it knows which chunks exist).
        bool StageShippedContent( std::vector<std::string>& baseKeys, bool developerInstruments,
                                  std::string& error )
        {
            const fs::path                                cooked = Common::DDC::PlatformCookedDir();
            std::vector<std::pair<std::string, fs::path>> staged;
            for ( const PackagedTree& tree : PackagedContentTrees() )
                if ( !StageTree( tree, cooked, developerInstruments, staged, error ) )
                    return false;

            std::vector<std::pair<std::string, std::string>> pinned;
            // The registry ships without the rows of what the package leaves out (editor-only resources, and
            // a Shipping package's developer-only shaders): they are not staged, and a row naming a file the
            // archive lacks is a load the game would attempt and fail at every start.
            Common::Utils::AssetRegistry shipped = Assets::ContentRegistry::Get();
            std::vector<std::string>     leftOutRows;
            for ( const Common::Utils::AssetRegistryEntry& row : shipped.Entries() )
                if ( IsLeftOutOfPackage( Common::AssetHandle::PathForStableKey( row.Key ), developerInstruments ) )
                    leftOutRows.push_back( row.Key );
            for ( const std::string& key : leftOutRows )
                shipped.Remove( key );
            pinned.emplace_back( ShippedRegistryKey(), shipped.Serialize() );
            if ( !CollectCookedWorlds( staged, pinned, error ) )
                return false;
            // A dev pak that carried content but no identity was an archive only the other entry point's
            // output could be started from; both entry points stage the same descriptor.
            if ( !CollectDescriptor( Project::ProjectContext::Current(), pinned, error ) )
                return false;

            std::error_code ec;
            for ( const auto& [key, bytes] : pinned )
            {
                const fs::path target = cooked / key;
                fs::create_directories( target.parent_path(), ec );
                if ( auto written = Common::Utils::FileSystem::WriteContentToFileAtomic( target, bytes );
                     !written.IsSuccess() )
                {
                    error = "the cooked tree could not take '" + key + "': " + written.GetError();
                    return false;
                }
                baseKeys.push_back( key );
            }
            return true;
        }

        // THE ONLY READ the archive is built from: every file under Saved/Cooked/<Platform>, keyed by its
        // path relative to that tree. The base-pinned keys go into the base archive as blobs; the rest are
        // divided by the chunk plan.
        bool CollectCookedTree( const std::vector<std::string>&                   baseKeys,
                                std::vector<std::pair<std::string, fs::path>>&    contentFiles,
                                std::vector<std::pair<std::string, std::string>>& baseBlobs, CopyStats& stats,
                                std::string& error )
        {
            const fs::path  cooked = Common::DDC::PlatformCookedDir();
            std::error_code ec;
            for ( auto it = fs::recursive_directory_iterator( cooked, ec );
                  it != fs::recursive_directory_iterator(); it.increment( ec ) )
            {
                if ( ec )
                {
                    error = "walk failed under " + cooked.string() + ": " + ec.message();
                    return false;
                }
                if ( !it->is_regular_file() )
                    continue;
                const std::string key =
                     it->path().lexically_normal().lexically_relative( cooked ).generic_string();
                if ( std::find( baseKeys.begin(), baseKeys.end(), key ) != baseKeys.end() )
                {
                    auto bytes = Common::Utils::FileSystem::ReadFileContent( it->path() );
                    if ( !bytes.IsSuccess() )
                    {
                        error = "the cooked tree lost '" + key + "': " + bytes.GetError();
                        return false;
                    }
                    stats.Bytes += bytes.GetValue().size();
                    baseBlobs.emplace_back( key, bytes.GetValue() );
                }
                else
                {
                    contentFiles.emplace_back( key, it->path() );
                    // The error_code overload returns uintmax_t(-1) on failure; a size that cannot be read
                    // is a count that skips, not a failure — the file itself is packed.
                    std::error_code sizeEc;
                    const auto      size = fs::file_size( it->path(), sizeEc );
                    if ( !sizeEc )
                        stats.Bytes += size;
                }
                ++stats.Files;
            }
            if ( ec )
            {
                error = "the cooked tree " + cooked.string() + " cannot be read: " + ec.message();
                return false;
            }
            return true;
        }

        // THE DIVISION, DERIVED FROM THIS PROJECT'S OWN DATA. The registry stack carries the edges,
        // and the scheme file carries only what cannot be derived from them (chunk roots, and the
        // keys pinned to the base). The scheme is REQUIRED: an absent, blank or broken file refuses
        // the package by path, and a one-archive project says so in its file (owner, 2026-09-25).
        // It is READ by the caller before the cook (see PackageGame), and only the plan is built
        // here, because the plan needs the registry the cook and the gather produce.
        Common::ResultStr<Common::Content::ChunkPlan> PlanTheDivision( const Common::Content::ChunkScheme& scheme )
        {
            return Common::Content::BuildChunkPlan( Assets::ContentRegistry::Get(), scheme );
        }

        // What the packager says about the division, so a one-archive project and a divided one are
        // distinguishable in the log rather than by counting files in a folder afterwards.
        std::string DivisionSummary( const Common::Content::ChunkPlan&         plan,
                                     const Common::Content::ChunkedWriteStats& written )
        {
            if ( plan.Count() == 1 )
                return {};
            std::ostringstream text;
            text << "  divided into " << plan.Count() << " archive(s):";
            for ( std::size_t i = 0; i < plan.Count(); ++i )
                text << "  " << plan.Names()[i] << "=" << written.Entries[i] << " file(s)/"
                     << ( written.Bytes[i] / ( std::size_t{ 1024 } * 1024 ) ) << " MB";
            if ( !plan.UnresolvedEdges().empty() )
                text << "  WARNING: " << plan.UnresolvedEdges().size()
                     << " dependency edge(s) named no registry row, so a chunk may be missing what they "
                        "pointed at";
            return text.str();
        }

        // THE VISUAL C++ RUNTIME THE WINDOWS GAME CARRIES (PK-W1). The workspace builds with the DLL CRT
        // (/MD) — the Vulkan SDK's prebuilt shaderc and spirv-cross libraries are stamped
        // RuntimeLibrary=MD_DynamicRelease, so the static CRT cannot link — and a Windows install that
        // never ran the Visual C++ Redistributable refuses to start an /MD executable with a system
        // dialog the game cannot catch. Microsoft permits app-local deployment of the redistributable
        // files, so the package carries them next to Runtime.exe.
        //
        // THE REDIST OF THE VISUAL STUDIO THAT BUILT THE GAME, found in this order, each a real source:
        //   1. %VCToolsRedistDir% — set by the Developer Command Prompt (vcvarsall), naming exactly the
        //      toolset that shell builds with;
        //   2. vswhere (installed with every Visual Studio 2017+) → the newest install with the x64 VC
        //      tools → its VC\Auxiliary\Build\Microsoft.VCRedistVersion.default.txt → VC\Redist\MSVC\<ver>.
        //      That is the install msbuild uses on the CI runner (microsoft/setup-msbuild picks the same
        //      "latest") and on a single-VS machine.
        // Neither found is a refusal naming both, never a package without the runtime.
        //
        // Returns <redist>\x64\Microsoft.VC<toolset>.CRT.
        // A process environment variable, empty when unset. The packager reads it once, on its own
        // thread, before any job starts, and nothing in the editor ever calls setenv.
        std::string EnvironmentVariable( const char* name )
        {
            const char* value = std::getenv( name ); // NOLINT(concurrency-mt-unsafe) - see above
            return value != nullptr ? std::string( value ) : std::string();
        }

        Common::ResultStr<fs::path> FindVcCrtRedistDir()
        {
            using Found = fs::path;
            fs::path    redist;
            std::string tried;
            if ( const std::string env = EnvironmentVariable( "VCToolsRedistDir" ); !env.empty() )
            {
                redist = env;
                tried  = "%VCToolsRedistDir% = " + redist.string();
            }
            else
            {
                const std::string programFiles = EnvironmentVariable( "ProgramFiles(x86)" );
                if ( programFiles.empty() )
                    return Common::MakeError<Found>(
                         "%VCToolsRedistDir% is not set (not a Developer Command Prompt) "
                         "and %ProgramFiles(x86)% is not set either, so vswhere.exe "
                         "cannot be located" );
                const fs::path vswhere =
                     fs::path( programFiles ) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
                std::error_code ec;
                if ( !fs::exists( vswhere, ec ) )
                    return Common::MakeFormattedError<Found>(
                         "%VCToolsRedistDir% is not set (not a Developer Command Prompt) and {} does not exist",
                         vswhere.string() );

                // cmd strips the OUTER pair of quotes of a /c line, so the quoted path needs one more pair.
                const std::string command = "\"\"" + vswhere.string() +
                                            "\" -latest -products * -requires "
                                            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property "
                                            "installationPath\"";
#ifdef _WIN32
                FILE* pipe = _popen( command.c_str(), "r" );
#else
                FILE* pipe = popen( command.c_str(), "r" );
#endif
                if ( pipe == nullptr )
                    return Common::MakeFormattedError<Found>( "{} could not be started", vswhere.string() );
                std::string           output;
                std::array<char, 512> chunk{};
                while ( std::fgets( chunk.data(), static_cast<int>( chunk.size() ), pipe ) != nullptr )
                    output += chunk.data();
#ifdef _WIN32
                _pclose( pipe );
#else
                pclose( pipe );
#endif
                while ( !output.empty() && std::isspace( static_cast<unsigned char>( output.back() ) ) != 0 )
                    output.pop_back();
                if ( const auto newline = output.find_first_of( "\r\n" ); newline != std::string::npos )
                    output.resize( newline );
                if ( output.empty() )
                    return Common::MakeFormattedError<Found>(
                         "{} found no Visual Studio with the x64 VC tools "
                         "(Microsoft.VisualStudio.Component.VC.Tools.x86.x64)",
                         vswhere.string() );

                const fs::path versionFile =
                     fs::path( output ) / "VC" / "Auxiliary" / "Build" / "Microsoft.VCRedistVersion.default.txt";
                auto version = Common::Utils::FileSystem::ReadFileContent( versionFile );
                if ( !version )
                    return Common::MakeFormattedError<Found>( "the redist version of {} is unknown: {}", output,
                                                              version.GetError() );
                std::string ver = version.ExtractValue();
                std::erase_if( ver, []( unsigned char c ) { return std::isspace( c ); } );
                redist = fs::path( output ) / "VC" / "Redist" / "MSVC" / ver;
                tried  = "vswhere → " + redist.string();
            }

            // Microsoft.VC143.CRT for Visual Studio 2022; the toolset number is the install's, not ours.
            std::vector<fs::path> crtDirs;
            std::error_code       ec;
            for ( fs::directory_iterator it( redist / "x64", ec ), end; !ec && it != end; it.increment( ec ) )
            {
                const std::string name = it->path().filename().string();
                if ( it->is_directory( ec ) && name.starts_with( "Microsoft.VC" ) && name.ends_with( ".CRT" ) )
                    crtDirs.push_back( it->path() );
            }
            if ( crtDirs.empty() )
                return Common::MakeFormattedError<Found>( "no x64\\Microsoft.VC*.CRT directory under the VC++ "
                                                          "redistributable ({})",
                                                          tried );
            // Two toolsets side by side: the newer runtime is backwards-compatible with the older
            // compiler's output (Microsoft's binary-compatibility guarantee for v14x), never the reverse.
            std::sort( crtDirs.begin(), crtDirs.end() );
            if ( crtDirs.size() > 1 )
                LOG_INFO( "[Package] {} VC++ runtime directories under {}; shipping the newest, {}",
                          crtDirs.size(), ( redist / "x64" ).string(), crtDirs.back().filename().string() );
            return Common::MakeSuccess( fs::path( crtDirs.back() ) );
        }
    } // namespace

    // THE PACKAGE SHIPS THE REGISTRY OF THE DISK IT PACKS, by construction: the registry is gathered
    // here, after the cook, from the content roots this package is built from (UE's cook builds its
    // registry the same way — it is an output of the cook, never an input a person keeps in step).
    // A packaged game has no content roots to fall back to, so a content file with no row does not
    // reach the player; gathering at this moment makes "on disk" and "in the registry" one list.
    //
    // What remains a refusal is a file that COULD NOT ENTER the registry (its header refused): it
    // is on the disk and would be packed, yet no row names it, so the game could never load it.
    // Every such file is named, not the first — one fix per file, one attempt for all of them.
    std::string GatherShippedRegistry()
    {
        std::vector<std::string> refused;
        std::vector<std::string> unboxed;
        const auto               gathered = Assets::ContentRegistry::Gather( &refused, &unboxed );
        if ( !gathered )
            return "The asset registry could not be gathered: " + gathered.GetError();

        std::string text;
        if ( !refused.empty() )
        {
            text = std::to_string( refused.size() ) +
                   " content file(s) on the disk this package is built from could not enter the asset registry, "
                   "so the packaged game could not load them:";
            for ( const std::string& refusal : refused )
                text += "\n  " + refusal;
        }
        // T2.7: a noise volume, hero-cloud body or painted layout is created from its row by GUID when a scene
        // names it, so one reachable only by its path would ship as a reference nothing can resolve. (Checked
        // here since AF8: the packager is the one writer of the shipped registry.)
        const std::vector<std::string> pathOnly =
             Common::Content::PathOnlyOnDemandRows( Assets::ContentRegistry::Get() );
        if ( !pathOnly.empty() )
        {
            text += ( text.empty() ? "" : "\n" ) + std::to_string( pathOnly.size() ) +
                    " on-demand content file(s) state no GUID, so the packaged game could not create them:";
            for ( const std::string& problem : pathOnly )
                text += "\n  " + problem;
        }
        // A mesh cooked before its header stated its box would ship a row without one, and the game's world
        // partition would place it by its origin alone. The packager does not decode meshes (the standalone
        // tool does not link the mesh reader), so it names them instead of shipping them boxless.
        if ( !unboxed.empty() )
        {
            text += ( text.empty() ? "" : "\n" ) + std::to_string( unboxed.size() ) +
                    " mesh(es) state no box in their header — re-cook them in the editor (Assets > Rebuild "
                    "Cooked Assets) before packaging:";
            for ( const std::string& key : unboxed )
                text += "\n  " + key;
        }
        return text;
    }

    namespace
    {
        // ── A MAC BUNDLE CARRIES ITS OWN VULKAN, AND THE PACKAGER PROVES IT (PKG2b) ──────────────────────
        //
        // The Runtime is linked against Homebrew's loader by absolute path
        // (/opt/homebrew/opt/vulkan-loader/lib/libvulkan.1.dylib), and so is the loader's own install name.
        // Copying the dylibs into Contents/Frameworks changed nothing about that: dyld resolved the absolute
        // path first, so every "bundled" package still loaded the developer's Homebrew loader and failed to
        // start on a machine without one. The copies are therefore relinked (install_name_tool), re-signed
        // (an edited Mach-O's ad-hoc signature no longer matches, and arm64 refuses to run it), and the
        // result is read back with otool: a bundle whose binaries still name a path outside the system or
        // the bundle is refused, because it is a package that only runs on the machine that built it.
        constexpr const char* kBundledLoaderName = "@executable_path/../Frameworks/libvulkan.1.dylib";
        constexpr const char* kBundledDriverName = "@executable_path/../Frameworks/libMoltenVK.dylib";

        struct CommandOutput
        {
            int         Status = -1;
            std::string Text;
        };

        std::string ShellQuote( const std::string& text )
        {
            std::string quoted = "'";
            for ( const char c : text )
                quoted += c == '\'' ? std::string( "'\\''" ) : std::string( 1, c );
            return quoted + "'";
        }

        CommandOutput RunCaptured( const std::string& command )
        {
            CommandOutput out;
            FILE*         pipe = popen( ( command + " 2>&1" ).c_str(), "r" );
            if ( pipe == nullptr )
                return out;
            std::array<char, 4096> chunk{};
            while ( fgets( chunk.data(), static_cast<int>( chunk.size() ), pipe ) != nullptr )
                out.Text += chunk.data();
            out.Status = pclose( pipe );
            return out;
        }

        Common::BoolResultStr Run( const std::string& command )
        {
            if ( const CommandOutput done = RunCaptured( command ); done.Status != 0 )
                return Common::MakeError( "'" + command + "' failed (status " + std::to_string( done.Status ) +
                                          "): " + done.Text );
            return Common::MakeSuccess( true );
        }

        // Mach-O (thin, either width) or universal: the only files otool has anything to say about. A
        // synthesized test runtime is a PE image and is correctly not one.
        bool IsMachO( const fs::path& file )
        {
            std::ifstream                             in( file, std::ios::binary );
            std::array<char, sizeof( std::uint32_t )> bytes{};
            if ( !in.read( bytes.data(), bytes.size() ) )
                return false;
            const auto magic = std::bit_cast<std::uint32_t>( bytes );
            return magic == 0xfeedfaceu || magic == 0xfeedfacfu || magic == 0xcefaedfeu || magic == 0xcffaedfeu ||
                   magic == 0xcafebabeu || magic == 0xbebafecau;
        }

        // What a shipped binary may name: the OS, or a path inside the bundle.
        bool IsBundleSafeReference( const std::string& path )
        {
            return path.starts_with( "/usr/lib/" ) || path.starts_with( "/System/Library/" ) ||
                   path.starts_with( "@executable_path/" ) || path.starts_with( "@loader_path/" );
        }

        // Every dylib reference (`otool -L`: the install name and each dependency) and every LC_RPATH entry
        // (`otool -l`) of @p binary, in the order otool prints them.
        Common::ResultStr<std::vector<std::string>> LinkedPaths( const fs::path& binary )
        {
            std::vector<std::string> paths;
            const CommandOutput      libs = RunCaptured( "otool -L " + ShellQuote( binary.string() ) );
            if ( libs.Status != 0 )
                return Common::MakeError<std::vector<std::string>>( "otool -L " + binary.string() +
                                                                    " failed: " + libs.Text );
            std::istringstream lines( libs.Text );
            std::string        line;
            std::getline( lines, line ); // "<file>:" header
            while ( std::getline( lines, line ) )
            {
                const size_t start = line.find_first_not_of( " \t" );
                const size_t paren = line.find( " (compatibility" );
                if ( start != std::string::npos && paren != std::string::npos && paren > start )
                    paths.push_back( line.substr( start, paren - start ) );
            }
            const CommandOutput loads = RunCaptured( "otool -l " + ShellQuote( binary.string() ) );
            if ( loads.Status != 0 )
                return Common::MakeError<std::vector<std::string>>( "otool -l " + binary.string() +
                                                                    " failed: " + loads.Text );
            std::istringstream commands( loads.Text );
            bool               inRpath = false;
            while ( std::getline( commands, line ) )
            {
                if ( line.find( "cmd LC_RPATH" ) != std::string::npos )
                    inRpath = true;
                else if ( inRpath && line.find( " path " ) != std::string::npos )
                {
                    const size_t start = line.find( " path " ) + 6;
                    const size_t end   = line.find( " (offset" );
                    paths.push_back( line.substr( start, end == std::string::npos ? end : end - start ) );
                    inRpath = false;
                }
            }
            return Common::MakeSuccess( std::move( paths ) );
        }

        // THE SELF-CHECK: every Mach-O in Contents/ names only the OS or the bundle. Refuses with every
        // offending file and path, not the first, so one packaging run lists the whole fix.
        Common::BoolResultStr BundleIsSelfContained( const fs::path& appRoot )
        {
            std::string     offenders;
            std::error_code ec;
            for ( auto it = fs::recursive_directory_iterator( appRoot / "Contents", ec );
                  !ec && it != fs::recursive_directory_iterator(); it.increment( ec ) )
            {
                if ( !it->is_regular_file( ec ) || !IsMachO( it->path() ) )
                    continue;
                const auto paths = LinkedPaths( it->path() );
                if ( !paths )
                    return Common::MakeError( paths.GetError() );
                for ( const std::string& path : paths.GetValue() )
                    if ( !IsBundleSafeReference( path ) )
                        offenders +=
                             "\n  " + it->path().lexically_relative( appRoot ).generic_string() + " -> " + path;
            }
            if ( ec )
                return Common::MakeError( "could not walk " + appRoot.string() + ": " + ec.message() );
            if ( !offenders.empty() )
                return Common::MakeError( "the bundle is not self-contained - these binaries name a path outside "
                                          "the OS and the bundle, so it runs only where that path exists:" +
                                          offenders );
            return Common::MakeSuccess( true );
        }

        // Copies Homebrew's Vulkan loader, MoltenVK and its ICD manifest into the bundle, repoints the player
        // binary and both dylibs at the bundled copies, re-signs what changed, then runs the self-check.
        Common::BoolResultStr BundleVulkan( const fs::path& appRoot, const fs::path& playerBinary )
        {
            const char*     envPrefix = std::getenv( "HOMEBREW_PREFIX" ); // NOLINT(concurrency-mt-unsafe)
            const fs::path  brew      = envPrefix != nullptr ? fs::path( envPrefix ) : fs::path( "/opt/homebrew" );
            const fs::path  loaderSrc = brew / "lib" / "libvulkan.1.dylib";
            const fs::path  mvkSrc    = brew / "lib" / "libMoltenVK.dylib";
            const fs::path  icdSrc    = brew / "etc" / "vulkan" / "icd.d" / "MoltenVK_icd.json";
            std::error_code ec;
            for ( const fs::path& needed : { loaderSrc, mvkSrc, icdSrc } )
                if ( !fs::exists( needed, ec ) )
                    return Common::MakeError( "the bundle cannot carry Vulkan: " + needed.string() +
                                              " does not exist (brew install vulkan-loader molten-vk, or set "
                                              "HOMEBREW_PREFIX)" );

            const fs::path frameworks = appRoot / "Contents" / "Frameworks";
            const fs::path icdDir     = appRoot / "Contents" / "Resources" / "vulkan" / "icd.d";
            const fs::path loader     = frameworks / "libvulkan.1.dylib";
            const fs::path driver     = frameworks / "libMoltenVK.dylib";
            fs::create_directories( frameworks, ec );
            fs::create_directories( icdDir, ec );
            // canonical(): the Homebrew names are symlinks, and the bundle carries the files they point at.
            for ( const auto& [from, to] : { std::pair{ loaderSrc, loader }, std::pair{ mvkSrc, driver } } )
            {
                fs::copy_file( fs::canonical( from, ec ), to, fs::copy_options::overwrite_existing, ec );
                if ( ec )
                    return Common::MakeError( "could not copy " + from.string() +
                                              " into the bundle: " + ec.message() );
                fs::permissions( to, fs::perms::owner_write, fs::perm_options::add, ec );
            }

            // The loader's standard bundle location for driver manifests (Contents/Resources/vulkan/icd.d);
            // library_path is resolved relative to the manifest, so it climbs to Frameworks.
            auto icdRead = Common::Utils::FileSystem::ReadFileContent( icdSrc );
            if ( !icdRead )
                return Common::MakeError( icdRead.GetError() );
            std::string icd    = icdRead.ExtractValue();
            const auto  keyPos = icd.find( "\"library_path\"" );
            const auto  valStart =
                 keyPos == std::string::npos ? std::string::npos : icd.find( '\"', icd.find( ':', keyPos ) );
            const auto valEnd = valStart == std::string::npos ? std::string::npos : icd.find( '\"', valStart + 1 );
            if ( valEnd == std::string::npos )
                return Common::MakeError( icdSrc.string() + " has no library_path the bundle could repoint" );
            icd = icd.substr( 0, valStart + 1 ) + "../../../Frameworks/libMoltenVK.dylib" + icd.substr( valEnd );
            if ( const auto written =
                      Common::Utils::FileSystem::WriteContentToFileAtomic( icdDir / "MoltenVK_icd.json", icd );
                 !written )
                return Common::MakeError( "MoltenVK_icd.json was not written: " + written.GetError() );

            // Relink: the dylibs' own install names, then every reference of the player binary that names a
            // Vulkan loader outside the bundle (a synthesized non-Mach-O test runtime has none).
            if ( auto r = Run( "install_name_tool -id " + std::string( kBundledLoaderName ) + " " +
                               ShellQuote( loader.string() ) );
                 !r )
                return r;
            if ( auto r = Run( "install_name_tool -id " + std::string( kBundledDriverName ) + " " +
                               ShellQuote( driver.string() ) );
                 !r )
                return r;
            std::vector<fs::path> signs = { loader, driver };
            if ( IsMachO( playerBinary ) )
            {
                const auto paths = LinkedPaths( playerBinary );
                if ( !paths )
                    return Common::MakeError( paths.GetError() );
                for ( const std::string& path : paths.GetValue() )
                    if ( path.ends_with( "/libvulkan.1.dylib" ) && !IsBundleSafeReference( path ) )
                        if ( auto r = Run( "install_name_tool -change " + ShellQuote( path ) + " " +
                                           kBundledLoaderName + " " + ShellQuote( playerBinary.string() ) );
                             !r )
                            return r;
                signs.push_back( playerBinary );
            }
            for ( const fs::path& binary : signs )
                if ( auto r = Run( "codesign --force --sign - " + ShellQuote( binary.string() ) ); !r )
                    return r;

            return BundleIsSelfContained( appRoot );
        }
    } // namespace

    PackageResult PackageGame( const PackageOptions& options )
    {
        using Project::ProjectContext;

        if ( !ProjectContext::HasProject() )
            return { false, "No project is open.", "" };

        // THE SCHEME BEFORE THE COOK. A missing or broken ContentChunks.json is a refusal that needs
        // nothing but the file, so it is answered before the cook's minutes of work and before any
        // write: reading it after the cook made the person wait for a result that was never usable.
        const auto scheme = Common::Content::LoadChunkScheme( Common::Content::ChunkSchemePath() );
        if ( !scheme )
            return { false, scheme.GetError(), "" };

        // THE COOK FIRST, because it is a PRODUCER OF THE REGISTRY the gather below reads. Cook BEFORE
        // packing: every deterministic startup cost — shader SPIR-V, font atlases, icon SDFs, and the
        // textures the runtime has no decoder for — is paid here, once, into the project's Cooked/
        // tree, so the census below ships the artifacts and the player's first launch reads instead
        // of rebuilding. The cook writes files the registry must name, so the gather comes after
        // it; gathering before would ship a registry missing this cook's own output. It writes only
        // into the project's own cache, never into the output directory, so the rule below still holds.
        // Cooked for the TARGET runtime's profile (options.Config), not this editor's: a Debug editor
        // packaging a Release game must produce Release cache keys or the shipped cache never hits.
        // The shader SET is the target's too: a Shipping runtime cannot load the developer-instrument
        // programs, so its package neither cooks nor carries them (Common/Core/DeveloperOnlyShaders.hpp).
        const bool      developerInstruments = Common::ConfigHasDeveloperInstruments( options.Config );
        const CookStats cook =
             CookContentCaches( Core::SpirvDebugInfoForConfigName( options.Config ), developerInstruments );

        // BEFORE ANYTHING IS WRITTEN. A refusal after the output directory exists leaves half a
        // package behind, and half a package is the thing somebody ships by accident.
        if ( const std::string stale = GatherShippedRegistry(); !stale.empty() )
            return { false, stale, "" };

        const std::string projectName = ProjectContext::Current().Name;
        const std::string safeName    = Common::Settings::SanitizeProductName( projectName );

        // THE TARGET IS THIS EDITOR'S OWN HOST, and everything below that used to be a macOS literal now
        // comes out of that one description (PackageTarget.hpp): the binary's name, whether a .app is a
        // thing at all, the launcher, and the build script named in the error. The panel reads the same
        // description to say which platform it can produce, so the two cannot disagree — which is the
        // repair П6 was opened for.
        const TargetPlatformInfo& host = HostPlatformInfo();

        // 1) The Runtime binary for the chosen configuration, where the engine's build puts it: the
        // checkout's `build/Bin/<Config>/`, beside the engine directory (UE: EngineDir()/Binaries/<Platform>).
        // Read off EngineDir(), never off the working directory — a packager started from anywhere finds
        // the same binary. The FILE NAME is the host's: looking for an extensionless `Runtime` on Windows
        // could only ever fail, and it failed by naming a macOS build script in the message.
        const fs::path runtimeBin = Common::Constants::Path::EngineDir().parent_path() / "build" / "Bin" /
                                    options.Config / host.RuntimeBinary;
        std::error_code ec;
        if ( !fs::exists( runtimeBin, ec ) )
            return { false,
                     "Runtime binary not found (" + runtimeBin.string() +
                          "). Build it first: " + host.BuildScript + " " + options.Config,
                     "" };

        // The Visual C++ runtime DLLs the Windows player needs beside it (FindVcCrtRedistDir says why),
        // decided by Runtime.exe's own import table and resolved BEFORE the output directory exists: a
        // package without them is one that does not start, and a Debug Runtime (debug CRT, not
        // redistributable) is refused here by name.
        std::vector<fs::path> appLocalRuntime;
        if ( host.Platform == TargetPlatform::Windows )
        {
            auto crtDir = FindVcCrtRedistDir();
            if ( !crtDir )
                return { false,
                         "The Visual C++ runtime DLLs the game needs could not be found, so the package would not "
                         "start on a machine without the redistributable: " +
                              crtDir.GetError(),
                         "" };
            auto closure = Common::Utils::AppLocalRuntimeClosure( runtimeBin, crtDir.GetValue() );
            if ( !closure )
                return { false, "Cannot decide the app-local C++ runtime: " + closure.GetError(), "" };
            appLocalRuntime = closure.ExtractValue();
        }

        // Layout: a macOS .app bundle (default there) or a plain folder. Same content either way — the
        // pak keys are mount-root-relative, so whatever directory holds Content.dpak becomes the content
        // root the VFS serves from.
        //
        // A .app asked for on a host that has no such concept is REFUSED OUT LOUD rather than obeyed or
        // dropped: obeying it produced a bundle-shaped directory with a bash launcher inside on Windows,
        // and dropping it quietly is the silent substitution §1.4 forbids.
        bool bundle = options.MacAppBundle;
        if ( bundle && !host.SupportsAppBundle )
        {
            LOG_WARN( "[Package] a .app bundle was requested but {} has no such layout — packaging as a "
                      "plain folder with {}",
                      host.DisplayName, host.LauncherName );
            bundle = false;
        }

        // THE CONTENT SITS WHERE THE PLAYER LOOKS FROM ITS OWN EXECUTABLE (П5): FileSystem::PackagedContentDir,
        // one rule on both sides. A plain folder: beside the player binary. A .app: Contents/Resources —
        // Apple's signing rule keeps Contents/MacOS for code only. The player needs no flag and no launcher
        // to find it; running the binary directly works.
        const fs::path root    = fs::path( options.OutputDir ) / ( bundle ? safeName + ".app" : safeName );
        const fs::path gameDir = bundle ? root / "Contents" / "MacOS" : root;
        const char*    binName = bundle ? kBundlePlayerBinary : host.RuntimeBinary;
        const fs::path contentDir = Common::Utils::FileSystem::PackagedContentDir( gameDir );

        fs::create_directories( gameDir, ec );
        if ( !ec )
            fs::create_directories( contentDir, ec );
        if ( ec )
            return { false, "Cannot create output dir " + root.string() + ": " + ec.message(), "" };

        CopyStats   stats;
        std::string error;

        auto makeExecutable = [&]( const fs::path& p )
        {
            fs::permissions( p,
                             fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                                  fs::perms::others_read | fs::perms::others_exec,
                             ec );
        };

        // 2) Player binary.
        fs::copy_file( runtimeBin, gameDir / binName, fs::copy_options::overwrite_existing, ec );
        if ( ec )
            return { false, "Cannot copy the Runtime binary: " + ec.message(), "" };
        makeExecutable( gameDir / binName );
        ++stats.Files;

        // 2b) Windows: the app-local C++ runtime, beside the binary — the first directory the loader
        // searches, ahead of System32, so the game uses the copy it was tested with.
        for ( const fs::path& dll : appLocalRuntime )
        {
            fs::copy_file( dll, gameDir / dll.filename(), fs::copy_options::overwrite_existing, ec );
            if ( ec )
                return { false, "Cannot copy the C++ runtime " + dll.string() + ": " + ec.message(), "" };
            ++stats.Files;
        }
        if ( !appLocalRuntime.empty() )
        {
            std::string names;
            for ( const fs::path& dll : appLocalRuntime )
                names += ( names.empty() ? "" : ", " ) + dll.filename().string();
            LOG_INFO( "[Package] app-local C++ runtime from {}: {}",
                      appLocalRuntime.front().parent_path().string(), names );
        }

        // 3) The cook ran first, above the registry check — see there.

        // 4) ALL content AND the descriptor go into the archive SET — a base plus one archive per
        // chunk (UE .pak model), tree by tree out of the shared census (PackagedContentTrees.hpp) —
        // assets, cooked cache, shaders, fonts, icons — with the regenerated .deproj at the base
        // archive's ROOT under the one name the player looks for. The Runtime mounts the base beside
        // its own binary, reads the chunk list out of it, mounts those, and every content read
        // resolves through the same stack. Nothing loose, no flag.
        //
        // ONE ARCHIVE IS A STATED CHOICE, not the absence of one. The scheme was loaded above and a
        // project without ContentChunks.json never gets here (LoadChunkScheme refuses by path). A
        // scheme with `"Chunks": []` — what WriteDefaultChunkScheme writes — plans nothing but the
        // base chunk, so everything lands in Content.dpak; each named chunk adds a Chunk_<name>.dpak
        // beside it (ChunkArchivePath).
        std::vector<std::pair<std::string, fs::path>>    contentFiles;
        std::vector<std::pair<std::string, std::string>> baseBlobs;
        Common::Content::ChunkedWriteStats               writtenArchives;
        std::string                                      divisionSummary;
        {
            // Everything that ships is staged into the one cooked tree, then packed from it and from nothing else.
            std::vector<std::string> baseKeys;
            if ( !StageShippedContent( baseKeys, developerInstruments, error ) ||
                 !CollectCookedTree( baseKeys, contentFiles, baseBlobs, stats, error ) )
                return { false, error, "" };

            const auto plan = PlanTheDivision( scheme.GetValue() );
            if ( !plan )
                return { false, plan.GetError(), "" };

            auto written = Common::Content::WriteChunkedPaks( contentDir / "Content.dpak", plan.GetValue(),
                                                              contentFiles, baseBlobs );
            if ( !written )
                return { false, written.GetError(), "" };
            writtenArchives = written.GetValue();
            divisionSummary = DivisionSummary( plan.GetValue(), writtenArchives );
        }

        // 5) THE RECORD OF WHAT THIS RELEASE HANDS OUT (П7) — taken HERE because here is the only place
        // it can be taken. `PakTool patch` compares the next release's archive against the manifest of
        // this one, and a manifest can only be recorded while the version it describes still exists: a
        // release packaged without one is a release that can never be patched, and nothing later can
        // reconstruct it from the shipped folder. The patch READER has been in the tree since П3
        // (Runtime/Source/PackagedContent.cpp mounts every Patch*.dpak over the base); this is the
        // writer, and until now the path a real game takes had none.
        //
        // FROM THE ARCHIVE, not from the trees that went into it. The archive is what shipped — after
        // the raw-mesh filter, after the cook, with the descriptor in it — so a manifest of the source
        // trees would describe a release that does not exist, and the first thing to notice would be a
        // patch that re-ships files nobody changed.
        //
        // BESIDE THE PACKAGE, NOT INSIDE IT: the product is a binary and one archive, and the player
        // needs nothing from this file. See GamePackager.hpp.
        //
        // A FAILURE HERE FAILS THE PACKAGE, like every other step. The alternative is a package that
        // exists and can never be updated, discovered on the day somebody needs to ship a fix.
        //
        // EVERY ARCHIVE OF THE SET, not just the base. A manifest that described only the base would
        // report every chunked file as REMOVED the next time a patch was generated, and the patch
        // would re-ship the whole division to "restore" content that never left.
        const fs::path manifestPath = fs::path( options.OutputDir ) / ( safeName + kContentManifestExtension );
        {
            Common::Utils::ContentManifest release;
            for ( const fs::path& archive : writtenArchives.Archives )
            {
                const Common::Utils::PakReader packed( archive );
                if ( !packed.IsOpen() )
                    return { false,
                             archive.string() +
                                  " could not be reopened to record the release manifest: " + packed.OpenError(),
                             "" };
                // NAMED, NOT A TEMPORARY IN THE RANGE EXPRESSION. `for ( x : FromPak(p).Entries() )`
                // compiles and is undefined before C++23: the ContentManifest dies at the end of the
                // expression that initializes the range, and the loop then walks a freed vector. It
                // did exactly that here — the recorded manifest came out with ZERO entries against a
                // four-entry archive, and PackagedContent said so.
                const Common::Utils::ContentManifest ofArchive = Common::Utils::ContentManifest::FromPak( packed );
                for ( const Common::Utils::ContentManifestEntry& entry : ofArchive.Entries() )
                    release.Insert( entry );
            }

            const std::string manifest = release.Serialize();
            if ( const auto written =
                      Common::Utils::FileSystem::WriteContentToFileAtomic( manifestPath, manifest );
                 !written )
                return { false,
                         "The release manifest " + manifestPath.string() +
                              " could not be written: " + written.GetError() +
                              ". Without it this build can never be patched, so it is not a package.",
                         "" };
        }

        // 6) Bundle only: the bundle carries its own Vulkan - loader, MoltenVK and the ICD manifest - and the
        // player binary is relinked to the loader inside it (BundleVulkan above). No fallback to the target
        // machine's Homebrew: a bundle that needs one is not self-contained, so it is refused, not shipped.
        if ( bundle )
        {
            if ( const auto bundled = BundleVulkan( root, gameDir / binName ); !bundled )
                return { false, bundled.GetError(), "" };
            stats.Files += 3;
        }

        // 7) Launcher + (bundle) Info.plist. The launcher script is the bundle's CFBundleExecutable.
        //
        // WHAT THE LAUNCHER IS STILL FOR, now that it names neither the project (П5) nor the Vulkan driver
        // (ENG-ROOT-4b: the player finds the bundle's MoltenVK_icd.json itself, from its own executable
        // position — VulkanContext.cpp SelectDriverManifest) nor the working directory (the log goes to the
        // game's user directory): only being CFBundleExecutable. Running the binary directly works as well.
        if ( bundle )
        {
            std::ostringstream run;
            run << "#!/usr/bin/env bash\n"
                << "# Launches " << projectName << " (packaged by the Desert Editor).\n"
                << "set -euo pipefail\n"
                << "DIR=\"$(cd \"$(dirname \"$0\")\" && pwd)\"\n"
                << "exec \"$DIR/" << kBundlePlayerBinary << "\" \"$@\"\n";
            const fs::path launcher = gameDir / kBundleLauncherName;
            // This script IS the bundle's CFBundleExecutable — without it macOS reports the app as
            // damaged, which is the least diagnosable failure in this whole function.
            if ( const auto written = Common::Utils::FileSystem::WriteContentToFileAtomic( launcher, run.str() );
                 !written )
                return { false, "Could not write the launcher " + launcher.string() + ": " + written.GetError(),
                         "" };
            makeExecutable( launcher );

            std::ostringstream plist;
            plist << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                  << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                     "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                  << "<plist version=\"1.0\"><dict>\n"
                  << "  <key>CFBundleName</key><string>" << projectName << "</string>\n"
                  << "  <key>CFBundleExecutable</key><string>" << kBundleLauncherName << "</string>\n"
                  << "  <key>CFBundleIdentifier</key><string>com.desertengine."
                  << BundleIdentifierComponent( safeName ) << "</string>\n"
                  << "  <key>CFBundlePackageType</key><string>APPL</string>\n"
                  << "  <key>CFBundleShortVersionString</key><string>1.0</string>\n"
                  << "  <key>NSHighResolutionCapable</key><true/>\n"
                  << "</dict></plist>\n";
            const fs::path plistPath = root / "Contents" / "Info.plist";
            if ( const auto written =
                      Common::Utils::FileSystem::WriteContentToFileAtomic( plistPath, plist.str() );
                 !written )
                return { false, "Could not write " + plistPath.string() + ": " + written.GetError(), "" };
        }
        else
        {
            // The plain-folder launcher, in the host's own shell: cd and run. It sets no Vulkan
            // environment on either host — on macOS the player picks its MoltenVK manifest itself
            // (VulkanContext.cpp SelectDriverManifest; a plain folder has no Frameworks, so that is the
            // one the build machine recorded), on Windows the driver installs the loader. Writing the bash version on Windows produced a `run.sh`
            // nothing there can execute.
            std::ostringstream run;
            if ( host.Platform == TargetPlatform::Windows )
            {
                run << "@echo off\r\n"
                    << "REM Launches " << projectName << " (packaged by the Desert Editor).\r\n"
                    << "cd /d \"%~dp0\"\r\n"
                    << "\"" << host.RuntimeBinary << "\" %*\r\n";
            }
            else
            {
                run << "#!/usr/bin/env bash\n"
                    << "# Launches " << projectName << " (packaged by the Desert Editor).\n"
                    << "set -euo pipefail\n"
                    << "cd \"$(dirname \"$0\")\"\n"
                    << "exec ./" << host.RuntimeBinary << " \"$@\"\n";
            }
            const fs::path launcher = root / host.LauncherName;
            if ( const auto written = Common::Utils::FileSystem::WriteContentToFileAtomic( launcher, run.str() );
                 !written )
                return { false, "Could not write " + launcher.string() + ": " + written.GetError(), "" };
            makeExecutable( launcher );
        }

        std::ostringstream msg;
        msg << "Packaged '" << projectName << "' -> " << fs::absolute( root, ec ).string() << "  (" << stats.Files
            << " files, " << ( stats.Bytes / ( std::size_t{ 1024 } * 1024 ) ) << " MB, " << options.Config
            << " runtime" << ( bundle ? ", Vulkan bundled" : "" ) << ")";
        msg << divisionSummary;
        // WHAT THE COOK COULD NOT PUT IN, said in the result rather than left to a log line nobody
        // reads — and carried as NUMBERS on PackageResult as well as prose, because a caller has to be
        // able to BRANCH on it (see PackageResult's own note). Both kinds are named because they are
        // different facts: content that could not be read is already-broken content the project may
        // have chosen to ship, an artifact that could not be written is a hole in the shipped cache.
        if ( cook.Failures > 0 )
            msg << "  WARNING: " << cook.Failures
                << " asset(s) could not be cooked; the game will read the sources at every start (or "
                   "report them broken)";
        if ( cook.StoreFailures > 0 )
            msg << "  WARNING: " << cook.StoreFailures
                << " cooked artifact(s) could not be written; the game will rebuild them at every start";
        LOG_INFO( "[Package] {}", msg.str() );
        return { true,          msg.str(),          fs::absolute( root, ec ).string(),
                 cook.Failures, cook.StoreFailures, fs::absolute( manifestPath, ec ).string() };
    }
    PackageResult BuildContentPak()
    {
        using Project::ProjectContext;
        if ( !ProjectContext::HasProject() )
            return { false, "No project is open.", "" };

        // The scheme before the cook, for PackageGame's reason.
        const auto scheme = Common::Content::LoadChunkScheme( Common::Content::ChunkSchemePath() );
        if ( !scheme )
            return { false, scheme.GetError(), "" };

        // Same cook as PackageGame, for THIS build's profile: the dev pak serves the runtime the
        // developer launches next to this editor, which is built in the same configuration. (A
        // cross-config dev runtime misses and self-heals into loose Cooked/ — dev machines are
        // writable; only the shipped package must never rely on that.) FIRST, for PackageGame's
        // reason: the cook writes files the gather below must name.
        const CookStats cook = CookContentCaches( Core::SpirvDebugInfoThisBuild(), DESERT_DEV_INSTRUMENTS != 0 );

        // The same gather PackageGame makes, for the same reason: this archive is what a developer's
        // Runtime mounts, so its registry must name what the archive holds.
        if ( const std::string stale = GatherShippedRegistry(); !stale.empty() )
            return { false, stale, "" };

        std::error_code ec;
        const fs::path pakPath = fs::path( ProjectContext::Directory() ) / "Content.dpak";

        CopyStats   stats;
        std::string error;

        // The same cooked tree PackageGame packs — one staging, two entry points — so "a .dpak this tree
        // produced" means ONE thing rather than two, descriptor and registry included.
        std::vector<std::pair<std::string, fs::path>>    contentFiles;
        std::vector<std::pair<std::string, std::string>> baseBlobs;
        std::vector<std::string>                         baseKeys;
        if ( !StageShippedContent( baseKeys, DESERT_DEV_INSTRUMENTS != 0, error ) ||
             !CollectCookedTree( baseKeys, contentFiles, baseBlobs, stats, error ) )
            return { false, error, "" };

        // AND THE SAME DIVISION. A dev archive set that was not divided the way the shipped one is
        // would make the developer's runtime read a layout no player ever gets, which is the one
        // property this entry point exists to avoid.
        const auto plan = PlanTheDivision( scheme.GetValue() );
        if ( !plan )
            return { false, plan.GetError(), "" };

        const auto written =
             Common::Content::WriteChunkedPaks( pakPath, plan.GetValue(), contentFiles, baseBlobs );
        if ( !written )
            return { false, written.GetError(), "" };

        std::ostringstream msg;
        msg << "Content.dpak rebuilt: " << stats.Files << " file(s), "
            << ( stats.Bytes / ( std::size_t{ 1024 } * 1024 ) ) << " MB -> "
            << fs::absolute( pakPath, ec ).string();
        msg << DivisionSummary( plan.GetValue(), written.GetValue() );
        // Both counts, for the reason PackageGame's twin above states at length.
        if ( cook.Failures > 0 )
            msg << "  WARNING: " << cook.Failures
                << " asset(s) could not be cooked; the game will read the sources at every start (or "
                   "report them broken)";
        if ( cook.StoreFailures > 0 )
            msg << "  WARNING: " << cook.StoreFailures
                << " cooked artifact(s) could not be written; the game will rebuild them at every start";
        LOG_INFO( "[Package] {}", msg.str() );
        return { true, msg.str(), fs::absolute( pakPath, ec ).string(), cook.Failures, cook.StoreFailures };
    }
} // namespace Desert::Editor
