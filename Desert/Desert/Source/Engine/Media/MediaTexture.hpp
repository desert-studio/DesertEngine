#pragma once

#include <Engine/Media/MediaCodecs.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace Desert::Graphic
{
    class Image2D;
    class ComputePipeline;
} // namespace Desert::Graphic

namespace Desert::Media
{
    class MediaPlayer;

    // A MediaPlayer's picture on the GPU (UE's UMediaTexture): one STABLE RGBA8 image whose pointer survives
    // every frame, so a UI brush or a material samples the same Image2D for the life of the clip.
    //
    // The decoder's planes are uploaded as they come (R8 for 8-bit, R16 for 10/12-bit) and a compute pass
    // (Programs/Media/MediaYuvToRgb.shader) converts them with the frame's own matrix and range. There is no
    // CPU colour conversion anywhere in the engine: this is the one path from a VideoFrame to pixels.
    class MediaTexture
    {
    public:
        MediaTexture();
        ~MediaTexture();
        MediaTexture( const MediaTexture& )            = delete;
        MediaTexture& operator=( const MediaTexture& ) = delete;

        // Uploads and converts the player's current frame when its FrameSerial moved since the last call.
        // Call outside a render pass (it submits its own compute work). Empty on success or when nothing
        // changed; the error otherwise (the previous picture stays in the image).
        std::string Update( const MediaPlayer& player );

        // Converts `frame` unconditionally (the serial-free entry Update is built on).
        std::string Upload( const VideoFrame& frame );

        // nullptr until the first frame has been converted.
        Graphic::Image2D* GetImage() const
        {
            return m_Output.get();
        }
        uint32_t GetWidth() const
        {
            return m_Width;
        }
        uint32_t GetHeight() const
        {
            return m_Height;
        }

    private:
        std::string Prepare( const VideoFrame& frame );

        std::shared_ptr<Graphic::Image2D>                m_Output;
        std::array<std::shared_ptr<Graphic::Image2D>, 3> m_Planes;
        std::shared_ptr<Graphic::ComputePipeline>        m_Pipeline;
        uint32_t                                         m_Width       = 0;
        uint32_t                                         m_Height      = 0;
        uint32_t                                         m_BitDepth    = 0;
        MediaChroma                                      m_Chroma      = MediaChroma::I420;
        uint64_t                                         m_SeenSerial  = 0;
        bool                                             m_HasSerial   = false;
    };

    // Kr and Kb of the matrix a frame names (Rec.601/709/2020): the two numbers the conversion is built from.
    struct MediaColorCoefficients
    {
        float Kr = 0.0f;
        float Kb = 0.0f;
    };
    MediaColorCoefficients CoefficientsFor( MediaColorMatrix matrix );
} // namespace Desert::Media
