#include "Render2D.hpp"

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RenderPassDeclaration.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/Shader.hpp>
#include <Engine/Graphic/Framebuffer.hpp>
#include <Engine/Graphic/VertexBuffer.hpp>
#include <Engine/Graphic/IndexBuffer.hpp>
#include <Engine/Graphic/Texture.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Materials/MaterialExecutor.hpp>
#include <Engine/Graphic/Materials/Properties/Texture2DProperty.hpp>
#include <Engine/Core/Formats/MaterialParamRow.hpp>
#include <Engine/Graphic/Render2D/Render2DExecutorRetire.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <format>

namespace Desert::Graphic::Render2D
{
    // Out of line because MaterialExecutor is incomplete in the header. The white texture needs nothing
    // said here: a Texture2D unregisters its own image (Texture.hpp).
    Render2D::~Render2D() = default;

    Common::BoolResultStr Render2D::Init( const std::shared_ptr<Framebuffer>& target )
    {
        // The three pipelines, the two buffers and the 1x1 white texture below belong to the UI backend
        // and to no asset — one scope rather than six claims. See Engine/Graphic/ResourceLedger.hpp.
        const ResourceAttributionScope owned( ResourceOwner::UserInterface );

        if ( !target )
            return Common::MakeError( "Render2D::Init: null target framebuffer" );

        auto* shaderService = Runtime::ResourceRegistry::GetShaderService();
        if ( !shaderService )
            return Common::MakeError( "Render2D::Init: no shader service" );

        m_Shader = shaderService->GetByName( "UI2D" );
        if ( !m_Shader )
            return Common::MakeError( "Render2D::Init: missing shader 'UI2D'" );

        // Pixel-space quads: pos(vec2) + uv(vec2) + straight RGBA(vec4). Matches DrawList2D::Vertex2D.
        const VertexBufferLayout layout( { VertexBufferElement( ShaderDataType::Float2, "a_Position" ),
                                           VertexBufferElement( ShaderDataType::Float2, "a_TexCoord" ),
                                           VertexBufferElement( ShaderDataType::Float4, "a_Color" ) } );

        GraphicsPipelineSpecification spec;
        spec.DebugName         = "UI2DPipeline";
        spec.Shader            = m_Shader;
        spec.Framebuffer       = target;
        spec.Layout            = layout;
        spec.Topology          = PrimitiveTopology::Triangles;
        spec.CullMode          = CullMode::None;
        spec.DepthTestEnabled  = false; // UI is a flat overlay — no depth
        spec.DepthWriteEnabled = false;
        spec.BlendEnable       = true; // straight-alpha composite over the scene
        spec.UseLoadRenderPass = true; // draw ON TOP of the composited scene, don't clear it

        const auto uiPipeline = GraphicsPipeline::Create( spec );
        if ( !uiPipeline )
            return Common::MakeError( "Render2D::Init: " + uiPipeline.GetError() );
        m_Pipeline = uiPipeline.GetValue();

        // Text pipeline: identical state, but the UIText shader samples the SDF glyph atlas. Same vertex
        // layout (pos/uv/colour), so text and shape quads share the one dynamic vertex buffer.
        m_TextShader = shaderService->GetByName( "UIText" );
        if ( !m_TextShader )
            return Common::MakeError( "Render2D::Init: missing shader 'UIText'" );

        GraphicsPipelineSpecification textSpec = spec;
        textSpec.DebugName                     = "UITextPipeline";
        textSpec.Shader                        = m_TextShader;
        const auto textPipeline                = GraphicsPipeline::Create( textSpec );
        if ( !textPipeline )
            return Common::MakeError( "Render2D::Init: " + textPipeline.GetError() );
        m_TextPipeline = textPipeline.GetValue();

        // Glass pipeline: same state again, but the UIGlass shader samples the blurred scene snapshot and
        // masks itself with a rounded-rect SDF. Optional — a project whose shaders predate it still runs,
        // glass just falls back to a flat tinted panel.
        m_GlassShader = shaderService->GetByName( "UIGlass" );
        if ( m_GlassShader )
        {
            GraphicsPipelineSpecification glassSpec = spec;
            glassSpec.DebugName                     = "UIGlassPipeline";
            glassSpec.Shader                        = m_GlassShader;
            // Glass is OPTIONAL, so a refusal here is not an Init failure — but it is named, because a
            // silent flat tint reads exactly like "the designer did not enable glass".
            if ( const auto glassPipeline = GraphicsPipeline::Create( glassSpec ) )
                m_GlassPipeline = glassPipeline.GetValue();
            else
                LOG_ERROR( "Render2D: backdrop-blur panels draw as flat tint — {}", glassPipeline.GetError() );
        }
        else
        {
            LOG_WARN( "Render2D: shader 'UIGlass' not found — backdrop-blur panels draw as flat tint" );
        }

        // 1x1 white texture so solid shapes collapse to their vertex colour (texture * colour == colour).
        // Created once and reused; the pipeline is rebuilt every Init but the texture/buffers persist.
        // The retained-layer composite (UE Retainer Box's effect material). Not optional: a canvas that
        // retains a layer has no other way to show it.
        m_RetainerShader = shaderService->GetByName( "UIRetainer" );
        if ( !m_RetainerShader )
            return Common::MakeError( "Render2D::Init: missing shader 'UIRetainer'" );
        GraphicsPipelineSpecification retainerSpec = spec;
        retainerSpec.DebugName                     = "UIRetainerPipeline";
        retainerSpec.Shader                        = m_RetainerShader;
        const auto retainerPipeline                = GraphicsPipeline::Create( retainerSpec );
        if ( !retainerPipeline )
            return Common::MakeError( std::format( "Render2D::Init: {}", retainerPipeline.GetError() ) );
        m_RetainerPipeline = retainerPipeline.GetValue();

        if ( !m_WhiteTexture )
        {
            const unsigned char           whitePixel[4] = { 255, 255, 255, 255 };
            Core::Formats::ImagePixelData data          = std::vector<unsigned char>( whitePixel, whitePixel + 4 );

            auto texResult = Texture2D::Create( "Render2D_White", 1, 1, Core::Formats::ImageFormat::RGBA8F,
                                                std::move( data ) );
            if ( !texResult )
                return Common::MakeError( "Render2D::Init: failed to create white texture" );
            m_WhiteTexture = texResult.ExtractValue();

            if ( auto* imgService = Runtime::ResourceRegistry::GetImageService() )
                m_WhiteImage = static_cast<Image2D*>( imgService->Resolve( m_WhiteTexture->GetImageHandle() ) );
        }

        // The UI materials' pipelines are compiled against the same target and must be rebuilt with the
        // three above — a pipeline outliving its render pass is a device-lost, not a wrong picture.
        m_MaterialCache.Rebuild( target );

        return Common::MakeSuccess( true );
    }

