#pragma once

#include <Render2DCore/DrawList2D.hpp>
#include <Engine/Graphic/Render2D/PreparedDraws.hpp>
#include <Engine/Graphic/Render2D/UIMaterialCache.hpp>
#include <Engine/Graphic/Shader.hpp>

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>

#include <glm/glm.hpp>

#include <memory>
#include <unordered_map>
#include <vector>

namespace Desert::Graphic
{
    class Framebuffer;
    class GraphicsPipeline;
    class Shader;
    class Texture2D;
    class Image2D;
    class VertexBuffer;
    class IndexBuffer;
    class MaterialExecutor;
    class RenderPassDeclaration;
} // namespace Desert::Graphic

namespace Desert::Graphic::Render2D
{
    // GPU backend for the 2D batcher: owns the UI2D pipeline, one growable dynamic vertex+index buffer and a
    // 1x1 white texture, and turns a DrawList2D into draw calls. Callers record primitives into GetDrawList()
    // between BeginFrame() and Flush(); Flush() uploads the geometry once and issues one Renderer::DrawIndexed
    // per state batch. Flush() runs inside a graph raster pass (its PassContext), never outside the graph.
    class Render2D
    {
    public:
        Render2D() = default;
        ~Render2D();
        // Owns per-view GPU state (pipelines, buffers, executors keyed by image address) that two
        // copies could not share.
        Render2D( const Render2D& )            = delete;
        Render2D& operator=( const Render2D& ) = delete;
        Render2D( Render2D&& )                 = delete;
        Render2D& operator=( Render2D&& )      = delete;

        // (Re)creates the pipeline against @p target (the scene HDR framebuffer, composited via a load pass).
        // Call after every Scene::Init — the framebuffers are recreated there. Idempotent.
        Common::BoolResultStr Init( const std::shared_ptr<Framebuffer>& target );

        bool IsInitialized() const
        {
            return m_Pipeline != nullptr;
        }

        // The UI2D pipeline (built against the scene target). External-pass registration needs its spec so
        // the render graph sets up the matching load render pass this backend's draws record into.
        const std::shared_ptr<GraphicsPipeline>& GetPipeline() const
        {
            return m_Pipeline;
        }

        // Start a frame: set the pixel->clip projection for @p viewportPx (x,y,w,h) and clear the draw list.
        void BeginFrame( const glm::vec4& viewportPx );

        DrawList2D& GetDrawList()
        {
            return m_DrawList;
        }
        const DrawList2D& GetDrawList() const
        {
            return m_DrawList;
        }

        // Upload the recorded geometry and draw it into the current render pass. No-op when nothing was recorded.
        // Glass rects sample @p backdrop (this frame's BackdropBlur transient, bound by name as u_Backdrop through
        // RDG::PassBindings over @p context, the UI node's context) at up to its coarsest mip, read from the
        // texture's own description (PassContext::GetTextureDesc); with an invalid ref (no blur this frame) they
        // draw as a flat tinted panel. Every batch is drawn through Renderer::DrawIndexed over @p context; the
        // first refused draw is returned (the remaining batches are still drawn and the caches still retired).
        // @p firstBlock is the index DeclareBindings' first block got in the node's setup (0 unless the node
        // declared blocks of its own before it).
        [[nodiscard]] Common::BoolResultStr Flush( const RDG::PassContext& context, RDG::TextureRef backdrop,
                                                   uint32_t firstBlock );

        // RDG-FAULT1. The UI node's SETUP half of Flush: one binding block per command Flush will draw, in draw
        // order (Flush opens block n for its n-th drawn command), each against the layout of the pipeline it
        // draws with and the route fill of its executor. The executors are filled here (a UI material's row,
        // push matrix and index; a 2D batch's projection), so the block validation sees what the draw will
        // carry. A glass command declares u_Backdrop (@p backdrop, LinearClamp, every mip) and its push block.
        // Called after the canvas walk recorded this frame's draw list, before Flush, with the same backdrop.
        void DeclareBindings( RDG::PassBuilder& pass, RDG::TextureRef backdrop );
        void DeclareBindings( RenderPassDeclaration& declared, RDG::TextureRef backdrop );

        // Flush for @p list (not the own one) inside @p context's raster pass. A retained layer's renderer draws
        // the walk's layer list through this, so the list is never copied.
        // The list's draws must have been prepared by DeclareListBindings in the same pass's setup; @p firstBlock
        // as for Flush.
        [[nodiscard]] Common::BoolResultStr FlushList( const RDG::PassContext& context, const DrawList2D& list,
                                                       RDG::TextureRef backdrop, uint32_t firstBlock );
        // DeclareBindings for @p list (a retained layer's): the setup half of FlushList.
        void DeclareListBindings( RDG::PassBuilder& pass, const DrawList2D& list, RDG::TextureRef backdrop );

