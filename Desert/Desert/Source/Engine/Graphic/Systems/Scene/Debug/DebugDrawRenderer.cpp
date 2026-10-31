#include <Engine/Graphic/Systems/Scene/Debug/DebugDrawRenderer.hpp>

#if DESERT_DEV_INSTRUMENTS
#include <Common/Core/Logger.hpp>
#include <Engine/Core/DebugDraw.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/ViewTargetLayouts.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Render2DCore/Text/BakedFont.hpp>
#include <Render2DCore/Text/Utf8.hpp>

#include <algorithm>
#include <format>
#include <vector>

namespace Desert::Graphic
{
    namespace
    {
        // The debug font's height in screen pixels at Scale 1 - UE's DrawDebugString uses the engine's small
        // font, about this size on a 1080p viewport.
        constexpr float kStringPixelHeight = 18.0f;
        // The screen panel's background reaches this many pixels past its text on every side.
        constexpr float kPanelPadding = 6.0f;

        // Lays the engine font's glyphs out along a pen: what a world string and a panel's runs share.
        struct TextPen
        {
            Render2D::DrawList2D& List;
            const Runtime::Font&  Font;
            float                 PixelHeight; // the text's height in screen pixels at scale 1

            [[nodiscard]] float ScaleOf( const float scale ) const
            {
                return PixelHeight * scale / Font.Baked.PixelHeight;
            }

            [[nodiscard]] float LineHeight( const float scale ) const
            {
                return Font.Baked.LineHeight() * ScaleOf( scale );
            }

            [[nodiscard]] float Measure( const std::string& text, const float scale ) const
            {
                const float k     = ScaleOf( scale );
                float       width = 0.0f;
                for ( size_t i = 0; i < text.size(); )
                    if ( const auto it = Font.Baked.Glyphs.find( Text::Utf8Next( text, i ) );
                         it != Font.Baked.Glyphs.end() )
                        width += it->second.Advance * k;
                return width;
            }

            // @p topLeft is the top of the line; a one-pixel shadow first (UE bDrawShadow), then the text over it.
            // Returns the pen's advance.
            float Draw( const glm::vec2& topLeft, const std::string& text, const glm::vec4& color,
                        const float scale ) const
            {
                const Text::BakedFont& baked    = Font.Baked;
                const float            k        = ScaleOf( scale );
                const float            baseline = topLeft.y + baked.Ascent * k;
                float                  advance  = 0.0f;
                for ( int pass = 0; pass < 2; ++pass )
                {
                    const glm::vec2 offset = pass == 0 ? glm::vec2( 1.0f, 1.0f ) : glm::vec2( 0.0f );
                    const glm::vec4 tint   = pass == 0 ? glm::vec4( 0.0f, 0.0f, 0.0f, color.a ) : color;
                    float           pen    = topLeft.x;
                    for ( size_t i = 0; i < text.size(); )
                    {
                        const auto it = baked.Glyphs.find( Text::Utf8Next( text, i ) );
                        if ( it == baked.Glyphs.end() )
                            continue;
                        const Text::Glyph& g = it->second;
                        if ( g.Width > 0.0f && g.Height > 0.0f )
                        {
                            const glm::vec2 min( pen + g.OffsetX * k, baseline + g.OffsetY * k );
                            List.AddText( Font.Atlas.get(), min + offset,
                                          min + glm::vec2( g.Width, g.Height ) * k + offset, { g.U0, g.V0 },
                                          { g.U1, g.V1 }, tint );
                        }
                        pen += g.Advance * k;
                    }
                    advance = pen - topLeft.x;
                }
                return advance;
            }
        };

        size_t BatchOf( const Core::DebugDrawLine& line )
        {
            return static_cast<size_t>( line.Depth ) * 2u + ( line.Thickness > 0.0f ? 1u : 0u );
        }

