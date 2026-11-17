#include "HLODBakeWriter.hpp"

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/MaterialFormat.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>
#include <Engine/Core/Serialize/StoredAssetForm.hpp>
#include <Engine/Core/Serialize/WorldCells.hpp>

#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Logger.hpp>

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <fstream>
#include <sstream>
#include <utility>

// STB_IMAGE_WRITE_IMPLEMENTATION is compiled into stb_image.cpp; declarations only here.
#include <stb_image/stb_image_write.h>

namespace Desert::Editor::HLODBake
{
    namespace
    {
        // The engine's StandardSurface: the material every baked HLOD binds.
        constexpr std::string_view kSurfaceShaderGuid = "4f1cac6af403a010c792d835dd6f7d44";
        constexpr std::string_view kSurfaceShaderPath = "engine:Shaders/Programs/Surface/StandardSurface.shader";
        constexpr std::string_view kAlbedoSlot        = "AlbedoTexture";
        // SurfaceResolveORM (Common/SurfaceInputs.glslh) reads r = AO, g = Roughness, b = Metallic and MULTIPLIES
        // roughness by RoughnessFactor (schema default 0.5) and metallic by MetallicFactor (default 0): the baked
        // values are the sources' own, so both factors are 1. AOStrength's default (1) takes the map as it is.
        constexpr std::string_view kORMSlot         = "OrmTexture";
        constexpr std::string_view kRoughnessFactor = "RoughnessFactor";
        constexpr std::string_view kMetallicFactor  = "MetallicFactor";
        // StandardSurface's tangent-space normal map (Intent NormalMap; NormalScale defaults to 1).
        constexpr std::string_view kNormalSlot = "NormalTexture";
        // Emitted radiance = texel * EmissiveColor * EmissiveIntensity (SurfaceInputs.glslh): the texel holds the
        // radiance divided by the brightest channel, the colour is white and the intensity that channel (UE's
        // flattened emissive scale, FFlattenMaterial::EmissiveScale).
        constexpr std::string_view kEmissiveSlot      = "EmissiveTexture";
        constexpr std::string_view kEmissiveColor     = "EmissiveColor";
        constexpr std::string_view kEmissiveIntensity = "EmissiveIntensity";
        // The RGBA16F G-buffer targets: 4 half floats per texel.
        constexpr std::size_t kHalfTexelBytes = 8;
        // The z a stored tangent normal keeps at least: a world normal at or behind the mesh's surface plane
        // (a slope the coarse HLOD mesh flattens) is tipped back to the hemisphere the decoder reconstructs
        // (Common/TangentNormal.glslh derives z from x and y).
        constexpr float kMinTangentZ = 1.0e-3f;
        constexpr float kFlatEpsilon = 1.0e-6f;

        [[nodiscard]] glm::vec4 HalfTexel( const std::vector<uint8_t>& texels, std::size_t texel )
        {
            uint16_t halves[4] = {};
            std::memcpy( halves, texels.data() + texel * kHalfTexelBytes, sizeof( halves ) );
            return glm::vec4( glm::unpackHalf1x16( halves[0] ), glm::unpackHalf1x16( halves[1] ),
                              glm::unpackHalf1x16( halves[2] ), glm::unpackHalf1x16( halves[3] ) );
        }

        [[nodiscard]] Common::ResultStr<std::vector<std::byte>> EncodePng( const std::vector<uint8_t>& rgba,
                                                                           uint32_t width, uint32_t height )
        {
            std::vector<std::byte> out;
            stbi_flip_vertically_on_write( 0 );
            const int written = stbi_write_png_to_func(
                 []( void* context, void* data, int size )
                 {
                     auto*       bytes = static_cast<std::vector<std::byte>*>( context );
                     const auto* begin = static_cast<const std::byte*>( data );
                     bytes->insert( bytes->end(), begin, begin + size );
                 },
                 &out, static_cast<int>( width ), static_cast<int>( height ), 4, rgba.data(),
                 static_cast<int>( width ) * 4 );
            if ( written == 0 || out.empty() )
                return Common::MakeError<std::vector<std::byte>>(
                     std::format( "stbi_write_png_to_func refused a {}x{} RGBA8 image", width, height ) );
            return Common::MakeSuccess( std::move( out ) );
        }

