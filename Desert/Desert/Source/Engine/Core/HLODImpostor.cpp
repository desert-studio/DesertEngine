#include <Engine/Core/HLODImpostor.hpp>

#include <Engine/Geometry/MeshCore/Spatial/MeshAABBTree3.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <optional>
#include <string>

namespace Desert::Core
{
    namespace
    {
        // Empty texels around a silhouette that take a covered neighbour's colour: enough for the bilinear and the
        // mip-1 footprint of a far quad, few enough that a frame never fills from its own far side.
        constexpr int   kDilationPasses = 4;
        constexpr float kViewTolerance  = 1.0e-3f;
        constexpr float kEpsilon        = 1.0e-6f;

        // sign() with +1 at zero: the octahedral fold must send an axis-aligned direction to one edge, not to the
        // centre (glm::sign(0) = 0 would collapse the fold).
        [[nodiscard]] float SignNotZero( float value )
        {
            return value >= 0.0f ? 1.0f : -1.0f;
        }

        [[nodiscard]] uint8_t UnitToByte( float value )
        {
            return static_cast<uint8_t>( std::clamp( std::lround( value * 255.0f ), 0L, 255L ) );
        }

        [[nodiscard]] bool PowerOfTwo( int32_t value )
        {
            return value > 0 && ( value & ( value - 1 ) ) == 0;
        }
    } // namespace

    Common::BoolResultStr ValidateHLODImpostor( const HLODImpostorSettings& settings )
    {
        if ( settings.FramesPerSide < 8 || settings.FramesPerSide > 16 )
            return Common::MakeError<bool>(
                 std::format( "FramesPerSide {} is outside 8..16", settings.FramesPerSide ) );
        if ( settings.FrameResolution < 32 || settings.FrameResolution > 1024 ||
             !PowerOfTwo( settings.FrameResolution ) )
            return Common::MakeError<bool>(
                 std::format( "FrameResolution {} is not a power of two in 32..1024", settings.FrameResolution ) );
        const int64_t side = static_cast<int64_t>( settings.FramesPerSide ) * settings.FrameResolution;
        if ( side > 8192 )
            return Common::MakeError<bool>( std::format( "the atlas side {} x {} = {} texels is above 8192",
                                                         settings.FramesPerSide, settings.FrameResolution,
                                                         side ) );
        return BOOLSUCCESS;
    }

    uint32_t HLODImpostorAtlasSide( const HLODImpostorSettings& settings )
    {
        return static_cast<uint32_t>( settings.FramesPerSide ) * static_cast<uint32_t>( settings.FrameResolution );
    }

    glm::vec2 ImpostorOctahedralEncode( const glm::vec3& direction, bool hemisphere )
    {
        if ( hemisphere )
        {
            // UE HemiOctahedronToUnitVector's inverse, +Y up: the upper half projected onto the |x| + |z| <= 1
            // diamond, then rotated 45 degrees so the diamond fills the square.
            const glm::vec3 up( direction.x, std::max( direction.y, 0.0f ), direction.z );
            const float     sum = std::abs( up.x ) + up.y + std::abs( up.z );
            if ( sum < kEpsilon )
                return glm::vec2( 1.0f, 1.0f ); // straight down: the +X horizon frame (any horizon frame sees as little)
            const glm::vec2 p( up.x / sum, up.z / sum );
            return glm::vec2( p.x + p.y, p.x - p.y ) * 0.5f + 0.5f;
        }
        const float sum = std::abs( direction.x ) + std::abs( direction.y ) + std::abs( direction.z );
        glm::vec2   p( direction.x / sum, direction.z / sum );
        if ( direction.y < 0.0f )
            p = glm::vec2( ( 1.0f - std::abs( p.y ) ) * SignNotZero( p.x ),
                           ( 1.0f - std::abs( p.x ) ) * SignNotZero( p.y ) );
        return p * 0.5f + 0.5f;
    }