        // A thick segment as two triangles facing the camera: the quad's width runs across both the segment
        // and the eye ray, so it reads as a band from any side (UE FBatchedElements' thick lines).
        void AppendQuad( std::vector<MaterialDebugLine::LineVertex>& out, const Core::DebugDrawLine& line,
                         const glm::vec3& eye )
        {
            const glm::vec3 dir     = line.End - line.Start;
            const glm::vec3 toEye   = eye - ( line.Start + line.End ) * 0.5f;
            glm::vec3       across  = glm::cross( dir, toEye );
            const float     acrossL = glm::length( across );
            if ( acrossL <= 0.0f )
                return; // the segment points at the eye: it covers no area on screen
            across *= line.Thickness * 0.5f / acrossL;
            const glm::vec4 a0( line.Start - across, 1.0f );
            const glm::vec4 a1( line.Start + across, 1.0f );
            const glm::vec4 b0( line.End - across, 1.0f );
            const glm::vec4 b1( line.End + across, 1.0f );
            for ( const glm::vec4& p : { a0, a1, b1, a0, b1, b0 } )
                out.push_back( { p, line.Color } );
        }
    } // namespace

    bool DebugDrawRenderer::HasLines() const
    {
        return m_World != nullptr && !m_World->GetLines().empty();
    }

    bool DebugDrawRenderer::HasStrings() const
    {
        return m_World != nullptr && ( !m_World->GetStrings().empty() || !m_World->GetScreenPanels().empty() );
    }

    bool DebugDrawRenderer::EnsureLinePipelines()
    {
        if ( m_Pipelines[0] )
            return true;
        if ( m_LinePipelinesRefused )
            return false;
        // Asked once: a refusal is logged with its reason and the lines stay undrawn for this view's life,
        // rather than a log line every frame.
        m_LinePipelinesRefused = true;
        const std::shared_ptr<Shader> shader =
             Runtime::ResourceRegistry::GetShaderService()->GetByName( "DebugLine" );
        if ( !shader )
        {
            LOG_ERROR( "[DebugDraw] the debug lines will not draw: the DebugLine shader is not loaded" );
            return false;
        }
        for ( size_t batch = 0; batch < kBatchCount; ++batch )
        {
            const bool                    foreground = batch >= 2;
            const bool                    thick      = ( batch & 1u ) != 0;
            GraphicsPipelineSpecification spec;
            spec.DebugName =
                 std::format( "DebugDraw{}{}", foreground ? "Foreground" : "World", thick ? "Quads" : "Lines" );
            spec.Shader       = shader;
            spec.TargetLayout = SceneTargetLayout();
            spec.Topology     = thick ? PrimitiveTopology::Triangles : PrimitiveTopology::Lines;
            spec.LineWidth    = 1.0f;
            // World: tested against the scene, never written (an overlay, as the bounding boxes). Foreground:
            // over everything.
            spec.DepthTestEnabled  = !foreground;
            spec.DepthWriteEnabled = false;
            spec.DepthCompareOp    = DepthCompare::CloserOrEqual;
            spec.CullMode          = CullMode::None;
            const auto pipeline    = GraphicsPipeline::Create( spec );
            if ( !pipeline )
            {
                LOG_ERROR( "[DebugDraw] the debug lines will not draw: pipeline '{}': {}", spec.DebugName,
                           pipeline.GetError() );
                m_Pipelines = {};
                return false;
            }
            m_Pipelines[batch] = pipeline.GetValue();
            m_Materials[batch] = std::make_unique<MaterialDebugLine>();
        }
        m_LinePipelinesRefused = false;
        return true;
    }