    void Render2D::BeginFrame( const glm::vec4& viewportPx )
    {
        // Pixel -> clip. Top-left origin, y down: the engine uses a negative-height viewport (GL-style
        // Y-up NDC), so mapping bottom=y+h to NDC -1 and top=y to NDC +1 lands the origin at the top-left.
        m_Projection =
             glm::ortho( viewportPx.x, viewportPx.x + viewportPx.z, viewportPx.y + viewportPx.w, viewportPx.y );
        m_ViewportPx = viewportPx;
        m_DrawList.Reset();
        m_Prepared.Reset();
        m_PreparedList = nullptr;
        m_Retained.clear();
        m_TargetOrigin = glm::vec2( 0.0f );
    }

    void Render2D::EnsureCapacity( uint32_t vertexCount, uint32_t indexCount )
    {
        // THE CAPACITY IS RECORDED ONLY ON SUCCESS, and that is a fix rather than tidying. Both
        // `Invalidate()` results were dropped here while `m_*Capacity = cap` ran unconditionally, so a
        // failed GPU allocation left the renderer believing it owned a buffer of the new size: this
        // function then never retried, and every later draw wrote through the failed buffer. Leaving the
        // old capacity means the next call attempts the growth again.
        if ( vertexCount > m_VertexCapacity )
        {
            const uint32_t cap = std::max( vertexCount, m_VertexCapacity ? m_VertexCapacity * 2 : 4096u );
            m_VertexBuffer     = VertexBuffer::Create( cap * (uint32_t)sizeof( Vertex2D ), BufferUsage::Dynamic );
            const auto allocated = m_VertexBuffer->Invalidate(); // Create() only constructs; this allocates
            if ( allocated.IsSuccess() )
                m_VertexCapacity = cap;
            else
                LOG_ERROR( "[Render2D] vertex buffer growth to {} verts failed: {}", cap, allocated.GetError() );
        }
        if ( indexCount > m_IndexCapacity )
        {
            const uint32_t cap = std::max( indexCount, m_IndexCapacity ? m_IndexCapacity * 2 : 8192u );
            m_IndexBuffer      = IndexBuffer::Create( cap * (uint32_t)sizeof( uint32_t ), BufferUsage::Dynamic );
            const auto allocated = m_IndexBuffer->Invalidate();
            if ( allocated.IsSuccess() )
                m_IndexCapacity = cap;
            else
                LOG_ERROR( "[Render2D] index buffer growth to {} indices failed: {}", cap, allocated.GetError() );
        }
    }

