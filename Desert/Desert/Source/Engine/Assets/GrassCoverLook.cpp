#include <Engine/Assets/GrassCoverLook.hpp>

#include <Engine/Assets/BuilderSlot.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/MaterialData.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Assets/MeshDisplacementBake.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>

namespace Desert::Assets
{
    namespace
    {
        BuilderSlot<GrassCoverLookBuilder> s_GrassCoverLookBuilder;

        // The texture a material's chain assigns to @p name (child first): its asset path and derived-data key;
        // nullopt when nobody assigns one.
        Common::ResultStr<std::optional<std::pair<std::filesystem::path, uint64_t>>>
        AssignedTexture( const MaterialChain& chain, const std::string& name )
        {
            using Result    = std::optional<std::pair<std::filesystem::path, uint64_t>>;
            uint64_t handle = 0;
            for ( const auto& link : chain.Links )
                if ( ( handle = link->Data().GetTexture( name ) ) != 0 )
                    break;
            if ( handle == 0 )
                return Common::MakeSuccess<Result>( std::nullopt );
            const std::string material = chain.Links.front()->GetMetadata().Filepath.generic_string();
            const auto        row      = ContentRegistry::RowOf( Common::Content::ContentKind::Texture, handle );
            if ( !row )
                return Common::MakeFormattedError<Result>(
                     "material '{}': its '{}' texture (handle {:016x}) is not a texture this project has scanned",
                     material, name, handle );
            auto key = ReadTextureAssetKey( row->Path );
            if ( !key.IsSuccess() )
                return Common::MakeFormattedError<Result>( "material '{}': '{}' texture '{}': {}", material, name,
                                                           row->Path.generic_string(), key.GetError() );
            return Common::MakeSuccess<Result>( std::pair{ row->Path, key.GetValue().DerivedDataKey } );
        }

        template <typename T>
        void Mix( uint64_t& h, const T& value )
        {
            std::array<unsigned char, sizeof( T )> bytes{};
            std::memcpy( bytes.data(), &value, sizeof( T ) );
            for ( const unsigned char b : bytes )
                h = ( h ^ b ) * 1099511628211ull;
        }

    } // namespace

    Common::ResultStr<std::optional<GrassCoverLookInput>>
    ResolveGrassCoverLookInput( AssetManager& manager, const Common::Content::AssetGuid& material,
                                const std::span<const glm::vec4> rects )
    {
        using Result  = std::optional<GrassCoverLookInput>;
        auto resolved = ResolveMaterialChain( manager, material );
        if ( !resolved.IsSuccess() )
            return Common::MakeError<Result>( resolved.GetError() );
        const MaterialChain& chain = resolved.GetValue();

        // The albedo is the template's colour map: its first Texture2D a Colour texture is bound to (UE: the
        // texture feeding BaseColor - here the slot whose Intent says it is looked at as colour).
        const auto colour = std::ranges::find_if(
             chain.Meta.Params, []( const Core::Formats::ShaderParam& p )
             { return p.IsTexture && !p.IsCubeTexture && p.SlotIntent == Core::Formats::TextureIntent::Colour; } );
        if ( colour == chain.Meta.Params.end() )
            return Common::MakeSuccess<Result>( std::nullopt );
        auto albedo = AssignedTexture( chain, colour->Name );
        if ( !albedo.IsSuccess() )
            return Common::MakeError<Result>( albedo.GetError() );
        if ( !albedo.GetValue() )
            return Common::MakeSuccess<Result>( std::nullopt );

        GrassCoverLookInput input;
        input.AlbedoAsset = albedo.GetValue()->first;
        input.AlbedoKey   = albedo.GetValue()->second;
        input.Rects.assign( rects.begin(), rects.end() );
        if ( input.Rects.empty() )
            input.Rects.emplace_back( 0.0f, 0.0f, 1.0f, 1.0f );

        // The texels the template cuts away are not drawn, so they are not the cover's (a mask unassigned samples
        // the template's default, which passes everywhere).
        if ( chain.Meta.OpacityMask )
        {
            const Core::Formats::SurfaceOpacityMask& m    = *chain.Meta.OpacityMask;
            auto                                     mask = AssignedTexture( chain, m.Texture );
            if ( !mask.IsSuccess() )
                return Common::MakeError<Result>( mask.GetError() );
            if ( mask.GetValue() )
            {
                input.MaskAsset   = mask.GetValue()->first;
                input.MaskKey     = mask.GetValue()->second;
                input.MaskChannel = m.ChannelParam.empty()
                                         ? m.Channel
                                         : static_cast<uint32_t>( std::clamp(
                                                MaterialChainParam( chain, m.ChannelParam ).x, 0.0f, 3.0f ) );
                input.MaskClip    = m.ClipParam.empty() ? 0.0f : MaterialChainParam( chain, m.ClipParam ).x;
            }
        }
        return Common::MakeSuccess<Result>( std::move( input ) );
    }

