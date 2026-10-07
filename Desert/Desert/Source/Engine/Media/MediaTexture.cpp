#include "MediaTexture.hpp"

#include <Engine/Graphic/GpuBatch.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Media/MediaPlayer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

#include <format>

namespace Desert::Media
{
    namespace
    {
        constexpr uint32_t kMediaYuvGroupSize = 16; // LocalSize of MediaYuvToRgb.shader
        constexpr size_t   kMaxInFlight = 3; // conversions pending on the GPU before the oldest is waited for

        // Must match MediaYuvToRgb.shader's PushConstants block.
        struct MediaYuvPush
        {
            float Kr, Kb, SampleScale, FullRange;
            float Unused0, Unused1, Unused2, LumaOnly;
        };
    } // namespace

    MediaColorCoefficients CoefficientsFor( MediaColorMatrix matrix )
    {
        switch ( matrix )
        {
            case MediaColorMatrix::BT601:
                return { 0.299f, 0.114f };
            case MediaColorMatrix::BT709:
                return { 0.2126f, 0.0722f };
            case MediaColorMatrix::BT2020:
                return { 0.2627f, 0.0593f };
        }
        return { 0.2126f, 0.0722f };
    }

    MediaTexture::MediaTexture()  = default;
    MediaTexture::~MediaTexture() = default;

    std::string MediaTexture::Update( const MediaPlayer& player )
    {
        const VideoFrame* frame = player.GetCurrentFrame();
        if ( frame == nullptr )
            return {};
        if ( m_HasSerial && player.FrameSerial() == m_SeenSerial )
            return {};
        std::string error = Upload( *frame );
        if ( error.empty() )
        {
            // The serial is recorded only when the picture reached the image: a refused upload is retried
            // on the next Update instead of being remembered as shown.
            m_SeenSerial = player.FrameSerial();
            m_HasSerial  = true;
        }
        return error;
    }

    std::string MediaTexture::Prepare( const VideoFrame& frame )
    {
        if ( frame.Width == 0 || frame.Height == 0 )
            return "a video frame with no size";

        if ( !m_Pipeline )
        {
            auto* shaders = Runtime::ResourceRegistry::GetShaderService();
            if ( shaders == nullptr )
                return "no shader service";
            auto shader = shaders->GetByName( "MediaYuvToRgb" );
            if ( !shader )
                return "the shader MediaYuvToRgb is not registered";
            auto built = Graphic::ComputePipeline::Create( { .Shader = shader, .DebugName = "MediaYuvToRgb" } );
            if ( !built )
                return std::format( "the YUV->RGB pipeline was not built: {}", built.GetError() );
            m_Pipeline = built.GetValue();
        }

        const bool sameShape = m_Output && frame.Width == m_Width && frame.Height == m_Height &&
                               frame.BitDepth == m_BitDepth && frame.Chroma == m_Chroma;
        if ( sameShape )
            return {};

        // A new shape (the first frame, or a stream that changed size mid-way): the output and the planes
        // are made once for it and then written in place every frame.
        const auto planeFormat =
             frame.BitDepth > 8 ? Core::Formats::ImageFormat::R16_UNORM : Core::Formats::ImageFormat::R8_UNORM;
        const uint32_t planeCount = frame.Chroma == MediaChroma::I400 ? 1u : 3u;
        for ( uint32_t i = 0; i < 3; ++i )
        {
            m_Planes[i].reset();
            if ( i >= planeCount )
                continue;
            if ( frame.PlaneWidth[i] == 0 || frame.PlaneHeight[i] == 0 )
                return std::format( "video plane {} has no size", i );
            const Core::Formats::Image2DSpecification spec = {
                 .Tag        = std::format( "MediaPlane{}", i ),
                 .Width      = frame.PlaneWidth[i],
                 .Height     = frame.PlaneHeight[i],
                 .Format     = planeFormat,
                 .Mips       = 1u,
                 .Data       = Core::Formats::EmptyPixelData{},
                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                 .Properties = Core::Formats::Sample,
                 .MipLevels  = {},
            };
            m_Planes[i] = Graphic::Image2D::Create( spec );
            if ( !m_Planes[i] )
                return std::format( "video plane {} image was not created", i );
        }

        const Core::Formats::Image2DSpecification outputSpec = {
             .Tag        = "MediaTexture",
             .Width      = frame.Width,
             .Height     = frame.Height,
             .Format     = Core::Formats::ImageFormat::RGBA8F,
             .Mips       = 1u,
             .Data       = Core::Formats::EmptyPixelData{},
             .Usage      = Core::Formats::Image2DUsage::Image2D,
             .Properties = Core::Formats::Storage | Core::Formats::Sample,
             .MipLevels  = {},
        };
        m_Output = Graphic::Image2D::Create( outputSpec );
        if ( !m_Output )
            return "the video output image was not created";

        m_Width    = frame.Width;
        m_Height   = frame.Height;
        m_BitDepth = frame.BitDepth;
        m_Chroma   = frame.Chroma;
        return {};
    }

