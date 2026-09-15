// LineJump — the instrument that turns "there is some banding in there" into a number.
//
// It reports, over a rectangle of a PNG, the LARGEST step in mean luminance between two ADJACENT lines
// — separately along both axes, rows and columns — together with the index of the line the step happens
// at, and the MEAN step, which is the noise floor the maximum has to be read against.
//
// WHY IT EXISTS, AND WHY ImageStat DOES NOT COVER IT. ImageStat answers "is this frame brighter, is it
// more contrasty, is it still blue" from percentiles taken over the WHOLE rectangle. A band is local to
// a line: a hard horizontal edge a few thousandths of a grey level tall shifts no percentile at all,
// because the pixels either side of it are ordinary sky pixels that were already in the distribution.
// The two defects this was built for both measured "clean" on every ImageStat figure. So this is not a
// convenience wrapper over ImageStat — it is the axis ImageStat structurally cannot see.
//
// BOTH AXES, ALWAYS. The lens-flare defect existed on rows AND on columns, and the column half went
// unnoticed for a stage and a half for exactly one reason: nobody measured it. Reporting only the axis
// the eye happened to notice is how that happens again, so both are always printed.
//
// WHERE, NOT JUST HOW MUCH. Task Ц9 diagnosed by comparing the row NUMBER a band sat at against where a
// mechanism predicted it should sit — the aerial-perspective slice boundary, the march-step plane, the
// Sky-View LUT row. A maximum without its index is a fact you cannot argue with a hypothesis about.
//
// AND THE NOISE FLOOR. A maximum of 0.02 means nothing on its own: over a mean step of 0.019 it is
// texture, over a mean step of 0.002 it is an edge. The mean is printed next to the maximum so the
// comparison does not require a second run.
//
// Usage:  LineJump <png> <x0> <y0> <x1> <y1> [<png> <x0> <y0> <x1> <y1> ...]
//
// Several images in one invocation, for the same reason ImageStat takes several: a jump of 0.054 is
// only a defect once 0.025 from the repaired build is on the line below it.

#define STB_IMAGE_IMPLEMENTATION
#include <ToolMain.hpp>

#include <stb_image/stb_image.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
    // The step statistics along one axis. `MaxIndex` is the index of the line BEFORE the step, so the
    // band edge lies between MaxIndex and MaxIndex + 1.
    struct AxisJump
    {
        double MaxJump  = 0.0;
        int    MaxIndex = 0;
        double MeanJump = 0.0;
    };

    // `means` holds the mean luminance of consecutive lines, the first of which is line `first`.
    // Requires at least two lines; the caller refuses the rectangle otherwise rather than reporting a
    // zero that would read as "no banding".
    AxisJump MeasureAxis( const std::vector<double>& means, int first )
    {
        AxisJump result;
        double   sum = 0.0;
        for ( size_t i = 1; i < means.size(); ++i )
        {
            const double step = std::fabs( means[i] - means[i - 1] );
            sum += step;
            if ( step > result.MaxJump )
            {
                result.MaxJump  = step;
                result.MaxIndex = first + static_cast<int>( i ) - 1;
            }
        }
        result.MeanJump = sum / static_cast<double>( means.size() - 1 );
        return result;
    }

    std::string BaseName( const char* path )
    {
        std::string name = path;
        const auto  sl   = name.find_last_of( '/' );
        return sl == std::string::npos ? name : name.substr( sl + 1 );
    }
} // namespace

static int RunTool( int argc, char** argv )
{
    if ( argc < 6 )
    {
        printf( "usage: LineJump <png> <x0> <y0> <x1> <y1> [<png> <x0> <y0> <x1> <y1> ...]\n" );
        return 1;
    }

    int failures = 0;
    for ( int i = 1; i + 4 < argc; i += 5 )
    {
        const char* path = argv[i];
        int x0 = atoi( argv[i + 1] ), y0 = atoi( argv[i + 2] ), x1 = atoi( argv[i + 3] ), y1 = atoi( argv[i + 4] );

        // Three channels asked for explicitly: a greyscale or palette PNG would otherwise hand back one
        // byte per pixel and the luminance below would read past the end of every row.
        int            w = 0, h = 0, sourceChannels = 0;
        unsigned char* px = stbi_load( path, &w, &h, &sourceChannels, 3 );
        if ( !px )
        {
            printf( "%-28s FAILED TO LOAD: %s\n", BaseName( path ).c_str(), stbi_failure_reason() );
            ++failures;
            continue;
        }

        x0 = std::max( 0, x0 );
        y0 = std::max( 0, y0 );
        x1 = std::min( x1, w );
        y1 = std::min( y1, h );

        // Said out loud with the numbers rather than reported as a jump of zero. "No banding" and "the
        // rectangle was too small to hold a pair of adjacent lines" must never print the same thing.
        if ( x1 - x0 < 2 || y1 - y0 < 2 )
        {
            printf( "%-28s REFUSED: rectangle %d,%d..%d,%d is %dx%d inside a %dx%d image; a step between "
                    "adjacent lines needs at least 2 rows and 2 columns\n",
                    BaseName( path ).c_str(), x0, y0, x1, y1, x1 - x0, y1 - y0, w, h );
            stbi_image_free( px );
            ++failures;
            continue;
        }

        // Rec.709 luminance over the sRGB-encoded bytes — deliberately the same expression ImageStat
        // uses, so a row of this tool and a row of that one describe the same quantity. Both axes are
        // accumulated in one pass over the rectangle.
        const int           rows = y1 - y0, cols = x1 - x0;
        std::vector<double> rowSum( static_cast<size_t>( rows ), 0.0 );
        std::vector<double> colSum( static_cast<size_t>( cols ), 0.0 );
        for ( int y = y0; y < y1; ++y )
            for ( int x = x0; x < x1; ++x )
            {
                const unsigned char* p = px + static_cast<size_t>( y * w + x ) * 3;
                const double         Y = ( 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2] ) / 255.0;
                rowSum[static_cast<size_t>( y - y0 )] += Y;
                colSum[static_cast<size_t>( x - x0 )] += Y;
            }
        for ( double& v : rowSum )
            v /= static_cast<double>( cols );
        for ( double& v : colSum )
            v /= static_cast<double>( rows );

        const AxisJump byRow = MeasureAxis( rowSum, y0 );
        const AxisJump byCol = MeasureAxis( colSum, x0 );

        printf( "%-28s  rows max %.5f @y %-5d mean %.5f   cols max %.5f @x %-5d mean %.5f\n",
                BaseName( path ).c_str(), byRow.MaxJump, byRow.MaxIndex, byRow.MeanJump, byCol.MaxJump,
                byCol.MaxIndex, byCol.MeanJump );
        stbi_image_free( px );
    }

    return failures ? 1 : 0;
}

// The entry point, one line. Anything this tool throws is named on stderr with the tool's own name
// instead of reaching std::terminate, which would print the exception's TYPE and nothing else — see
// Tools/Shared/ToolMain.hpp.
int main( int argc, char** argv )
{
    return Desert::Tools::RunMain( "LineJump", argc, argv, &RunTool );
}
