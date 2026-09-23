#include <Engine/Core/IO/ImageReader.hpp>

#include <Common/Utilities/FileSystem.hpp>

#include <stb_image/stb_image.h>

#include <cstdint>
#include <cstring>

namespace Desert::Core::IO
{
    // The FILE BYTES are read through Common::Utils::FileSystem (disk first, then a mounted .dpak) and
    // handed to stb's *_from_memory loader — stb never touches paths itself.

    const ImageReader::ImageReaderGifInfo ImageReader::ReadGif( const Common::Filepath& filepath )
    {
        ImageReaderGifInfo returnData;

        const auto read = Common::Utils::FileSystem::ReadByteFileContent( filepath );
        if ( !read )
            return returnData; // FrameCount stays 0 -> caller-visible failure
        const auto& bytes = read.GetValue();

        // stb decodes every frame at once: `data` is `frames` stacked RGBA8 images and `delays` is a
        // per-frame duration array (already milliseconds — stb stores 10 * the GIF's 1/100s value).
        int*     delays = nullptr;
        int      width = 0, height = 0, frames = 0, channels = 0;
        stbi_uc* data = stbi_load_gif_from_memory( bytes.data(), static_cast<int>( bytes.size() ), &delays, &width,
                                                   &height, &frames, &channels, STBI_rgb_alpha );
        if ( !data || frames <= 0 || width <= 0 || height <= 0 )
        {
            if ( data )
                stbi_image_free( data );
            if ( delays )
                stbi_image_free( delays );
            return returnData; // FrameCount stays 0 -> caller-visible failure
        }

        returnData.Width      = static_cast<uint32_t>( width );
        returnData.Height     = static_cast<uint32_t>( height );
        returnData.FrameCount = static_cast<uint32_t>( frames );

        const size_t total = static_cast<size_t>( width ) * height * 4 * frames;
        returnData.Data.resize( total );
        memcpy( returnData.Data.data(), data, total );

        returnData.DelaysMs.resize( frames );
        for ( int i = 0; i < frames; ++i )
        {
            const int d            = delays ? delays[i] : 0;
            returnData.DelaysMs[i] = d > 0 ? d : 100; // 0-delay frames default to 100 ms (10 fps)
        }

        stbi_image_free( data );
        if ( delays )
            stbi_image_free( delays );

        return returnData;
    }

} // namespace Desert::Core::IO