    MaterialExecutor* Render2D::ExecutorFor( ExecutorCache& cache, const std::shared_ptr<Shader>& shader,
                                             const char* sampler, const void* texture, Image2D* image )
    {
        const uint64_t frame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();

        auto              it = cache.find( texture );
        MaterialExecutor* exec;
        if ( it != cache.end() )
        {
            exec                     = it->second.Executor.get();
            it->second.LastUsedFrame = frame;
        }
        else
        {
            auto owned = MaterialExecutor::Create( "Render2D", shader );
            exec       = owned.get();
            cache.emplace( texture, CachedExecutor{ std::move( owned ), frame } );
        }

        if ( auto texProp = exec->GetTexture2DProperty( sampler ) )
            texProp->SetImage( image, RDG::Access::SampledGraphics );
        return exec;
    }

    namespace
    {
        // The glass draw's push block: projection, the rect in ITS OWN space, its corner radius, the blur LOD,
        // 1/viewport (the shader maps gl_FragCoord into the snapshot with it) and the two rows that map a screen
        // fragment back into that own space. 128 bytes, the engine's push-block cap.
        //
        // The inverse travels as ROWS rather than as a mat3 because a std430 mat3 is three 16-byte columns of
        // which four floats are padding, and the block has no room for four floats of nothing.
        struct GlassPush
        {
            glm::mat4 Projection;
            glm::vec4 Rect;
            glm::vec4 Params;
            glm::vec4 InvRow0;
            glm::vec4 InvRow1;
        };
        static_assert( sizeof( GlassPush ) == 128 );

        // A retained layer's composite push block (UIRetainer.shader).
        struct RetainerPush
        {
            glm::mat4 Projection;
            glm::vec4 Rect; // min.xy, max.xy screen px
            glm::vec4 Uv;   // xy = layer extent in target UV, zw = 1 / target size
            glm::vec4 Mask; // x = mask on, y = invert, z = opacity, w = haze on
            glm::vec4 Haze; // amplitude px, cell px, cells/s, time s
        };
        static_assert( sizeof( RetainerPush ) == 128, "UIRetainer.shader push block" );

        RetainerPush RetainerPushOf( const glm::mat4& projection, const DrawCommand& cmd, const glm::vec4& uv,
                                     const bool hasMask )
        {
            const RetainerEffect& fx = cmd.Effect;
            return RetainerPush{ projection, cmd.RetainedRect, uv,
                                 glm::vec4( fx.Mask && hasMask ? 1.0f : 0.0f, fx.InvertMask ? 1.0f : 0.0f,
                                            fx.Opacity, fx.Haze ? 1.0f : 0.0f ),
                                 glm::vec4( fx.HazeAmplitude, fx.HazeScale, fx.HazeSpeed, fx.Time ) };
        }
    } // namespace

    Render2D::ResolvedCommand Render2D::Resolve( const DrawCommand& cmd, const bool backdrop )
    {
        ResolvedCommand resolved;
        if ( cmd.IndexCount == 0 )
            return resolved;
        if ( cmd.Retained )
        {
            const auto it = m_Retained.find( &cmd );
            if ( it == m_Retained.end() || !it->second.Content.IsValid() )
            {
                if ( !m_RefusedUnrendered )
                {
                    LOG_ERROR( "[Render2D] a retained UI layer was not rendered this frame, so its composite "
                               "is not drawn: the host must call Render2D::AddRetainedPasses on the graph after "
                               "the walk and before the pass that Flushes, and DeclareRetainedReads in that "
                               "pass's setup (see Render2D.hpp)" );
                }
                m_RefusedUnrendered = true;
                return resolved;
            }
            resolved.Kind     = CommandKind::Retained;
            resolved.Pipeline = m_RetainerPipeline.get();
            resolved.Layout   = &m_RetainerLayout;
            resolved.Retained = &it->second;
            if ( !it->second.Mask.IsValid() )
            {
                // Without a mask, u_Mask is the engine's white image, a written slot of the retainer executor,
                // which then carries the push block; a slot is never bound by both routes.
                MaterialExecutor* rexec =
                     ExecutorFor( m_RetainerExecutors, m_RetainerShader, "u_Mask", nullptr, m_WhiteImage );
                if ( rexec == nullptr )
                {
                    LOG_ERROR( "[Render2D] a retained UI layer's composite is not drawn: no UIRetainer executor" );
                    return ResolvedCommand{};
                }
                resolved.Plain    = rexec;
                resolved.Executor = rexec;
            }
            return resolved;
        }
        if ( cmd.Glass && m_GlassPipeline && backdrop )
        {
            resolved.Kind     = CommandKind::Glass;
            resolved.Pipeline = m_GlassPipeline.get();
            resolved.Layout   = &m_GlassLayout;
            return resolved;
        }
        if ( cmd.Material )
        {
            // A UI-DOMAIN MATERIAL FILL. The batch carries the resolved entry the canvas walk got from
            // UIMaterialCache::Resolve - never null, and never null-and-meaning-fine: a handle the UI path cannot
            // execute resolved to the magenta error entry back there, with the reason logged.
            // An entry whose row would leave the row buffer unwritten binds the default UI material instead - a
            // per-draw decision made here, so setup (DeclareInto) and Flush agree, never the whole node's fault.
            const auto* entry = m_MaterialCache.DrawableOrDefault(
                 static_cast<const UIMaterialCache::Entry*>( cmd.Material ), m_Projection );
            if ( !entry || !entry->Pipeline || !entry->Material )
                return resolved;
            resolved.Kind     = CommandKind::Material;
            resolved.Pipeline = entry->Pipeline.get();
            resolved.Material = entry->Material.get();
            resolved.Executor = resolved.Material->GetMaterialExecutor();
            resolved.Layout   = &entry->Layout;
            return resolved;
        }
        MaterialExecutor* exec;
        if ( cmd.Text )
        {
            // Text always carries a valid font-atlas texture; route it to the SDF pipeline.
            exec              = ExecutorFor( m_TextExecutors, m_TextShader, "u_SDFAtlas", cmd.Texture,
                                             const_cast<Image2D*>( static_cast<const Image2D*>( cmd.Texture ) ) );
            resolved.Pipeline = m_TextPipeline.get();
            resolved.Layout   = &m_TextLayout;
        }
        else
        {
            Image2D* img =
                 cmd.Texture ? const_cast<Image2D*>( static_cast<const Image2D*>( cmd.Texture ) ) : m_WhiteImage;
            exec              = ExecutorFor( m_Executors, m_Shader, "u_Texture", cmd.Texture, img );
            resolved.Pipeline = m_Pipeline.get();
            resolved.Layout   = &m_PlainLayout;
        }
        if ( !exec )
            return ResolvedCommand{};
        resolved.Kind     = CommandKind::Plain;
        resolved.Plain    = exec;
        resolved.Executor = exec;
        return resolved;
    }

