// AssetRegistryTool — writes and checks a project's COOKED ASSET REGISTRY without booting an engine.
//
//   AssetRegistryTool cook  <project.deproj> [--disk]   write Cooked/AssetRegistry.dreg
//   AssetRegistryTool check <project.deproj> [--disk]   exit 1 if that registry disagrees with its source
//   AssetRegistryTool list  <project.deproj>            print every row, kind first
//
// ── TWO SOURCES, AND THE DIFFERENCE IS NAMED RATHER THAN SMOOTHED OVER ────────────────────────────
//
// By default `cook` and `check` ask THE REPOSITORY: `git ls-files`, filtered through the content
// census. `--disk` asks THIS MACHINE: every content file under the roots, whatever git thinks of it.
// They are different questions and they have different right answers, so the tool prints which one it
// answered on every run rather than letting a reader assume.
//
// WHY THE REPOSITORY IS THE DEFAULT. `Cooked/AssetRegistry.dreg` is COMMITTED, and a committed file is
// a claim about what a clean clone carries. Cooking it from the disk writes the cooking machine's
// state into a shared artifact: measured on `dev`, ten rows for files no other checkout has, all of
// them one developer's `Editor/Cooked/` output, which `.gitignore` excludes by design. The gate that
// holds this file (`Desert/Tests/Editor/CookedRegistryGate`) asks the repository question too, so
// `cook` then `check` then CI agree by construction rather than by luck.
//
// WHEN `--disk` IS THE RIGHT ONE. A project that is not a git checkout at all — a game somebody is
// making with this engine — has no repository to ask, and its registry is simply a description of its
// disk. The packager's refusal is the other case: it is about to pack a directory, so what is IN that
// directory is exactly its question, and `Editor/Source/Editor/Packaging/GamePackager.cpp` says so at
// the refusal.
//
// RUN IT FROM THE DIRECTORY THE ENGINE RUNS FROM — the one that holds `Resources/`. Engine resource
// roots are never remapped by a project and so resolve against the working directory; the tool refuses
// rather than silently cooking a registry with no shaders in it.
//
// ── WHY A TOOL AND NOT ONLY THE EDITOR'S COOK ─────────────────────────────────────────────────────
//
// Since GAP_ANALYSIS T2.4 neither host walks the content roots at boot; both read the registry. That
// leaves two jobs no editor session can do.
//
//   * BOOTSTRAP. A project with no registry has no content, and the editor cannot write one on the way
//     up — its own shader preload is the first thing that reads the registry, so it never reaches the
//     cook. Measured while building this: the editor died at `Could not find the shader: StaticMeshPBR`
//     before its first frame. `cook` is what breaks that circle, and it needs no GPU and no window.
//   * THE GATE. `check` is the failing half of T2.7: once the walk is gone, a file that is on disk and
//     not in the registry does NOT reach the shipped game, and the failure is packaged-build-only —
//     exactly the class `AssetPreloadCensus` was built for in the eager model. It errors rather than
//     warning, per the task's own wording, and CI runs it through Desert/Tests/Editor/CookedRegistryGate.
//
// ── WHAT IT DELIBERATELY DOES NOT WRITE ───────────────────────────────────────────────────────────
//
// The DECLARED IDENTITY column (a `.tex`'s own `Handle`, a `.demat`'s `MaterialId`), the DEPENDENCY
// EDGES and the BOUNDS of a mesh or a prefab. All three require parsing the asset with the class that
// owns its format, and tools here link `Common` only — deliberately, so that they still build on a
// machine that cannot build an engine. The editor fills them (`ContentRegistry::NoteAsset` at the moment
// `CreateAsset` parses the file, the bounds in its post-boot cook) and writes them back, so the columns
// converge after one session rather than being wrong for ever; this tool carries them across a cook.
//
// So `check` asserts what this tool can honestly know: that the set of rows and the set of content
// files on disk are the same set, that each row's recorded size is the file's size, and that each row's
// HEADER column (GUID and subsystem versions) is what the file's own header states — read without the
// body, as UE's asset registry reads a package summary (AF7). It does not assert the u64 identity column,
// because a tool that cannot compute a value must not judge it.

