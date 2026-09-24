#include "GamePackager.hpp"
#include "PackageCook.hpp"
#include "PackageTarget.hpp"
#include "PackagedContentTrees.hpp"

#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/WorldCells.hpp>
#include <Engine/Core/ShaderCompiler/ShaderCacheKey.hpp>
#include <Engine/Project/ProjectContext.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Project/ProjectFormat.hpp>
#include <Common/Utilities/ContentManifest.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Content/ContentChunks.hpp>
#include <Common/Content/ContentScan.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>

#include <Common/Utilities/AssetRegistry.hpp>
#include <Common/Utilities/PakFile.hpp>

#include <algorithm>
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

        // Lists every file under `from` as ("<keyPrefix>/<relative>", source), skipping raw mesh
        // sources and packing texture assets in their cooked form when asked (the project asset tree).
        //
        // IT COLLECTS INSTEAD OF WRITING, and that is the whole seam the chunked layout needed. It
        // used to stream straight into one `PakWriter`, which fixed the number of archives at one in
        // the only function that knows what the files are; now the list is built first and
        // `WriteChunkedPaks` decides which archive each entry belongs to. Nothing else about the
        // traversal changed.
        // The pak key the cooked registry ships under — the one path the packager writes rather than collects.
        std::string ShippedRegistryKey()
        {
            return ( fs::path( Common::Constants::Path::COOKED_DIR_NAME ) / "AssetRegistry.dreg" )
                 .generic_string();
        }

        bool CollectTree( const fs::path& from, const std::string& keyPrefix, bool skipRawMeshSources,
                          std::vector<std::pair<std::string, fs::path>>& files, CopyStats& stats,
                          std::string& error )
        {
            std::error_code ec;
            if ( !fs::exists( from, ec ) )
                return true; // nothing to pack is fine (e.g. no Cooked/ yet)

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
                if ( skipRawMeshSources && IsRawMeshSource( src ) )
                    continue;

                const fs::path rel = fs::relative( src, from, ec );
                if ( ec )
                {
                    error = "cannot relativize " + src.string() + ": " + ec.message();
                    return false;
                }
                // A registry left on the disk from before the registry built itself (AF9) is not packed:
                // the one the pak carries is written from this process's gather below, under the same key,
                // and a stale copy would collide with it or, worse, be the one the game read.
                if ( keyPrefix + "/" + rel.generic_string() == ShippedRegistryKey() )
                    continue;
                fs::path packed = src;
                if ( skipRawMeshSources && src.extension() == Assets::kTextureAssetExtension )
                {
                    auto staged = StageCookedTextureAsset( src, rel );
                    if ( !staged.IsSuccess() )
                    {
                        error = staged.GetError();
                        return false;
                    }
                    packed = staged.GetValue();
                }
                files.emplace_back( keyPrefix + "/" + rel.generic_string(), packed );
                ++stats.Files;
                // The error_code overload returns uintmax_t(-1) on failure, so an unchecked add here
                // does not report a slightly wrong size — it reports 16 exabytes, and the package's own
                // success message is where that lands. The file is already IN the archive at this point;
                // only the reported total is affected, so this is a count that skips, not a failure.
                // Its OWN error_code, not the walk's: the loop's `if ( ec )` at the top reads "the
                // directory walk failed", and letting a size query write into that slot would report a
                // walk failure for a file that was read perfectly well.
                std::error_code sizeEc;
                const auto      size = fs::file_size( packed, sizeEc );
                if ( !sizeEc )
                    stats.Bytes += size;
            }
            return true;
        }

        std::string SanitizeName( std::string name )
        {
            for ( auto& ch : name )
                if ( ch == ' ' || ch == '/' || ch == '\\' )
                    ch = '_';
            return name.empty() ? std::string( "Game" ) : name;
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
        // at the start and every other cell on a worker when the camera wants it (WorldStreamer.hpp), from
        // WorldCells::CookedWorldDirectory of the scene's key. Cooked here, from the scene as it is on disk, with
        // the registry the editor plans with — so the cells are the ones the editor's Play streams. A scene
        // file that states a partition and does not cook fails the package: the game would fail on it too.
        bool CollectCookedWorlds( const std::vector<std::pair<std::string, fs::path>>& contentFiles,
                                  std::vector<std::pair<std::string, std::string>>& baseBlobs, CopyStats& stats,
                                  std::string& error )
        {
            for ( const auto& [key, path] : contentFiles )
            {
                if ( path.extension() != ".desce" )
                    continue;
                auto text = Common::Utils::FileSystem::ReadFileContent( path );
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
                if ( !scene.GetValue().WorldPartition.has_value() )
                    continue;
                auto cooked = Core::WorldCells::CookWorld( scene.GetValue(),
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
                    ++stats.Files;
                    stats.Bytes += file.Bytes.size();
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

        // THE DIVISION, DERIVED FROM THIS PROJECT'S OWN DATA. The registry stack carries the edges,
        // and the scheme file carries only what cannot be derived from them (chunk roots, and the
        // keys pinned to the base). An ABSENT scheme is not an error and not a default set of
        // chunks: it is a project that has not been divided, and it must package to exactly the one
        // archive every project produced before chunks existed.
        Common::ResultStr<Common::Content::ChunkPlan> PlanTheDivision()
        {
            Common::Content::ChunkScheme scheme;
            const fs::path               schemePath = Common::Content::ChunkSchemePath();
            if ( Common::Utils::FileSystem::Exists( schemePath ) )
            {
                const auto text = Common::Utils::FileSystem::ReadFileContent( schemePath.string() );
                if ( !text )
                    return Common::MakeFormattedError<Common::Content::ChunkPlan>(
                         "{} exists but could not be read: {}", schemePath.string(), text.GetError() );
                auto parsed = Common::Content::ParseChunkScheme( text.GetValue() );
                if ( !parsed )
                    return Common::MakeFormattedError<Common::Content::ChunkPlan>( "{}: {}", schemePath.string(),
                                                                                   parsed.GetError() );
                scheme = parsed.GetValue();
            }
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

    PackageResult PackageGame( const PackageOptions& options )
    {
        using Project::ProjectContext;

        if ( !ProjectContext::HasProject() )
            return { false, "No project is open.", "" };

        // THE COOK FIRST, because it is a PRODUCER OF THE REGISTRY the gather below reads. Cook BEFORE
        // packing: every deterministic startup cost — shader SPIR-V, font atlases, icon SDFs, and the
        // textures the runtime has no decoder for — is paid here, once, into the project's Cooked/
        // tree, so the census below ships the artifacts and the player's first launch reads instead
        // of rebuilding. The cook writes files the registry must name, so the gather comes after
        // it; gathering before would ship a registry missing this cook's own output. It writes only
        // into the project's own cache, never into the output directory, so the rule below still holds.
        // Cooked for the TARGET runtime's profile (options.Config), not this editor's: a Debug editor
        // packaging a Release game must produce Release cache keys or the shipped cache never hits.
        const CookStats cook = CookContentCaches( Core::SpirvDebugInfoForConfigName( options.Config ) );

        // BEFORE ANYTHING IS WRITTEN. A refusal after the output directory exists leaves half a
        // package behind, and half a package is the thing somebody ships by accident.
        if ( const std::string stale = GatherShippedRegistry(); !stale.empty() )
            return { false, stale, "" };

        const std::string projectName = ProjectContext::Current().Name;
        const std::string safeName    = SanitizeName( projectName );

        // THE TARGET IS THIS EDITOR'S OWN HOST, and everything below that used to be a macOS literal now
        // comes out of that one description (PackageTarget.hpp): the binary's name, whether a .app is a
        // thing at all, the launcher, and the build script named in the error. The panel reads the same
        // description to say which platform it can produce, so the two cannot disagree — which is the
        // repair П6 was opened for.
        const TargetPlatformInfo& host = HostPlatformInfo();

        // 1) The Runtime binary for the chosen configuration (editor cwd is Editor/). The FILE NAME is
        // the host's: looking for an extensionless `Runtime` on Windows could only ever fail, and it
        // failed by naming a macOS build script in the message.
        const fs::path  runtimeBin = fs::path( ".." ) / "build" / "Bin" / options.Config / host.RuntimeBinary;
        std::error_code ec;
        if ( !fs::exists( runtimeBin, ec ) )
            return { false,
                     "Runtime binary not found (" + runtimeBin.string() +
                          "). Build it first: " + host.BuildScript + " " + options.Config,
                     "" };

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

        // THE CONTENT SITS BESIDE THE PLAYER BINARY, IN BOTH LAYOUTS (П5). The Runtime has exactly one
        // rule for finding a game — look in its own executable's directory — and a bundle that put the
        // archive in Contents/Resources could not satisfy it, so the launcher had to cd there and hand
        // the descriptor over as `--project`. That flag is what made the package unstartable by hand:
        // a player who ran the binary directly got "No game to run". Removing the flag means removing
        // the reason it was needed, which is this split. Contents/Resources is simply not produced —
        // macOS requires no such directory, and a second place the player has to be told about is
        // exactly the knowledge a shipped game must not depend on.
        const fs::path root    = fs::path( options.OutputDir ) / ( bundle ? safeName + ".app" : safeName );
        const fs::path gameDir = bundle ? root / "Contents" / "MacOS" : root;
        const fs::path fwDir   = root / "Contents" / "Frameworks"; // bundle only
        const char*    binName = bundle ? kBundlePlayerBinary : host.RuntimeBinary;

        fs::create_directories( gameDir, ec );
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

        // 3) The cook ran first, above the registry check — see there.

        // 4) ALL content AND the descriptor go into the archive SET — a base plus one archive per
        // chunk (UE .pak model), tree by tree out of the shared census (PackagedContentTrees.hpp) —
        // assets, cooked cache, shaders, fonts, icons — with the regenerated .deproj at the base
        // archive's ROOT under the one name the player looks for. The Runtime mounts the base beside
        // its own binary, reads the chunk list out of it, mounts those, and every content read
        // resolves through the same stack. Nothing loose, no flag.
        //
        // AN UNDIVIDED PROJECT STILL PRODUCES EXACTLY ONE Content.dpak. The plan's base chunk is the
        // total default, so a project with no scheme file packages byte-for-byte the way it did
        // before chunks existed.
        std::vector<std::pair<std::string, fs::path>>    contentFiles;
        std::vector<std::pair<std::string, std::string>> baseBlobs;
        Common::Content::ChunkedWriteStats               writtenArchives;
        std::string                                      divisionSummary;
        {
            for ( const PackagedTree& tree : PackagedContentTrees() )
                if ( !CollectTree( tree.Source, tree.PakKey, tree.StripRawMeshSources, contentFiles, stats,
                                   error ) )
                    return { false, error, "" };

            // THE COOKED REGISTRY IS WRITTEN HERE, at packaging, from the registry this process gathered: the
            // packaged game has no content roots to gather from, and nothing in git carries one (AF9a).
            baseBlobs.emplace_back( ShippedRegistryKey(), Assets::ContentRegistry::Get().Serialize() );
            if ( !CollectCookedWorlds( contentFiles, baseBlobs, stats, error ) )
                return { false, error, "" };

            // FAILS THE PACKAGE, like every other step in this function: an archive without the
            // descriptor is a folder of content the player cannot identify as a game, and the
            // failure would land on somebody who has no sources.
            if ( !CollectDescriptor( ProjectContext::Current(), baseBlobs, error ) )
                return { false, error, "" };
            ++stats.Files;

            const auto plan = PlanTheDivision();
            if ( !plan )
                return { false, plan.GetError(), "" };

            auto written = Common::Content::WriteChunkedPaks( gameDir / "Content.dpak", plan.GetValue(),
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

        // 6) Bundle only: MoltenVK + the Vulkan loader travel INSIDE Contents/Frameworks so the player
        // machine needs no Homebrew. The ICD json is rewritten to point at the bundled dylib (the
        // loader resolves library_path relative to the json file).
        bool bundledVulkan = false;
        if ( bundle )
        {
            const char*    envPrefix = std::getenv( "HOMEBREW_PREFIX" );
            const fs::path brew      = envPrefix ? fs::path( envPrefix ) : fs::path( "/opt/homebrew" );

            const fs::path loaderSrc = brew / "lib" / "libvulkan.1.dylib";
            const fs::path mvkSrc    = brew / "lib" / "libMoltenVK.dylib";
            const fs::path icdSrc    = brew / "etc" / "vulkan" / "icd.d" / "MoltenVK_icd.json";

            if ( fs::exists( loaderSrc, ec ) && fs::exists( mvkSrc, ec ) && fs::exists( icdSrc, ec ) )
            {
                fs::create_directories( fwDir, ec );
                // copy_options::none on a symlink source copies the TARGET file (what we want).
                fs::copy_file( fs::canonical( loaderSrc, ec ), fwDir / "libvulkan.1.dylib",
                               fs::copy_options::overwrite_existing, ec );
                fs::copy_file( fs::canonical( mvkSrc, ec ), fwDir / "libMoltenVK.dylib",
                               fs::copy_options::overwrite_existing, ec );

                // Guarded by fs::exists(icdSrc) above; an unreadable file degrades to the same
                // "library_path key not found" no-op patch the old empty read produced.
                auto        icdRead = Common::Utils::FileSystem::ReadFileContent( icdSrc );
                std::string icd     = icdRead ? icdRead.ExtractValue() : std::string{};
                const auto  keyPos = icd.find( "\"library_path\"" );
                if ( keyPos != std::string::npos )
                {
                    const auto valStart = icd.find( '\"', icd.find( ':', keyPos ) );
                    const auto valEnd   = icd.find( '\"', valStart + 1 );
                    if ( valStart != std::string::npos && valEnd != std::string::npos )
                        icd = icd.substr( 0, valStart + 1 ) + "./libMoltenVK.dylib" + icd.substr( valEnd );
                }
                const auto icdWritten =
                     Common::Utils::FileSystem::WriteContentToFileAtomic( fwDir / "MoltenVK_icd.json", icd );
                if ( !icdWritten )
                    LOG_WARN( "[Package] MoltenVK_icd.json was not written: {} — the .app falls back to "
                              "the target machine's Homebrew Vulkan",
                              icdWritten.GetError() );

                // Not fatal, and now honest about it: the bundle only CLAIMS to carry Vulkan when the
                // ICD that points at the bundled dylib is really there. It used to claim it whenever
                // the copies succeeded, so a package with an unwritten ICD reported "Vulkan bundled"
                // and then failed to find a driver on a machine without Homebrew.
                bundledVulkan = !ec && icdWritten.IsSuccess();
                stats.Files += 3;
            }
            else
            {
                LOG_WARN( "[Package] Homebrew Vulkan artifacts not found under {} — the .app will fall "
                          "back to the target machine's Homebrew",
                          brew.string() );
            }
        }

        // 7) Launcher + (bundle) Info.plist. The launcher script is the bundle's CFBundleExecutable:
        // dyld reads DYLD_* only at process start, so the env MUST be set before the real binary execs.
        //
        // WHAT THE LAUNCHER IS STILL FOR, now that it no longer names the project (П5): the Vulkan
        // environment, and only that. Finder gives a double-clicked .app no VK_ICD_FILENAMES and no
        // DYLD_FALLBACK_LIBRARY_PATH, and both must exist BEFORE the image is loaded, so no amount of
        // work inside the player can replace this. It is therefore not a second way to start the game
        // — running the binary directly works and is tested — it is the environment the host does not
        // provide.
        if ( bundle )
        {
            std::ostringstream run;
            run << "#!/usr/bin/env bash\n"
                << "# Launches " << projectName << " (packaged by the Desert Editor).\n"
                << "set -euo pipefail\n"
                << "DIR=\"$(cd \"$(dirname \"$0\")\" && pwd)\"\n"
                << "if [ -f \"$DIR/../Frameworks/MoltenVK_icd.json\" ]; then\n"
                << "  export VK_ICD_FILENAMES=\"$DIR/../Frameworks/MoltenVK_icd.json\"\n"
                << "  export "
                   "DYLD_FALLBACK_LIBRARY_PATH=\"$DIR/../"
                   "Frameworks${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}\"\n"
                << "else\n"
                << "  BREW_PREFIX=\"${HOMEBREW_PREFIX:-$(brew --prefix 2>/dev/null || echo /opt/homebrew)}\"\n"
                << "  export "
                   "VK_ICD_FILENAMES=\"${VK_ICD_FILENAMES:-$BREW_PREFIX/etc/vulkan/icd.d/MoltenVK_icd.json}\"\n"
                << "  export "
                   "DYLD_FALLBACK_LIBRARY_PATH=\"$BREW_PREFIX/"
                   "lib${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}\"\n"
                << "fi\n"
                // The game directory IS this script's own directory now, so the cd is only about where
                // engine_log.txt lands — the player finds its content from its executable path.
                << "cd \"$DIR\"\n"
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
                  << "  <key>CFBundleIdentifier</key><string>com.desertengine." << safeName << "</string>\n"
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
            // The plain-folder launcher, in the host's own shell. On macOS it has to find MoltenVK
            // through Homebrew (there is no Frameworks directory outside a bundle); on Windows the
            // Vulkan loader is installed by the graphics driver and there is nothing to point at, so the
            // script only has to cd and run. Writing the bash version on Windows produced a `run.sh`
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
                    << "BREW_PREFIX=\"${HOMEBREW_PREFIX:-$(brew --prefix 2>/dev/null || echo /opt/homebrew)}\"\n"
                    << "export "
                       "VK_ICD_FILENAMES=\"${VK_ICD_FILENAMES:-$BREW_PREFIX/etc/vulkan/icd.d/"
                       "MoltenVK_icd.json}\"\n"
                    << "export "
                       "DYLD_FALLBACK_LIBRARY_PATH=\"$BREW_PREFIX/"
                       "lib${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}\"\n"
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
            << " runtime" << ( bundle ? ( bundledVulkan ? ", Vulkan bundled" : ", Vulkan NOT bundled" ) : "" )
            << ")";
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

        // Same cook as PackageGame, for THIS build's profile: the dev pak serves the runtime the
        // developer launches next to this editor, which is built in the same configuration. (A
        // cross-config dev runtime misses and self-heals into loose Cooked/ — dev machines are
        // writable; only the shipped package must never rely on that.) FIRST, for PackageGame's
        // reason: the cook writes files the gather below must name.
        const CookStats cook = CookContentCaches( Core::SpirvDebugInfoThisBuild() );

        // The same gather PackageGame makes, for the same reason: this archive is what a developer's
        // Runtime mounts, so its registry must name what the archive holds.
        if ( const std::string stale = GatherShippedRegistry(); !stale.empty() )
            return { false, stale, "" };

        std::error_code ec;
        const fs::path pakPath = fs::path( ProjectContext::Directory() ) / "Content.dpak";

        CopyStats   stats;
        std::string error;

        // The same census PackageGame packs — one list, two entry points (see PackagedContentTrees.hpp).
        std::vector<std::pair<std::string, fs::path>>    contentFiles;
        std::vector<std::pair<std::string, std::string>> baseBlobs;
        for ( const PackagedTree& tree : PackagedContentTrees() )
            if ( !CollectTree( tree.Source, tree.PakKey, tree.StripRawMeshSources, contentFiles, stats, error ) )
                return { false, error, "" };

        // THE COOKED REGISTRY IS WRITTEN HERE, at packaging, from the registry this process gathered: the
        // packaged game has no content roots to gather from, and nothing in git carries one (AF9a).
        baseBlobs.emplace_back( ShippedRegistryKey(), Assets::ContentRegistry::Get().Serialize() );
        if ( !CollectCookedWorlds( contentFiles, baseBlobs, stats, error ) )
            return { false, error, "" };

        // ...and the same descriptor, so "a .dpak this tree produced" means ONE thing rather than two.
        // A dev pak that carried content but no identity was an archive only the other entry point's
        // output could be started from, and the difference between the two would be discovered by
        // whoever dropped a Runtime beside this one and got "No game to run".
        if ( !CollectDescriptor( ProjectContext::Current(), baseBlobs, error ) )
            return { false, error, "" };
        ++stats.Files;

        // AND THE SAME DIVISION. A dev archive set that was not divided the way the shipped one is
        // would make the developer's runtime read a layout no player ever gets, which is the one
        // property this entry point exists to avoid.
        const auto plan = PlanTheDivision();
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
