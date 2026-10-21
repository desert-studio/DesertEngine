// TextureCook — cook named texture sources into a project's Cooked/ tree.
//
//   TextureCook <project.deproj> [--engine-dir <dir>] [--intent <name>] <source> [<source> ...]
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
// `--intent` is what the texture IS (Core::Formats::kTextureIntentNames: Colour, NormalMap, Mask, Data), the
// word a material slot gives an image imported for it (TextureImporter::ImportSourceAsset). Written on a FIRST
// import only — an existing asset keeps its settings, as in the editor. Without it a loose image is
// Unspecified (sRGB), which is wrong for a normal or data map imported outside a mesh.
//
// Exit status: 0 when every source ends with a correct `.tex` on the disk (Cooked or Fresh), 1 when any
// did not, 2 for a command line it cannot act on. Every source gets one line on stdout saying which.

#include <ToolMain.hpp>
#include <ToolProject.hpp>

#include <Editor/Import/TextureImporter.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace
{
    int Usage()
    {
        std::fprintf(
             stderr,
             "usage: TextureCook <project.deproj> [--engine-dir <dir>] [--intent <name>] <source> [<source> ...]\n"
             "       TextureCook <project.deproj> [--engine-dir <dir>] --emit <asset.detex> <out.tex>\n"
             "  paths are relative to the project's folder, e.g. Resources/Splash/Splash.detex\n"
             "  --engine-dir: the folder holding the engine's Resources/ (default: from this binary)\n"
             "  --intent: Colour, NormalMap, Mask or Data, for a source imported the first time\n" );
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

             // --engine-dir, when given, overrides the engine directory found from the executable.
             int      first = 2;
             fs::path engineDirOverride;
             if ( std::string( args[first] ) == "--engine-dir" )
             {
                 if ( count < first + 3 )
                     return Usage();
                 engineDirOverride = args[first + 1];
                 first += 2;
             }
             // The engine directory from the executable (or --engine-dir), the log in the project's
             // Saved/Logs, the project opened outside the recent list: one call for every tool that opens a
             // project (ToolProject.hpp).
             const Desert::Tools::OpenedToolProject opened =
                  Desert::Tools::OpenToolProject( args[1], engineDirOverride );
             if ( !opened.Refused.empty() )
             {
                 std::fprintf( stderr, "TextureCook: %s\n", opened.Refused.c_str() );
                 return 2;
             }
             const fs::path& deproj = opened.Descriptor;

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

             auto intent = Desert::Core::Formats::TextureIntent::Unspecified;
             if ( count > first && std::string( args[first] ) == "--intent" )
             {
                 if ( count < first + 3 )
                     return Usage();
                 intent = Desert::Core::Formats::TextureIntentFromName( args[first + 1] );
                 if ( intent == Desert::Core::Formats::TextureIntent::Count )
                 {
                     std::fprintf( stderr,
                                   "TextureCook: '%s' is not a texture intent (Colour, NormalMap, Mask, Data)\n",
                                   args[first + 1] );
                     return 2;
                 }
                 first += 2;
             }

             Desert::Editor::TextureImporter importer;
             int                             status = 0;
             for ( int i = first; i < count; ++i )
             {
                 const fs::path  source = InProject( args[i] );
                 std::error_code ec;
                 if ( !fs::is_regular_file( source, ec ) )
                 {
                     std::fprintf( stderr, "TextureCook: '%s' is not a file under '%s'\n", args[i],
                                   deproj.parent_path().string().c_str() );
                     status = 1;
                     continue;
                 }
                 // A raw image is imported first, with the intent, and the ASSET is cooked (Cook would
                 // import it as Unspecified); an asset path is its own asset.
                 fs::path asset = source;
                 if ( Desert::Editor::TextureImporter::AssetPathFor( source ) != source )
                 {
                     const auto imported = Desert::Editor::TextureImporter::ImportSourceAsset( source, intent );
                     if ( !imported.IsSuccess() )
                     {
                         std::fprintf( stderr, "TextureCook: %s: %s\n", args[i], imported.GetError().c_str() );
                         status = 1;
                         continue;
                     }
                     asset = imported.GetValue();
                 }
                 const auto result = importer.Cook( asset );
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
