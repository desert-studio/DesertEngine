// ImageDiff — the instrument that says how two rendered frames differ, and whether the difference is a
// SMEAR or a SPARKLE.
//
// WHY IT EXISTS, AND WHY THE TWO INSTRUMENTS NEXT TO IT DO NOT COVER IT. ImageStat measures the
// luminance distribution of ONE image; LineJump measures steps between adjacent lines of ONE image.
// Neither compares two. Every "N differing pixels of 980 480, max delta M" in Docs/Clouds/CALIBRATION.md
// was produced by a throwaway script that was written, used and deleted, which is why the same sentence
// appears in eleven places with no way to reproduce any of them. This is that script, kept.
//
// AND ONE NUMBER THAT NO THROWAWAY SCRIPT PRODUCED. A temporal reconstruction fails in two opposite
// directions and a scalar difference cannot tell them apart:
//
//   * history kept when it should have been rejected — a GHOST, a displaced copy of the image, so the
//     error field is large and SMOOTH;
//   * history rejected when it should have been kept — SPECKLE, because the pixel falls back on one
//     quarter-resolution sample, so the error field is the same size and changes sign between neighbours.
//
// Adding a validation check trades the first for the second. Deciding whether that trade is worth making
// therefore needs the SHAPE of the error, not its size, and `coherence` below is that shape: the mean
// absolute luma error divided by the mean absolute first difference of the same field. Independent
// samples put it under one; a slowly-varying field pushes it up without bound.
//
// Usage:  ImageDiff <a.png> <b.png> <x0> <y0> <x1> <y1> [<a.png> <b.png> <x0> <y0> <x1> <y1> ...]
//
// Several pairs in one invocation for the same reason ImageStat takes several images: a coherence of 3.1
// means nothing until the repaired build's 1.4 is on the line below it.

#define STB_IMAGE_IMPLEMENTATION
#include <ToolMain.hpp>

#include <stb_image/stb_image.h>

#include "ImageDiffMath.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{
    std::string BaseName( const char* path )
    {
        std::string name  = path;
        const auto  slash = name.find_last_of( '/' );
        return slash == std::string::npos ? name : name.substr( slash + 1 );
    }
} // namespace

static int RunTool( int argc, char** argv )
{
    if ( argc < 7 )
    {
        std::printf( "usage: ImageDiff <a.png> <b.png> <x0> <y0> <x1> <y1> [...]\n" );
        return 2;
    }

    int failures = 0;
    for ( int i = 1; i + 5 < argc; i += 6 )
    {
        const char* pathA = argv[i];
        const char* pathB = argv[i + 1];
        const int   x0    = std::atoi( argv[i + 2] );
        const int   y0    = std::atoi( argv[i + 3] );
        int         x1    = std::atoi( argv[i + 4] );
        int         y1    = std::atoi( argv[i + 5] );

        int wa, ha, ca, wb, hb, cb;
        // Forced to four components so the two loads agree on stride whatever the PNGs carry. A frame
        // written with an alpha channel and one written without are the same picture, and refusing to
        // compare them would be an accident of the writer rather than a fact about the render.
        unsigned char* a = stbi_load( pathA, &wa, &ha, &ca, 4 );
        unsigned char* b = stbi_load( pathB, &wb, &hb, &cb, 4 );
        if ( !a || !b )
        {
            std::printf( "%-24s %-24s FAILED TO LOAD\n", BaseName( pathA ).c_str(), BaseName( pathB ).c_str() );
            stbi_image_free( a );
            stbi_image_free( b );
            ++failures;
            continue;
        }
        if ( wa != wb || ha != hb )
        {
            std::printf( "%-24s %-24s SIZE MISMATCH %dx%d vs %dx%d\n", BaseName( pathA ).c_str(),
                         BaseName( pathB ).c_str(), wa, ha, wb, hb );
            stbi_image_free( a );
            stbi_image_free( b );
            ++failures;
            continue;
        }

        // Clamping the FAR edge only, and saying so. A rectangle that runs off the right of the image is
        // the usual way of writing "to the edge" and costs nothing to honour; one that starts off the
        // left is a mistake, and Compare refuses it rather than moving it.
        if ( x1 > wa )
            x1 = wa;
        if ( y1 > ha )
            y1 = ha;

        Desert::Tools::ImageDiff::DiffResult d;
        if ( !Desert::Tools::ImageDiff::Compare( a, b, wa, ha, 4, x0, y0, x1, y1, d ) )
        {
            std::printf( "%-24s %-24s BAD RECTANGLE %d %d %d %d in %dx%d\n", BaseName( pathA ).c_str(),
                         BaseName( pathB ).c_str(), x0, y0, x1, y1, wa, ha );
            stbi_image_free( a );
            stbi_image_free( b );
            ++failures;
            continue;
        }

        std::printf( "%-22s vs %-22s  differing %8zu / %8zu (%6.3f%%)  max %3d @%d,%d  mean %.4f  rms %.4f  "
                     "bias %+.4f  coherence %.3f\n",
                     BaseName( pathA ).c_str(), BaseName( pathB ).c_str(), d.Differing, d.Pixels,
                     100.0 * static_cast<double>( d.Differing ) / static_cast<double>( d.Pixels ), d.MaxAbs,
                     d.MaxX, d.MaxY, d.MeanAbs, d.RmsAbs, d.LumaBias, d.Coherence );

        stbi_image_free( a );
        stbi_image_free( b );
    }

    return failures == 0 ? 0 : 1;
}

// The entry point, one line. Anything this tool throws is named on stderr with the tool's own name
// instead of reaching std::terminate, which would print the exception's TYPE and nothing else — see
// Tools/Shared/ToolMain.hpp.
int main( int argc, char** argv )
{
    return Desert::Tools::RunMain( "ImageDiff", argc, argv, &RunTool );
}