    SystemRasterPass DebugDrawRenderer::LinesPass()
    {
        const std::shared_ptr<Framebuffer>& target = m_Owner.GetTargetFramebuffer();
        if ( !target || !HasLines() || !EnsureLinePipelines() )
            return {};
        return SystemRasterPass{
             .Name = "DebugDrawLines",
             // The batches were uploaded in Declare; the exec only draws them.
             .ExecuteFunc = [this]( RDG::PassContext&, const FrameGraphRefs& ) -> Common::BoolResultStr
             {
                 for ( size_t batch = 0; batch < kBatchCount; ++batch )
                 {
                     if ( m_BatchVertices[batch] == 0 )
                         continue;
                     Renderer::GetInstance().SubmitPulled( m_Pipelines[batch].get(), m_BatchVertices[batch], 1.0f,
                                                           m_Materials[batch]->GetMaterialExecutor() );
                 }
                 return BOOLSUCCESS;
             },
             .TargetFramebuffer = target,
             // RENDER-THREADS M1 (UE: a pass's buffers are filled before the graph records): the line batches are
             // built and written into each batch material here, in the node's setup - the write may grow the
             // material's storage buffer, which drops its view copies - and each material that draws is prepared,
             // so the exec creates nothing.
             .Declare =
                  [this]( RenderPassDeclaration& declared, const FrameGraphRefs& )
             {
                 m_BatchVertices       = {};
                 const ViewFrame* view = m_Owner.GetViewFrame();
                 if ( view == nullptr || m_World == nullptr )
                     return;
                 std::array<std::vector<MaterialDebugLine::LineVertex>, kBatchCount> batches;
                 for ( const Core::DebugDrawLine& line : m_World->GetLines() )
                 {
                     auto& out = batches[BatchOf( line )];
                     if ( line.Thickness > 0.0f )
                         AppendQuad( out, line, view->CameraPosition );
                     else
                     {
                         out.push_back( { glm::vec4( line.Start, 1.0f ), line.Color } );
                         out.push_back( { glm::vec4( line.End, 1.0f ), line.Color } );
                     }
                 }
                 for ( size_t batch = 0; batch < kBatchCount; ++batch )
                 {
                     if ( batches[batch].empty() )
                         continue;
                     m_Materials[batch]->Update( *view, batches[batch] );
                     declared.Prepares( *m_Materials[batch]->GetMaterialExecutor() );
                     m_BatchVertices[batch] = static_cast<uint32_t>( batches[batch].size() );
                 }
             } };
    }

    Common::BoolResultStr DebugDrawRenderer::EnsureText( const std::shared_ptr<Framebuffer>& target )
    {
        // The pipeline is built against the target's formats, and the target is recreated on a resize.
        if ( m_Text.IsInitialized() && m_TextTarget == target.get() )
            return BOOLSUCCESS;
        // The text renderer's objects are this view's ledger rows (Render2D::Init).
        if ( const auto initialized = m_Text.Init( target, m_Owner.GetLedgerInstance() ); !initialized )
            return initialized;
        m_TextTarget = target.get();
        return BOOLSUCCESS;
    }