        // RETAINED LAYERS (UE Retainer Box) as graph passes. Adds to @p graph one raster pass per layer the
        // recorded list composites (at any depth, the most nested first), each drawing into its pooled offscreen
        // RGBA target imported as a graph external (Renderer::ImportImage: the graph places the barriers and keeps
        // the image's layout record). MUST be called after the walk and BEFORE the pass that Flushes is added;
        // that pass's setup then calls DeclareRetainedReads so the composites it samples are declared reads. A
        // Flush whose list composites a layer this did not add refuses that composite with a log line. Returns the
        // first layer that could not be added (the others still are); unused pooled targets are retired here.
        [[nodiscard]] Common::BoolResultStr AddRetainedPasses( RDG::Builder& graph );
        // Declares, on the pass that Flushes this renderer's list, a sampled read of every retained layer (and
        // mask) its composites draw. Call in that pass's setup, after AddRetainedPasses.
        void DeclareRetainedReads( RDG::PassBuilder& pass ) const;

        // How many pooled layer targets are alive (for the host that reports it, and for a test).
        [[nodiscard]] uint32_t RetainedTargetCount() const
        {
            return static_cast<uint32_t>( m_RetainedPool.size() );
        }

        // This backend's UI-material cache. The canvas walk resolves an element's `.demat` through it and
        // hands the resolved entry to DrawList2D::AddMaterialRect; Flush then draws with that entry's own
        // pipeline. It lives HERE and not behind a service because a pipeline belongs to one framebuffer's
        // render pass, and this object is the only thing that knows which framebuffer that is.
        UIMaterialCache& Materials()
        {
            return m_MaterialCache;
        }

        // Did the last Flush() draw any glass? The host reports this to the SceneRenderer so the blur
        // pyramid is only built for canvases that actually use it.
        [[nodiscard]] bool UsedBackdrop() const
        {
            return m_UsedBackdrop;
        }

    private:
        // One pooled layer target: an RGBA8 framebuffer (its colour image is what the graph imports; its render
        // pass is what the layer renderer's pipelines are built against), the graph-external record of that image
        // for the graph being built, and the Render2D that draws the layer. Sizes are rounded up to
        // kRetainedQuantum so a layer that grows by a pixel keeps its target; a target no frame in flight can
        // still read is destroyed (MayRetireExecutor).
        struct RetainedTarget
        {
            std::shared_ptr<Framebuffer> Target;
            RDG::ExternalTexture         External;
            std::unique_ptr<Render2D>    Renderer;
            uint32_t                     Width         = 0;
            uint32_t                     Height        = 0;
            uint64_t                     LastUsedFrame = 0;
        };

        // What AddRetainedPasses left for one composite command: graph refs, valid for the graph being built only.
        struct RetainedPicture
        {
            RDG::TextureRef Content;
            RDG::TextureRef Mask;                   // invalid = no mask (the engine's white is bound)
            glm::vec4       Uv = glm::vec4( 0.0f ); // xy = layer extent in target UV, zw = 1 / target size
        };

        // One layer to draw into one target: a retainer's content, or its mask (MaskOf = the content job).
        struct RetainedJob
        {
            Render2D*          Owner  = nullptr; // whose m_Retained receives the picture
            const DrawCommand* Cmd    = nullptr;
            RetainedTarget*    Target = nullptr;
            const DrawList2D*  Layer  = nullptr;
            uint32_t           Width  = 0;
            uint32_t           Height = 0;
            size_t             MaskOf = SIZE_MAX; // SIZE_MAX = this job is content
            RDG::TextureRef    Mask;              // the mask job's layer, set before this content job is added
        };

        [[nodiscard]] Common::BoolResultStr AddRetainedPassesOf( RDG::Builder& graph, const DrawList2D& root );
        RetainedTarget* AcquireRetainedTarget( uint32_t width, uint32_t height, uint64_t frame );
        static void                         OpenTarget( RetainedTarget& target, const glm::vec4& rect );

        // Grow the dynamic buffers to hold at least the given counts (reused across frames otherwise).
        void EnsureCapacity( uint32_t vertexCount, uint32_t indexCount );

        // What one command draws with, decided in ONE place for the setup (DeclareBindings) and the exec (Flush),
        // so the n-th drawn command of both is the same command: Skip (nothing drawn), the glass pipeline, a UI
        // material's own pipeline + executor, or the 2D/text pipeline with the executor of its texture.
        enum class CommandKind
        {
            Skip,
            Glass,
            Retained,
            Material,
            Plain
        };
        // What one drawn command binds: made by Resolve ONCE per command in the setup (DeclareInto) and kept in
        // m_Prepared until that frame's Flush records it. Non-owning: every pointer is into this Render2D (its
        // pipelines, executor caches, layouts) or into the UIMaterialCache entry the command resolved to.
        struct ResolvedCommand
        {
            CommandKind               Kind     = CommandKind::Skip;
            GraphicsPipeline*         Pipeline = nullptr;
            const MaterialExecutor*   Executor = nullptr;
            MaterialExecutor*         Plain    = nullptr; // the 2D/text executor (its projection is pushed)
            DataDrivenMaterial*       Material = nullptr; // a UI material's
            ShaderBindingLayoutCache* Layout   = nullptr; // keyed on Pipeline's shader
            const RetainedPicture*    Retained = nullptr; // a retained composite's picture (m_Retained's entry)
        };
        ResolvedCommand Resolve( const DrawCommand& cmd, bool backdrop );
        template <class Declaration>
        void DeclareInto( Declaration& declared, const DrawList2D& list, RDG::TextureRef backdrop );