        // The GUID an existing material file states, so a re-bake keeps the identity every reference holds.
        [[nodiscard]] std::optional<Common::Content::TextAssetHeaderSerialized>
        ExistingMaterialHeader( const std::filesystem::path& file )
        {
            std::ifstream in( file, std::ios::binary );
            if ( !in )
                return std::nullopt;
            std::stringstream text;
            text << in.rdbuf();
            auto parsed = Assets::ParseMaterialJson( file.generic_string(), text.str() );
            if ( !parsed )
            {
                LOG_WARN( "[Build HLODs] '{0}' is not a readable material ({1}); it is rewritten with a new GUID",
                          file.generic_string(), parsed.GetError() );
                return std::nullopt;
            }
            return parsed.GetValue().Header;
        }
    } // namespace

    Common::BoolResultStr CheckSizes( const GBufferTexels& texels )
    {
        const std::size_t count = static_cast<std::size_t>( texels.Width ) * texels.Height;
        if ( texels.A.size() != count * 4 )
            return Common::MakeError( std::format( "the albedo readback holds {} bytes, expected {}x{} RGBA8",
                                                   texels.A.size(), texels.Width, texels.Height ) );
        if ( texels.B.size() != count * kHalfTexelBytes || texels.Emissive.size() != count * kHalfTexelBytes )
            return Common::MakeError(
                 std::format( "the G-buffer B / emissive readbacks hold {} / {} bytes, expected {}x{} RGBA16F",
                              texels.B.size(), texels.Emissive.size(), texels.Width, texels.Height ) );
        return BOOLSUCCESS;
    }

    glm::vec3 WorldNormal( const GBufferTexels& texels, std::size_t texel )
    {
        return glm::vec3( HalfTexel( texels.B, texel ) );
    }

    bool Covered( const GBufferTexels& texels, std::size_t texel )
    {
        const glm::vec3 n = WorldNormal( texels, texel );
        return glm::dot( n, n ) >= kFlatEpsilon;
    }

    float Roughness( const GBufferTexels& texels, std::size_t texel )
    {
        return HalfTexel( texels.B, texel ).w;
    }

    float MaterialAO( const GBufferTexels& texels, std::size_t texel )
    {
        return HalfTexel( texels.Emissive, texel ).w;
    }

    glm::vec3 EmissiveRadiance( const GBufferTexels& texels, std::size_t texel )
    {
        return glm::vec3( HalfTexel( texels.Emissive, texel ) );
    }

    uint8_t UnitToByte( float value )
    {
        return static_cast<uint8_t>( std::clamp( std::lround( value * 255.0f ), 0L, 255L ) );
    }

    uint8_t LinearToSrgb8( float linear )
    {
        const float c = std::clamp( linear, 0.0f, 1.0f );
        const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow( c, 1.0f / 2.4f ) - 0.055f;
        return UnitToByte( s );
    }

    float Srgb8ToLinear( uint8_t code )
    {
        const float s = static_cast<float>( code ) / 255.0f;
        return s <= 0.04045f ? s / 12.92f : std::pow( ( s + 0.055f ) / 1.055f, 2.4f );
    }