    template <class Declaration>
    void Render2D::DeclareInto( Declaration& declared, const DrawList2D& list, const RDG::TextureRef backdrop )
    {
        if ( !m_Pipeline || !m_TextPipeline || list.Empty() )
            return; // Flush draws nothing either
        // THE ONE PREPARATION of this frame's draws (PreparedDraws): Resolve - and for a UI material
        // UIMaterialCache::DrawableOrDefault's PrepareDraw - runs here once per command; Flush records the result.
        const bool backdropValid = backdrop.IsValid();
        m_PreparedList = &list;
        m_Prepared.Prepare( list.GetCommands(),
                            [&]( const DrawCommand& cmd ) -> std::optional<ResolvedCommand>
                            {
                                ResolvedCommand resolved = Resolve( cmd, backdropValid );
                                if ( resolved.Kind == CommandKind::Skip )
                                {
                                    return std::nullopt;
                                }
                                return resolved;
                            } );
        for ( const auto& draw : m_Prepared.Draws() )
        {
            const ResolvedCommand&    resolved = draw.Value;
            ShaderBindingLayoutCache& layout   = *resolved.Layout;
            if ( resolved.Kind == CommandKind::Retained )
            {
                // The composite samples the layer (and its mask) the retained pass drew: graph textures, read by
                // name through this block. A masked composite's push block goes through the graph too; a
                // mask-less one is carried by the retainer executor (u_Mask = white), written here.
                const DrawCommand&     cmd    = list.GetCommands()[draw.Command];
                const RetainedPicture& pic    = *resolved.Retained;
                const auto&            shader = resolved.Pipeline->GetSpecification().Shader;
                if ( pic.Mask.IsValid() )
                {
                    declared.Bindings( layout.Get( shader ), RDG::OtherRouteFill{} )
                         .Sampled( "u_Content", pic.Content, RDG::Access::SampledGraphics,
                                   RDG::SubresourceRange::All(), RDG::SamplerDesc::LinearClamp() )
                         .Sampled( "u_Mask", pic.Mask, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                                   RDG::SamplerDesc::LinearClamp() )
                         .PushConstantBytes( static_cast<uint32_t>( sizeof( RetainerPush ) ) );
                }
                else
                {
                    const RetainerPush push = RetainerPushOf( m_Projection, cmd, pic.Uv, false );
                    resolved.Plain->PushConstant( &push, static_cast<uint32_t>( sizeof( push ) ) );
                    declared.Bindings( layout.Get( shader ), resolved.Executor->GetRouteFill() )
                         .Sampled( "u_Content", pic.Content, RDG::Access::SampledGraphics,
                                   RDG::SubresourceRange::All(), RDG::SamplerDesc::LinearClamp() );
                }
                continue;
            }
            if ( resolved.Kind == CommandKind::Glass )
            {
                // The sampler the glass sampled its backdrop with before: LinearClamp over every mip.
                declared
                     .Bindings( layout.Get( resolved.Pipeline->GetSpecification().Shader ), RDG::OtherRouteFill{} )
                     .Sampled( "u_Backdrop", backdrop, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                               RDG::SamplerDesc::LinearClamp() )
                     .PushConstantBytes( static_cast<uint32_t>( sizeof( GlassPush ) ) );
                continue;
            }
            // A material draw's row, projection and row index were written by Resolve (UIMaterialCache::
            // PrepareDraw, the one place - it also validated exactly this block).
            if ( resolved.Kind != CommandKind::Material )
            {
                resolved.Plain->PushConstant( &m_Projection, (uint32_t)sizeof( glm::mat4 ) );
            }
            declared.Bindings( layout.Get( resolved.Pipeline->GetSpecification().Shader ),
                               resolved.Executor->GetRouteFill() );
        }
    }

