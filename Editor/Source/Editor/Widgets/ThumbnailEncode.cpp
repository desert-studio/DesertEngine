#include <Editor/Widgets/ThumbnailEncode.hpp>

#include <filesystem>
#include <system_error>

// STB_IMAGE_WRITE_IMPLEMENTATION is compiled into stb_image.cpp; declarations only here.
#include <stb_image/stb_image_write.h>

namespace Desert::Editor::ThumbnailEncode
{
    Common::ResultStr<std::vector<uint8_t>> Downscale( const std::vector<uint8_t>& rgba, uint32_t from, uint32_t to )
    {
        if ( to == 0 || from % to != 0 )
            return Common::MakeFormattedError<std::vector<uint8_t>>(
                 "Downscale: {} px is not a whole multiple of {} px", from, to );
        if ( rgba.size() != static_cast<std::size_t>( from ) * from * 4 )
            return Common::MakeFormattedError<std::vector<uint8_t>>( "Downscale: {} bytes, expected {} ({}x{} RGBA8)",
                                                                     rgba.size(),
                                                                     static_cast<std::size_t>( from ) * from * 4,
                                                                     from, from );

        const uint32_t       f = from / to;
        std::vector<uint8_t> out( static_cast<std::size_t>( to ) * to * 4 );
        for ( uint32_t y = 0; y < to; ++y )
            for ( uint32_t x = 0; x < to; ++x )
                for ( uint32_t c = 0; c < 4; ++c )
                {
                    uint32_t sum = 0;
                    for ( uint32_t sy = 0; sy < f; ++sy )
                        for ( uint32_t sx = 0; sx < f; ++sx )
                            sum += rgba[( ( ( ( ( y * f ) + sy ) * from ) + ( x * f ) + sx ) * 4 ) + c];
                    out[( ( ( y * to ) + x ) * 4 ) + c] = static_cast<uint8_t>( sum / ( f * f ) );
                }
        return Common::MakeSuccess( std::move( out ) );
    }

    Common::BoolResultStr WritePng( const std::vector<uint8_t>& rgba, uint32_t side, const std::string& png )
    {
        if ( rgba.size() != static_cast<std::size_t>( side ) * side * 4 )
            return Common::MakeFormattedError<bool>( "WritePng '{}': {} bytes, expected {} ({}x{} RGBA8)", png,
                                                     rgba.size(), static_cast<std::size_t>( side ) * side * 4, side,
                                                     side );
        std::error_code ec;
        std::filesystem::create_directories( std::filesystem::path( png ).parent_path(), ec );

        // stbi_write_png streams straight into its destination; a reader polling the real name would see a
        // truncated file. Written aside, then renamed — the rename is the moment the picture exists.
        stbi_flip_vertically_on_write( 0 ); // the readback is already upright
        const std::string temp   = png + ".part";
        const int         stride = static_cast<int>( side ) * 4;
        if ( stbi_write_png( temp.c_str(), static_cast<int>( side ), static_cast<int>( side ), 4, rgba.data(),
                             stride ) == 0 )
            return Common::MakeFormattedError<bool>( "stbi_write_png refused to write '{}' ({}x{} RGBA8)", temp,
                                                     side, side );
        std::filesystem::rename( temp, png, ec );
        if ( ec )
        {
            std::error_code cleanup;
            std::filesystem::remove( temp, cleanup );
            return Common::MakeFormattedError<bool>( "'{}' was written but could not be moved into place from '{}': {}",
                                                     png, temp, ec.message() );
        }
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Editor::ThumbnailEncode
