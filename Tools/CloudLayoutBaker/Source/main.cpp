// CloudLayoutBaker — turns a picture into a `.dclayout`, the painted sky of Docs/Clouds §PT.
//
//   CloudLayoutBaker --out <path.dclayout> [--image <path.png>] [--figure <name>]
//                    [--mask-image <path.png>] [--mask-channel 0..3]
//                    [--size <n>] [--channels r,g,b,a] [--mask]
//
// WHY THE FIGURES ARE BUILT IN. The acceptance criterion of this phase is a frame in which the sky follows
// a shape somebody drew, and a criterion whose input is an image file in a repository is a criterion that
// cannot be re-run when the generator changes — the picture would have to be found, and nobody would know
// whether the one they found was the one that was measured. Two shapes are therefore GENERATED from a
// formula: `stripe`, whose boundary is a straight line a ground-level camera can see, and `letter-d`,
// which is unmistakable from above and cannot be confused with anything a hash produces.
//
//   * `stripe`   the western half of the world painted, the eastern half clear. Visible from the GROUND,
//                which is where the game is played and where a curved sky makes a shape hard to read.
//   * `letter-d` a stem and a bowl. Legible only from above the layer, which is what the top-down shot of
//                the protocol is for, and the strongest possible answer to "did the sky follow the
//                painting" — a hash does not produce a letter.
//   * `brush-d`  the same letter drawn by the BRUSH, one continuous stroke through
//                `Assets::PaintCloudLayoutPolyline` — the identical function the Cloud Layout panel's drag
//                drives. `--brush-radius` sets the stroke's width, which is what decides whether the glyph
//                survives to the sky at all: wider than the placement cell it reads, narrower it does not.
//
// `--image` and `--mask-image` are the path an artist takes, and it is the same path the editor takes: the
// PNG is read to RGBA8 and handed to exactly the functions the Cloud Layout panel calls,
// Assets::SetCloudLayoutCanvasPatternFromImage and ...MaskFromImage. The tool and the panel cannot disagree
// about what a picture means because there is one function per table and both call it — which
// CALIBRATION.md §PTP checks by baking one picture both ways and comparing the files byte for byte.
//
// TWO PICTURES AND NOT ONE, since O-4: the pattern says WHERE the clouds are and the mask says how much to
// ADD or REMOVE, and they are separate texture inputs of the cloud material exactly as
// `Layout_CloudGlobalPattern` and `Layout_GlobalCloudMask` are in Unreal's. One image cannot carry both —
// four species slots plus a mask is five planes — which is why the mask used to have to BE the pattern's
// alpha.
//
// GPU-FREE and ASSET-LAYER-FREE; the only filesystem it touches is the picture it decodes and the one
// write, which goes through Common's write primitive. See the premake file for why that is checked
// rather than hoped for.

#include <ToolMain.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <Engine/Assets/CloudLayout.hpp>