        // One cached executor and the frame it was last drawn with. THE STAMP IS THE WHOLE FIX: without
        // it nothing could ever be removed from these caches safely, and so nothing was removed at all —
        // an executor and its descriptor set per texture address ever bound, for the life of the process
        // (A8-1). Retirement is decided by Render2D::MayRetireExecutor, which is asserted rather than
        // described.
        struct CachedExecutor
        {
            std::unique_ptr<MaterialExecutor> Executor;
            uint64_t                          LastUsedFrame = 0;
        };

        using ExecutorCache = std::unordered_map<const void*, CachedExecutor>;

        // Destroy the entries no frame still in flight can be reading. Called once per Flush, after the
        // last draw is recorded.
        void RetireUnusedExecutors();

        // Lazily-created MaterialExecutor per bound texture, one @p cache per shader (UI2D vs UIText). Each
        // executor owns its own descriptor set, so switching textures across batches never overwrites a live
        // binding. @p sampler is the shader's sampler2D name the texture is bound to.
        MaterialExecutor* ExecutorFor( ExecutorCache& cache, const std::shared_ptr<Shader>& shader,
                                       const char* sampler, const void* texture, Image2D* image );

        std::shared_ptr<Shader>           m_Shader;      // UI2D  (solid / image)
        std::shared_ptr<Shader>           m_TextShader;  // UIText (SDF glyphs)
        std::shared_ptr<Shader>           m_GlassShader; // UIGlass (backdrop blur)
        std::shared_ptr<GraphicsPipeline> m_Pipeline;
        std::shared_ptr<GraphicsPipeline> m_TextPipeline;
        std::shared_ptr<GraphicsPipeline> m_GlassPipeline;
        ShaderBindingLayoutCache          m_PlainLayout;    // keyed on m_Pipeline's shader
        ShaderBindingLayoutCache          m_TextLayout;     // keyed on m_TextPipeline's shader
        ShaderBindingLayoutCache          m_GlassLayout;    // keyed on m_GlassPipeline's shader
        ShaderBindingLayoutCache          m_RetainerLayout; // keyed on m_RetainerPipeline's shader
        std::shared_ptr<VertexBuffer>     m_VertexBuffer;
        std::shared_ptr<IndexBuffer>      m_IndexBuffer;
        uint32_t                          m_VertexCapacity = 0;
        uint32_t                          m_IndexCapacity  = 0;

        std::shared_ptr<Texture2D> m_WhiteTexture;
        Image2D*                   m_WhiteImage = nullptr;

        UIMaterialCache m_MaterialCache; // UI-domain `.demat` fills, keyed by asset handle

        ExecutorCache m_Executors;     // UI2D, keyed by bound Image2D* (null => white)
        ExecutorCache m_TextExecutors; // UIText, keyed by font atlas Image2D*
        // UIRetainer with the engine's white image written to u_Mask (an unmasked composite; the layer itself and
        // a mask layer are graph textures bound through RDG::PassBindings). One entry, keyed by null.
        ExecutorCache m_RetainerExecutors;

        std::shared_ptr<Shader>                                 m_RetainerShader;
        std::shared_ptr<GraphicsPipeline>                       m_RetainerPipeline;
        std::vector<std::unique_ptr<RetainedTarget>>            m_RetainedPool;
        std::unordered_map<const DrawCommand*, RetainedPicture> m_Retained; // this frame's, by command
        glm::vec2 m_TargetOrigin      = glm::vec2( 0.0f );                  // layer px origin
        bool      m_RefusedUnrendered = false;

        bool m_UsedBackdrop = false; // glass drawn in the last Flush -> keep the pyramid alive

        DrawList2D m_DrawList;
        // The frame's draws as the setup prepared them (DeclareInto); Flush records exactly these. Reset by
        // BeginFrame and after Flush, so nothing prepared outlives its frame.
        PreparedDraws<ResolvedCommand> m_Prepared;
        const DrawList2D*              m_PreparedList = nullptr; // the list m_Prepared was prepared from
        glm::mat4  m_Projection = glm::mat4( 1.0f );
        glm::vec4  m_ViewportPx = { 0.0f, 0.0f, 0.0f, 0.0f }; // x,y,w,h — the unclipped scissor / reset rect
    };
} // namespace Desert::Graphic::Render2D
