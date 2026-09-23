#pragma once

#include <cstdint>
#include <vector>

namespace Desert::Core::IO
{
    // THE ENGINE'S LAST SOURCE-IMAGE DECODER, and it is a GIF decoder only on purpose. `Read`, `ReadHDR`
    // and `IsHDR` lived here until the sky panorama started reading its cooked `.tex`: every still image
    // reaches the runtime through the container `TextureImporter` writes, and a PNG/HDR decode at run time
    // is a defect, not a fallback. `ReadGif` stays for exactly one caller, AnimatedImageService, until a
    // GIF is cooked to video at import (its own task; the lead's decision 2026-09-23 is that GIF becomes a
    // SOURCE format for the video path, not a flipbook in the texture container).
    // Desert/Tests/Engine/RuntimeSourceDecoders pins that exception by name.
    class ImageReader
    {
    public:
        // A decoded animated GIF: all frames stacked in Data (FrameCount * Width * Height * 4, RGBA8) plus
        // each frame's on-screen duration. FrameCount == 0 on failure (or a non-GIF file).
        struct ImageReaderGifInfo
        {
            uint32_t                   Width = 0, Height = 0, FrameCount = 0;
            std::vector<unsigned char> Data;     // FrameCount consecutive RGBA8 frames
            std::vector<int>           DelaysMs; // per-frame duration in milliseconds (>= 1)
        };

        static const ImageReaderGifInfo ReadGif( const Common::Filepath& filepath );
    };
} // namespace Desert::Core::IO