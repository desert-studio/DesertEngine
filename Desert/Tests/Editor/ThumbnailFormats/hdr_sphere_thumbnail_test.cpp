// THE HDR BALL IS CHECKED AGAINST GEOMETRY WORKED OUT BY HAND, NOT AGAINST ITSELF.
//
// Every expectation below is derived from a map whose texels are a KNOWN function of their own
// longitude/latitude and from a view whose frame is known without the painter's help (yaw 0 / pitch 0:
// camera on +Z, +X right, +Y up — the convention HdrSphereThumbnail.hpp states). The painter's own
// `Sample` is used only as the lookup the expected colour goes through; the DIRECTION it is asked for is
// computed here. So the two defects this picture can have and still look like "a ball with a sky on it"
// — the reflected view ray instead of the normal, and u/v transposed — each land on a different texel
// than the one this file predicts.

#include <Editor/Widgets/CloudThumbnail.hpp>
#include <Editor/Widgets/HdrSphereThumbnail.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>

namespace
{
    namespace HS = Desert::Editor::HdrSphereThumbnail;

    constexpr float kPi = 3.14159265358979323846f;

    // A map that says where it is: red rises with u around the horizon, green with v down from the
    // zenith. Width twice the height, as every equirectangular capture is, so a transposed read indexes a
    // different aspect and cannot land on the same texel by symmetry. Kept below 1 so the tonemap stays in
    // its steep, distinguishing part.
    HS::EquirectMap SelfDescribingMap()
    {
        HS::EquirectMap map;
        map.Width  = 256;
        map.Height = 128;
        map.Rgb.resize( static_cast<size_t>( map.Width ) * map.Height * 3u );
        for ( uint32_t y = 0; y < map.Height; ++y )
            for ( uint32_t x = 0; x < map.Width; ++x )
            {
                const float  u  = ( static_cast<float>( x ) + 0.5f ) / static_cast<float>( map.Width );
                const float  v  = ( static_cast<float>( y ) + 0.5f ) / static_cast<float>( map.Height );
                const size_t at = ( static_cast<size_t>( y ) * map.Width + x ) * 3u;
                // A smooth bump in u (no seam jump across the probes) and a ramp in v.
                map.Rgb[at + 0] = 0.05f + 0.6f * ( 0.5f - 0.5f * std::cos( 2.0f * kPi * u ) );
                map.Rgb[at + 1] = 0.05f + 0.6f * v;
                map.Rgb[at + 2] = 0.2f;
            }
        return map;
    }

    struct Rgb8
    {
        int R, G, B;
    };

    Rgb8 PixelAt( const std::vector<unsigned char>& rgba, uint32_t side, uint32_t x, uint32_t y )
    {
        const size_t at = ( static_cast<size_t>( y ) * side + x ) * 4u;
        return { rgba[at], rgba[at + 1], rgba[at + 2] };
    }

    void ExpectWithinOne( const Rgb8& got, const std::array<unsigned char, 3>& want, const char* what )
    {
        EXPECT_LE( std::abs( got.R - want[0] ), 1 ) << what << ": red " << got.R << " vs " << int( want[0] );
        EXPECT_LE( std::abs( got.G - want[1] ), 1 ) << what << ": green " << got.G << " vs " << int( want[1] );
        EXPECT_LE( std::abs( got.B - want[2] ), 1 ) << what << ": blue " << got.B << " vs " << int( want[2] );
    }

    constexpr uint32_t kSide = 256;
} // namespace

TEST( HdrSphereThumbnail, TheMappingIsTheBakesOwnAndUAndVAreNotTransposed )
{
    // PanoramaSampleUV, stated by hand: u = atan2(z, x) / 2pi + 0.5, v = acos(y) / pi.
    const glm::vec2 up = HS::DirectionToUv( { 0.0f, 1.0f, 0.0f } );
    EXPECT_NEAR( up.y, 0.0f, 1e-5f ) << "straight up must read the TOP row (v = 0)";

    const glm::vec2 plusX = HS::DirectionToUv( { 1.0f, 0.0f, 0.0f } );
    EXPECT_NEAR( plusX.x, 0.5f, 1e-5f );
    EXPECT_NEAR( plusX.y, 0.5f, 1e-5f );

    const glm::vec2 plusZ = HS::DirectionToUv( { 0.0f, 0.0f, 1.0f } );
    EXPECT_NEAR( plusZ.x, 0.75f, 1e-5f ) << "+Z is a quarter turn from +X around the horizon";
    EXPECT_NEAR( plusZ.y, 0.5f, 1e-5f );

    // And the lookup reads the texel that UV names: +Z sits on the horizon at u = 0.75.
    const HS::EquirectMap map    = SelfDescribingMap();
    const glm::vec3       sample = HS::Sample( map, { 0.0f, 0.0f, 1.0f } );
    EXPECT_NEAR( sample.r, 0.05f + 0.6f * ( 0.5f - 0.5f * std::cos( 2.0f * kPi * 0.75f ) ), 2e-3f );
    EXPECT_NEAR( sample.g, 0.05f + 0.6f * 0.5f, 6e-3f );
}