    glm::vec3 ImpostorOctahedralDecode( const glm::vec2& uv, bool hemisphere )
    {
        const glm::vec2 q = uv * 2.0f - 1.0f;
        if ( hemisphere )
        {
            const glm::vec2 t = glm::vec2( q.x + q.y, q.x - q.y ) * 0.5f;
            return glm::normalize( glm::vec3( t.x, 1.0f - std::abs( t.x ) - std::abs( t.y ), t.y ) );
        }
        glm::vec3 n( q.x, 1.0f - std::abs( q.x ) - std::abs( q.y ), q.y );
        if ( n.y < 0.0f )
        {
            const float x = n.x;
            n.x           = ( 1.0f - std::abs( n.z ) ) * SignNotZero( x );
            n.z           = ( 1.0f - std::abs( x ) ) * SignNotZero( n.z );
        }
        return glm::normalize( n );
    }

    ImpostorFrameBlend ImpostorBlendFrames( const glm::vec3& direction, int32_t framesPerSide, bool hemisphere )
    {
        const float     last = static_cast<float>( framesPerSide - 1 );
        const glm::vec2 grid = ImpostorOctahedralEncode( direction, hemisphere ) * last;
        // The cell the point lies in; the far edge (uv = 1) belongs to the last cell, not to a cell past the grid.
        const glm::vec2  cell = glm::clamp( glm::floor( grid ), glm::vec2( 0.0f ), glm::vec2( last - 1.0f ) );
        const glm::vec2  f    = grid - cell;
        const glm::ivec2 base( static_cast<int32_t>( cell.x ), static_cast<int32_t>( cell.y ) );

        ImpostorFrameBlend blend;
        if ( f.x + f.y <= 1.0f )
        {
            blend.Frames  = { base, base + glm::ivec2( 1, 0 ), base + glm::ivec2( 0, 1 ) };
            blend.Weights = glm::vec3( 1.0f - f.x - f.y, f.x, f.y );
        }
        else
        {
            blend.Frames  = { base + glm::ivec2( 1, 1 ), base + glm::ivec2( 1, 0 ), base + glm::ivec2( 0, 1 ) };
            blend.Weights = glm::vec3( f.x + f.y - 1.0f, 1.0f - f.y, 1.0f - f.x );
        }
        // Clamp the float residue of a point exactly on an edge (a weight of -1e-8 would subtract a frame).
        blend.Weights = glm::max( blend.Weights, glm::vec3( 0.0f ) );
        blend.Weights /= blend.Weights.x + blend.Weights.y + blend.Weights.z;
        return blend;
    }

    ImpostorSphere HLODImpostorSphere( const glm::vec3& boundsMin, const glm::vec3& boundsMax )
    {
        ImpostorSphere sphere;
        sphere.Centre = ( boundsMin + boundsMax ) * 0.5f;
        sphere.Radius = glm::length( boundsMax - boundsMin ) * 0.5f + 1.0f;
        return sphere;
    }

    std::vector<HLODCaptureView> HLODImpostorViews( const glm::vec3& boundsMin, const glm::vec3& boundsMax,
                                                    const HLODImpostorSettings& settings )
    {
        const ImpostorSphere         sphere = HLODImpostorSphere( boundsMin, boundsMax );
        const float                  last   = static_cast<float>( settings.FramesPerSide - 1 );
        std::vector<HLODCaptureView> views;
        const auto side = static_cast<std::size_t>( settings.FramesPerSide );
        views.reserve( side * side );
        for ( int32_t row = 0; row < settings.FramesPerSide; ++row )
            for ( int32_t col = 0; col < settings.FramesPerSide; ++col )
            {
                const glm::vec2 uv( static_cast<float>( col ) / last, static_cast<float>( row ) / last );
                const glm::vec3 toCamera = ImpostorOctahedralDecode( uv, settings.Hemisphere );
                views.push_back( HLODCaptureViewFacing( -toCamera, sphere.Centre, sphere.Radius ) );
            }
        return views;
    }

