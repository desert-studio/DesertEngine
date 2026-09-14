// DomeSheet — the whole celestial dome in one readable picture.
//
// WHY IT EXISTS. The cloud programme's verification protocol was six look directions: three elevations
// times two azimuths. Six rays is a SAMPLE of the sky, and this project's history is a list of what a
// sample costs. An empty zenith above about twenty degrees survived a ten-merge programme because every
// frame was shot from the horizon. Full-width horizontal bands needed sunward azimuth AND high elevation
// AT THE SAME TIME, so a protocol that varied one axis at a time could never produce the frame that
// showed them; they were found by accident, once, and had been there the whole time
// (Docs/Clouds/REVIEW_622a01a6.md section 9). Neither defect is subtle in a frame. Both were invisible in
// the frames that were taken.
//
// So this tool sweeps azimuth and elevation together and assembles the result into one sheet a person
// reads in a second. It does TWO jobs, and their being one program is the point:
//
//   --plan       print the sampling plan: one line per tile, TAB separated, each line carrying the
//                `--look` vector, the file stem and the human label. The capture script does not compute
//                any of them.
//   (assemble)   read the captured PNGs back, shrink them, lay them out and BURN THE LABEL IN.
//
// The label a reader sees and the ray the editor was pointed along therefore come out of one function
// (DomeSheetLayout.hpp, MakeDomeSample). A sheet that mislabels its tiles is worse than no sheet, because
// it still looks like evidence, and the two sides of that mistake are individually correct in a way no
// test of either side alone would catch — which is the defect shape the verify skill's table lists four
// times over.
//
// Depends on nothing but the vendored stb, exactly as ImageStat, LineJump and ImageDiff do.
//
// Usage:
//   DomeSheet --plan [--elevations 5,25,45,65,85] [--azimuths 8]
//   DomeSheet --out <sheet.png> --title "<caption>" [--cols N] [--scale K] [--gap G] [--label-scale S]
//             <tile.png> <label> [<tile.png> <label> ...]
//
// The assemble form takes the label beside its file rather than re-deriving it from the name: a tile
// whose label had to be guessed from its path is a tile that can be labelled wrongly by a rename.

#define STB_IMAGE_IMPLEMENTATION
#include <ToolMain.hpp>

#include <stb_image/stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image/stb_image_write.h>

#include "DomeSheetLayout.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace Desert::DomeSheet;

namespace
{
    /// A comma-separated list of integers, consumed WHOLE. The partial-read failure this avoids is the
    /// one the editor's own command line was rewritten to avoid: "5,25,x" must not silently mean "5,25".
    bool ParseIntList( const char* text, std::vector<int>& out )
    {
        out.clear();
        const char* cursor = text;
        while ( *cursor != '\0' )
        {
            char*      end   = nullptr;
            const long value = std::strtol( cursor, &end, 10 );
            if ( end == cursor )
                return false;
            out.push_back( static_cast<int>( value ) );
            cursor = end;
            if ( *cursor == ',' )
                ++cursor;
            else if ( *cursor != '\0' )
                return false;
        }
        return !out.empty();
    }

    int EmitPlan( const std::vector<int>& elevations, int azimuths )
    {
        const std::vector<DomeSample> plan = MakeDomePlan( elevations, azimuths );
        if ( plan.empty() )
        {
            std::fprintf( stderr, "DomeSheet: an empty plan (elevations %zu, azimuths %d)\n", elevations.size(),
                          azimuths );
            return 2;
        }

        for ( const DomeSample& sample : plan )
        {
            // Six digits, which is what `--look` is parsed at and enough that two adjacent columns of a
            // 360-column sweep would still differ. TAB separated because the label contains spaces.
            std::printf( "%s\t%.6f,%.6f,%.6f\t%s\n", DomeStem( sample ).c_str(), sample.LookX, sample.LookY,
                         sample.LookZ, DomeLabel( sample ).c_str() );
        }
        return 0;
    }

    bool LoadImage( const char* path, Image& out )
    {
        int            width    = 0;
        int            height   = 0;
        int            channels = 0;
        unsigned char* data     = stbi_load( path, &width, &height, &channels, 3 );
        if ( data == nullptr )
            return false;

        out.Width  = width;
        out.Height = height;
        out.Pixels.assign( data, data + static_cast<std::size_t>( width ) * height * 3 );
        stbi_image_free( data );
        return true;
    }
} // namespace