#include <stb_image/stb_image.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace
{
    int Usage()
    {
        std::fprintf( stderr,
                      "usage: CloudLayoutBaker --out <path.dclayout> [--image <path.png>] [--figure <name>]\n"
                      "                        [--mask-image <path.png>] [--mask-channel 0..3]\n"
                      "                        [--size <n>] [--channels r,g,b,a] [--mask]\n"
                      "                        [--brush-radius <texels>] [--brush-hardness <0..1>]\n"
                      "  --out       where to write the layout\n"
                      "  --image     the PATTERN picture: a square PNG/JPG/TGA, RGBA8 after decode\n"
                      "  --mask-image  the ADD/REMOVE MASK, as its own square picture - 128 neutral,\n"
                      "              brighter adds cloud, darker removes it. Same side as the pattern\n"
                      "  --mask-channel  which channel of --mask-image carries it (default 0; a grey PNG\n"
                      "              decodes with red, green and blue equal)\n"
                      "  --figure    generate instead of reading: 'stripe', 'letter-d' or 'brush-d'\n"
                      "  --size      side of a generated figure in texels, %u..%u (default 512)\n"
                      "  --brush-radius    'brush-d' only: half the stroke's width in texels (default 26)\n"
                      "  --brush-hardness  'brush-d' only: 0 feathered, 1 crisp (default 1)\n"
                      "  --channels  which SOURCE channel feeds each of the four species slots,\n"
                      "              e.g. 0,0,0,0 to put one greyscale painting on every slot (default 0,1,2,3)\n"
                      "  --mask      FIGURES ONLY: make the generated glyph lay a mask that follows it.\n"
                      "              A picture's mask comes in through --mask-image, which is its own\n"
                      "              texture exactly as it is in the material\n",
                      Desert::Assets::kCloudLayoutMinResolution, Desert::Assets::kCloudLayoutMaxResolution );
        return 2;
    }

    /// The western half painted on every channel, the eastern half clear, with a soft edge one texel wide
    /// so the boundary is a line rather than a staircase of the table's own resolution.
    ///
    /// THE MEAN IS EXACTLY A HALF BY CONSTRUCTION, which matters: the pattern is applied about its own
    /// mean, so a figure whose mean is a half moves the sky's cover by nothing at all while moving every
    /// individual cloud. That makes it the fixture that separates "the painting is applied" from "the
    /// painting made the sky thicker".
    std::vector<unsigned char> Stripe( uint32_t side, bool maskFollowsFigure )
    {
        std::vector<unsigned char> pixels( static_cast<size_t>( side ) * side * 4u, 0u );

        for ( uint32_t y = 0; y < side; ++y )
            for ( uint32_t x = 0; x < side; ++x )
            {
                const float u = ( static_cast<float>( x ) + 0.5f ) / static_cast<float>( side );

                // Two edges, not one: the table wraps, so a single step would put a second, unintended
                // boundary at u = 0 where the last column meets the first. Both are placed deliberately.
                const float edge = 1.0f / static_cast<float>( side );
                const float lit  = ( u > edge && u < 0.5f - edge )                 ? 1.0f
                                   : ( u <= edge || u >= 0.5f - edge ) && u < 0.5f ? 0.5f
                                                                                   : 0.0f;

                const unsigned char v  = static_cast<unsigned char>( std::lround( lit * 255.0f ) );
                const size_t        at = ( static_cast<size_t>( y ) * side + x ) * 4u;

                pixels[at + 0] = v;
                pixels[at + 1] = v;
                pixels[at + 2] = v;
                pixels[at + 3] = maskFollowsFigure ? v : 128u;
            }

        return pixels;
    }

    /// A capital D: a rectangular stem and half an annulus. Analytic rather than a font, because a font is
    /// a dependency and a licence for one glyph, and because a formula regenerates at any resolution.
    ///
    /// THE PROPORTIONS ARE CHOSEN TO SURVIVE THE SKY, not to look good in a texture viewer. The stroke is a
    /// TENTH of the world period wide — at the shipped 48 km region that is 4.8 km, which is more than one
    /// placement cell across (3.0 km), so the letter can be built out of whole clouds rather than out of
    /// fragments the lattice cannot express.
    std::vector<unsigned char> LetterD( uint32_t side, bool maskFollowsFigure )
    {
        std::vector<unsigned char> pixels( static_cast<size_t>( side ) * side * 4u, 0u );

        for ( uint32_t y = 0; y < side; ++y )
            for ( uint32_t x = 0; x < side; ++x )
            {
                const float u = ( static_cast<float>( x ) + 0.5f ) / static_cast<float>( side );
                const float v = ( static_cast<float>( y ) + 0.5f ) / static_cast<float>( side );

                const bool stem = u > 0.22f && u < 0.32f && v > 0.18f && v < 0.82f;

                const float dx   = u - 0.32f;
                const float dy   = ( v - 0.50f ) * 0.78f; // squashed, so the bowl is taller than it is wide
                const float r    = std::sqrt( dx * dx + dy * dy );
                const bool  bowl = u >= 0.32f && r > 0.20f && r < 0.30f;

                // The bar that closes the top and bottom of the bowl onto the stem.
                const bool bar =
                     u >= 0.22f && u < 0.42f && ( ( v > 0.18f && v < 0.28f ) || ( v > 0.72f && v < 0.82f ) );

                const unsigned char lit = ( stem || bowl || bar ) ? 255u : 0u;
                const size_t        at  = ( static_cast<size_t>( y ) * side + x ) * 4u;

                pixels[at + 0] = lit;
                pixels[at + 1] = lit;
                pixels[at + 2] = lit;
                pixels[at + 3] = maskFollowsFigure ? lit : 128u;
            }

        return pixels;
    }

    /// The same capital D, drawn by the BRUSH instead of by a formula — one continuous stroke up the stem
    /// and round the bowl, which is what the drag in the Cloud Layout panel lays down.
    ///
    /// WHY THIS EXISTS BESIDE `letter-d` RATHER THAN REPLACING IT. `letter-d` is the shipped fixture of
    /// §PT: `Layout_LetterD.dclayout` and the scenes that bind it are pinned to its bytes, and a figure
    /// that changed shape would silently re-authorise a published measurement. This one is the brush's
    /// own acceptance figure — the frame that proves a painted letter reaches the sky has to be painted by
    /// the thing being delivered, and `Assets::PaintCloudLayoutPolyline` here is the identical function
    /// the panel's mouse drives. GUI input cannot be synthesised on the build machine, so this is how a
    /// brush stroke is put in front of a renderer without a hand on the mouse.
    ///
    /// THE STROKE WIDTH IS THE POINT OF THE FLAG. At 512 texels over the shipped 48 km region one texel is
    /// 93.75 m, so a radius of 26 at full hardness is a 4.9 km stroke — wider than the 3.0 km placement
    /// cell, and the letter survives. A radius of 5 is 0.94 km, a third of a cell, and it does not. Both
    /// frames come out of the same command with one number changed.
    std::vector<unsigned char> BrushLetterD( uint32_t side, bool maskFollowsFigure, float radiusTexels,
                                             float hardness )
    {
        // A PLAIN RGBA SOURCE PICTURE, like the other two figures, rather than a canvas — the alpha plane
        // is the FIGURE'S own, and the mask is taken from it below by the same import the artist's `--mask`
        // uses. That keeps one statement of what an alpha means and, incidentally, keeps this figure's
        // bytes identical to the ones §PT measured.
        std::vector<unsigned char> pixels( static_cast<size_t>( side ) * side * 4u, 0u );

        // THE MASK'S GROUND IS 0 AND NOT NEUTRAL when the mask follows the figure, exactly as `letter-d`
        // has it: the mask is signed about 128, so a ground of 0 clears the sky everywhere the letter is
        // not and the glyph is what is left. A neutral ground would leave the background sky untouched and
        // the letter would have to fight the coverage slider to be seen.
        const auto ground = Desert::Assets::FillCloudLayoutPlaneChannel(
             pixels, side, 3u, 4u, maskFollowsFigure ? 0u : Desert::Assets::kCloudLayoutMaskNeutral );
        if ( !ground )
        {
            std::fprintf( stderr, "%s\n", ground.GetError().c_str() );
            return {};
        }

        const float            s = static_cast<float>( side );
        std::vector<glm::vec2> path;

        // Up the stem, then round the bowl and back to where the stem began: ONE drag, the way the letter
        // is actually drawn.
        path.emplace_back( 0.27f * s, 0.80f * s );
        path.emplace_back( 0.27f * s, 0.20f * s );

        constexpr int kArcSteps = 90; // two degrees a step: finer than a texel at every side we allow
        for ( int step = 0; step <= kArcSteps; ++step )
        {
            const float angle = -1.5707963f + 3.1415927f * ( static_cast<float>( step ) / kArcSteps );
            path.emplace_back( ( 0.27f + 0.26f * std::cos( angle ) ) * s,
                               ( 0.50f + 0.30f * std::sin( angle ) ) * s );
        }

        Desert::Assets::CloudLayoutBrush brush;
        brush.RadiusTexels = radiusTexels;
        brush.Hardness     = hardness;
        brush.Ink          = 1.0f;

        // Every channel the source has: the greyscale figure goes on R, G and B so any `--channels`
        // mapping finds it, and on alpha only when alpha is the mask.
        const uint32_t last = maskFollowsFigure ? 4u : 3u;
        for ( uint32_t channel = 0; channel < last; ++channel )
        {
            const auto painted =
                 Desert::Assets::PaintCloudLayoutPolyline( pixels, side, channel, 4u, path, brush );
            if ( !painted )
            {
                std::fprintf( stderr, "%s\n", painted.GetError().c_str() );
                return {};
            }
        }

        std::printf( "painted the brush letter: radius %.1f texels, hardness %.2f, stroke %.1f texels wide\n",
                     radiusTexels, hardness, Desert::Assets::CloudLayoutBrushWidthTexels( brush ) );

        return pixels;
    }

    bool ParseChannels( const char* text, uint32_t out[Desert::Assets::kCloudLayoutChannels] )
    {
        int a = 0, b = 0, c = 0, d = 0;
        if ( std::sscanf( text, "%d,%d,%d,%d", &a, &b, &c, &d ) != 4 )
            return false;

        const int values[] = { a, b, c, d };
        for ( uint32_t i = 0; i < Desert::Assets::kCloudLayoutChannels; ++i )
        {
            if ( values[i] < 0 || values[i] > 3 )
                return false;
            out[i] = static_cast<uint32_t>( values[i] );
        }
        return true;
    }
} // namespace