    Common::BoolResultStr DebugDrawRenderer::RecordStrings( const float width, const float height )
    {
        m_Text.BeginFrame( { 0.0f, 0.0f, width, height } );
        const ViewFrame* view = m_Owner.GetViewFrame();
        if ( view == nullptr || m_World == nullptr )
            return BOOLSUCCESS;

        Runtime::FontService& fonts      = *Runtime::ResourceRegistry::GetFontService();
        const uint64_t        fontHandle = fonts.DefaultFontHandle();
        // Every string's codepoints first: a re-bake (non-ASCII text) must happen before the font is resolved,
        // or the frame would draw against the atlas the re-bake retires.
        for ( const Core::DebugDrawString& s : m_World->GetStrings() )
            (void)fonts.RequestGlyphs( fontHandle, Text::Utf8Decode( s.Text ) );
        for ( const Core::DebugDrawScreenPanel& panel : m_World->GetScreenPanels() )
            for ( const Core::DebugDrawCanvasLine& line : panel.Lines )
                for ( const Core::DebugDrawTextRun& run : line.Runs )
                    (void)fonts.RequestGlyphs( fontHandle, Text::Utf8Decode( run.Text ) );
        const Runtime::Font* font = fonts.Get( fontHandle );
        if ( font == nullptr || !font->Atlas || !font->Baked.Valid() || font->Baked.PixelHeight <= 0.0f )
            return Common::MakeError( "the debug strings have no font: the engine's default font did not bake" );

        Render2D::DrawList2D& list = m_Text.GetDrawList();
        const TextPen         pen{ list, *font, kStringPixelHeight };
        for ( const Core::DebugDrawString& s : m_World->GetStrings() )
        {
            const glm::vec4 clip = view->ViewProjection * glm::vec4( s.Position, 1.0f );
            if ( clip.w <= 0.0f )
                continue; // behind the eye
            const glm::vec2 ndc( clip.x / clip.w, clip.y / clip.w );
            const glm::vec2 anchor( ( ndc.x * 0.5f + 0.5f ) * width, ( ndc.y * 0.5f + 0.5f ) * height );
            pen.Draw( anchor, s.Text, s.Color, s.Scale );
        }

        // The screen panels (UE's debug canvas): a background as wide as the widest line, the lines below each
        // other from the panel's origin.
        for ( const Core::DebugDrawScreenPanel& panel : m_World->GetScreenPanels() )
        {
            const float lineHeight = pen.LineHeight( panel.Scale );
            float       widest     = 0.0f;
            for ( const Core::DebugDrawCanvasLine& line : panel.Lines )
            {
                float lineWidth = 0.0f;
                for ( const Core::DebugDrawTextRun& run : line.Runs )
                    lineWidth += pen.Measure( run.Text, panel.Scale );
                widest = std::max( widest, lineWidth );
            }
            if ( panel.Background.a > 0.0f && !panel.Lines.empty() )
            {
                const glm::vec2 padding( kPanelPadding );
                list.AddRectFilled( panel.Origin - padding,
                                    panel.Origin + glm::vec2( widest, lineHeight * panel.Lines.size() ) + padding,
                                    panel.Background );
            }
            glm::vec2 at = panel.Origin;
            for ( const Core::DebugDrawCanvasLine& line : panel.Lines )
            {
                glm::vec2 runAt = at;
                for ( const Core::DebugDrawTextRun& run : line.Runs )
                    runAt.x += pen.Draw( runAt, run.Text, run.Color, panel.Scale );
                at.y += lineHeight;
            }
        }
        return BOOLSUCCESS;
    }

    SystemRasterPass DebugDrawRenderer::StringsPass()
    {
        const std::shared_ptr<Framebuffer>& target = m_Owner.GetTargetFramebuffer();
        if ( !target || !HasStrings() )
            return {};
        if ( const auto ready = EnsureText( target ); !ready )
        {
            LOG_ERROR( "[DebugDraw] the debug strings will not draw: {}", ready.GetError() );
            return {};
        }
        return SystemRasterPass{
             .Name        = "DebugDrawStrings",
             .ExecuteFunc = [this]( RDG::PassContext& context, const FrameGraphRefs& ) -> Common::BoolResultStr
             { return m_Text.Flush( context, RDG::TextureRef{}, 0 ); },
             .TargetFramebuffer = target,
             // The list is recorded in the node's setup, where its binding blocks are declared (the UI canvas
             // pass's order: Render2D::DeclareBindings sees the draws Flush will make).
             .Declare =
                  [this, target]( RenderPassDeclaration& declared, const FrameGraphRefs& )
             {
                 // Laid out at the node's render area (RenderPassDeclaration::TargetExtent), not at `target`'s:
                 // after the temporal resolve the strings draw into the OUTPUT-extent overlay set, and the scene
                 // target is the smaller render extent - laid out there, a label past it was cut (UE's debug
                 // canvas draws at the view's output size).
                 const glm::uvec2 extent = declared.TargetExtent();
                 if ( const auto recorded =
                           RecordStrings( static_cast<float>( extent.x ), static_cast<float>( extent.y ) );
                      !recorded )
                 {
                     LOG_ERROR( "[DebugDraw] {}", recorded.GetError() );
                 }
                 m_Text.DeclareBindings( declared, RDG::TextureRef{} );
             } };
    }
} // namespace Desert::Graphic
#endif // DESERT_DEV_INSTRUMENTS
