// TextureCook — cook named texture sources into a project's Cooked/ tree.
//
//   TextureCook <project.deproj> [--engine-dir <dir>] <source> [<source> ...]
//   TextureCook <project.deproj> [--engine-dir <dir>] --emit <asset.detex> <out.tex>
//
// `--emit` writes an asset's platform data (its DDC entry, derived first if absent) to a file. It exists for
// ONE file: Resources/Splash/Splash.tex, which the splash reads before the engine (and so the DDC) exists.
//
// Each <source> (and each --emit path) is relative to the PROJECT's folder — the descriptor's — whatever
// folder the shell is in (Common::Constants::Path::FullPath; the process never changes directory), and
// lands where the editor would put it: the `.detex` asset beside the source, its platform data in the DDC. The
// cook is `TextureImporter::Cook` itself, so an up-to-date `.tex` is reported Fresh and not rewritten.
//
// Exit status: 0 when every source ends with a correct `.tex` on the disk (Cooked or Fresh), 1 when any
// did not, 2 for a command line it cannot act on. Every source gets one line on stdout saying which.

#include <ToolMain.hpp>
#include <ToolEngineDir.hpp>

#include <Editor/Import/TextureImporter.hpp>
#include <Engine/Project/ProjectContext.hpp>

#include <Common/Core/Logger.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace
{
    int Usage()
    {
        std::fprintf( stderr,
                      "usage: TextureCook <project.deproj> [--engine-dir <dir>] <source> [<source> ...]\n"
                      "       TextureCook <project.deproj> [--engine-dir <dir>] --emit <asset.detex> <out.tex>\n"
                      "  paths are relative to the project's folder, e.g. Resources/Splash/Splash.detex\n"
                      "  --engine-dir: the folder holding the engine's Resources/ (default: from this binary)\n" );
        return 2;
    }

    const char* OutcomeName( const Desert::Editor::TextureCookOutcome outcome )
    {
        switch ( outcome )
        {
            case Desert::Editor::TextureCookOutcome::Cooked:
                return "cooked";
            case Desert::Editor::TextureCookOutcome::Fresh:
                return "fresh";
            case Desert::Editor::TextureCookOutcome::Failed:
                return "FAILED";
            case Desert::Editor::TextureCookOutcome::Unwritten:
                return "UNWRITTEN";
        }
        return "UNKNOWN";
    }
} // namespace

int main( int argc, char** argv )
{
    return Desert::Tools::RunMain(
         "TextureCook", argc, argv,
         []( int count, char** args ) -> int
         {
             if ( count < 3 )
                 return Usage();

             std::error_code ec;
             const fs::path  deproj = fs::absolute( args[1], ec );
             if ( ec || !fs::is_regular_file( deproj, ec ) )
             {
                 std::fprintf( stderr, "TextureCook: '%s' is not a .deproj file this process can read\n",
                               args[1] );
                 return 2;
             }

             // The engine directory from the executable (or --engine-dir), never from the working
             // directory: the process stays where the shell started it (ToolEngineDir.hpp).
             int      first = 2;
             fs::path engineDirOverride;
             if ( std::string( args[first] ) == "--engine-dir" )
             {
                 if ( count < first + 3 )
                     return Usage();
                 engineDirOverride = args[first + 1];
                 first += 2;
             }
             if ( const std::string refused = Desert::Tools::SetEngineDirFromExecutable( engineDirOverride );
                  !refused.empty() )
             {
                 std::fprintf( stderr, "TextureCook: %s\n", refused.c_str() );
                 return 2;
             }

             Common::Logger::LogInit();
             Common::Logger::RelocateLogFile( deproj.parent_path() );

             if ( !Desert::Project::ProjectContext::Open( deproj.string(),
                                                          Desert::Project::ProjectContext::RecordInRecent::No ) )
             {
                 std::fprintf( stderr, "TextureCook: could not open '%s' (missing or corrupt .deproj)\n",
                               deproj.string().c_str() );
                 return 2;
             }

             // Project-relative, as the usage states (UE: FPaths::ConvertRelativePathToFull off ProjectDir).
             const auto InProject = []( const char* path ) { return Common::Constants::Path::FullPath( path ); };

             if ( std::string( args[first] ) == "--emit" )
             {
                 if ( count != first + 3 )
                     return Usage();
                 const fs::path asset  = InProject( args[first + 1] );
                 const fs::path target = InProject( args[first + 2] );
                 const auto     bytes  = Desert::Editor::TextureImporter::BuildPlatformData( asset );
                 if ( !bytes.IsSuccess() )
                 {
                     std::fprintf( stderr, "TextureCook: %s\n", bytes.GetError().c_str() );
                     return 1;
                 }
                 std::ofstream out( target, std::ios::binary | std::ios::trunc );
                 out.write( bytes.GetValue().data(), static_cast<std::streamsize>( bytes.GetValue().size() ) );
                 out.close();
                 if ( !out )
                 {
                     std::fprintf( stderr, "TextureCook: could not write '%s'\n", target.string().c_str() );
                     return 1;
                 }
                 std::fprintf( stdout, "TextureCook: emitted %s -> %s (%zu bytes)\n", asset.string().c_str(),
                               target.string().c_str(), bytes.GetValue().size() );
                 return 0;
             }

             Desert::Editor::TextureImporter importer;
             int                             status = 0;
             for ( int i = first; i < count; ++i )
             {
                 const fs::path source = InProject( args[i] );
                 if ( !fs::is_regular_file( source, ec ) )
                 {
                     std::fprintf( stderr, "TextureCook: '%s' is not a file under '%s'\n", args[i],
                                   deproj.parent_path().string().c_str() );
                     status = 1;
                     continue;
                 }
                 const auto result = importer.Cook( source );
                 const bool ok     = result.Outcome == Desert::Editor::TextureCookOutcome::Cooked ||
                                 result.Outcome == Desert::Editor::TextureCookOutcome::Fresh;
                 std::fprintf( ok ? stdout : stderr, "TextureCook: %s %s -> %s\n", OutcomeName( result.Outcome ),
                               args[i], Desert::Editor::TextureImporter::AssetPathFor( source ).string().c_str() );
                 if ( !ok )
                     status = 1;
             }
             return status;
         } );
}