    void Render2D::DeclareBindings( RDG::PassBuilder& pass, const RDG::TextureRef backdrop )
    {
        DeclareInto( pass, m_DrawList, backdrop );
    }

    void Render2D::DeclareBindings( RenderPassDeclaration& declared, const RDG::TextureRef backdrop )
    {
        DeclareInto( declared, m_DrawList, backdrop );
    }

    void Render2D::DeclareListBindings( RDG::PassBuilder& pass, const DrawList2D& list, const RDG::TextureRef backdrop )
    {
        DeclareInto( pass, list, backdrop );
    }

    Common::BoolResultStr Render2D::Flush( const RDG::PassContext& context, RDG::TextureRef backdrop,
                                           const uint32_t firstBlock )
    {
        return FlushList( context, m_DrawList, backdrop, firstBlock );
    }

    Common::BoolResultStr Render2D::FlushList( const RDG::PassContext& context, const DrawList2D& list,
                                               RDG::TextureRef backdrop, const uint32_t firstBlock )
    {
        if ( !m_Pipeline || !m_TextPipeline )
        {
            return Common::MakeError( "Render2D::Flush: the 2D pipelines were not created (Init)" );
        }
        if ( list.Empty() )
        {
            return BOOLSUCCESS;
        }
        if ( !m_Prepared.Ready() || m_PreparedList != &list )
        {
            return Common::MakeError( "Render2D::Flush: the draws were not prepared - DeclareBindings (or "
                                      "DeclareListBindings for this list) must run in the node's setup for this "
                                      "frame's draw list" );
        }

        const auto& verts = list.GetVertices();
        const auto& idx   = list.GetIndices();

        EnsureCapacity( (uint32_t)verts.size(), (uint32_t)idx.size() );

        // REFUSE THE WHOLE FLUSH RATHER THAN DRAW FROM ONE OF THE TWO. The vertex and index buffers are
        // one geometry between them: if only the indices arrived, every command below indexes the
        // PREVIOUS frame's vertices — which is not a stale picture, it is triangles built from unrelated
        // positions, and out-of-range indices at that when the batch shrank. Drawing nothing for one
        // frame is a recoverable glitch; drawing that is not.
        const auto vertices =
             m_VertexBuffer->SetData( (void*)verts.data(), (uint32_t)( verts.size() * sizeof( Vertex2D ) ), 0 );
        const auto indices =
             m_IndexBuffer->SetData( (void*)idx.data(), (uint32_t)( idx.size() * sizeof( uint32_t ) ), 0 );
        if ( !vertices.IsSuccess() || !indices.IsSuccess() )
        {
            LOG_ERROR( "[Render2D] the batch was not uploaded, so nothing is drawn this frame. "
                       "vertices: {} | indices: {}",
                       vertices.IsSuccess() ? "ok" : vertices.GetError(),
                       indices.IsSuccess() ? "ok" : indices.GetError() );
            // The draw list is deliberately left standing: it is Reset() at the start of the next frame,
            // so nothing accumulates, and if the failure was transient the same batch is simply
            // re-uploaded then. No draw was issued, so no backdrop was used either.
            m_UsedBackdrop = false;
            return Common::MakeError( std::string( "Render2D::Flush: the batch was not uploaded: " ) +
                                      ( vertices.IsSuccess() ? indices.GetError() : vertices.GetError() ) );
        }

        auto& renderer     = Renderer::GetInstance();
        bool  usedBackdrop = false;
        // The first refused draw; the remaining batches are still drawn, so one bad batch costs one batch.
        Common::BoolResultStr failure = BOOLSUCCESS;

        // Clip a batch (UILayout ClipContents) via the scissor, or reset it to the full viewport.
        const auto ApplyScissor = [&]( const DrawCommand& cmd )
        {
            // A layer's renderer draws screen px into a target whose (0,0) is m_TargetOrigin; the main one
            // has a zero origin, so this is the plain screen scissor there.
            glm::vec4 box = ( cmd.ClipRect.z > 0.0f && cmd.ClipRect.w > 0.0f ) ? cmd.ClipRect : m_ViewportPx;
            box.x -= m_TargetOrigin.x;
            box.y -= m_TargetOrigin.y;
            if ( box.x < 0.0f )
            {
                box.z += box.x;
                box.x = 0.0f;
            }
            if ( box.y < 0.0f )
            {
                box.w += box.y;
                box.y = 0.0f;
            }
            renderer.SetScissor( static_cast<int32_t>( box.x ), static_cast<int32_t>( box.y ),
                                 static_cast<uint32_t>( std::max( box.z, 0.0f ) ),
                                 static_cast<uint32_t>( std::max( box.w, 0.0f ) ) );
        };

        // The draws the setup prepared (DeclareBindings), recorded as they are: the n-th opens block n. Nothing is
        // resolved, prepared or validated again here.
        const auto& commands = list.GetCommands();
        uint32_t    block    = firstBlock;
        for ( const auto& draw : m_Prepared.Draws() )
        {
            const DrawCommand&     cmd      = commands[draw.Command];
            const ResolvedCommand& resolved = draw.Value;
            const uint32_t         index    = block++;

            if ( resolved.Kind == CommandKind::Retained )
            {
                // A retained layer's composite. The layer (and its mask) are graph textures this pass's setup
                // declared (DeclareRetainedReads + the block DeclareInto opened); with a mask the push block goes
                // through the graph bindings, without one the retainer executor (u_Mask = white) carries it.
                const RetainedPicture& pic = *resolved.Retained;
                ApplyScissor( cmd );
                RDG::PassBindings bindings( context, context.GetBindingBlock( index ) );
                if ( pic.Mask.IsValid() )
                {
                    const RetainerPush push = RetainerPushOf( m_Projection, cmd, pic.Uv, true );
                    bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                }
                const Common::BoolResultStr drawn =
                     renderer.DrawIndexed( bindings, *resolved.Pipeline, resolved.Plain, *m_VertexBuffer,
                                           *m_IndexBuffer, cmd.IndexCount, cmd.IndexOffset );
                if ( !drawn.IsSuccess() && failure.IsSuccess() )
                {
                    failure =
                         Common::MakeError( "a retained UI layer's composite was not drawn: " + drawn.GetError() );
                }
                continue;
            }

            if ( resolved.Kind == CommandKind::Glass )
            {
                // The coarsest LOD the glass may sample is the pyramid's own last mip (UE: Texture->Desc.NumMips).
                const Common::ResultStr<RDG::TextureDesc> backdropDesc = context.GetTextureDesc( backdrop );
                if ( !backdropDesc.IsSuccess() )
                {
                    if ( failure.IsSuccess() )
                        failure = Common::MakeError( "a glass panel was not drawn: " + backdropDesc.GetError() );
                    continue;
                }
                const uint32_t  backdropMaxLod = backdropDesc.GetValue().Mips - 1;
                const GlassPush push{
                     m_Projection, cmd.GlassRect,
                     glm::vec4( cmd.GlassRound, cmd.GlassLod * static_cast<float>( backdropMaxLod ),
                                m_ViewportPx.z > 0.0f ? 1.0f / m_ViewportPx.z : 0.0f,
                                m_ViewportPx.w > 0.0f ? 1.0f / m_ViewportPx.w : 0.0f ),
                     glm::vec4( cmd.GlassInverse[0].x, cmd.GlassInverse[1].x, cmd.GlassInverse[2].x,
                                cmd.GlassFeather ),
                     glm::vec4( cmd.GlassInverse[0].y, cmd.GlassInverse[1].y, cmd.GlassInverse[2].y, 0.0f ) };

                ApplyScissor( cmd );
                RDG::PassBindings bindings( context, context.GetBindingBlock( index ) );
                bindings.PushConstants( &push, (uint32_t)sizeof( push ) );
                const Common::BoolResultStr drawn =
                     renderer.DrawIndexed( bindings, *resolved.Pipeline, nullptr, *m_VertexBuffer, *m_IndexBuffer,
                                           cmd.IndexCount, cmd.IndexOffset );
                if ( !drawn.IsSuccess() && failure.IsSuccess() )
                    failure = Common::MakeError( "a glass panel was not drawn: " + drawn.GetError() );
                usedBackdrop = true;
                continue;
            }

            // A UI material's row / push matrix / index and a 2D batch's projection were filled in the setup
            // (DeclareBindings); the exec only clips and draws.
            ApplyScissor( cmd );
            const Common::BoolResultStr drawn = renderer.DrawIndexed(
                 RDG::PassBindings( context, context.GetBindingBlock( index ) ), *resolved.Pipeline,
                 resolved.Executor, *m_VertexBuffer, *m_IndexBuffer, cmd.IndexCount, cmd.IndexOffset );
            if ( !drawn.IsSuccess() && failure.IsSuccess() )
                failure = Common::MakeError( resolved.Kind == CommandKind::Material
                                                  ? "a UI material batch was not drawn: " + drawn.GetError()
                                                  : "a 2D batch was not drawn: " + drawn.GetError() );
        }

        // Leave the scissor at the full viewport so nothing downstream inherits a UI clip.
        renderer.SetScissor( static_cast<int32_t>( m_ViewportPx.x - m_TargetOrigin.x ),
                             static_cast<int32_t>( m_ViewportPx.y - m_TargetOrigin.y ),
                             static_cast<uint32_t>( m_ViewportPx.z ), static_cast<uint32_t>( m_ViewportPx.w ) );

        m_UsedBackdrop = usedBackdrop;

        m_Prepared.Reset();
        m_PreparedList = nullptr;
        RetireUnusedExecutors();
        m_MaterialCache.RetireUnused();
        return failure;
    }