    uint64_t GrassCoverLookKey( const GrassCoverLookInput& input )
    {
        uint64_t h = 1469598103934665603ull;
        Mix( h, kGrassCoverLookBuilderVersion );
        // The textures by content (their derived-data keys), never by path: a moved file keeps the look.
        Mix( h, input.AlbedoKey );
        Mix( h, input.MaskAsset.empty() );
        Mix( h, input.MaskKey );
        Mix( h, input.MaskChannel );
        Mix( h, std::bit_cast<uint32_t>( input.MaskClip ) );
        Mix( h, static_cast<uint64_t>( input.Rects.size() ) );
        for ( const glm::vec4& r : input.Rects )
            for ( int c = 0; c < 4; ++c )
                Mix( h, std::bit_cast<uint32_t>( r[c] ) );
        return Common::DDC::MakeKey( kGrassCoverLookDeriver, h, nullptr, 0 );
    }

    GrassCoverLook AverageGrassCoverLook( const GrassCoverLookInput& input, const GrassCoverLookLevel& albedo,
                                          const std::optional<GrassCoverLookLevel>& mask )
    {
        GrassCoverLook look;
        if ( albedo.Width == 0 || albedo.Height == 0 )
            return look;
        glm::dvec3 sum( 0.0 );
        double     weight = 0.0, aspects = 0.0, covered = 0.0;
        for ( const glm::vec4& r : input.Rects )
        {
            // The texels whose centres fall inside the rect, as the rasteriser covers a card.
            const auto first = []( float uv, uint32_t n )
            { return static_cast<int64_t>( std::ceil( uv * static_cast<float>( n ) - 0.5f ) ); };
            const int64_t x0 = std::max<int64_t>( first( std::min( r.x, r.z ), albedo.Width ), 0 );
            const int64_t x1 = std::min<int64_t>( first( std::max( r.x, r.z ), albedo.Width ), albedo.Width );
            const int64_t y0 = std::max<int64_t>( first( std::min( r.y, r.w ), albedo.Height ), 0 );
            const int64_t y1 = std::min<int64_t>( first( std::max( r.y, r.w ), albedo.Height ), albedo.Height );
            if ( x1 <= x0 || y1 <= y0 )
                continue;
            // The card's world area (in blade heights squared): its rect's aspect, as BladeAt sizes the strip.
            const double aspect = std::abs( static_cast<double>( r.z ) - r.x ) /
                                  std::max( std::abs( static_cast<double>( r.w ) - r.y ), 1.0e-4 );
            glm::dvec3 cardSum( 0.0 );
            double     kept = 0.0;
            for ( int64_t y = y0; y < y1; ++y )
                for ( int64_t x = x0; x < x1; ++x )
                {
                    if ( mask && mask->Width > 0 && mask->Height > 0 )
                    {
                        // The mask texel under the same UV (the two maps may differ in size).
                        const double   u  = ( static_cast<double>( x ) + 0.5 ) / albedo.Width;
                        const double   v  = ( static_cast<double>( y ) + 0.5 ) / albedo.Height;
                        const uint32_t mx = std::min( static_cast<uint32_t>( u * mask->Width ), mask->Width - 1 );
                        const uint32_t my =
                             std::min( static_cast<uint32_t>( v * mask->Height ), mask->Height - 1 );
                        const float m = mask->Texels[static_cast<size_t>( my ) * mask->Width + mx]
                                                    [static_cast<int>( std::min( input.MaskChannel, 3u ) )];
                        if ( m < input.MaskClip || m <= 0.0f )
                            continue;
                    }
                    const glm::vec4& t = albedo.Texels[static_cast<size_t>( y ) * albedo.Width + x];
                    cardSum += glm::dvec3( t.r, t.g, t.b );
                    kept += 1.0;
                }
            // Each texel of this card stands for aspect / texels of world area.
            const double perTexel = aspect / static_cast<double>( ( x1 - x0 ) * ( y1 - y0 ) );
            sum += cardSum * perTexel;
            weight += kept * perTexel;
            aspects += aspect;
            covered += kept * perTexel;
        }
        if ( weight > 0.0 )
            look.Albedo = glm::vec3( sum / weight );
        look.Coverage = aspects > 0.0 ? static_cast<float>( covered / aspects ) : 0.0f;
        return look;
    }