    Common::ResultStr<HLODImpostorAtlas> BakeImpostorAtlas( const Geometry::DynamicMesh3& source,
                                                            const glm::vec3& boundsMin, const glm::vec3& boundsMax,
                                                            std::span<const HLODCapturePhoto> photos,
                                                            const HLODImpostorSettings&       settings )
    {
        using Result = HLODImpostorAtlas;
        if ( auto valid = ValidateHLODImpostor( settings ); !valid )
            return Common::MakeError<Result>( valid.GetError() );
        const auto        framesPerSide = static_cast<std::size_t>( settings.FramesPerSide );
        const std::size_t frames        = framesPerSide * framesPerSide;
        if ( photos.size() != frames )
            return Common::MakeError<Result>( std::format( "{} capture photos for a {}x{} frame grid ({} frames)",
                                                           photos.size(), settings.FramesPerSide,
                                                           settings.FramesPerSide, frames ) );
        if ( source.TriangleCount() == 0 )
            return Common::MakeError<Result>( "the impostor's source mesh holds no triangles" );

        const auto                         resolution = static_cast<uint32_t>( settings.FrameResolution );
        const std::vector<HLODCaptureView> views      = HLODImpostorViews( boundsMin, boundsMax, settings );
        for ( std::size_t i = 0; i < frames; ++i )
        {
            const HLODCapturePhoto& photo  = photos[i];
            const std::size_t       pixels = static_cast<std::size_t>( photo.Width ) * photo.Height;
            if ( photo.Width != resolution || photo.Height != resolution )
                return Common::MakeError<Result>(
                     std::format( "frame {} is a {}x{} photo, the grid's frames are {}x{}", i, photo.Width,
                                  photo.Height, resolution, resolution ) );
            if ( photo.BaseColorMetallic.size() != pixels * 4 || photo.NormalRoughness.size() != pixels ||
                 photo.Occlusion.size() != pixels )
                return Common::MakeError<Result>( std::format(
                     "frame {}'s {}x{} photo holds {} / {} / {} values", i, photo.Width, photo.Height,
                     photo.BaseColorMetallic.size(), photo.NormalRoughness.size(), photo.Occlusion.size() ) );
            const float off = glm::length( photo.View.Forward - views[i].Forward );
            if ( off > kViewTolerance )
                return Common::MakeError<Result>( std::format(
                     "frame {}'s photo looks along ({:f}, {:f}, {:f}), the frame along ({:f}, {:f}, {:f})", i,
                     photo.View.Forward.x, photo.View.Forward.y, photo.View.Forward.z, views[i].Forward.x,
                     views[i].Forward.y, views[i].Forward.z ) );
        }

        HLODImpostorAtlas out;
        out.Settings             = settings;
        out.Sphere               = HLODImpostorSphere( boundsMin, boundsMax );
        out.Side                 = HLODImpostorAtlasSide( settings );
        const std::size_t texels = static_cast<std::size_t>( out.Side ) * out.Side;
        out.BaseColor.assign( texels * 4, 0 );
        out.NormalDepth.assign( texels * 4, 0 );
        out.ORM.assign( texels * 4, 0 );
        std::vector<uint8_t> filled( texels, 0 );

        const Geometry::DynamicMeshAABBTree3 sourceTree( &source );
        const float                          eyeDistance = out.Sphere.Radius + 1.0f;
        const float                          step        = 2.0f / static_cast<float>( resolution );

        for ( std::size_t i = 0; i < frames; ++i )
        {
            const HLODCapturePhoto& photo = photos[i];
            const HLODCaptureView&  view  = views[i];
            const uint32_t          col   = static_cast<uint32_t>( i % framesPerSide );
            const uint32_t          row   = static_cast<uint32_t>( i / framesPerSide );
            for ( uint32_t y = 0; y < resolution; ++y )
                for ( uint32_t x = 0; x < resolution; ++x )
                {
                    const std::size_t pixel = static_cast<std::size_t>( y ) * resolution + x;
                    const glm::vec4&  nr    = photo.NormalRoughness[pixel];
                    const glm::vec3   normal( nr.x, nr.y, nr.z );
                    if ( glm::dot( normal, normal ) < kEpsilon )
                        continue; // the clear colour: nothing drew here
                    const std::size_t texel = ( static_cast<std::size_t>( row ) * resolution + y ) * out.Side +
                                              static_cast<std::size_t>( col ) * resolution + x;

                    // The photo's own orthographic ray through this pixel's centre (HLODCaptureView's projection).
                    const float     px = ( static_cast<float>( x ) + 0.5f ) * step - 1.0f;
                    const float     py = 1.0f - ( static_cast<float>( y ) + 0.5f ) * step;
                    const glm::vec3 origin =
                         view.Eye + view.Right * ( px * view.HalfExtent ) + view.Up * ( py * view.HalfExtent );
                    double hitT  = 0.0;
                    float  depth = 0.5f;
                    if ( sourceTree.FindNearestHitTriangle( glm::dvec3( origin ), glm::dvec3( view.Forward ),
                                                            hitT ) != Geometry::DynamicMesh3::InvalidID )
                        depth = ( static_cast<float>( hitT ) - eyeDistance ) / out.Sphere.Radius * 0.5f + 0.5f;
                    else
                        ++out.DepthMisses;

                    const glm::vec3 unit = glm::normalize( normal );
                    std::memcpy( &out.BaseColor[texel * 4], &photo.BaseColorMetallic[pixel * 4], 3 );
                    out.BaseColor[texel * 4 + 3]   = 255;
                    out.NormalDepth[texel * 4 + 0] = UnitToByte( unit.x * 0.5f + 0.5f );
                    out.NormalDepth[texel * 4 + 1] = UnitToByte( unit.y * 0.5f + 0.5f );
                    out.NormalDepth[texel * 4 + 2] = UnitToByte( unit.z * 0.5f + 0.5f );
                    out.NormalDepth[texel * 4 + 3] = UnitToByte( depth );
                    out.ORM[texel * 4 + 0]         = UnitToByte( photo.Occlusion[pixel] );
                    out.ORM[texel * 4 + 1]         = UnitToByte( nr.w );
                    out.ORM[texel * 4 + 2]         = photo.BaseColorMetallic[pixel * 4 + 3];
                    out.ORM[texel * 4 + 3]         = 255;
                    filled[texel]                  = 1;
                    ++out.CoveredTexels;
                }
        }
        if ( out.CoveredTexels == 0 )
            return Common::MakeError<Result>( std::format(
                 "nothing drew in any of the {} impostor photos ({}x{} each)", frames, resolution, resolution ) );

        // Dilation inside each frame: an empty texel takes the mean of its filled 4-neighbours of the same frame;
        // coverage (BaseColor.a, ORM.a) stays 0, so the alpha test still cuts the silhouette where it was drawn.
        std::vector<uint8_t> next = filled;
        for ( int pass = 0; pass < kDilationPasses; ++pass )
        {
            for ( uint32_t y = 0; y < out.Side; ++y )
                for ( uint32_t x = 0; x < out.Side; ++x )
                {
                    const std::size_t texel = static_cast<std::size_t>( y ) * out.Side + x;
                    if ( filled[texel] != 0 )
                        continue;
                    const uint32_t fx = x % resolution;
                    const uint32_t fy = y % resolution;
                    std::size_t    neighbours[4];
                    int            count = 0;
                    if ( fx > 0 && filled[texel - 1] != 0 )
                        neighbours[count++] = texel - 1;
                    if ( fx + 1 < resolution && filled[texel + 1] != 0 )
                        neighbours[count++] = texel + 1;
                    if ( fy > 0 && filled[texel - out.Side] != 0 )
                        neighbours[count++] = texel - out.Side;
                    if ( fy + 1 < resolution && filled[texel + out.Side] != 0 )
                        neighbours[count++] = texel + out.Side;
                    if ( count == 0 )
                        continue;
                    for ( int channel = 0; channel < 3; ++channel )
                    {
                        int baseColor = 0, normal = 0, orm = 0;
                        for ( int n = 0; n < count; ++n )
                        {
                            baseColor += out.BaseColor[neighbours[n] * 4 + channel];
                            normal += out.NormalDepth[neighbours[n] * 4 + channel];
                            orm += out.ORM[neighbours[n] * 4 + channel];
                        }
                        out.BaseColor[texel * 4 + channel]   = static_cast<uint8_t>( baseColor / count );
                        out.NormalDepth[texel * 4 + channel] = static_cast<uint8_t>( normal / count );
                        out.ORM[texel * 4 + channel]         = static_cast<uint8_t>( orm / count );
                    }
                    int depth = 0;
                    for ( int n = 0; n < count; ++n )
                        depth += out.NormalDepth[neighbours[n] * 4 + 3];
                    out.NormalDepth[texel * 4 + 3] = static_cast<uint8_t>( depth / count );
                    next[texel]                    = 1;
                }
            filled = next;
        }
        return Common::MakeSuccess( std::move( out ) );
    }
    Geometry::MeshSavedForm HLODImpostorCardMesh( const ImpostorSphere& sphere )
    {
        constexpr std::array<glm::vec2, 4> kCorners = { glm::vec2( -1.0f, -1.0f ), glm::vec2( 1.0f, -1.0f ),
                                                        glm::vec2( 1.0f, 1.0f ), glm::vec2( -1.0f, 1.0f ) };
        Geometry::MeshSavedForm card;
        Geometry::MeshOverlaySavedForm corners;
        for ( const glm::vec2& c : kCorners )
        {
            const glm::vec3 p = sphere.Centre + sphere.Radius * glm::vec3( c.x, c.y, c.x * c.y );
            card.Positions.insert( card.Positions.end(), { p.x, p.y, p.z } );
            corners.Values.insert( corners.Values.end(), { c.x, c.y } );
        }
        card.Triangles   = { 0, 1, 2, 0, 2, 3 };
        card.PolyGroups  = { 0, 0 };
        card.MaterialIds = { 0, 0 };
        corners.Triangles = card.Triangles;
        Geometry::MeshOverlaySavedForm normal;
        normal.Values    = { 0.0f, 0.0f, 1.0f };
        normal.Triangles = { 0, 0, 0, 0, 0, 0 };
        card.Normals     = std::move( normal );
        Geometry::MeshOverlaySavedForm radius;
        radius.Values    = { sphere.Radius, sphere.Radius };
        radius.Triangles = { 0, 0, 0, 0, 0, 0 };
        card.UVs.push_back( std::move( radius ) );
        card.UVs.push_back( std::move( corners ) );
        return card;
    }