    std::string MediaTexture::Upload( const VideoFrame& frame )
    {
        // Finished batches let go of their staging copies; a GPU kMaxInFlight conversions behind is waited for.
        while ( !m_InFlight.empty() && m_InFlight.front()->IsComplete() )
            m_InFlight.pop_front();
        while ( m_InFlight.size() >= kMaxInFlight )
        {
            m_InFlight.front()->Wait();
            m_InFlight.pop_front();
        }
        if ( std::string error = Prepare( frame ); !error.empty() )
            return error;

        auto begun = Graphic::GpuBatch::Begin();
        if ( !begun )
            return std::format( "the video conversion batch was refused: {}", begun.GetError() );
        std::unique_ptr<Graphic::GpuBatch> batch = begun.ExtractValue();
        for ( uint32_t i = 0; i < 3; ++i )
        {
            if ( !m_Planes[i] )
                continue;
            const auto uploaded = m_Planes[i]->RecordSetData(
                 *batch,
                 Core::Formats::ImagePixelData( frame.Planes[i].UploadBytes() ) ); // copied once, into staging
            if ( !uploaded.IsSuccess() )
                return std::format( "video plane {} did not reach the GPU: {}", i, uploaded.GetError() );
        }

        const MediaColorCoefficients k        = CoefficientsFor( frame.Matrix );
        const auto                   maxCode  = static_cast<float>( ( 1u << frame.BitDepth ) - 1u );
        const bool                   lumaOnly = !m_Planes[1];
        const MediaYuvPush           push{ k.Kr,
                                 k.Kb,
                                 frame.BitDepth > 8 ? 65535.0f / maxCode : 1.0f,
                                 frame.FullRange ? 1.0f : 0.0f,
                                 0.0f,
                                 0.0f,
                                 0.0f,
                                 lumaOnly ? 1.0f : 0.0f };

        Graphic::Image2D* y = m_Planes[0].get();
        // Outside the frame graph (its own batch): the pipeline records each plane's transition to the
        // declared access itself.
        constexpr auto kPlaneRead = Graphic::RDG::Access::SampledCompute;
        m_Pipeline->SetInput( 0, y, kPlaneRead, Graphic::RDG::SubresourceRange::All() );
        m_Pipeline->SetInput( 1, lumaOnly ? y : m_Planes[1].get(), kPlaneRead, Graphic::RDG::SubresourceRange::All() );
        m_Pipeline->SetInput( 2, lumaOnly ? y : m_Planes[2].get(), kPlaneRead, Graphic::RDG::SubresourceRange::All() );
        m_Pipeline->SetOutput( 3, m_Output.get(), 0 );
        m_Pipeline->SetPushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
        m_Pipeline->Record( *batch, ( m_Width + kMediaYuvGroupSize - 1 ) / kMediaYuvGroupSize,
                            ( m_Height + kMediaYuvGroupSize - 1 ) / kMediaYuvGroupSize, 1u );
        batch->Retain( m_Pipeline );
        batch->Retain( m_Output );
        for ( const auto& plane : m_Planes )
            if ( plane )
                batch->Retain( plane );
        if ( const auto submitted = batch->Submit(); !submitted )
            return std::format( "the video conversion batch did not submit: {}", submitted.GetError() );
        m_InFlight.push_back( std::move( batch ) );
        return {};
    }
} // namespace Desert::Media
