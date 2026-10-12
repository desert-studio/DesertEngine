#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>

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
    // RDG-A2 P5: the pyramid is a TRANSIENT of the frame graph (SceneRenderer::AddFrameBackdropBlur creates it
    // from GetPyramidDesc, sized from THIS frame's view, so there is no image to resize and nothing that outlives
    // the frame). The UI glass reads it as FrameTransients::BackdropBlur through the extension pass context it
    // is handed and binds it by name (u_Backdrop) — no Image2D of it crosses into a pass body.
    class BackdropBlurRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        Common::BoolResultStr Initialize() override
        {
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

        // The pyramid of this frame: half the view, mip-capped so the coarsest level is ~1/32 of the screen.
        // nullopt while the view or the pipeline is missing (no node is added then and the glass reads nothing).
        [[nodiscard]] std::optional<RDG::TextureDesc> GetPyramidDesc() const
        {
            const auto scene = m_TargetFramebuffer.lock();
            if ( !scene || !scene->GetColorAttachmentImage() || !m_DownsamplePipeline )
                return std::nullopt;
            const uint32_t bw = std::max( 1u, scene->GetFramebufferWidth() / 2 );
            const uint32_t bh = std::max( 1u, scene->GetFramebufferHeight() / 2 );
            return RDG::TextureDesc{ .Size   = { .Width = bw, .Height = bh },
                                     .Format = Core::Formats::ImageFormat::RGBA32F,
                                     .Mips   = std::min( kMaxMips, Utils::CalculateMipCount( bw, bh ) ) };
        }

        // Scene colour -> pyramid mip 0, then mip - 1 -> mip, bright-pass off (a plain blur pyramid). The setup
        // declares block 0 (DeclareDownsampleBindings); RecordDownsample dispatches from it.
        void DeclareDownsampleBindings( RDG::PassBuilder& pass, RDG::TextureRef sceneColor,
                                        RDG::TextureRef pyramid, uint32_t mip ) const
        {
            if ( !m_DownsamplePipeline )
                return; // RecordDownsample refuses by name
            auto block = pass.Bindings( m_DownsampleLayout.Get( m_DownsamplePipeline->GetSpecification().Shader ),
                                        Renderer::GetPipelineRouteFill( *m_DownsamplePipeline ) );
            if ( mip == 0 )
            {
                block.Sampled( "u_Source", sceneColor, RDG::Access::SampledCompute, RDG::SubresourceRange::All(),
                               RDG::SamplerDesc::LinearClamp() );
            }
            else
            {
                block.Sampled( "u_Source", pyramid, RDG::Access::SampledCompute,
                               RDG::SubresourceRange::Mip( mip - 1 ), RDG::SamplerDesc::LinearClamp() );
            }
            block.Storage( "u_Output", pyramid, RDG::Access::StorageWrite, mip )
                 .PushConstantBytes( static_cast<uint32_t>( sizeof( DownsamplePush ) ) );
        }

        [[nodiscard]] Common::BoolResultStr RecordDownsample( const RDG::PassContext& context,
                                                              const RDG::TextureDesc& desc, uint32_t mip ) const
        {
            if ( !m_DownsamplePipeline )
                return Common::MakeError( "BackdropBlurRenderer: the downsample pipeline is not initialised" );
            const bool     first = ( mip == 0 );
            const uint32_t bw    = desc.Size.Width;
            const uint32_t bh    = desc.Size.Height;
            uint32_t       srcW  = 0;
            uint32_t       srcH  = 0;
            if ( first )
            {
                const auto scene = m_TargetFramebuffer.lock();
                if ( !scene )
                    return Common::MakeError( "BackdropBlurRenderer: the scene framebuffer is gone" );
                srcW = scene->GetFramebufferWidth();
                srcH = scene->GetFramebufferHeight();
            }
            else
            {
                srcW = MipSize( bw, mip - 1 );
                srcH = MipSize( bh, mip - 1 );
            }

            const DownsamplePush push{
                 glm::vec2( 1.0f / static_cast<float>( srcW ), 1.0f / static_cast<float>( srcH ) ), 0, 0.0f };

            RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            bindings.PushConstants( &push, sizeof( push ) );
            return Renderer::GetInstance().DispatchCompute( bindings, *m_DownsamplePipeline,
                                                            GroupCount( MipSize( bw, mip ) ),
                                                            GroupCount( MipSize( bh, mip ) ), 1 );
        }

    private:
        struct DownsamplePush
        {
            glm::vec2 SrcTexelSize;
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

        std::shared_ptr<ComputePipeline> m_DownsamplePipeline;
        mutable ShaderBindingLayoutCache m_DownsampleLayout; // the downsample shader's layout, kept between frames
    };
} // namespace Desert::Graphic::System