    void Render2D::RetireUnusedExecutors()
    {
        // The window is ExecutorRetireWindow(), shared with UIMaterialCache: one answer to how long a
        // recorded frame lives, not a number written here.
        const uint64_t frame  = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        const uint32_t window = ExecutorRetireWindow();

        for ( ExecutorCache* cache : { &m_Executors, &m_TextExecutors, &m_RetainerExecutors } )
        {
            for ( auto it = cache->begin(); it != cache->end(); )
            {
                if ( MayRetireExecutor( it->second.LastUsedFrame, frame, window ) )
                    it = cache->erase( it );
                else
                    ++it;
            }
        }
    }

    Common::BoolResultStr Render2D::AddRetainedPasses( RDG::Builder& graph )
    {
        const Common::BoolResultStr added = AddRetainedPassesOf( graph, m_DrawList );

        const uint64_t frame  = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        const uint32_t window = ExecutorRetireWindow();
        std::erase_if( m_RetainedPool, [&]( const std::unique_ptr<RetainedTarget>& t )
                       { return MayRetireExecutor( t->LastUsedFrame, frame, window ); } );
        return added;
    }

    void Render2D::DeclareRetainedReads( RDG::PassBuilder& pass ) const
    {
        for ( const auto& [cmd, pic] : m_Retained )
        {
            if ( pic.Content.IsValid() )
            {
                pass.Read( pic.Content, RDG::Access::SampledGraphics );
            }
            if ( pic.Mask.IsValid() )
            {
                pass.Read( pic.Mask, RDG::Access::SampledGraphics );
            }
        }
    }