#include <ToolMain.hpp>

#include <Common/Content/ContentKinds.hpp>
#include <Common/Content/ContentScan.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Project/ProjectFormat.hpp>
#include <Common/Utilities/AssetRegistry.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>

namespace fs = std::filesystem;

namespace
{
    int Usage()
    {
        std::fprintf( stderr, "usage: AssetRegistryTool <cook|check|list> <project.deproj> [--disk]\n"
                              "  default source: the files git tracks (what a clean clone carries)\n"
                              "  --disk:         the content roots on this machine\n" );
        return 2;
    }

    int Fail( const std::string& message )
    {
        std::fprintf( stderr, "AssetRegistryTool: %s\n", message.c_str() );
        return 1;
    }

    // Points `Constants::Path` at the project, which is what makes every content root — and therefore
    // every stable key — mean the same thing here as it does inside the engine. Without it the roots
    // are the built-in sandbox's, relative to wherever the tool was launched, and the keys would name
    // a checkout rather than a project. That exact defect ("a path resolved from the process's WORKING
    // DIRECTORY rather than from the file being worked on") is named four times over in Constants.hpp.
    Common::ResultStr<fs::path> OpenProject( const fs::path& projectPath )
    {
        const auto json = Common::Utils::FileSystem::ReadFileContent( projectPath.string() );
        if ( !json )
            return Common::MakeFormattedError<fs::path>( "cannot read {}: {}", projectPath.string(),
                                                         json.GetError() );

        const auto project = Common::Project::ReadProjectFile( json.GetValue() );
        if ( !project )
            return Common::MakeFormattedError<fs::path>( "{} is not a readable project: {}", projectPath.string(),
                                                         project.GetError() );

        const fs::path projectDir = fs::absolute( projectPath ).parent_path().lexically_normal();
        Common::Constants::Path::SetProjectRoot( projectDir, project.GetValue().AssetsRoot );

        // AND THE WORKING DIRECTORY HAS TO BE THE ENGINE'S, which is a refusal rather than a note.
        //
        // `SetProjectRoot` moves every PROJECT root and leaves the ENGINE resource roots alone, by
        // design (Constants.hpp: "Engine resources are never remapped"). Those are relative paths —
        // `Resources/Shaders/` — so they resolve against the process's working directory, and both
        // hosts `cd` into the directory that holds them before starting (scripts/MacOS/RunEditor.sh).
        // A cook run from anywhere else finds no shaders, writes a registry with 76 rows missing, and
        // exits 0: a silent partial answer about the one file the whole boot now depends on. Measured
        // while building this — the first cook produced 171 rows instead of 247.
        std::error_code shaderEc;
        if ( !fs::is_directory( Common::Constants::Path::SHADERDIR_PATH, shaderEc ) )
        {
            return Common::MakeFormattedError<fs::path>(
                 "'{}' does not exist from the current directory. Engine resources are never remapped by "
                 "the project, so they resolve against the WORKING DIRECTORY — run this tool from the "
                 "same directory the editor runs from (the one that holds Resources/), or the registry "
                 "would be written without a single shader row",
                 Common::Constants::Path::SHADERDIR_PATH.string() );
        }

        return Common::MakeSuccess( fs::path( projectDir ) );
    }

    // The file list, from whichever of the two sources was asked for, plus the sentence that says which
    // — printed on every run, because a tool that answers one of two questions without saying which is
    // the instrument this whole round was spent repairing.
    struct ContentSource
    {
        std::map<std::string, Common::Content::ContentFile> Files;
        std::string                                         What; // the whole sentence, for the log
        std::string                                         Noun; // reads inside a disagreement
        bool                                                Usable = false;
    };