    void RasterizeUV( const Geometry::RenderMeshData& mesh, uint32_t width, uint32_t height,
                      const UVTexelVisitor& visit )
    {
        const glm::vec2 extent( static_cast<float>( width ), static_cast<float>( height ) );
        const auto      edge = []( const glm::vec2& a, const glm::vec2& b, const glm::vec2& p )
        { return ( b.x - a.x ) * ( p.y - a.y ) - ( b.y - a.y ) * ( p.x - a.x ); };
        for ( const auto& submesh : mesh.Submeshes )
            for ( uint32_t t = submesh.IndexOffset / 3; t < ( submesh.IndexOffset + submesh.IndexCount ) / 3; ++t )
            {
                // Index = one triangle; the submesh range is in uint32_t units (DynamicMeshRenderConversion).
                const ::Desert::Index&                     triangle = mesh.Indices[t];
                const std::array<const Desert::Vertex*, 3> v        = {
                     &mesh.Vertices[submesh.VertexOffset + triangle.V1],
                     &mesh.Vertices[submesh.VertexOffset + triangle.V2],
                     &mesh.Vertices[submesh.VertexOffset + triangle.V3] };
                const glm::vec2 p[3] = { v[0]->TexCoord * extent, v[1]->TexCoord * extent,
                                         v[2]->TexCoord * extent };
                const float     area = edge( p[0], p[1], p[2] );
                if ( std::abs( area ) < kFlatEpsilon )
                    continue;
                const float minX = std::min( { p[0].x, p[1].x, p[2].x } );
                const float maxX = std::max( { p[0].x, p[1].x, p[2].x } );
                const float minY = std::min( { p[0].y, p[1].y, p[2].y } );
                const float maxY = std::max( { p[0].y, p[1].y, p[2].y } );
                const long  c0   = std::max( 0L, std::lround( std::floor( minX - 0.5f ) ) );
                const long  c1 =
                     std::min( static_cast<long>( width ) - 1, std::lround( std::ceil( maxX - 0.5f ) ) );
                const long r0 = std::max( 0L, std::lround( std::floor( minY - 0.5f ) ) );
                const long r1 =
                     std::min( static_cast<long>( height ) - 1, std::lround( std::ceil( maxY - 0.5f ) ) );
                for ( long r = r0; r <= r1; ++r )
                    for ( long c = c0; c <= c1; ++c )
                    {
                        const glm::vec2 q( static_cast<float>( c ) + 0.5f, static_cast<float>( r ) + 0.5f );
                        const float     w0 = edge( p[1], p[2], q ) / area;
                        const float     w1 = edge( p[2], p[0], q ) / area;
                        const float     w2 = 1.0f - w0 - w1;
                        if ( w0 < -kFlatEpsilon || w1 < -kFlatEpsilon || w2 < -kFlatEpsilon )
                            continue;
                        visit( static_cast<std::size_t>( r ) * width + static_cast<std::size_t>( c ),
                               glm::vec3( w0, w1, w2 ), v );
                    }
            }
    }

    glm::mat3 InterpolatedFrame( const glm::vec3& weights, const std::array<const Desert::Vertex*, 3>& v )
    {
        glm::vec3 t( 0.0f );
        glm::vec3 b( 0.0f );
        glm::vec3 n( 0.0f );
        for ( int c = 0; c < 3; ++c )
        {
            t += weights[c] * glm::normalize( v[c]->Tangent );
            b += weights[c] * glm::normalize( v[c]->Bitangent );
            n += weights[c] * glm::normalize( v[c]->Normal );
        }
        return glm::mat3( t, b, n );
    }

    bool EncodeTangentNormal( const glm::vec3& world, const glm::mat3& tbn, uint8_t* rgba )
    {
        if ( glm::dot( world, world ) < kFlatEpsilon || std::abs( glm::determinant( tbn ) ) < kFlatEpsilon )
            return false;
        glm::vec3 n = glm::inverse( tbn ) * glm::normalize( world );
        if ( glm::dot( n, n ) < kFlatEpsilon )
            return false;
        n = glm::normalize( n );
        if ( n.z < kMinTangentZ )
            n = glm::normalize( glm::vec3( n.x, n.y, kMinTangentZ ) );
        rgba[0] = UnitToByte( n.x * 0.5f + 0.5f );
        rgba[1] = UnitToByte( n.y * 0.5f + 0.5f );
        rgba[2] = UnitToByte( n.z * 0.5f + 0.5f );
        rgba[3] = 255;
        return true;
    }

    std::vector<uint8_t> FlatNormals( uint32_t width, uint32_t height )
    {
        std::vector<uint8_t> out( static_cast<std::size_t>( width ) * height * 4 );
        for ( std::size_t i = 0; i < out.size(); i += 4 )
        {
            out[i + 0] = 128;
            out[i + 1] = 128;
            out[i + 2] = 255;
            out[i + 3] = 255;
        }
        return out;
    }

