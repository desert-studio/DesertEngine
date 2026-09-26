#include "Render2D.hpp"

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/Shader.hpp>
#include <Engine/Graphic/Framebuffer.hpp>
#include <Engine/Graphic/VertexBuffer.hpp>
#include <Engine/Graphic/IndexBuffer.hpp>
#include <Engine/Graphic/Texture.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Materials/MaterialExecutor.hpp>
#include <Engine/Graphic/Materials/Properties/StorageBufferProperty.hpp>
#include <Engine/Graphic/Materials/Properties/Texture2DProperty.hpp>
#include <Engine/Core/Formats/MaterialParamRow.hpp>
#include <Engine/Graphic/Render2D/Render2DExecutorRetire.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

namespace Desert::Graphic::Render2D
{
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
            texProp->SetImage( image );
        return exec;
    }

    void Render2D::Flush()
    {
        if ( !m_Pipeline || !m_TextPipeline || m_DrawList.Empty() )
            return;

        const auto& verts = m_DrawList.GetVertices();
        const auto& idx   = m_DrawList.GetIndices();

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
            return;
        }

        auto& renderer     = Renderer::GetInstance();
        bool  usedBackdrop = false;

        // Clip a batch (UILayout ClipContents) via the scissor, or reset it to the full viewport.
        const auto ApplyScissor = [&]( const DrawCommand& cmd )
        {
            if ( cmd.ClipRect.z > 0.0f && cmd.ClipRect.w > 0.0f )
                renderer.SetScissor( (int32_t)cmd.ClipRect.x, (int32_t)cmd.ClipRect.y, (uint32_t)cmd.ClipRect.z,
                                     (uint32_t)cmd.ClipRect.w );
            else
                renderer.SetScissor( (int32_t)m_ViewportPx.x, (int32_t)m_ViewportPx.y, (uint32_t)m_ViewportPx.z,
                                     (uint32_t)m_ViewportPx.w );
        };

        for ( const auto& cmd : m_DrawList.GetCommands() )
        {
            if ( cmd.IndexCount == 0 )
                continue;

            MaterialExecutor* exec;
            GraphicsPipeline* pipeline;
            if ( cmd.Glass && m_GlassPipeline && m_Backdrop )
            {
                exec     = ExecutorFor( m_GlassExecutors, m_GlassShader, "u_Backdrop", m_Backdrop, m_Backdrop );
                pipeline = m_GlassPipeline.get();
                if ( !exec )
                    continue;

                // Per-element push block: projection, the rect in ITS OWN space, its corner radius, the
                // blur LOD, 1/viewport (the shader maps gl_FragCoord into the snapshot with it) and the
                // two rows that map a screen fragment back into that own space. 128 bytes, which is the
                // size every Vulkan implementation is required to offer.
                //
                // The inverse travels as ROWS rather than as a mat3 because a std430 mat3 is three
                // 16-byte columns of which four floats are padding, and the block has no room for four
                // floats of nothing.
                struct GlassPush
                {
                    glm::mat4 Projection;
                    glm::vec4 Rect;
                    glm::vec4 Params;
                    glm::vec4 InvRow0;
                    glm::vec4 InvRow1;
                } push{ m_Projection, cmd.GlassRect,
                        glm::vec4( cmd.GlassRound, cmd.GlassLod * static_cast<float>( m_BackdropMaxLod ),
                                   m_ViewportPx.z > 0.0f ? 1.0f / m_ViewportPx.z : 0.0f,
                                   m_ViewportPx.w > 0.0f ? 1.0f / m_ViewportPx.w : 0.0f ),
                        glm::vec4( cmd.GlassInverse[0].x, cmd.GlassInverse[1].x, cmd.GlassInverse[2].x,
                                   cmd.GlassFeather ),
                        glm::vec4( cmd.GlassInverse[0].y, cmd.GlassInverse[1].y, cmd.GlassInverse[2].y, 0.0f ) };

                ApplyScissor( cmd );
                exec->PushConstant( &push, (uint32_t)sizeof( push ) );
                renderer.SubmitIndexed( pipeline, m_VertexBuffer.get(), m_IndexBuffer.get(), cmd.IndexCount,
                                        cmd.IndexOffset, exec );
                usedBackdrop = true;
                continue;
            }

            if ( cmd.Material )
            {
                // A UI-DOMAIN MATERIAL FILL. The batch carries the resolved entry the canvas walk got
                // from UIMaterialCache::Resolve — never null, and never null-and-meaning-fine: a handle
                // the UI path cannot execute resolved to the magenta error entry back there, with the
                // reason logged, so there is nothing left here to fall back from.
                const auto* entry = static_cast<const UIMaterialCache::Entry*>( cmd.Material );
                if ( !entry->Pipeline || !entry->Material )
                    continue;

                auto* material = entry->Material.get();

                // THE PARAMETERS ARE A ROW, NOT PUSH BYTES. One row per material and therefore index 0 —
                // a UI material is shared by every element pointing at the same asset, which is exactly
                // what keeps two such elements in one batch. `SetMaterialIndex` writes that index into
                // the push block at Core::Formats::kMaterialIndexPushOffset (64), the same offset the
                // mesh path writes it at, so the two transports cannot drift.
                const auto& row = material->GetParamRow();
                if ( !row.empty() )
                    if ( auto* sb = material->Get<StorageBufferProperty>( Core::Formats::kMaterialRowBlockName ) )
                        sb->SetRawData( row.data(), static_cast<uint32_t>( row.size() * sizeof( glm::vec4 ) ) );

                ApplyScissor( cmd );
                // 64 bytes of projection at offset 0 + 4 of row index at 64 = 68 of the 128 available.
                // Common/UIVertex.glslh is the other half of this: the mat4 slot the mesh path calls
                // Transform carries the batcher's pixel -> clip projection in the UI domain.
                material->SetPushMatrix( m_Projection );
                material->SetMaterialIndex( 0 );
                renderer.SubmitIndexed( entry->Pipeline.get(), m_VertexBuffer.get(), m_IndexBuffer.get(),
                                        cmd.IndexCount, cmd.IndexOffset, material->GetMaterialExecutor() );
                continue;
            }

            if ( cmd.Text )
            {
                // Text always carries a valid font-atlas texture; route it to the SDF pipeline.
                exec     = ExecutorFor( m_TextExecutors, m_TextShader, "u_SDFAtlas", cmd.Texture,
                                        const_cast<Image2D*>( static_cast<const Image2D*>( cmd.Texture ) ) );
                pipeline = m_TextPipeline.get();
            }
            else
            {
                Image2D* img = cmd.Texture ? const_cast<Image2D*>( static_cast<const Image2D*>( cmd.Texture ) )
                                           : m_WhiteImage;
                exec         = ExecutorFor( m_Executors, m_Shader, "u_Texture", cmd.Texture, img );
                pipeline     = m_Pipeline.get();
            }
            if ( !exec )
                continue;

            ApplyScissor( cmd );

            exec->PushConstant( &m_Projection, (uint32_t)sizeof( glm::mat4 ) );
            renderer.SubmitIndexed( pipeline, m_VertexBuffer.get(), m_IndexBuffer.get(), cmd.IndexCount,
                                    cmd.IndexOffset, exec );
        }

        // Leave the scissor at the full viewport so nothing downstream inherits a UI clip.
        renderer.SetScissor( (int32_t)m_ViewportPx.x, (int32_t)m_ViewportPx.y, (uint32_t)m_ViewportPx.z,
                             (uint32_t)m_ViewportPx.w );

        m_UsedBackdrop = usedBackdrop;

        RetireUnusedExecutors();
        m_MaterialCache.RetireUnused();
    }

    void Render2D::RetireUnusedExecutors()
    {
        // The window is ExecutorRetireWindow(), shared with UIMaterialCache: one answer to how long a
        // recorded frame lives, not a number written here.
        const uint64_t frame  = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        const uint32_t window = ExecutorRetireWindow();

        for ( ExecutorCache* cache : { &m_Executors, &m_TextExecutors, &m_GlassExecutors } )
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
} // namespace Desert::Graphic::Render2D