TEST( HdrSphereThumbnail, TheDiscCentreShowsTheMapTowardTheCamera )
{
    const HS::EquirectMap            map    = SelfDescribingMap();
    const HS::SphereView             view   = HS::kThumbnailView;
    const std::vector<unsigned char> pixels = HS::Paint( map, kSide, view );
    ASSERT_EQ( pixels.size(), size_t( kSide ) * kSide * 4u );

    // The camera direction, derived here from the stated convention rather than read off the painter.
    const float     yaw      = view.YawDegrees * kPi / 180.0f;
    const float     pitch    = view.PitchDegrees * kPi / 180.0f;
    const glm::vec3 toCamera = { std::sin( yaw ) * std::cos( pitch ), std::sin( pitch ),
                                 std::cos( yaw ) * std::cos( pitch ) };
    ExpectWithinOne( PixelAt( pixels, kSide, kSide / 2, kSide / 2 ), HS::ToDisplay( HS::Sample( map, toCamera ) ),
                     "disc centre" );
}

TEST( HdrSphereThumbnail, OffCentreProbesReadTheSphereNormalNotTheReflection )
{
    // Front view: camera on +Z, so a pixel at disc coordinates (sx, sy) has the world normal
    // (sx, sy, sqrt(1 - sx^2 - sy^2)). The reflected view ray there would be (2 sx nz, 2 sy nz, 2 nz^2 - 1),
    // which for these probes is tens of degrees away and a clearly different colour in this map.
    const HS::EquirectMap            map    = SelfDescribingMap();
    const std::vector<unsigned char> pixels = HS::Paint( map, kSide, HS::SphereView{ 0.0f, 0.0f } );

    const float half   = 0.5f * kSide;
    const float radius = HS::kDiscRadiusFraction * half;
    const struct
    {
        uint32_t    X, Y;
        const char* Name;
    } probes[] = { { kSide / 2 + 60, kSide / 2, "right of centre" },
                   { kSide / 2 - 45, kSide / 2 - 50, "upper left" },
                   { kSide / 2 + 20, kSide / 2 + 70, "below" } };
    for ( const auto& probe : probes )
    {
        const float     sx = ( static_cast<float>( probe.X ) + 0.5f - half ) / radius;
        const float     sy = ( half - ( static_cast<float>( probe.Y ) + 0.5f ) ) / radius;
        const glm::vec3 normal( sx, sy, std::sqrt( 1.0f - sx * sx - sy * sy ) );
        ExpectWithinOne( PixelAt( pixels, kSide, probe.X, probe.Y ), HS::ToDisplay( HS::Sample( map, normal ) ),
                         probe.Name );
    }
}

TEST( HdrSphereThumbnail, TheCornerIsBackdropAndTheSilhouetteHasTheStatedRadius )
{
    const HS::EquirectMap            map    = SelfDescribingMap();
    const std::vector<unsigned char> pixels = HS::Paint( map, kSide, HS::kThumbnailView );
    const auto&                      back   = Desert::Editor::CloudThumbnail::kBackdrop;

    for ( const auto& corner : { std::pair<uint32_t, uint32_t>{ 0, 0 }, { kSide - 1, kSide - 1 } } )
    {
        const Rgb8 got = PixelAt( pixels, kSide, corner.first, corner.second );
        EXPECT_EQ( got.R, back[0] );
        EXPECT_EQ( got.G, back[1] );
        EXPECT_EQ( got.B, back[2] );
    }
    for ( size_t i = 3; i < pixels.size(); i += 4 )
        ASSERT_EQ( pixels[i], 255u ) << "a transparent pixel: the grid would composite it over its own colour";

    // Walk the middle row from the left edge to the first pixel that is not backdrop.
    uint32_t first = 0;
    while ( first < kSide / 2 )
    {
        const Rgb8 got = PixelAt( pixels, kSide, first, kSide / 2 );
        if ( got.R != back[0] || got.G != back[1] || got.B != back[2] )
            break;
        ++first;
    }
    const float measured = 0.5f * kSide - static_cast<float>( first );
    const float stated   = HS::kDiscRadiusFraction * 0.5f * kSide;
    EXPECT_NEAR( measured, stated, 1.0f ) << "the silhouette's radius is " << measured << " px, not " << stated;
}

TEST( HdrSphereThumbnail, ANonHdrPayloadIsRefusedByName )
{
    const std::vector<unsigned char> png     = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0 };
    const auto                       decoded = HS::Decode( png );
    if ( decoded )
    {
        ADD_FAILURE() << "a PNG signature was decoded as an environment map";
        return;
    }
    EXPECT_NE( decoded.GetError().find( "Radiance" ), std::string::npos ) << decoded.GetError();
}