    std::vector<GrassCardHull> ComputeGrassCardHulls( const GrassCoverLookInput&                 input,
                                                      const std::span<const GrassCoverLookLevel> chain )
    {
        std::vector<GrassCardHull> hulls( input.Rects.size() );
        constexpr float            kBands = static_cast<float>( kGrassCardHullBands );
        for ( GrassCardHull& hull : hulls )
            for ( auto& bands : hull.Bands )
                bands.fill( glm::vec2( 2.0f, -2.0f ) ); // empty: lo > hi
        // A clip at or under zero cuts nothing (the clip is `mask < clip`): every card is its full rect.
        if ( input.MaskClip <= 0.0f )
        {
            for ( GrassCardHull& hull : hulls )
                for ( auto& bands : hull.Bands )
                    bands.fill( glm::vec2( -1.0f, 1.0f ) );
            return hulls;
        }
        // Half a unorm8 step under the clip: a texel the decoder rounds a hair below it still counts.
        const float threshold = input.MaskClip - 0.5f / 255.0f;
        const int   channel   = static_cast<int>( std::min( input.MaskChannel, 3u ) );
        if ( chain.empty() || chain.front().Width == 0 || chain.front().Height == 0 )
            return hulls;
        const double w0 = chain.front().Width, h0 = chain.front().Height;
        // How far from a texel's centre a fragment may stand and still read it, in that level's texels and in
        // level-0 texels (the vertex stage's class rule, GrassBladeHull.glslh BladeHullClass): a bilinear tap
        // reads one texel of its level around it; the taps of one pixel spread over its footprint, at most
        // 2^(c - 1) level-0 texels along either screen axis in class c (the class's level is log2 of the blade's
        // texels per pixel plus one) - taken whole either way, for a footprint skewed off the blade's axes. The last class bounds no footprint: an anisotropic pixel (16 taps at most) spreads
        // over 16 texels of the level it samples, 8 either way.
        const auto reach = [&]( uint32_t cls, double levelTexel, double levelTexel0 )
        {
            if ( cls + 1u == kGrassCardHullClasses )
                return 9.0 * levelTexel;
            return levelTexel + std::exp2( static_cast<double>( cls ) - 1.0 ) * levelTexel0;
        };
        const double maxReachTexels = 9.0; // the widest reach above, in texels of the level
        for ( size_t level = 0; level < chain.size(); ++level )
        {
            const GrassCoverLookLevel& mip = chain[level];
            if ( mip.Width == 0 || mip.Height == 0 ||
                 mip.Texels.size() < static_cast<size_t>( mip.Width ) * mip.Height )
                continue;
            const double w = mip.Width, h = mip.Height;
            // The classes this level belongs to: every c >= level (the last class takes every level).
            const uint32_t firstClass =
                 static_cast<uint32_t>( std::min<size_t>( level, kGrassCardHullClasses - 1u ) );
            for ( size_t r = 0; r < input.Rects.size(); ++r )
            {
                const glm::vec4& rect = input.Rects[r];
                const double     du   = static_cast<double>( rect.z ) - rect.x; // side -1 at x, +1 at z
                const double     dv   = static_cast<double>( rect.y ) - rect.w; // t 0 at w (root), 1 at y (tip)
                if ( std::abs( du ) < 1.0e-9 || std::abs( dv ) < 1.0e-9 )
                    continue;
                // Every texel whose widest reach overlaps the rect.
                const double  padX = std::max( maxReachTexels, std::exp2( kGrassCardHullClasses - 2.0 ) * w / w0 + 1.0 );
                const double  padY = std::max( maxReachTexels, std::exp2( kGrassCardHullClasses - 2.0 ) * h / h0 + 1.0 );
                const int64_t x0   = std::max<int64_t>(
                     static_cast<int64_t>( std::floor( std::min( rect.x, rect.z ) * w - 0.5 - padX ) ), 0 );
                const int64_t x1 = std::min<int64_t>(
                     static_cast<int64_t>( std::ceil( std::max( rect.x, rect.z ) * w + 0.5 + padX ) ), mip.Width );
                const int64_t y0 = std::max<int64_t>(
                     static_cast<int64_t>( std::floor( std::min( rect.y, rect.w ) * h - 0.5 - padY ) ), 0 );
                const int64_t y1 = std::min<int64_t>(
                     static_cast<int64_t>( std::ceil( std::max( rect.y, rect.w ) * h + 0.5 + padY ) ), mip.Height );
                GrassCardHull& hull = hulls[r];
                for ( int64_t y = y0; y < y1; ++y )
                    for ( int64_t x = x0; x < x1; ++x )
                    {
                        if ( mip.Texels[static_cast<size_t>( y ) * mip.Width + static_cast<size_t>( x )][channel] <
                             threshold )
                            continue;
                        const double uc = ( static_cast<double>( x ) + 0.5 ) / w;
                        const double vc = ( static_cast<double>( y ) + 0.5 ) / h;
                        for ( uint32_t c = firstClass; c < kGrassCardHullClasses; ++c )
                        {
                            const double rx = reach( c, 1.0 / w, 1.0 / w0 );
                            const double ry = reach( c, 1.0 / h, 1.0 / h0 );
                            // The texel's reach in t, and the bands it touches.
                            const double ta = ( vc - ry - rect.w ) / dv, tb = ( vc + ry - rect.w ) / dv;
                            const double t0 = std::max( std::min( ta, tb ), 0.0 );
                            const double t1 = std::min( std::max( ta, tb ), 1.0 );
                            // ... and across, in side units.
                            const double sa = 2.0 * ( uc - rx - rect.x ) / du - 1.0;
                            const double sb = 2.0 * ( uc + rx - rect.x ) / du - 1.0;
                            const float  s0 = static_cast<float>( std::max( std::min( sa, sb ), -1.0 ) );
                            const float  s1 = static_cast<float>( std::min( std::max( sa, sb ), 1.0 ) );
                            if ( t1 < t0 || s1 < s0 )
                                continue;
                            const uint32_t b0 = std::min( static_cast<uint32_t>( std::floor( t0 * kBands ) ),
                                                          kGrassCardHullBands - 1u );
                            const uint32_t b1 = std::min( static_cast<uint32_t>( std::floor( t1 * kBands ) ),
                                                          kGrassCardHullBands - 1u );
                            for ( uint32_t b = b0; b <= b1; ++b )
                            {
                                glm::vec2& span = hull.Bands[c][b];
                                span.x          = std::min( span.x, s0 );
                                span.y          = std::max( span.y, s1 );
                            }
                        }
                    }
            }
        }
        return hulls;
    }

