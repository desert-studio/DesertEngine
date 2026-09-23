#pragma once

#include <cstdint>
#include <vector>

namespace Desert::Core::IO
{
    class ImageReader
    {
    public:
        struct ImageReaderHDRInfo
        {
            uint32_t           Width, Height, Channels;
            std::vector<float> Data;
        };

        struct ImageReaderInfo
        {
            uint32_t                   Width, Height, Channels;
            std::vector<unsigned char> Data;
        };

        // A decoded animated GIF: all frames stacked in Data (FrameCount * Width * Height * 4, RGBA8) plus
        // each frame's on-screen duration. FrameCount == 0 on failure (or a non-GIF file).
        struct ImageReaderGifInfo
        {
            uint32_t                   Width = 0, Height = 0, FrameCount = 0;
            std::vector<unsigned char> Data;     // FrameCount consecutive RGBA8 frames
            std::vector<int>           DelaysMs; // per-frame duration in milliseconds (>= 1)
        };

        static bool                     IsHDR( const Common::Filepath& filepath );
        static const ImageReaderHDRInfo ReadHDR( const Common::Filepath& filepath );
        static const ImageReaderInfo    Read( const Common::Filepath& filepath, bool alpha = true );
        static const ImageReaderGifInfo ReadGif( const Common::Filepath& filepath );
    };
} // namespace Desert::Core::IO