    std::vector<std::size_t> NearestCovered( const std::vector<uint8_t>& covered, uint32_t width, uint32_t height )
    {
        const std::size_t        count = static_cast<std::size_t>( width ) * height;
        constexpr std::size_t    kNone = static_cast<std::size_t>( -1 );
        std::vector<std::size_t> nearest( count, kNone );
        std::deque<std::size_t>  front;
        for ( std::size_t i = 0; i < count && i < covered.size(); ++i )
            if ( covered[i] != 0 )
            {
                nearest[i] = i;
                front.push_back( i );
            }
        if ( front.empty() )
            return {};
        while ( !front.empty() )
        {
            const std::size_t at = front.front();
            front.pop_front();
            const std::size_t x     = at % width;
            const std::size_t y     = at / width;
            const auto        reach = [&]( std::size_t to )
            {
                if ( nearest[to] != kNone )
                    return;
                nearest[to] = nearest[at];
                front.push_back( to );
            };
            if ( x > 0 )
                reach( at - 1 );
            if ( x + 1 < width )
                reach( at + 1 );
            if ( y > 0 )
                reach( at - width );
            if ( y + 1 < height )
                reach( at + width );
        }
        return nearest;
    }

    void SmearUncovered( std::vector<uint8_t>& rgba, const std::vector<uint8_t>& covered, uint32_t width,
                         uint32_t height )
    {
        const std::vector<std::size_t> nearest = NearestCovered( covered, width, height );
        for ( std::size_t i = 0; i < nearest.size(); ++i )
            if ( nearest[i] != i )
                std::memcpy( rgba.data() + i * 4, rgba.data() + nearest[i] * 4, 4 );
    }