    Common::BoolResultStr Render2D::AddRetainedPassesOf( RDG::Builder& graph, const DrawList2D& root )
    {
        const uint64_t frame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();

        // Expand: every retainer, at any depth, becomes a job; a job's nested retainers are appended after
        // it, so walking the jobs backwards adds the most nested layer's pass first: by the time a layer's pass is
        // added, the layers it composites are already in its renderer's m_Retained, and its setup declares them.
        std::vector<RetainedJob>                             jobs;
        std::vector<std::pair<Render2D*, const DrawList2D*>> pending{ { this, &root } };
        while ( !pending.empty() )
        {
            const auto [owner, list] = pending.back();
            pending.pop_back();
            for ( const DrawCommand& cmd : list->GetCommands() )
            {
                if ( !cmd.Retained || cmd.RetainedLayer >= list->GetLayers().size() )
                    continue;
                const glm::vec4 r = cmd.RetainedRect;
                const auto      w = static_cast<uint32_t>( r.z - r.x );
                const auto      h = static_cast<uint32_t>( r.w - r.y );
                if ( w == 0 || h == 0 )
                    continue;

                RetainedTarget* content = AcquireRetainedTarget( w, h, frame );
                if ( content == nullptr )
                    continue;
                const DrawList2D* layer = list->GetLayers()[cmd.RetainedLayer].get();
                OpenTarget( *content, r );
                const size_t contentJob = jobs.size();
                jobs.push_back(
                     { .Owner = owner, .Cmd = &cmd, .Target = content, .Layer = layer, .Width = w, .Height = h } );
                pending.emplace_back( content->Renderer.get(), layer );

                if ( !cmd.Effect.Mask )
                    continue;
                const auto& masks = root.GetMaskLayers();
                const auto  it    = masks.find( cmd.RetainedMask );
                if ( it == masks.end() )
                    continue;
                RetainedTarget* mask = AcquireRetainedTarget( content->Width, content->Height, frame );
                if ( mask == nullptr )
                    continue;
                const DrawList2D* maskLayer = root.GetLayers()[it->second].get();
                OpenTarget( *mask, r );
                jobs.push_back( { .Owner  = owner,
                                  .Cmd    = &cmd,
                                  .Target = mask,
                                  .Layer  = maskLayer,
                                  .Width  = w,
                                  .Height = h,
                                  .MaskOf = contentJob } );
                pending.emplace_back( mask->Renderer.get(), maskLayer );
            }
        }

        auto&                 renderer = Renderer::GetInstance();
        Common::BoolResultStr failure  = BOOLSUCCESS;
        for ( auto job = jobs.rbegin(); job != jobs.rend(); ++job )
        {
            RetainedTarget& target = *job->Target;
            if ( const auto imported =
                      renderer.ImportImage( target.Target->GetColorAttachmentImage( 0 ), target.External );
                 !imported )
            {
                LOG_ERROR( "[Render2D] retained layer {}x{} not drawn: {}", target.Width, target.Height,
                           imported.GetError() );
                if ( failure.IsSuccess() )
                {
                    failure = Common::MakeError( "a retained UI layer was not added: " + imported.GetError() );
                }
                continue;
            }
            const RDG::TextureRef layerRef      = graph.RegisterExternal( target.External, "UIRetainedLayer" );
            Render2D*             layerRenderer = target.Renderer.get();
            const DrawList2D*     layer         = job->Layer;
            graph.AddPass(
                 "UIRetainedLayer", RDG::PassFlags::Raster,
                 [layerRef, layerRenderer, layer]( RDG::PassBuilder& pass )
                 {
                     // Transparent: the layer is premultiplied.
                     pass.ColorTarget( 0, layerRef, RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) );
                     layerRenderer->DeclareRetainedReads( pass );
                     layerRenderer->DeclareListBindings( pass, *layer, RDG::TextureRef{} );
                 },
                 // The layer's blocks are the pass's only ones (DeclareRetainedReads declares reads, no block).
                 [layerRenderer, layer]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return layerRenderer->FlushList( context, *layer, RDG::TextureRef{}, 0 ); } );