    namespace
    {
        // version, Albedo rgb + Coverage, MaskSize xy, hull count; then the hulls, [class][band] (lo, hi) floats.
        constexpr size_t kGrassCoverLookHead = sizeof( uint32_t ) + 4 * sizeof( float ) + 3 * sizeof( uint32_t );
        constexpr size_t kGrassCardHullBytes =
             static_cast<size_t>( kGrassCardHullClasses ) * kGrassCardHullBands * 2u * sizeof( float );
    } // namespace

    std::string EncodeGrassCoverLook( const GrassCoverLook& look )
    {
        std::string    bytes( kGrassCoverLookHead + look.Hulls.size() * kGrassCardHullBytes, '\0' );
        const float    values[4] = { look.Albedo.r, look.Albedo.g, look.Albedo.b, look.Coverage };
        const uint32_t sizes[3]  = { look.MaskSize.x, look.MaskSize.y, static_cast<uint32_t>( look.Hulls.size() ) };
        std::memcpy( bytes.data(), &kGrassCoverLookBuilderVersion, sizeof( uint32_t ) );
        std::memcpy( bytes.data() + sizeof( uint32_t ), values, sizeof( values ) );
        std::memcpy( bytes.data() + sizeof( uint32_t ) + sizeof( values ), sizes, sizeof( sizes ) );
        size_t at = kGrassCoverLookHead;
        for ( const GrassCardHull& hull : look.Hulls )
            for ( const auto& bands : hull.Bands )
                for ( const glm::vec2& span : bands )
                {
                    const float pair[2] = { span.x, span.y };
                    std::memcpy( bytes.data() + at, pair, sizeof( pair ) );
                    at += sizeof( pair );
                }
        return bytes;
    }

