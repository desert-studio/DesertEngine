#include "EmbeddedSourceTexture.hpp"

#include "SourceTexturePath.hpp"

#include <Editor/Import/TextureSourceFormats.hpp>

#include <assimp/scene.h>

// STB_IMAGE_WRITE_IMPLEMENTATION is compiled into stb_image.cpp; declarations only here.
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <format>
#include <string_view>
#include <vector>

namespace Desert::Editor
{
    namespace
    {
        bool IsCompressed( const aiTexture& texture )
        {
            return texture.mHeight == 0;
        }

        // The compressed image's extension from assimp's format hint ("png", "jpg", "jpeg" ...), lower-case.
        std::string CompressedExtension( const aiTexture& texture )
        {
            std::string hint( texture.achFormatHint );
            std::ranges::transform( hint, hint.begin(),
                                    []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
            return hint;
        }

        Common::ResultStr<std::string> EncodedBytes( const aiTexture& texture )
        {
            if ( IsCompressed( texture ) )
            {
                // A compressed texture's pcData is the encoded file, mWidth bytes long, typed as texels.
                std::string bytes( static_cast<std::size_t>( texture.mWidth ), '\0' );
                std::memcpy( bytes.data(), texture.pcData, bytes.size() );
                return Common::MakeSuccess( std::move( bytes ) );
            }

            // aiTexel is B,G,R,A; the PNG wants R,G,B,A.
            const std::size_t    texels = static_cast<std::size_t>( texture.mWidth ) * texture.mHeight;
            std::vector<uint8_t> rgba( texels * 4 );
            for ( std::size_t i = 0; i < texels; ++i )
            {
                const aiTexel& t = texture.pcData[i];
                rgba[i * 4 + 0]  = t.r;
                rgba[i * 4 + 1]  = t.g;
                rgba[i * 4 + 2]  = t.b;
                rgba[i * 4 + 3]  = t.a;
            }
            std::string encoded;
            const auto  append = []( void* context, void* data, int size )
            {
                static_cast<std::string*>( context )->append( static_cast<const char*>( data ),
                                                              static_cast<std::size_t>( size ) );
            };
            const int width  = static_cast<int>( texture.mWidth );
            const int height = static_cast<int>( texture.mHeight );
            if ( stbi_write_png_to_func( append, &encoded, width, height, 4, rgba.data(), width * 4 ) == 0 )
                return Common::MakeError<std::string>( "cannot encode its texels to PNG" );
            return Common::MakeSuccess( std::move( encoded ) );
        }
    } // namespace

    std::filesystem::path EmbeddedTexturePath( const std::filesystem::path& sourcePath, unsigned index,
                                               const aiTexture& texture )
    {
        const std::string ext = IsCompressed( texture ) ? CompressedExtension( texture ) : "png";
        return sourcePath.parent_path() / std::format( "{}_{}.{}", sourcePath.stem().string(), index, ext );
    }

    Common::ResultStr<SourceTextureFile> ResolveSourceTexture( const aiScene&               scene,
                                                               const std::filesystem::path& sourcePath,
                                                               const std::string&           reference )
    {
        const auto [texture, index] = scene.GetEmbeddedTextureAndIndex( reference.c_str() );
        if ( texture == nullptr )
            return Common::MakeSuccess(
                 SourceTextureFile{ FindSourceTexture( sourcePath.parent_path(), reference ), std::nullopt } );

        const std::filesystem::path out =
             EmbeddedTexturePath( sourcePath, static_cast<unsigned>( index ), *texture );
        if ( TextureSourceFormatRank( out.extension().string() ) == kTextureSourceExtensionCount )
            return Common::MakeError<SourceTextureFile>(
                 std::format( "[Import][Tex] '{}': embedded texture '{}' is '{}', which no texture importer reads",
                              sourcePath.generic_string(), reference, out.extension().string() ) );

        const auto bytes = EncodedBytes( *texture );
        if ( !bytes.IsSuccess() )
            return Common::MakeError<SourceTextureFile>(
                 std::format( "[Import][Tex] '{}': embedded texture '{}': {}", sourcePath.generic_string(),
                              reference, bytes.GetError() ) );
        const auto written = WriteDerivedTexture( bytes.GetValue(), out );
        if ( !written.IsSuccess() )
            return Common::MakeError<SourceTextureFile>(
                 std::format( "[Import][Tex] '{}': embedded texture '{}': {}", sourcePath.generic_string(),
                              reference, written.GetError() ) );
        return Common::MakeSuccess( SourceTextureFile{ out, written.GetValue() } );
    }
} // namespace Desert::Editor