    Common::ResultStr<BakedMaterial> WriteBakedMaterial( const std::filesystem::path& scenePath,
                                                         const std::string& stem, BakedImages images )
    {
        using Result                    = BakedMaterial;
        const std::filesystem::path dir = ::Desert::Core::WorldCells::BuiltHLODDirectory( scenePath );
        std::error_code             ec;
        std::filesystem::create_directories( dir, ec );
        if ( ec )
            return Common::MakeError<Result>(
                 std::format( "'{}' could not be created: {}", dir.generic_string(), ec.message() ) );

        // Writes one baked map and answers its file and the GUID its header states.
        const auto writeTexture = [&]( const std::string& map, const std::vector<uint8_t>& rgba,
                                       ::Desert::Core::Formats::TextureIntent     intent,
                                       ::Desert::Core::Formats::TextureColorSpace colorSpace )
             -> Common::ResultStr<std::pair<std::filesystem::path, std::string>>
        {
            using Written = std::pair<std::filesystem::path, std::string>;
            auto png      = EncodePng( rgba, images.Width, images.Height );
            if ( !png )
                return Common::MakeError<Written>( png.GetError() );
            const std::filesystem::path   file = dir / std::format( "T_{}_{}.detex", stem, map );
            Assets::TextureImportSettings settings;
            settings.Intent     = intent;
            settings.ColorSpace = colorSpace;
            const std::string sourceKey =
                 Common::AssetHandle::StableKeyForPath( dir / std::format( "T_{}_{}.png", stem, map ) );
            auto written = Assets::WriteTextureSource( file, Common::Content::ContentKind::Texture, sourceKey,
                                                       std::move( png.ExtractValue() ), settings );
            if ( !written )
                return Common::MakeError<Written>( written.GetError() );
            auto texture = Assets::ReadTextureSourceAssetFile( file );
            if ( !texture )
                return Common::MakeError<Written>( texture.GetError() );
            // THE BAKE AUTHORS THESE SETTINGS. WriteTextureSource keeps an existing asset's settings (the
            // re-import rule for a person's texture); a baked texture has no person behind it, so an earlier
            // bake's settings are replaced here, GUID kept, and the next cook re-runs on the new intent.
            if ( texture.GetValue().Import.Settings != settings )
            {
                Assets::TextureSourceAsset rewritten = texture.ExtractValue();
                rewritten.Import.Settings            = settings;
                if ( auto w = Assets::WriteTextureSourceAssetFile( file, rewritten ); !w.IsSuccess() )
                    return Common::MakeError<Written>( w.GetError() );
                Assets::ContentRegistry::NoteFile( file );
                return Common::MakeSuccess( Written{ file, Common::Content::AssetGuidToText( rewritten.Guid ) } );
            }
            Assets::ContentRegistry::NoteFile( file );
            return Common::MakeSuccess(
                 Written{ file, Common::Content::AssetGuidToText( texture.GetValue().Guid ) } );
        };

        const std::filesystem::path matFile = dir / std::format( "M_{}.demat", stem );
        Assets::MaterialData        material;
        material.Header = ExistingMaterialHeader( matFile ); // the GUID survives a re-bake
        material.Shader =
             Assets::AssetGuidRef{ std::string( kSurfaceShaderGuid ), std::string( kSurfaceShaderPath ) };
        const auto bind =
             [&material]( std::string_view slot, const std::pair<std::filesystem::path, std::string>& map )
        {
            Assets::MaterialAssetRef ref;
            ref.Name = std::string( slot );
            ref.Guid = map.second;
            ref.Path = Common::AssetHandle::StableKeyForPath( map.first );
            material.Textures.push_back( std::move( ref ) );
        };

        std::string written = "BaseColor";
        auto albedo = writeTexture( "BaseColor", images.BaseColor, ::Desert::Core::Formats::TextureIntent::Colour,
                                    ::Desert::Core::Formats::TextureColorSpace::SRGB );
        if ( !albedo )
            return Common::MakeError<Result>( albedo.GetError() );
        bind( kAlbedoSlot, albedo.GetValue() );
        if ( images.ORM.has_value() )
        {
            auto orm = writeTexture( "ORM", *images.ORM, ::Desert::Core::Formats::TextureIntent::PackedMasks,
                                     ::Desert::Core::Formats::TextureColorSpace::Linear );
            if ( !orm )
                return Common::MakeError<Result>( orm.GetError() );
            bind( kORMSlot, orm.GetValue() );
            material.Params.push_back(
                 Assets::MaterialShaderParam{ std::string( kRoughnessFactor ), glm::vec4( 1.0f ) } );
            material.Params.push_back(
                 Assets::MaterialShaderParam{ std::string( kMetallicFactor ), glm::vec4( 1.0f ) } );
            written += " + ORM";
        }
        if ( images.Normal.has_value() )
        {
            auto normal =
                 writeTexture( "Normal", *images.Normal, ::Desert::Core::Formats::TextureIntent::NormalMap,
                               ::Desert::Core::Formats::TextureColorSpace::Linear );
            if ( !normal )
                return Common::MakeError<Result>( normal.GetError() );
            bind( kNormalSlot, normal.GetValue() );
            written += " + Normal";
        }
        if ( images.Emissive.has_value() )
        {
            auto emissive =
                 writeTexture( "Emissive", *images.Emissive, ::Desert::Core::Formats::TextureIntent::Colour,
                               ::Desert::Core::Formats::TextureColorSpace::SRGB );
            if ( !emissive )
                return Common::MakeError<Result>( emissive.GetError() );
            bind( kEmissiveSlot, emissive.GetValue() );
            material.Params.push_back(
                 Assets::MaterialShaderParam{ std::string( kEmissiveColor ), glm::vec4( 1.0f ) } );
            material.Params.push_back( Assets::MaterialShaderParam{ std::string( kEmissiveIntensity ),
                                                                    glm::vec4( images.EmissiveScale ) } );
            written += std::format( " + Emissive (x{})", images.EmissiveScale );
        }
        material = Assets::StampMaterialHeader( std::move( material ) );
        if ( auto saved = Assets::WriteMaterialFile( matFile, material ); !saved )
            return Common::MakeError<Result>( saved.GetError() );
        Assets::ContentRegistry::NoteFile( matFile );

        // The HLOD block's slot path is a SCENE-RECORD reference, so it is spelled the way a scene stores a
        // material (StoredFormFor("MaterialAsset") = assets-relative), not as the tagged key the .demat's own
        // texture slot uses: a tagged key there is re-tagged on load ("assets:assets:...") and the slot stays
        // empty.
        const auto materialForm = ::Desert::Core::Serialize::StoredFormFor( "MaterialAsset" );
        if ( !materialForm )
            return Common::MakeError<Result>( "the material reference has no stored form (StoredAssetForm.cpp)" );
        LOG_INFO( "[Build HLODs] baked {0}: {1}x{2} {3} -> {4}", stem, images.Width, images.Height, written,
                  matFile.generic_string() );
        return Common::MakeSuccess(
             BakedMaterial{ .Guid = material.Header->Guid,
                            .Path = ::Desert::Core::Serialize::RenderStoredForm(
                                 *materialForm, Common::AssetHandle::StableKeyForPath( matFile ) ),
                            .File = matFile } );
    }
} // namespace Desert::Editor::HLODBake