    Common::ResultStr<GrassCoverLook> DecodeGrassCoverLook( const std::string_view bytes )
    {
        uint32_t version = 0;
        if ( bytes.size() < kGrassCoverLookHead )
            return Common::MakeFormattedError<GrassCoverLook>(
                 "grass cover look entry is {} bytes, under its {}-byte head", bytes.size(), kGrassCoverLookHead );
        std::memcpy( &version, bytes.data(), sizeof( uint32_t ) );
        if ( version != kGrassCoverLookBuilderVersion )
            return Common::MakeFormattedError<GrassCoverLook>( "grass cover look entry is version {}, not {}",
                                                               version, kGrassCoverLookBuilderVersion );
        float    values[4];
        uint32_t sizes[3];
        std::memcpy( values, bytes.data() + sizeof( uint32_t ), sizeof( values ) );
        std::memcpy( sizes, bytes.data() + sizeof( uint32_t ) + sizeof( values ), sizeof( sizes ) );
        const size_t expected = kGrassCoverLookHead + static_cast<size_t>( sizes[2] ) * kGrassCardHullBytes;
        if ( bytes.size() != expected )
            return Common::MakeFormattedError<GrassCoverLook>(
                 "grass cover look entry is {} bytes, not the {} its {} hull(s) take", bytes.size(), expected,
                 sizes[2] );
        GrassCoverLook look;
        look.Albedo   = glm::vec3( values[0], values[1], values[2] );
        look.Coverage = values[3];
        look.MaskSize = glm::uvec2( sizes[0], sizes[1] );
        look.Hulls.resize( sizes[2] );
        size_t at = kGrassCoverLookHead;
        for ( GrassCardHull& hull : look.Hulls )
            for ( auto& bands : hull.Bands )
                for ( glm::vec2& span : bands )
                {
                    float pair[2];
                    std::memcpy( pair, bytes.data() + at, sizeof( pair ) );
                    at += sizeof( pair );
                    span = glm::vec2( pair[0], pair[1] );
                }
        return Common::MakeSuccess( std::move( look ) );
    }

    void SetGrassCoverLookBuilder( GrassCoverLookBuilder builder )
    {
        s_GrassCoverLookBuilder.Set( std::move( builder ) );
    }

    Common::ResultStr<GrassCoverLook> LoadGrassCoverLook( const GrassCoverLookInput& input )
    {
        const uint64_t              key     = GrassCoverLookKey( input );
        const GrassCoverLookBuilder builder = s_GrassCoverLookBuilder.Get();
        if ( !builder )
        {
            auto hit = Common::DDC::Get( kGrassCoverLookDeriver, key );
            if ( !hit.has_value() )
                return Common::MakeFormattedError<GrassCoverLook>(
                     "albedo map '{}' has no grass cover look under DDC key {:016x} ({}), and this build cannot "
                     "derive it: the package was cooked without it",
                     input.AlbedoAsset.generic_string(), key,
                     Common::DDC::RelativePath( kGrassCoverLookDeriver, key ).generic_string() );
            return DecodeGrassCoverLook( *hit );
        }
        auto derived = Common::DDC::GetOrBuild( kGrassCoverLookDeriver, key, [&]() { return builder( input ); } );
        if ( !derived.IsSuccess() )
            return Common::MakeFormattedError<GrassCoverLook>(
                 "albedo map '{}': grass cover look: {}", input.AlbedoAsset.generic_string(), derived.GetError() );
        return DecodeGrassCoverLook( derived.GetValue() );
    }
} // namespace Desert::Assets