static int RunTool( int argc, char** argv )
{
    std::vector<int> elevations{ 5, 25, 45, 65, 85 };
    int              azimuths = 8;
    bool             planOnly = false;

    std::string output;
    std::string title;
    int         columns    = 0; // 0 = "as many as there are azimuths", resolved below
    int         scale      = 4;
    int         gap        = 6;
    int         labelScale = 2;

    std::vector<std::string> tilePaths;
    std::vector<std::string> tileLabels;

    for ( int i = 1; i < argc; ++i )
    {
        const std::string arg        = argv[i];
        auto              needsValue = [&]( const char* name ) -> const char*
        {
            if ( i + 1 >= argc )
            {
                std::fprintf( stderr, "DomeSheet: '%s' needs a value\n", name );
                std::exit( 2 );
            }
            return argv[++i];
        };

        if ( arg == "--plan" )
            planOnly = true;
        else if ( arg == "--elevations" )
        {
            const char* value = needsValue( "--elevations" );
            if ( !ParseIntList( value, elevations ) )
            {
                std::fprintf( stderr, "DomeSheet: --elevations '%s' is not a comma-separated integer list\n",
                              value );
                return 2;
            }
        }
        else if ( arg == "--azimuths" )
            azimuths = std::atoi( needsValue( "--azimuths" ) );
        else if ( arg == "--out" )
            output = needsValue( "--out" );
        else if ( arg == "--title" )
            title = needsValue( "--title" );
        else if ( arg == "--cols" )
            columns = std::atoi( needsValue( "--cols" ) );
        else if ( arg == "--scale" )
            scale = std::atoi( needsValue( "--scale" ) );
        else if ( arg == "--gap" )
            gap = std::atoi( needsValue( "--gap" ) );
        else if ( arg == "--label-scale" )
            labelScale = std::atoi( needsValue( "--label-scale" ) );
        else if ( !arg.empty() && arg[0] == '-' )
        {
            // Every token is recognised or the run stops, for the reason the editor's parser gives at
            // length: a dropped token here means a sheet assembled from the wrong tiles under the right
            // name, and that is a picture nobody can tell is wrong.
            std::fprintf( stderr, "DomeSheet: unrecognised argument '%s'\n", arg.c_str() );
            return 2;
        }
        else if ( tilePaths.size() == tileLabels.size() )
            tilePaths.push_back( arg );
        else
            tileLabels.push_back( arg );
    }

    if ( planOnly )
        return EmitPlan( elevations, azimuths );

    if ( output.empty() )
    {
        std::fprintf( stderr, "usage: DomeSheet --plan | --out <sheet.png> <tile.png> <label> ...\n" );
        return 2;
    }
    if ( tilePaths.size() != tileLabels.size() || tilePaths.empty() )
    {
        std::fprintf( stderr, "DomeSheet: %zu tiles and %zu labels; they come in pairs\n", tilePaths.size(),
                      tileLabels.size() );
        return 2;
    }
    if ( columns <= 0 )
        columns = azimuths;

    // Load and shrink first, so the geometry is built from the size the tiles ACTUALLY are rather than
    // from the size they were expected to be. A capture that came out at a different viewport size is a
    // thing that happens, and it must produce a wrong-looking sheet rather than a crash or a silent crop.
    std::vector<Image> tiles;
    tiles.reserve( tilePaths.size() );
    for ( const std::string& path : tilePaths )
    {
        Image loaded;
        if ( !LoadImage( path.c_str(), loaded ) )
        {
            std::fprintf( stderr, "DomeSheet: failed to load '%s'\n", path.c_str() );
            return 1;
        }
        tiles.push_back( BoxDownscale( loaded, scale ) );
    }

    int cellWidth  = 0;
    int cellHeight = 0;
    for ( const Image& tile : tiles )
    {
        cellWidth  = std::max( cellWidth, tile.Width );
        cellHeight = std::max( cellHeight, tile.Height );
    }

    const int           captionHeight = title.empty() ? 0 : ( kGlyphHeight * labelScale + 6 * labelScale );
    const SheetGeometry geometry =
         MakeSheetGeometry( static_cast<int>( tiles.size() ), columns, cellWidth, cellHeight, gap, captionHeight );

    Image sheet = MakeImage( geometry.SheetWidth(), geometry.SheetHeight(), 24 );

    if ( !title.empty() )
        DrawText( sheet, title, gap, 2 * labelScale, labelScale, 235, 235, 235 );

    for ( std::size_t i = 0; i < tiles.size(); ++i )
    {
        const int x = geometry.CellX( static_cast<int>( i ) );
        const int y = geometry.CellY( static_cast<int>( i ) );
        Blit( sheet, tiles[i], x, y );
        StampLabel( sheet, tileLabels[i], x, y, tiles[i].Width, labelScale );
    }

    if ( stbi_write_png( output.c_str(), sheet.Width, sheet.Height, 3, sheet.Pixels.data(), sheet.Width * 3 ) ==
         0 )
    {
        std::fprintf( stderr, "DomeSheet: failed to write '%s'\n", output.c_str() );
        return 1;
    }

    std::printf( "%s  %dx%d  %zu tiles, %d columns, 1/%d scale\n", output.c_str(), sheet.Width, sheet.Height,
                 tiles.size(), geometry.Columns, scale );
    return 0;
}

// The entry point, one line. Anything this tool throws is named on stderr with the tool's own name
// instead of reaching std::terminate, which would print the exception's TYPE and nothing else — see
// Tools/Shared/ToolMain.hpp.
int main( int argc, char** argv )
{
    return Desert::Tools::RunMain( "DomeSheet", argc, argv, &RunTool );
}