    std::string HLODImpostorKey( const Assets::InstancedStaticMeshComponentSer& part,
                                 const HLODImpostorSettings&                    settings )
    {
        // FNV-1a 64 over the stated inputs, each text followed by a 0 so ("ab","c") and ("a","bc") differ.
        uint64_t   hash  = 1469598103934665603ull;
        const auto bytes = [&]( const void* data, std::size_t size )
        {
            const auto* p = static_cast<const unsigned char*>( data );
            for ( std::size_t k = 0; k < size; ++k )
            {
                hash ^= p[k];
                hash *= 1099511628211ull;
            }
        };
        const auto text = [&]( const std::string& value )
        {
            bytes( value.data(), value.size() );
            const unsigned char zero = 0;
            bytes( &zero, 1 );
        };
        const auto texts = [&]( const std::optional<std::vector<std::string>>& values )
        {
            const int32_t count = values ? static_cast<int32_t>( values->size() ) : -1;
            bytes( &count, sizeof( count ) );
            if ( values )
                for ( const std::string& value : *values )
                    text( value );
        };
        text( part.MeshGuid.value_or( std::string() ) );
        text( part.MeshPath.value_or( std::string() ) );
        texts( part.MaterialGuids );
        texts( part.MaterialPaths );
        const uint8_t hemi = settings.Hemisphere ? 1 : 0;
        bytes( &settings.FramesPerSide, sizeof( settings.FramesPerSide ) );
        bytes( &hemi, 1 );
        bytes( &settings.FrameResolution, sizeof( settings.FrameResolution ) );
        return std::format( "{:016x}", hash );
    }
} // namespace Desert::Core
