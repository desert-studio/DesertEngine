#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <array>
#include <format>
#include <map>
#include <tuple>

namespace Desert::Graphic::System
{
    // THE BUFFER VIEWS DRAWN AFTER THE TEMPORAL RESOLVE (UE: the buffer visualization runs as a post pass over the
    // finished frame, never inside the lighting). Each writes one frame transient as a colour over the view's post
    // input, so the lit scene colour - which the view's scene colour history copies and the next frame's screen
    // traces (SSR, SSGI) read back - is still the LIT scene while the mode is on. A view mode drawn by the
    // lighting pass instead would hand that history a picture of the mode, and a trace that returns last frame's
    // radiance would then return the picture: the indirect-only view, drawn by the composite, fed SSGI its own
    // black output and showed nothing at all.
    //
    //   * Velocity (DeferredDebugMode::Velocity, UE's "Velocity"): "Debug: Velocity" samples the view's velocity
    //     (FrameTransients::Velocity, NDC current - previous at the render extent) - hue = direction, brightness =
    //     speed in pixels, black where the pixel is still. What is shown is the motion TAA reprojected with.
    //   * Indirect (DeferredDebugMode::GI): "Debug: Indirect" samples the denoised diffuse indirect the lighting
    //     composed this frame (FrameTransients::DiffuseIndirect, SSGI or RSM-GI) - the incident radiance the
    //     ambient multiplies by the albedo, as it was before the composite.
    //
    // The post input's format and sample count are the view's (the MSAA scene target without a temporal method,
    // the single-sample overlay colour with one), so a pipeline is made per (view, format, samples) on first use.
    class BufferViewRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        enum class View : int
        {
            Velocity = 0,
            Indirect = 1,
        };

        virtual Common::BoolResultStr Initialize() override
        {
            // Loaded by literal name, one call each: the package's shader census reads the names off these calls.
            const auto& shaders                              = Runtime::ResourceRegistry::GetShaderService();
            m_Shaders[static_cast<size_t>( View::Velocity )] = shaders->GetByName( "VelocityView" );
            m_Shaders[static_cast<size_t>( View::Indirect )] = shaders->GetByName( "IndirectView" );
            for ( size_t i = 0; i < kViews.size(); ++i )
                if ( !m_Shaders[i] )
                    return Common::MakeError( std::format( "{} shader not found", kViews[i].Shader ) );
            return BOOLSUCCESS;
        }

        // The pipeline of @p view that draws into a target of @p format at @p samples, made the first time it is
        // asked for.
        [[nodiscard]] Common::BoolResultStr Prepare( const View view, const Core::Formats::ImageFormat format,
                                                     const uint32_t samples )
        {
            const size_t index = static_cast<size_t>( view );
            m_Current          = nullptr;
            m_CurrentView      = view;
            if ( !m_Shaders[index] )
                return Common::MakeError( std::format( "{}: the {} shader is not loaded", kViews[index].PassName,
                                                       kViews[index].Shader ) );
            const auto key = std::make_tuple( static_cast<int>( view ), static_cast<int>( format ), samples );
            if ( const auto it = m_Pipelines.find( key ); it != m_Pipelines.end() )
            {
                m_Current = it->second;
                return BOOLSUCCESS;
            }
            GraphicsPipelineSpecification spec;
            spec.DebugName = kViews[index].Shader;
            spec.TargetLayout =
                 RenderTargetLayout{ .ColorFormats = { format }, .DepthFormat = std::nullopt, .Samples = samples };
            spec.Shader            = m_Shaders[index];
            spec.DepthTestEnabled  = false;
            spec.DepthWriteEnabled = false;
            const auto pipeline    = Graphic::GraphicsPipeline::Create( spec );
            if ( !pipeline )
                return Common::MakeError( pipeline.GetError() );
            m_Current = m_Pipelines.emplace( key, pipeline.GetValue() ).first->second;
            return BOOLSUCCESS;
        }

        // The pass name of @p view ("Debug: Velocity" / "Debug: Indirect").
        [[nodiscard]] static const char* PassName( const View view )
        {
            return kViews[static_cast<size_t>( view )].PassName;
        }

        // SETUP of the prepared view's pass: block 0 - the view's one input (u_Velocity / u_Indirect).
        void DeclareBindings( RDG::PassBuilder& pass, RDG::TextureRef input ) const
        {
            if ( !m_Current )
                return;
            const size_t index = static_cast<size_t>( m_CurrentView );
            pass.Bindings( m_BindingLayouts[index].Get( m_Shaders[index] ), RDG::OtherRouteFill{} )
                 .Sampled( kViews[index].Input, input, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                           RDG::SamplerDesc::LinearClamp() );
        }

        [[nodiscard]] Common::BoolResultStr Record( const RDG::PassContext& context ) const
        {
            if ( !m_Current )
                return Common::MakeError( "Debug buffer view: no pipeline was prepared for the target" );
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            return Renderer::GetInstance().DrawFullscreen( bindings, *m_Current, nullptr );
        }

    private:
        struct ViewDesc
        {
            const char* Shader;
            const char* Input;
            const char* PassName;
        };
        static constexpr std::array<ViewDesc, 2> kViews{ {
             { "VelocityView", "u_Velocity", "Debug: Velocity" },
             { "IndirectView", "u_Indirect", "Debug: Indirect" },
        } };

        std::array<std::shared_ptr<Shader>, 2>          m_Shaders;
        mutable std::array<ShaderBindingLayoutCache, 2> m_BindingLayouts; // one per shader of kViews
        std::map<std::tuple<int, int, uint32_t>, std::shared_ptr<GraphicsPipeline>> m_Pipelines;
        std::shared_ptr<GraphicsPipeline>                                           m_Current;
        View                                                                        m_CurrentView = View::Velocity;
    };
} // namespace Desert::Graphic::System