            if ( job->MaskOf != SIZE_MAX )
            {
                jobs[job->MaskOf].Mask = layerRef;
                continue;
            }
            RetainedPicture pic;
            pic.Content = layerRef;
            pic.Mask    = job->Mask;
            pic.Uv      = glm::vec4( static_cast<float>( job->Width ) / static_cast<float>( target.Width ),
                                     static_cast<float>( job->Height ) / static_cast<float>( target.Height ),
                                     1.0f / static_cast<float>( target.Width ),
                                     1.0f / static_cast<float>( target.Height ) );
            job->Owner->m_Retained[job->Cmd] = pic;
        }
        return failure;
    }

    Render2D::RetainedTarget* Render2D::AcquireRetainedTarget( uint32_t width, uint32_t height, uint64_t frame )
    {
        constexpr uint32_t kRetainedQuantum = 64;
        const uint32_t     W = ( width + kRetainedQuantum - 1 ) / kRetainedQuantum * kRetainedQuantum;
        const uint32_t     H = ( height + kRetainedQuantum - 1 ) / kRetainedQuantum * kRetainedQuantum;

        for ( auto& t : m_RetainedPool )
            if ( t->Width == W && t->Height == H && t->LastUsedFrame != frame )
            {
                t->LastUsedFrame = frame;
                return t.get();
            }

        const ResourceAttributionScope owned( ResourceOwner::UserInterface );
        auto                           t = std::make_unique<RetainedTarget>();
        FramebufferSpecification       spec;
        spec.Width       = W;
        spec.Height      = H;
        spec.Attachments = { Core::Formats::ImageFormat::RGBA8F };
        spec.DebugName   = "UIRetainedLayer";
        spec.NoResizeble = true;
        t->Target        = Framebuffer::Create( spec );
        if ( !t->Target )
        {
            LOG_ERROR( "[Render2D] a {}x{} retained-layer target could not be created; the layer is not drawn", W,
                       H );
            return nullptr;
        }
        // Framebuffer::Create only constructs: the colour image the graph imports and the VkRenderPass the layer
        // renderer's pipelines are built against are made by the first Resize.
        if ( const auto made = t->Target->Resize( W, H ); !made )
        {
            LOG_ERROR(
                 "[Render2D] a {}x{} retained-layer target could not be allocated: {}; the layer is not drawn", W,
                 H, made.GetError() );
            return nullptr;
        }
        t->Renderer = std::make_unique<Render2D>();
        if ( const auto init = t->Renderer->Init( t->Target ); !init )
        {
            LOG_ERROR( "[Render2D] retained-layer renderer: {}", init.GetError() );
            return nullptr;
        }
        t->Width         = W;
        t->Height        = H;
        t->LastUsedFrame = frame;
        m_RetainedPool.push_back( std::move( t ) );
        return m_RetainedPool.back().get();
    }

    void Render2D::OpenTarget( RetainedTarget& target, const glm::vec4& rect )
    {
        Render2D& r2d = *target.Renderer;
        // Screen px in, target px out: the layer's top-left lands on the target's (0,0), one to one.
        r2d.BeginFrame(
             { rect.x, rect.y, static_cast<float>( target.Width ), static_cast<float>( target.Height ) } );
        r2d.m_TargetOrigin = glm::vec2( rect.x, rect.y );
    }
} // namespace Desert::Graphic::Render2D
