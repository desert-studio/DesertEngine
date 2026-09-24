// AssetRegistryTool — gathers a project's asset registry without booting an engine.
//
//   AssetRegistryTool cook <project.deproj>   write Saved/Cooked/<Platform>/AssetRegistry.dreg
//   AssetRegistryTool list <project.deproj>   print every gathered row, kind first
//
// THE REGISTRY IS NOT A COMMITTED ARTIFACT. Every consumer gathers it where it is used: the editor and a
// loose Runtime at start (`ContentRegistry::Gather`, reusing <project>/Intermediate/AssetRegistry.cache),
// the packager right before it packs (`GamePackager`, which ships its own copy inside the pak). A stored
// copy therefore cannot drift into a consequence, and the two old jobs of this tool went with it: `check`
// compared a committed file with the tree (there is no committed file), and the repository/`--disk` split
// chose which tree to describe (a gather describes the content roots of the machine it runs on, the same
// ones the editor would read). "Every tracked content file is gathered" is CookedRegistryGate's case.
//
// What remains is a cook with no GPU and no window: the same gather, written where the packager stages
// its cook (`DDC::PlatformCookedDir`), for a build machine or a look at the rows. A file whose header the
// gather refuses fails the cook — a registry without it would ship without it.
//
// The identity and dependency columns need the asset's own parser, which this tool does not link (it links
// `Common` only, so it builds where an engine cannot). They come from this machine's registry cache when
// its row is current (same size and modification time), exactly as in the editor; a mesh's box comes from
// its 64-byte header.
//
// RUN IT FROM THE DIRECTORY THE ENGINE RUNS FROM — the one that holds `Resources/`. Engine resource
// roots are never remapped by a project and so resolve against the working directory; the tool refuses
// rather than silently cooking a registry with no shaders in it.

#include <ToolMain.hpp>

#include <Common/Content/ContentScan.hpp>
#include <Common/Content/DerivedDataCache.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Project/ProjectFormat.hpp>
#include <Common/Utilities/AssetRegistry.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace
{
    int Usage()
    {
        std::fprintf( stderr, "usage: AssetRegistryTool <cook|list> <project.deproj>\n" );
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

    // The gather both commands answer from. The machine's cache is used when readable; an unreadable one
    // is named and the gather starts from nothing, which costs a header read per file and loses no row.
    Common::Content::GatheredRegistry Gather()
    {
        Common::Content::RegistryCache cache;
        const fs::path                 cachePath = Common::Content::RegistryCachePath();
        if ( Common::Utils::FileSystem::Exists( cachePath ) )
        {
            const auto text = Common::Utils::FileSystem::ReadFileContent( cachePath.string() );
            if ( !text )
                std::fprintf( stderr, "AssetRegistryTool: %s not used: %s\n", cachePath.string().c_str(),
                              text.GetError().c_str() );
            else if ( const auto parsed = Common::Content::ParseRegistryCache( text.GetValue() ); !parsed )
                std::fprintf( stderr, "AssetRegistryTool: %s not used: %s\n", cachePath.string().c_str(),
                              parsed.GetError().c_str() );
            else
                cache = parsed.GetValue();
        }

        Common::Content::GatheredRegistry gathered = Common::Content::GatherContentRegistry( cache );
        std::printf( "AssetRegistryTool: gathered %zu row(s): %zu from the cache, %zu header(s) read, %zu "
                     "refused\n",
                     gathered.Registry.Count(), gathered.FromCache, gathered.Read, gathered.Refused.size() );
        return gathered;
    }

    int Cook( const fs::path& projectPath )
    {
        const auto opened = OpenProject( projectPath );
        if ( !opened )
            return Fail( opened.GetError() );

        const Common::Content::GatheredRegistry gathered = Gather();
        for ( const std::string& refusal : gathered.Refused )
            std::fprintf( stderr, "%s\n", refusal.c_str() );
        if ( !gathered.Refused.empty() )
            return Fail( std::to_string( gathered.Refused.size() ) +
                         " content file(s) refused by the gather; nothing was written, because a registry "
                         "without them would ship without them" );

        const fs::path  out = Common::DDC::PlatformCookedDir() / "AssetRegistry.dreg";
        std::error_code ec;
        fs::create_directories( out.parent_path(), ec );
        if ( ec )
            return Fail( "cannot create " + out.parent_path().string() + ": " + ec.message() );
        if ( const auto written =
                  Common::Utils::FileSystem::WriteContentToFileAtomic( out, gathered.Registry.Serialize() );
             !written )
            return Fail( written.GetError() );

        std::printf( "AssetRegistryTool: %zu row(s) in %s\n", gathered.Registry.Count(), out.string().c_str() );
        return 0;
    }

    int List( const fs::path& projectPath )
    {
        const auto opened = OpenProject( projectPath );
        if ( !opened )
            return Fail( opened.GetError() );

        const Common::Content::GatheredRegistry gathered = Gather();
        for ( const Common::Utils::AssetRegistryEntry& row : gathered.Registry.Entries() )
        {
            std::printf( "%-22s %10llu  %016llx  %zu dep(s)  %s\n", row.Kind.c_str(),
                         static_cast<unsigned long long>( row.Size ),
                         static_cast<unsigned long long>( row.EffectiveHandle() ), row.Dependencies.size(),
                         row.Key.c_str() );
        }
        for ( const std::string& refusal : gathered.Refused )
            std::fprintf( stderr, "%s\n", refusal.c_str() );
        return gathered.Refused.empty() ? 0 : 1;
    }
} // namespace

int main( int argc, char** argv )
{
    return Desert::Tools::RunMain( "AssetRegistryTool", argc, argv,
                                   []( int count, char** args ) -> int
                                   {
                                       if ( count != 3 )
                                           return Usage();
                                       const fs::path project( args[2] );
                                       if ( std::strcmp( args[1], "cook" ) == 0 )
                                           return Cook( project );
                                       if ( std::strcmp( args[1], "list" ) == 0 )
                                           return List( project );
                                       return Usage();
                                   } );
}
