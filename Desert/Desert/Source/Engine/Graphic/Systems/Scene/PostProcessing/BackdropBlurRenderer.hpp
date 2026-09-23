#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <memory>

namespace Desert::Graphic::System
{
    // A blurred snapshot of the scene colour, for UI "glass" (backdrop blur).
    //
    // WHY a snapshot at all: the UI canvas is drawn INTO the HDR scene target as a load overlay, and a
    // shader may not sample the attachment it is writing (feedback loop) — the same reason the glass
    // MESH path snapshots the scene before drawing refractive surfaces.
    //
    // WHY a mip chain instead of a blur kernel: the bloom downsample compute already implements a good
    // 13-tap progressive filter; run with the bright-pass OFF it is exactly a blur pyramid of the whole
    // image. Mip 0 is a mild blur and every further level roughly doubles it, so a UI element picks a
    // LOD instead of each element paying for its own kernel. No new shader, no new blur to maintain.
    //
    // Runs right before the UI phase (compute only, outside any render pass) and ONLY when the UI
    // actually asked for it — see SceneRenderer::SetBackdropBlurNeeded.
    class BackdropBlurRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        Common::BoolResultStr Initialize() override
        {
            const auto& target = m_TargetFramebuffer.lock();
            if ( !target )
                return Common::MakeError( "BackdropBlurRenderer: target framebuffer is not available" );

            if ( !CreateImage( target->GetFramebufferWidth(), target->GetFramebufferHeight() ) )
                return Common::MakeError( "BackdropBlurRenderer: failed to create the backdrop image" );

            const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "BloomDownsample" );
            if ( !shader )
                return Common::MakeError( "BackdropBlurRenderer: missing compute shader 'BloomDownsample'" );

            const auto built =
                 ComputePipeline::Create( { .Shader = shader, .DebugName = "BackdropBlurDownsample" } );
            if ( !built )
                return Common::MakeError( built.GetError() );
            m_DownsamplePipeline = built.GetValue();

            return BOOLSUCCESS;
        }

        void RegisterPasses( RenderGraphBuilder& ) override
        {
        }

        void Resize( uint32_t width, uint32_t height )
        {
            if ( width == 0 || height == 0 )
                return;
            // Image2D has no in-place resize; recreate (SceneRenderer::Resize already idled the GPU).
            CreateImage( width, height );
        }

        // The blur pyramid. Null until Execute() has run once — callers must handle that (the UI falls
        // back to a plain tinted panel).
        const std::shared_ptr<Image2D>& GetImage() const
        {
            return m_Image;
        }

        void Execute()
        {
            const auto& scene = m_TargetFramebuffer.lock();
            if ( !scene || !m_Image || !m_DownsamplePipeline )
                return;

            Image2D* sceneColor = scene->GetColorAttachmentImage().get();
            if ( !sceneColor )
                return;

            const uint32_t sceneW = scene->GetFramebufferWidth();
            const uint32_t sceneH = scene->GetFramebufferHeight();
            const uint32_t bw     = m_Image->GetWidth();
            const uint32_t bh     = m_Image->GetHeight();

            auto& renderer = Renderer::GetInstance();
            renderer.ComputeImageBeginWrite( m_Image.get() );

            for ( uint32_t i = 0; i < m_MipLevels; ++i )
            {
                const bool     first  = ( i == 0 );
                Image2D*       src    = first ? sceneColor : m_Image.get();
                const uint32_t srcMip = first ? 0u : i - 1;
                const uint32_t srcW   = first ? sceneW : MipSize( bw, i - 1 );
                const uint32_t srcH   = first ? sceneH : MipSize( bh, i - 1 );

                // FirstPass = 0 everywhere: the Karis average + bright-pass belong to bloom, not to a
                // backdrop, which must keep the scene's own colours.
                const DownsamplePush push{
                     glm::vec2( 1.0f / static_cast<float>( srcW ), 1.0f / static_cast<float>( srcH ) ),
                     static_cast<int32_t>( srcMip ), 0, 0.0f };

                m_DownsamplePipeline->SetInput( 0, src );
                m_DownsamplePipeline->SetOutput( 1, m_Image.get(), i );
                m_DownsamplePipeline->SetPushConstants( &push, sizeof( push ) );
                renderer.DispatchComputeInFrame( m_DownsamplePipeline.get(), GroupCount( MipSize( bw, i ) ),
                                                 GroupCount( MipSize( bh, i ) ), 1 );
            }

            renderer.ComputeImageEndWrite( m_Image.get() );
        }

        // Highest LOD a caller may ask for (the chain's coarsest level).
        uint32_t GetMaxLod() const
        {
            return m_MipLevels > 0 ? m_MipLevels - 1 : 0;
        }

    private:
        // Must match BloomDownsample's push block exactly.
        struct DownsamplePush
        {
            glm::vec2 SrcTexelSize;
            int32_t   SrcMip;
            int32_t   FirstPass;
            float     Threshold;
        };

        static constexpr uint32_t kMaxMips   = 5;  // half-res mip 0 => the coarsest is ~1/32 of the screen
        static constexpr uint32_t kGroupSize = 16; // must match the shader's local_size_*

        static uint32_t MipSize( uint32_t base, uint32_t mip )
        {
            return std::max( 1u, base >> mip );
        }
        static uint32_t GroupCount( uint32_t dim )
        {
            return ( dim + kGroupSize - 1 ) / kGroupSize;
        }

        bool CreateImage( uint32_t width, uint32_t height )
        {
            const uint32_t bw = std::max( 1u, width / 2 );
            const uint32_t bh = std::max( 1u, height / 2 );
            m_MipLevels       = std::min( kMaxMips, Utils::CalculateMipCount( bw, bh ) );

            const Core::Formats::Image2DSpecification spec = {
                 .Tag        = "BackdropBlur",
                 .Width      = bw,
                 .Height     = bh,
                 .Format     = Core::Formats::ImageFormat::RGBA32F,
                 .Mips       = m_MipLevels,
                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                 .Properties = Core::Formats::Storage | Core::Formats::Sample,
            };

            m_Image = Image2D::Create( spec );
            return m_Image != nullptr;
        }

        std::shared_ptr<Image2D>         m_Image;
        std::shared_ptr<ComputePipeline> m_DownsamplePipeline;
        uint32_t                         m_MipLevels = 1;
    };
} // namespace Desert::Graphic::System