    ContentSource SourceFor( const fs::path& projectPath, bool fromDisk )
    {
        if ( fromDisk )
        {
            return { Common::Content::ScanContentRoots(), "the content roots on this disk",
                     "on the content roots of this disk", true };
        }

        // The repository root is where the project lives, walked up until git answers. Passing the
        // project directory is enough: `git -C` resolves the enclosing checkout itself.
        auto tracked = Common::Content::TrackedContent( projectPath.parent_path() );
        if ( !tracked )
        {
            return { {},
                     "git could not answer for " + projectPath.parent_path().string() +
                          " (not a checkout, or no git). That is not the same as 'nothing is tracked', so "
                          "the tool refuses rather than writing an empty registry. Use --disk if this "
                          "project is genuinely not in a repository.",
                     {},
                     false };
        }
        return { std::move( *tracked ), "the files this repository tracks", "tracked by this repository", true };
    }

    int Cook( const fs::path& projectPath, bool fromDisk )
    {
        const auto opened = OpenProject( projectPath );
        if ( !opened )
            return Fail( opened.GetError() );

        const ContentSource source = SourceFor( projectPath, fromDisk );
        if ( !source.Usable )
            return Fail( source.What );
        std::printf( "AssetRegistryTool: cooking from %s\n", source.What.c_str() );

        const fs::path out = Common::Utils::AssetRegistry::DefaultPath();

        // THE EXISTING REGISTRY IS READ FIRST AND ITS IDENTITY AND EDGE COLUMNS ARE KEPT. Re-cooking is
        // a routine operation — CI runs it to compare — and a cook that dropped the columns it
        // cannot compute would make every run of this tool erase what the editor had learned, which is
        // the "middle link drops a property" shape, delivered by the tool meant to guard against it.
        Common::Utils::AssetRegistry previous;
        if ( Common::Utils::FileSystem::Exists( out ) )
        {
            auto loaded = Common::Utils::AssetRegistry::LoadFrom( out );
            if ( !loaded )
                return Fail( loaded.GetError() );
            previous = loaded.GetValue(); // GetValue() is a const reference; a move here would be a copy
        }

        Common::Utils::AssetRegistry registry;
        for ( const auto& [key, file] : source.Files )
        {
            Common::Utils::AssetRegistryEntry entry;
            entry.Key  = key;
            entry.Kind = std::string( Common::Content::KindName( file.Kind ) );
            entry.Size = file.Size;
            // THE HEADER COLUMN IS READ FROM THE FILE'S HEADER, never carried: it is the one identity this
            // tool can compute without the body. A header that is there and unreadable stops the cook -
            // writing the row without it would publish "this file states no GUID", which is false.
            if ( !file.HeaderError.empty() )
                return Fail( key + ": " + file.HeaderError );
            if ( file.Header )
            {
                if ( file.Header->Kind != file.Kind )
                    return Fail( key + ": its header states kind '" +
                                 std::string( Common::Content::KindName( file.Header->Kind ) ) +
                                 "' and it sits where a '" + entry.Kind + "' belongs" );
                entry.Guid     = file.Header->Guid;
                entry.Versions = file.Header->Subsystems;
                std::sort( entry.Versions.begin(), entry.Versions.end(),
                           []( const Common::Content::SubsystemVersion& a,
                               const Common::Content::SubsystemVersion& b ) { return a.Tag < b.Tag; } );
            }
            if ( const Common::Utils::AssetRegistryEntry* old = previous.FindByKey( key ) )
            {
                entry.Identity     = old->Identity;
                entry.Dependencies = old->Dependencies;
                entry.Bounds       = old->Bounds;
            }
            if ( const auto inserted = registry.Insert( std::move( entry ) ); !inserted )
                return Fail( inserted.GetError() );
        }

        // A COOK THAT EMPTIES A NON-EMPTY REGISTRY IS REFUSED, and nothing is written.
        //
        // It happened here, once, inside the repair this tool exists for: the file list came back empty
        // because `git ls-files --full-name` prints repository-relative paths and they were being joined
        // to the wrong root, so every file fell outside the content roots. The tool cheerfully replaced
        // 247 rows with none and printed "0 row(s)", which reads as success. Printing BOTH counts is
        // what makes a collapse visible; refusing is what makes it harmless.
        if ( registry.Empty() && !previous.Empty() )
        {
            std::fprintf( stderr,
                          "AssetRegistryTool: REFUSED — cooking would have replaced %zu row(s) with none, "
                          "and nothing was written. If this project really has no content, delete\n  %s\n"
                          "by hand; if it does have content, the source answered the wrong question — "
                          "compare with --disk.\n",
                          previous.Count(), out.string().c_str() );
            return 1;
        }

        std::error_code ec;
        fs::create_directories( out.parent_path(), ec );
        if ( const auto written = Common::Utils::FileSystem::WriteContentToFileAtomic( out, registry.Serialize() );
             !written )
            return Fail( written.GetError() );

        std::printf( "AssetRegistryTool: %zu row(s) -> %zu row(s) in %s\n", previous.Count(), registry.Count(),
                     out.string().c_str() );
        return 0;
    }

