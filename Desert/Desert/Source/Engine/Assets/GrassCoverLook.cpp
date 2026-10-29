#include <Engine/Assets/GrassCoverLook.hpp>

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
#include <mutex>

namespace Desert::Assets
{
    namespace
    {
        std::mutex            s_BuilderMutex;
        GrassCoverLookBuilder s_Builder;

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

        GrassCoverLookBuilder CurrentBuilder()
        {
            const std::lock_guard<std::mutex> lock( s_BuilderMutex );
            return s_Builder;
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

    std::string EncodeGrassCoverLook( const GrassCoverLook& look )
    {
        std::string bytes( 4 * sizeof( float ) + sizeof( uint32_t ), '\0' );
        const float values[4] = { look.Albedo.r, look.Albedo.g, look.Albedo.b, look.Coverage };
        std::memcpy( bytes.data(), &kGrassCoverLookBuilderVersion, sizeof( uint32_t ) );
        std::memcpy( bytes.data() + sizeof( uint32_t ), values, sizeof( values ) );
        return bytes;
    }

    Common::ResultStr<GrassCoverLook> DecodeGrassCoverLook( const std::string_view bytes )
    {
        uint32_t version = 0;
        if ( bytes.size() != 4 * sizeof( float ) + sizeof( uint32_t ) )
            return Common::MakeFormattedError<GrassCoverLook>( "grass cover look entry is {} bytes, not {}",
                                                               bytes.size(),
                                                               4 * sizeof( float ) + sizeof( uint32_t ) );
        std::memcpy( &version, bytes.data(), sizeof( uint32_t ) );
        if ( version != kGrassCoverLookBuilderVersion )
            return Common::MakeFormattedError<GrassCoverLook>( "grass cover look entry is version {}, not {}",
                                                               version, kGrassCoverLookBuilderVersion );
        float values[4];
        std::memcpy( values, bytes.data() + sizeof( uint32_t ), sizeof( values ) );
        GrassCoverLook look;
        look.Albedo   = glm::vec3( values[0], values[1], values[2] );
        look.Coverage = values[3];
        return Common::MakeSuccess( look );
    }

    void SetGrassCoverLookBuilder( GrassCoverLookBuilder builder )
    {
        const std::lock_guard<std::mutex> lock( s_BuilderMutex );
        s_Builder = std::move( builder );
    }

    Common::ResultStr<GrassCoverLook> LoadGrassCoverLook( const GrassCoverLookInput& input )
    {
        const uint64_t              key     = GrassCoverLookKey( input );
        const GrassCoverLookBuilder builder = CurrentBuilder();
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
