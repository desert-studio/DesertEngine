// ImageStat — the instrument that decides whether a rendering change made the picture better.
//
// It reports five numbers over a rectangle of a PNG: the mean luminance, the 5th, 50th and 95th
// percentiles, the contrast between the outer two, and the mean saturation. That is enough to answer the
// questions a screenshot argument never settles — is the image brighter or merely different, did the
// highlights move or the shadows, is the sky still blue.
//
// WHY IT EXISTS. The cloud programme spent three rounds tuning against a screenshot and got nowhere. Every
// defect that mattered was found the moment something measured it: clouds that were DARKER than the sky
// behind them (p95 below the clear-sky p95), a dynamic range of 0.106 against the reference's 0.482, a
// saturation that had collapsed while the eye read the frame as "about right". The full account is in
// Docs/Clouds/CALIBRATION.md.
//
// Usage:  ImageStat <png> <x0> <y0> <x1> <y1> [<png> <x0> <y0> <x1> <y1> ...]
//
// Several images in one invocation because the numbers are only meaningful side by side: a contrast of
// 0.33 means nothing until the reference's 0.48 is on the line below it.

#define STB_IMAGE_IMPLEMENTATION
#include <ToolMain.hpp>

#include <stb_image/stb_image.h>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

// Reports the statistics that distinguish "a sky with clouds in it" from "a pale wash": how bright the
// cloud highlights are, how dark the gaps are, and how saturated the blue is. Comparing two renders by
// eye is what got this programme into trouble; comparing them by these five numbers does not.
struct Stats
{
    double meanY, p05Y, p50Y, p95Y, meanSat, contrast;
};

static Stats Measure( const unsigned char* px, int w, int /*h*/, int ch, int x0, int y0, int x1, int y1 )
{
    std::vector<double> lum;
    lum.reserve( static_cast<size_t>( x1 - x0 ) * static_cast<size_t>( y1 - y0 ) );
    double satSum = 0;
    int    n      = 0;
    for ( int y = y0; y < y1; ++y )
        for ( int x = x0; x < x1; ++x )
        {
            const unsigned char* p = px + (size_t)( y * w + x ) * ch;
            double               r = p[0] / 255.0, g = p[1] / 255.0, b = p[2] / 255.0;
            double               Y  = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            double               mx = std::max( { r, g, b } ), mn = std::min( { r, g, b } );
            satSum += ( mx <= 1e-6 ) ? 0.0 : ( mx - mn ) / mx;
            lum.push_back( Y );
            ++n;
        }
    std::sort( lum.begin(), lum.end() );
    auto   q    = [&]( double f ) { return lum[(size_t)std::min<double>( lum.size() - 1, f * lum.size() )]; };
    double mean = 0;
    for ( double v : lum )
        mean += v;
    mean /= lum.size();
    Stats s{ mean, q( 0.05 ), q( 0.50 ), q( 0.95 ), satSum / n, 0 };
    s.contrast = s.p95Y - s.p05Y;
    return s;
}

static int RunTool( int argc, char** argv )
{
    for ( int i = 1; i + 4 < argc; i += 5 )
    {
        const char* path = argv[i];
        int x0 = atoi( argv[i + 1] ), y0 = atoi( argv[i + 2] ), x1 = atoi( argv[i + 3] ), y1 = atoi( argv[i + 4] );
        int w, h, ch;
        unsigned char* px = stbi_load( path, &w, &h, &ch, 0 );
        if ( !px )
        {
            printf( "%-28s FAILED TO LOAD\n", path );
            continue;
        }
        x1               = std::min( x1, w );
        y1               = std::min( y1, h );
        Stats       s    = Measure( px, w, h, ch, x0, y0, x1, y1 );
        std::string name = path;
        auto        sl   = name.find_last_of( '/' );
        if ( sl != std::string::npos )
            name = name.substr( sl + 1 );
        printf( "%-26s  mean %.3f  p05 %.3f  p50 %.3f  p95 %.3f  contrast %.3f  sat %.3f\n", name.c_str(), s.meanY,
                s.p05Y, s.p50Y, s.p95Y, s.contrast, s.meanSat );
        stbi_image_free( px );
    }
    return 0;
}

// The entry point, one line. Anything this tool throws is named on stderr with the tool's own name
// instead of reaching std::terminate, which would print the exception's TYPE and nothing else — see
// Tools/Shared/ToolMain.hpp.
int main( int argc, char** argv )
{
    return Desert::Tools::RunMain( "ImageStat", argc, argv, &RunTool );
}