    int Check( const fs::path& projectPath, bool fromDisk )
    {
        const auto opened = OpenProject( projectPath );
        if ( !opened )
            return Fail( opened.GetError() );

        const ContentSource source = SourceFor( projectPath, fromDisk );
        if ( !source.Usable )
            return Fail( source.What );
        std::printf( "AssetRegistryTool: checking against %s\n", source.What.c_str() );

        const fs::path out = Common::Utils::AssetRegistry::DefaultPath();
        if ( !Common::Utils::FileSystem::Exists( out ) )
            return Fail( out.string() + " does not exist: this project has no cooked asset registry, so "
                                        "a build of it would ship no content. Run 'AssetRegistryTool cook'." );

        auto loaded = Common::Utils::AssetRegistry::LoadFrom( out );
        if ( !loaded )
            return Fail( loaded.GetError() );

        // THE COMPARISON IS `Common::Content::Compare` and not a loop written here, which is
        // the point of that function existing: this tool, the CI gate and the packager all ask the
        // same question, and three answers to it would be three chances to disagree about what
        // "current" means.
        const auto problems = Common::Content::Compare( loaded.GetValue(), source.Files, source.Noun );

        for ( const Common::Content::RegistryDisagreement& problem : problems )
            std::fprintf( stderr, "%s\n", problem.Detail.c_str() );

        if ( !problems.empty() )
        {
            std::fprintf( stderr, "AssetRegistryTool: %zu disagreement(s) between %s and the content tree.\n",
                          problems.size(), out.string().c_str() );
            return 1;
        }

        std::printf( "AssetRegistryTool: %zu row(s), and the content tree agrees with every one of them\n",
                     loaded.GetValue().Count() );
        return 0;
    }

    int List( const fs::path& projectPath )
    {
        const auto opened = OpenProject( projectPath );
        if ( !opened )
            return Fail( opened.GetError() );

        auto loaded = Common::Utils::AssetRegistry::LoadFrom( Common::Utils::AssetRegistry::DefaultPath() );
        if ( !loaded )
            return Fail( loaded.GetError() );

        for ( const Common::Utils::AssetRegistryEntry& row : loaded.GetValue().Entries() )
        {
            std::printf( "%-22s %10llu  %016llx  %zu dep(s)  %s\n", row.Kind.c_str(),
                         static_cast<unsigned long long>( row.Size ),
                         static_cast<unsigned long long>( row.EffectiveHandle() ), row.Dependencies.size(),
                         row.Key.c_str() );
        }
        std::printf( "%zu row(s)\n", loaded.GetValue().Count() );
        return 0;
    }
} // namespace

int main( int argc, char** argv )
{
    return Desert::Tools::RunMain( "AssetRegistryTool", argc, argv,
                                   []( int count, char** args ) -> int
                                   {
                                       if ( count < 3 )
                                           return Usage();

                                       const fs::path project( args[2] );

                                       bool fromDisk = false;
                                       for ( int i = 3; i < count; ++i )
                                       {
                                           if ( std::strcmp( args[i], "--disk" ) != 0 )
                                               return Usage();
                                           fromDisk = true;
                                       }

                                       if ( std::strcmp( args[1], "cook" ) == 0 )
                                           return Cook( project, fromDisk );
                                       if ( std::strcmp( args[1], "check" ) == 0 )
                                           return Check( project, fromDisk );
                                       if ( std::strcmp( args[1], "list" ) == 0 )
                                           return List( project );
                                       return Usage();
                                   } );
}
