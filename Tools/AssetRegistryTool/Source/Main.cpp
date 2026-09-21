// AssetRegistryTool — writes and checks a project's COOKED ASSET REGISTRY without booting an engine.
//
//   AssetRegistryTool cook  <project.deproj>   walk the content roots, write Cooked/AssetRegistry.dreg
//   AssetRegistryTool check <project.deproj>   exit 1 if the committed registry disagrees with the tree
//   AssetRegistryTool list  <project.deproj>   print every row, kind first
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
// The DECLARED IDENTITY column (a `.tex`'s own `Handle`, a `.demat`'s `MaterialId`) and the DEPENDENCY
// EDGES. Both require parsing the asset with the class that owns its format, and tools here link
// `Common` only — deliberately, so that they still build on a machine that cannot build an engine. The
// editor fills both on its next boot (`ContentRegistry::NoteAsset` at the moment `CreateAsset` parses
// the file) and writes them back in its post-boot cook, so the columns converge after one session
// rather than being wrong for ever.
//
// So `check` asserts what this tool can honestly know: that the set of rows and the set of content
// files on disk are the same set, and that each row's recorded size is the file's size. It does not
// assert the identity column, because a tool that cannot compute a value must not judge it.

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
#include <string>

namespace fs = std::filesystem;

namespace
{
    int Usage()
    {
        std::fprintf( stderr, "usage: AssetRegistryTool <cook|check|list> <project.deproj>\n" );
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

    int Cook( const fs::path& projectPath )
    {
        const auto opened = OpenProject( projectPath );
        if ( !opened )
            return Fail( opened.GetError() );

        const fs::path out = Common::Utils::AssetRegistry::DefaultPath();

        // THE EXISTING REGISTRY IS READ FIRST AND ITS IDENTITY AND EDGE COLUMNS ARE KEPT. Re-cooking is
        // a routine operation — CI runs it to compare — and a cook that dropped the two columns it
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
        for ( const auto& [key, file] : Common::Content::ScanContentRoots() )
        {
            Common::Utils::AssetRegistryEntry entry;
            entry.Key  = key;
            entry.Kind = std::string( Common::Content::KindName( file.Kind ) );
            entry.Size = file.Size;
            if ( const Common::Utils::AssetRegistryEntry* old = previous.FindByKey( key ) )
            {
                entry.Identity     = old->Identity;
                entry.Dependencies = old->Dependencies;
            }
            if ( const auto inserted = registry.Insert( std::move( entry ) ); !inserted )
                return Fail( inserted.GetError() );
        }

        std::error_code ec;
        fs::create_directories( out.parent_path(), ec );
        if ( const auto written = Common::Utils::FileSystem::WriteContentToFileAtomic( out, registry.Serialize() );
             !written )
            return Fail( written.GetError() );

        std::printf( "AssetRegistryTool: %zu row(s) -> %s\n", registry.Count(), out.string().c_str() );
        return 0;
    }

    int Check( const fs::path& projectPath )
    {
        const auto opened = OpenProject( projectPath );
        if ( !opened )
            return Fail( opened.GetError() );

        const fs::path out = Common::Utils::AssetRegistry::DefaultPath();
        if ( !Common::Utils::FileSystem::Exists( out ) )
            return Fail( out.string() + " does not exist: this project has no cooked asset registry, so "
                                        "a build of it would ship no content. Run 'AssetRegistryTool cook'." );

        auto loaded = Common::Utils::AssetRegistry::LoadFrom( out );
        if ( !loaded )
            return Fail( loaded.GetError() );

        // THE COMPARISON IS `Common::Content::CompareWithDisk` and not a loop written here, which is
        // the point of that function existing: this tool, the CI gate and the packager all ask the
        // same question, and three answers to it would be three chances to disagree about what
        // "current" means.
        const auto problems =
             Common::Content::CompareWithDisk( loaded.GetValue(), Common::Content::ScanContentRoots() );

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
                                       if ( std::strcmp( args[1], "cook" ) == 0 )
                                           return Cook( project );
                                       if ( std::strcmp( args[1], "check" ) == 0 )
                                           return Check( project );
                                       if ( std::strcmp( args[1], "list" ) == 0 )
                                           return List( project );
                                       return Usage();
                                   } );
}