static int RunTool( int argc, char** argv )
{
    std::string out;
    std::string image;
    std::string maskImage;
    std::string figure;
    uint32_t    side        = 512u;
    uint32_t    maskChannel = 0u;
    bool        takeMask    = false;

    // The brush figure's defaults: 26 texels of radius at 512 over a 48 km region is a 4.9 km stroke, which
    // clears the 3.0 km placement cell. Crisp by default, because a letter is a letter.
    float brushRadius   = 26.0f;
    float brushHardness = 1.0f;

    // REMEMBERED SO THEY CAN BE REFUSED. A brush number handed to `--figure stripe` would do nothing, and a
    // flag that does nothing is how somebody comes to believe they baked a wider letter than they did.
    bool brushArgumentGiven = false;

    uint32_t channels[Desert::Assets::kCloudLayoutChannels] = { 0u, 1u, 2u, 3u };

    for ( int i = 1; i < argc; ++i )
    {
        const auto next = [&]( const char* what ) -> const char*
        {
            if ( i + 1 >= argc )
            {
                std::fprintf( stderr, "%s needs a value\n", what );
                return nullptr;
            }
            return argv[++i];
        };

        if ( std::strcmp( argv[i], "--out" ) == 0 )
        {
            const char* value = next( "--out" );
            if ( !value )
                return Usage();
            out = value;
        }
        else if ( std::strcmp( argv[i], "--image" ) == 0 )
        {
            const char* value = next( "--image" );
            if ( !value )
                return Usage();
            image = value;
        }
        else if ( std::strcmp( argv[i], "--mask-image" ) == 0 )
        {
            const char* value = next( "--mask-image" );
            if ( !value )
                return Usage();
            maskImage = value;
        }
        else if ( std::strcmp( argv[i], "--mask-channel" ) == 0 )
        {
            const char* value = next( "--mask-channel" );
            if ( !value )
                return Usage();
            const int parsed = std::atoi( value );
            if ( parsed < 0 || parsed > 3 )
            {
                std::fprintf( stderr, "--mask-channel %d must lie in [0, 3]\n", parsed );
                return Usage();
            }
            maskChannel = static_cast<uint32_t>( parsed );
        }
        else if ( std::strcmp( argv[i], "--figure" ) == 0 )
        {
            const char* value = next( "--figure" );
            if ( !value )
                return Usage();
            figure = value;
        }
        else if ( std::strcmp( argv[i], "--size" ) == 0 )
        {
            const char* value = next( "--size" );
            if ( !value )
                return Usage();
            side = static_cast<uint32_t>( std::atoi( value ) );
        }
        else if ( std::strcmp( argv[i], "--channels" ) == 0 )
        {
            const char* value = next( "--channels" );
            if ( !value || !ParseChannels( value, channels ) )
            {
                std::fprintf( stderr, "--channels wants four numbers in 0..3, e.g. 0,0,0,0\n" );
                return Usage();
            }
        }
        else if ( std::strcmp( argv[i], "--brush-radius" ) == 0 )
        {
            const char* value = next( "--brush-radius" );
            if ( !value )
                return Usage();
            brushRadius        = static_cast<float>( std::atof( value ) );
            brushArgumentGiven = true;
        }
        else if ( std::strcmp( argv[i], "--brush-hardness" ) == 0 )
        {
            const char* value = next( "--brush-hardness" );
            if ( !value )
                return Usage();
            brushHardness      = static_cast<float>( std::atof( value ) );
            brushArgumentGiven = true;
        }
        else if ( std::strcmp( argv[i], "--mask" ) == 0 )
        {
            takeMask = true;
        }
        else
        {
            std::fprintf( stderr, "unknown argument '%s'\n", argv[i] );
            return Usage();
        }
    }

    if ( out.empty() )
        return Usage();

    // EXACTLY ONE SOURCE, and saying so is cheaper than deciding which wins. A tool that quietly preferred
    // one over the other would write a file whose provenance is a guess about argument order.
    if ( image.empty() == figure.empty() )
    {
        std::fprintf( stderr, "give exactly one of --image and --figure\n" );
        return Usage();
    }

    // TWO SOURCES FOR THE MASK IS A SOURCE TOO MANY, and a tool that silently preferred one would write a
    // table whose provenance is a guess. `--mask` is the FIGURES' own switch — it makes the generated glyph
    // lay a mask that follows it — and `--mask-image` is the artist's separate picture.
    if ( takeMask && !maskImage.empty() )
    {
        std::fprintf( stderr, "--mask takes the source's own alpha and --mask-image reads a second "
                              "picture; give one\n" );
        return Usage();
    }

    if ( takeMask && !image.empty() )
    {
        std::fprintf( stderr, "--mask is for the generated figures; a picture's mask comes in through "
                              "--mask-image, which is its own texture\n" );
        return Usage();
    }

    if ( maskChannel != 0u && maskImage.empty() )
    {
        std::fprintf( stderr, "--mask-channel only means anything with --mask-image; refused rather than "
                              "ignored\n" );
        return Usage();
    }

    if ( brushArgumentGiven && figure != "brush-d" )
    {
        std::fprintf( stderr, "--brush-radius and --brush-hardness only mean anything to '--figure "
                              "brush-d'; refused rather than ignored\n" );
        return Usage();
    }

    std::vector<unsigned char> pixels;
    uint32_t                   width  = side;
    uint32_t                   height = side;

    if ( !figure.empty() )
    {
        if ( side < Desert::Assets::kCloudLayoutMinResolution || side > Desert::Assets::kCloudLayoutMaxResolution )
        {
            std::fprintf( stderr, "--size %u lies outside [%u, %u]\n", side,
                          Desert::Assets::kCloudLayoutMinResolution, Desert::Assets::kCloudLayoutMaxResolution );
            return 1;
        }

        if ( figure == "stripe" )
            pixels = Stripe( side, takeMask );
        else if ( figure == "letter-d" )
            pixels = LetterD( side, takeMask );
        else if ( figure == "brush-d" )
        {
            if ( !( brushRadius > 0.0f ) || brushRadius > static_cast<float>( side ) )
            {
                std::fprintf( stderr, "--brush-radius %.3f must be positive and no wider than the canvas\n",
                              brushRadius );
                return 1;
            }
            if ( !( brushHardness >= 0.0f ) || brushHardness > 1.0f )
            {
                std::fprintf( stderr, "--brush-hardness %.3f must lie in [0, 1]\n", brushHardness );
                return 1;
            }

            pixels = BrushLetterD( side, takeMask, brushRadius, brushHardness );
            if ( pixels.empty() )
                return 1;
        }
        else
        {
            std::fprintf( stderr, "unknown figure '%s'; known: stripe, letter-d, brush-d\n", figure.c_str() );
            return 1;
        }
    }
    else
    {
        int      w = 0, h = 0, comp = 0;
        stbi_uc* decoded = stbi_load( image.c_str(), &w, &h, &comp, 4 );
        if ( !decoded )
        {
            std::fprintf( stderr, "'%s' could not be read as an image: %s\n", image.c_str(),
                          stbi_failure_reason() ? stbi_failure_reason() : "unknown" );
            return 1;
        }

        width  = static_cast<uint32_t>( w );
        height = static_cast<uint32_t>( h );
        pixels.assign( decoded, decoded + static_cast<size_t>( w ) * h * 4 );
        stbi_image_free( decoded );

        std::printf( "read '%s': %dx%d, %d source channels\n", image.c_str(), w, h, comp );
    }

    // THE PATTERN AND THE MASK ARE TWO IMPORTS, which is decision O-4 and Unreal's two layout texture
    // parameters. The figures still lay their mask into the source's own alpha and it is read back out of
    // there by the same function `--mask-image` uses, so there is one statement of what a picture means and
    // the shipped fixtures come out byte for byte as §PT measured them.
    Desert::Assets::CloudLayoutCanvas canvas;

    if ( const auto brought =
              Desert::Assets::SetCloudLayoutCanvasPatternFromImage( canvas, pixels, width, height, channels );
         !brought )
    {
        std::fprintf( stderr, "%s\n", brought.GetError().c_str() );
        return 1;
    }

    if ( takeMask )
    {
        if ( const auto brought =
                  Desert::Assets::SetCloudLayoutCanvasMaskFromImage( canvas, pixels, width, height, 3u );
             !brought )
        {
            std::fprintf( stderr, "%s\n", brought.GetError().c_str() );
            return 1;
        }
    }
    else if ( !maskImage.empty() )
    {
        int      w = 0, h = 0, comp = 0;
        stbi_uc* decoded = stbi_load( maskImage.c_str(), &w, &h, &comp, 4 );
        if ( !decoded )
        {
            std::fprintf( stderr, "'%s' could not be read as an image: %s\n", maskImage.c_str(),
                          stbi_failure_reason() ? stbi_failure_reason() : "unknown" );
            return 1;
        }

        const std::vector<unsigned char> maskPixels( decoded, decoded + static_cast<size_t>( w ) * h * 4 );
        stbi_image_free( decoded );

        std::printf( "read the mask '%s': %dx%d, %d source channels\n", maskImage.c_str(), w, h, comp );

        if ( const auto brought = Desert::Assets::SetCloudLayoutCanvasMaskFromImage(
                  canvas, maskPixels, static_cast<uint32_t>( w ), static_cast<uint32_t>( h ), maskChannel );
             !brought )
        {
            std::fprintf( stderr, "%s\n", brought.GetError().c_str() );
            return 1;
        }
    }

    auto made = Desert::Assets::MakeCloudLayoutFromCanvas( canvas );
    if ( !made )
    {
        std::fprintf( stderr, "%s\n", made.GetError().c_str() );
        return 1;
    }

    Desert::Assets::CloudLayoutData layout = made.ExtractValue();

    // RE-BAKING KEEPS THE LAYOUT'S IDENTITY. An existing --out already has a GUID that scenes and materials
    // reference by handle, and the re-bake must stay byte-comparable with the committed file (CALIBRATION
    // §PTP); a fresh GUID would change both. The header alone is read. An existing file that is not a
    // readable layout envelope is refused by name rather than overwritten under a new identity.
    if ( std::filesystem::exists( out ) )
    {
        namespace CC                        = Common::Content;
        const CC::SubsystemVersion kKnown[] = {
             { Desert::Assets::kCloudLayoutSubsystemTag, Desert::Assets::kCloudLayoutContainerVersion } };
        std::ifstream in( out, std::ios::binary );
        const auto    header = CC::ReadEnvelopeHeader( in, CC::AssetHeaderReadContext{ kKnown } );
        if ( !header || header.GetValue().Asset.Kind != CC::ContentKind::CloudLayout )
        {
            std::fprintf( stderr,
                          "'%s' exists but is not a version-%u cloud layout envelope (%s); refusing to overwrite "
                          "it under a new GUID - migrate it with Tools/SceneMigrator or delete it first\n",
                          out.c_str(), Desert::Assets::kCloudLayoutContainerVersion,
                          header ? "wrong kind" : header.GetError().c_str() );
            return 1;
        }
        layout.Guid = header.GetValue().Asset.Guid;
    }

    auto encoded = Desert::Assets::EncodeCloudLayout( layout );
    if ( !encoded )
    {
        std::fprintf( stderr, "%s\n", encoded.GetError().c_str() );
        return 1;
    }

    const std::vector<unsigned char>& bytes = encoded.GetValue();

    // Through the write primitive, not a local std::ofstream (Д35). This tool used to exit 0 for a bake
    // whose bytes were still in the filebuf, and the shipped example layout is the artefact whose
    // provenance is this command — a silently short one would be re-derived from, not re-baked.
    if ( const auto written =
              Common::Utils::FileSystem::WriteBytesToFileAtomic( out, std::as_bytes( std::span( bytes ) ) );
         !written )
    {
        std::fprintf( stderr, "'%s' (%zu bytes) was not written: %s\n", out.c_str(), bytes.size(),
                      written.GetError().c_str() );
        return 1;
    }

    // The means are printed because they are the number that keeps the Coverage slider honest, and a
    // painting whose mean is near 0 or near 1 has very little room to redistribute anything — which is a
    // thing worth knowing when the sky does less than expected.
    std::printf( "wrote '%s': %ux%u, pattern %s, mask %s, %zu bytes, content %08x\n"
                 "  channel means: %.4f %.4f %.4f %.4f\n",
                 out.c_str(), layout.Resolution, layout.Resolution, layout.HasPattern() ? "yes" : "no",
                 layout.HasMask() ? "yes" : "no", bytes.size(), layout.ContentHash, layout.PatternMean[0],
                 layout.PatternMean[1], layout.PatternMean[2], layout.PatternMean[3] );

    return 0;
}

// The entry point, one line. Anything this tool throws is named on stderr with the tool's own name
// instead of reaching std::terminate, which would print the exception's TYPE and nothing else — see
// Tools/Shared/ToolMain.hpp.
int main( int argc, char** argv )
{
    return Desert::Tools::RunMain( "CloudLayoutBaker", argc, argv, &RunTool );
}
