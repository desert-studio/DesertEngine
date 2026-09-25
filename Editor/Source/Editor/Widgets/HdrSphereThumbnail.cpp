#include "HdrSphereThumbnail.hpp"

#include <Editor/Widgets/CloudThumbnail.hpp>

#include <Common/Core/GlslAsCpp.hpp>

// STB_IMAGE_IMPLEMENTATION is compiled once, in ThirdParty/stb/stb_image.cpp; this only declares.
#include <stb_image/stb_image.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace Desert::Editor::HdrSphereThumbnail
{
    namespace
    {
        // THE SHADER'S OWN TEXT, NOT A PARAPHRASE OF IT. `PanoramaSampleUV` is the one mapping the
        // PanoramaToCubemap and DiffuseIrradiance bakes read an equirectangular file with; compiling the
        // same file here means a change to the convention moves the ball and the skybox together, instead
        // of leaving a C++ copy that reads correct alone. Included by a path relative to this file rather
        // than through an include directory, because the Editor project does not put the shader tree on
        // its include path and one producer is not a reason to make every Editor TU see it. The anonymous
        // namespace and the glm spellings are the house arrangement for shader maths compiled as C++
        // (Desert/Tests/Engine/SkyPanorama/SkyPanoramaReference.hpp).
        using vec2 = glm::vec2;
        using vec3 = glm::vec3;
        using glm::acos;
        using glm::atan;
        using glm::clamp;
        DESERT_GLSL_AS_CPP_BEGIN
#include "../../../Resources/Shaders/Common/SkyPanorama.glslh"
        DESERT_GLSL_AS_CPP_END

        glm::vec3 Texel( const EquirectMap& map, uint32_t x, uint32_t y )
        {
            const size_t at = ( static_cast<size_t>( y ) * map.Width + x ) * 3u;
            return { map.Rgb[at + 0], map.Rgb[at + 1], map.Rgb[at + 2] };
        }

        // Rendition of SceneComposite.shader's TonemapACES — Stephen Hill's fit, the renderer's DEFAULT
        // operator (decision D-10). That shader keeps the function inline in a program rather than in a
        // shared .glslh, so it cannot be included here the way the panorama mapping is; the matrices are
        // transcribed column-major exactly as the GLSL `mat3(...)` constructors list them.
        glm::vec3 TonemapAces( const glm::vec3& color )
        {
            const glm::mat3 input( 0.59719f, 0.07600f, 0.02840f, 0.35458f, 0.90834f, 0.13383f, 0.04823f, 0.01566f,
                                   0.83777f );
            const glm::mat3 output( 1.60475f, -0.10208f, -0.00327f, -0.53108f, 1.10813f, -0.07276f, -0.07367f,
                                    -0.00605f, 1.07602f );
            const glm::vec3 v = input * color;
            const glm::vec3 a = v * ( v + 0.0245786f ) - 0.000090537f;
            const glm::vec3 b = v * ( 0.983729f * v + 0.4329510f ) + 0.238081f;
            return glm::clamp( output * ( a / b ), 0.0f, 1.0f );
        }

        float EncodeSrgb( float linear )
        {
            const float c = std::clamp( linear, 0.0f, 1.0f );
            return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow( c, 1.0f / 2.4f ) - 0.055f;
        }
    } // namespace

    glm::vec3 CameraDirection( const SphereView& view )
    {
        const float yaw   = glm::radians( view.YawDegrees );
        const float pitch = glm::radians( view.PitchDegrees );
        return { std::sin( yaw ) * std::cos( pitch ), std::sin( pitch ), std::cos( yaw ) * std::cos( pitch ) };
    }

    glm::vec2 DirectionToUv( const glm::vec3& direction )
    {
        return PanoramaSampleUV( direction );
    }

    glm::vec3 Sample( const EquirectMap& map, const glm::vec3& direction )
    {
        const glm::vec2 uv = DirectionToUv( direction );

        // Texel centres sit at half-integers, as the GPU's linear filter has them; u wraps (the seam is a
        // meridian like any other), v clamps (there is nothing past a pole).
        const float x  = uv.x * static_cast<float>( map.Width ) - 0.5f;
        const float y  = std::clamp( uv.y * static_cast<float>( map.Height ) - 0.5f, 0.0f,
                                     static_cast<float>( map.Height - 1u ) );
        const float fx = std::floor( x );
        const float fy = std::floor( y );
        const float tx = x - fx;
        const float ty = y - fy;

        const auto     w  = static_cast<int64_t>( map.Width );
        const auto     x0 = static_cast<uint32_t>( ( ( static_cast<int64_t>( fx ) % w ) + w ) % w );
        const uint32_t x1 = ( x0 + 1u ) % map.Width;
        const auto     y0 = static_cast<uint32_t>( fy );
        const uint32_t y1 = std::min( y0 + 1u, map.Height - 1u );

        const glm::vec3 top    = glm::mix( Texel( map, x0, y0 ), Texel( map, x1, y0 ), tx );
        const glm::vec3 bottom = glm::mix( Texel( map, x0, y1 ), Texel( map, x1, y1 ), tx );
        return glm::mix( top, bottom, ty );
    }

    std::array<unsigned char, 3> ToDisplay( const glm::vec3& linear )
    {
        constexpr float kExposure = 1.0f; // SceneComposite's manual default; a tile has no scene to adapt to
        const glm::vec3 mapped    = TonemapAces( linear * kExposure );
        std::array<unsigned char, 3> out{};
        for ( int c = 0; c < 3; ++c )
            out[static_cast<size_t>( c )] =
                 static_cast<unsigned char>( std::lround( 255.0f * EncodeSrgb( mapped[c] ) ) );
        return out;
    }

    std::vector<unsigned char> Paint( const EquirectMap& map, uint32_t side, const SphereView& view )
    {
        std::vector<unsigned char> out( static_cast<size_t>( side ) * side * 4u );

        // The camera's frame: forward is toward the centre, right and up complete it. Pitch is kept below
        // the poles by every caller (PreviewViewpoints clamps at 89), so `forward x worldUp` never vanishes.
        const glm::vec3 toCamera = CameraDirection( view );
        const glm::vec3 right    = glm::normalize( glm::cross( -toCamera, glm::vec3( 0.0f, 1.0f, 0.0f ) ) );
        const glm::vec3 up       = glm::cross( right, -toCamera );

        const float half   = 0.5f * static_cast<float>( side );
        const float radius = kDiscRadiusFraction * half;

        for ( uint32_t py = 0; py < side; ++py )
        {
            for ( uint32_t px = 0; px < side; ++px )
            {
                const float sx       = ( static_cast<float>( px ) + 0.5f - half ) / radius;
                const float sy       = ( half - ( static_cast<float>( py ) + 0.5f ) ) / radius;
                const float r2       = sx * sx + sy * sy;
                const float distance = std::sqrt( r2 ) * radius; // in pixels

                // One pixel of antialiasing across the silhouette: coverage is the fraction of the pixel
                // inside the circle, approximated along the radius. Without it the ball's outline stairs
                // at 64 px, which reads as a low-quality image rather than as a sphere.
                const float coverage = std::clamp( radius - distance + 0.5f, 0.0f, 1.0f );

                const std::array<unsigned char, 3>& backdrop = CloudThumbnail::kBackdrop;
                glm::vec3                           colour( backdrop[0], backdrop[1], backdrop[2] );
                if ( coverage > 0.0f )
                {
                    // Rim pixels whose centre lies just outside the circle take the silhouette's normal.
                    const float     scale  = r2 > 1.0f ? 1.0f / std::sqrt( r2 ) : 1.0f;
                    const float     nx     = sx * scale;
                    const float     ny     = sy * scale;
                    const float     nz     = std::sqrt( std::max( 0.0f, 1.0f - nx * nx - ny * ny ) );
                    const glm::vec3 normal = nx * right + ny * up + nz * toCamera;

                    const std::array<unsigned char, 3> ball = ToDisplay( Sample( map, normal ) );
                    const glm::vec3                    lit( ball[0], ball[1], ball[2] );
                    colour = glm::mix( colour, lit, coverage );
                }

                const size_t at = ( static_cast<size_t>( py ) * side + px ) * 4u;
                for ( int c = 0; c < 3; ++c )
                    out[at + static_cast<size_t>( c )] = static_cast<unsigned char>( std::lround( colour[c] ) );
                out[at + 3] = 255;
            }
        }
        return out;
    }

    Common::ResultStr<EquirectMap> Decode( const std::vector<unsigned char>& payload )
    {
        const auto size = static_cast<int>( payload.size() );
        if ( stbi_is_hdr_from_memory( payload.data(), size ) == 0 )
            return Common::MakeFormattedError<EquirectMap>(
                 "the {} bytes are not a Radiance HDR image (no '#?RADIANCE' / '#?RGBE' signature), so there "
                 "is no environment to put on the ball",
                 payload.size() );

        int                                               width = 0, height = 0, channels = 0;
        const std::unique_ptr<float, void ( * )( void* )> pixels(
             stbi_loadf_from_memory( payload.data(), size, &width, &height, &channels, 3 ), stbi_image_free );
        if ( !pixels )
            return Common::MakeFormattedError<EquirectMap>(
                 "stb_image could not decode the HDR payload ({} bytes): {}", payload.size(),
                 stbi_failure_reason() );
        if ( width < 2 || height < 2 )
            return Common::MakeFormattedError<EquirectMap>(
                 "the HDR image is {}x{}; an equirectangular map needs at least 2x2 texels to be filtered", width,
                 height );

        EquirectMap map;
        map.Width  = static_cast<uint32_t>( width );
        map.Height = static_cast<uint32_t>( height );
        map.Rgb.assign( pixels.get(), pixels.get() + static_cast<size_t>( width ) * height * 3u );
        return Common::MakeSuccess( std::move( map ) );
    }
} // namespace Desert::Editor::HdrSphereThumbnail
