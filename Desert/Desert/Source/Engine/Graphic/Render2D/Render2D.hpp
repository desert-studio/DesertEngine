#pragma once

#include <Engine/Graphic/Render2D/DrawList2D.hpp>
#include <Engine/Graphic/Render2D/UIMaterialCache.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <memory>
#include <unordered_map>

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
} // namespace Desert::Graphic

namespace Desert::Graphic::Render2D
{
    // GPU backend for the 2D batcher: owns the UI2D pipeline, one growable dynamic vertex+index buffer and a
    // 1x1 white texture, and turns a DrawList2D into draw calls. Callers record primitives into GetDrawList()
    // between BeginFrame() and Flush(); Flush() uploads the geometry once and issues one Renderer::SubmitIndexed
    // per state batch. Flush() must run INSIDE an active render pass (the UI-phase pass into the scene target).
    class Render2D
    {
    public:
        Render2D() = default;
        // Releases the white texture's image from the ImageService (see the definition for why).
        ~Render2D();
        // One owner of the white texture's registration: a copy would release it twice.
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
        void Flush();

        // This backend's UI-material cache. The canvas walk resolves an element's `.demat` through it and
        // hands the resolved entry to DrawList2D::AddMaterialRect; Flush then draws with that entry's own
        // pipeline. It lives HERE and not behind a service because a pipeline belongs to one framebuffer's
        // render pass, and this object is the only thing that knows which framebuffer that is.
        UIMaterialCache& Materials()
        {
            return m_MaterialCache;
        }

        // The blurred scene snapshot glass rects sample (BackdropBlurRenderer's pyramid), and how many LODs
        // it has. Set every frame by the host pass; null = glass falls back to a flat tinted panel.
        void SetBackdrop( Image2D* image, uint32_t maxLod )
        {
            m_Backdrop       = image;
            m_BackdropMaxLod = maxLod;
        }

        // Did the last Flush() draw any glass? The host reports this to the SceneRenderer so the blur
        // pyramid is only built for canvases that actually use it.
        [[nodiscard]] bool UsedBackdrop() const
        {
            return m_UsedBackdrop;
        }

    private:
        // Grow the dynamic buffers to hold at least the given counts (reused across frames otherwise).
        void EnsureCapacity( uint32_t vertexCount, uint32_t indexCount );

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
        std::shared_ptr<VertexBuffer>     m_VertexBuffer;
        std::shared_ptr<IndexBuffer>      m_IndexBuffer;
        uint32_t                          m_VertexCapacity = 0;
        uint32_t                          m_IndexCapacity  = 0;

        std::shared_ptr<Texture2D> m_WhiteTexture;
        Image2D*                   m_WhiteImage = nullptr;

        UIMaterialCache m_MaterialCache; // UI-domain `.demat` fills, keyed by asset handle

        ExecutorCache m_Executors;      // UI2D, keyed by bound Image2D* (null => white)
        ExecutorCache m_TextExecutors;  // UIText, keyed by font atlas Image2D*
        ExecutorCache m_GlassExecutors; // UIGlass, keyed by the backdrop Image2D*

        Image2D* m_Backdrop       = nullptr; // blurred scene snapshot (not owned)
        uint32_t m_BackdropMaxLod = 0;
        bool     m_UsedBackdrop   = false; // glass drawn in the last Flush -> keep the pyramid alive

        DrawList2D m_DrawList;
        glm::mat4  m_Projection = glm::mat4( 1.0f );
        glm::vec4  m_ViewportPx = { 0.0f, 0.0f, 0.0f, 0.0f }; // x,y,w,h — the unclipped scissor / reset rect
    };
} // namespace Desert::Graphic::Render2D
