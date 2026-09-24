// TextureCook — cook named texture sources into a project's Cooked/ tree.
//
//   TextureCook <project.deproj> <source> [<source> ...]
//
// Each <source> is a path relative to the directory the project's editor runs from (the one holding
// `Resources/` — the tool works from the descriptor's folder, as GamePackager does), and lands where the
// editor would put it: the `.detex` asset beside the source, its platform data in the DDC. The cook is
// `TextureImporter::Cook` itself, so an up-to-date `.tex` is reported Fresh and not rewritten.
//
// Exit status: 0 when every source ends with a correct `.tex` on the disk (Cooked or Fresh), 1 when any
// did not, 2 for a command line it cannot act on. Every source gets one line on stdout saying which.

#include <ToolMain.hpp>

#include <Editor/Import/TextureImporter.hpp>
#include <Engine/Project/ProjectContext.hpp>

#include <Common/Core/Logger.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace
{
    int Usage()
    {
        std::fprintf( stderr,
                      "usage: TextureCook <project.deproj> <source> [<source> ...]\n"
                      "  <source> is relative to the project's folder, e.g. Resources/Splash/Splash.jpg\n" );
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

             // Where the editor stands: engine resources resolve against the working directory.
             fs::current_path( deproj.parent_path(), ec );
             if ( ec )
             {
                 std::fprintf( stderr, "TextureCook: could not work from '%s': %s\n",
                               deproj.parent_path().string().c_str(), ec.message().c_str() );
                 return 2;
             }

             Common::Logger::LogInit();

             if ( !Desert::Project::ProjectContext::Open( deproj.string(),
                                                          Desert::Project::ProjectContext::RecordInRecent::No ) )
             {
                 std::fprintf( stderr, "TextureCook: could not open '%s' (missing or corrupt .deproj)\n",
                               deproj.string().c_str() );
                 return 2;
             }

             Desert::Editor::TextureImporter importer;
             int                             status = 0;
             for ( int i = 2; i < count; ++i )
             {
                 const fs::path source = args[i];
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
