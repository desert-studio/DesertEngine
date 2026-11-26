#include <Common/Core/DevInstruments.hpp>
#include <Common/Core/Profiler.hpp>
#include <Engine/Core/RenderTask.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Graphic/MemoryReadout.hpp>
#include <Engine/Graphic/BRDFLut.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/GeometryPool.hpp>
#include <Engine/Graphic/GpuMemoryBudget.hpp>
#include <Engine/Graphic/VirtualGeometry/VirtualMeshTable.hpp>
#include <Engine/Graphic/RendererClient.hpp>

#include <Engine/Graphic/RendererAPI.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Core/EngineContext.hpp>

#include <Engine/Graphic/Rhi/Rhi.hpp>

#include <Engine/Runtime/Services/Image/ImageService.hpp>
#include <Engine/Graphic/RendererContext.hpp>
#include <Common/Core/ResultStr.hpp>
#include <glm/glm.hpp>
#include <Common/Core/Memory/Buffer.hpp>
#include <Engine/Graphic/DefaultTextures.hpp>
#include <Engine/Graphic/FallbackTextures.hpp>
#include <Engine/Graphic/Shader.hpp>

namespace Desert::Graphic
{
    namespace
    {
        // The frame recorder the backend made at Init (IRhi::CreateRendererAPI); every static of the renderer
        // forwards to it.
        RendererAPI& Api()
        {
            return *Renderer::GetInstance().GetRendererAPI();
        }
    } // namespace

    Renderer::Renderer() = default;

    Renderer::~Renderer() = default;

    [[nodiscard]] Common::BoolResultStr Renderer::InitGraphicAPI()
    {
        if ( !m_Rhi )
            return Common::MakeError( "Renderer::Init: no rendering backend is attached (Renderer::AttachRhi "
                                      "runs before the window, from the RhiSelection)" );
        m_RendererAPI = m_Rhi->CreateRendererAPI( EngineContext::GetInstance().GetWindow() );
        m_RendererAPI->Init();

        return BOOLSUCCESS;
    }

    void Renderer::AttachClient( std::unique_ptr<IRendererClient> client )
    {
        m_Client = std::move( client );
    }

    Common::BoolResultStr Renderer::Init()
    {
        // The generated reflection is linked by the Application right before this call (Application::Application).
        if ( !m_Client )
            return Common::MakeError( "Renderer::Init: no renderer client is attached (Renderer::AttachClient runs "
                                      "before Init, from the Application: Core/EngineRendererClient)" );

        const auto& init = InitGraphicAPI();
        if ( !init )
        {
            return Common::MakeError( init.GetError() );
        }

        // The split-sum BRDF LUT is GENERATED at init (Karis integration, multithreaded) — no texture
        // file involved. The old on-disk BRDF_LUT.tga dependency was missing from the repo anyway, which
        // silently degraded IBL specular to the white-dummy fallback on every run.
        {
            // The device's own, and the LUT is the clearest case there is: it is computed, not read, so
            // there is no file to reload it from. See Engine/Graphic/ResourceLedger.hpp.
            const ResourceAttributionScope owned( ResourceOwner::Device );

            constexpr uint32_t kLutSize    = 256;
            constexpr uint32_t kLutSamples = 512;
            // A failed LUT used to become a null texture here and travel on: IBL specular then sampled
            // nothing for the whole session, which is the same silent degradation the comment above
            // describes for the old on-disk BRDF_LUT.tga. Init can say so — it returns a result.
            auto lut = Texture2D::Create( "BRDF_LUT (generated)", kLutSize, kLutSize,
                                          Core::Formats::ImageFormat::RGBA32F,
                                          GenerateBRDFLutRGBA32F( kLutSize, kLutSamples ) );
            if ( !lut )
            {
                return Common::MakeFormattedError<bool>(
                     "[Renderer] the generated BRDF LUT could not be created, so IBL specular would be "
                     "wrong for the whole session: {}",
                     lut.GetError() );
            }
            m_BRDFTexture = lut.ExtractValue();
            LOG_INFO( "[Renderer] BRDF LUT generated ({}x{}, {} samples)", kLutSize, kLutSize, kLutSamples );
        }

        return Common::MakeSuccess( true );
    }

    [[nodiscard]] Common::BoolResultStr Renderer::EndFrame()
    {
        return Api().EndFrame();
    }

    [[nodiscard]] Common::BoolResultStr Renderer::BeginFrame()
    {
        if ( const Common::BoolResultStr waited = WaitFrameSlot(); !waited )
            return waited;
        return BeginFrameRecording();
    }

    [[nodiscard]] Common::BoolResultStr Renderer::BeginFrameRecording()
    {
        return Api().BeginFrameRecording();
    }

    [[nodiscard]] Common::BoolResultStr Renderer::WaitFrameSlot()
    {
        // THE FRAME BOUNDARY'S FENCE (RenderTask::FrameEndSync) is not the first statement here any more
        // (FRAME-SLOT-1): this frame's submission list and command buffer are its slot's own
        // (VulkanRendererAPI::FrameSlotState), so the previous frame's job may still record into its slot. The
        // backend's BeginFrame joins it after the slot's GPU wait, before it binds the render graph's
        // single-slot frame objects; every path that begins a frame still passes it there.

        // THE MEMORY READING IS TAKEN HERE, ONCE A FRAME, IN THE SHIPPED PATH.
        //
        // Not in a HUD, and not behind a flag: the world programme's §0.4 acceptance criterion is about
        // the host the player runs, which draws no HUD at all, and a detector that only samples while an
        // editor panel is open measures the editor. Not in `EndFrame` either — the draw counters made
        // the same choice for the same reason: a frame that loses the device is never recorded, so work
        // hung off the end of a frame is skipped exactly when the numbers would have explained
        // something.
        //
        // BEFORE the backend call rather than after, so a frame the backend refuses still contributes
        // its reading. The refusal is the interesting frame.
        //
        // THE HEAP BUDGETS ARE READ IN EVERY CONFIGURATION, once a frame: GpuMemoryBudget divides them between
        // the texture pool and the shadow histories, in the player's frame as in the editor's. The rest of the
        // sample (the process's resident set, the watch) is gated at the call site, because the ARGUMENT is the
        // instrument: an empty `SampleFrame` would still pay for it.
        //
        // A ROW OF ITS OWN (THREAD-OVERLAP-1): the sample is a driver query (vkGetPhysicalDeviceMemoryProperties2
        // with the budget chain) and a process query, both inside "Renderer: BeginFrame" and outside the
        // backend's stage rows - unnamed, it would stay in that row's self time, the 0.78 ms the stages are
        // there to name.
#if DESERT_DEV_INSTRUMENTS
        {
            DESERT_PROFILE_SCOPE( "Memory: frame sample" );
            const bool wasOver = MemoryWatch::OverBudget();
            const MemoryReadout sample = MemoryReadout::TakeFrameSample();
            GpuMemoryBudget::Get().Update( sample.Device );
            MemoryWatch::SampleFrame( sample );
            // GRASS-VRAM-BUDGET-1: ONE line where the run enters the over-budget state and one where it leaves it
            // (VK_EXT_memory_budget): past the budget the driver pages video memory and the frame stalls wherever
            // the paged memory is next touched, which no pass's timing names.
            if ( const bool over = MemoryWatch::OverBudget(); over != wasOver )
            {
                constexpr double MB = 1024.0 * 1024.0;
                if ( over )
                {
                    LOG_INFO( "[Memory] device-local {:.0f} MB passed the VRAM budget {:.0f} MB: the driver pages "
                              "video memory and frames stall",
                              static_cast<double>( MemoryWatch::LastDeviceLocalUsage() ) / MB,
                              static_cast<double>( MemoryWatch::LastDeviceLocalBudget() ) / MB );
                }
                else
                {
                    LOG_INFO( "[Memory] device-local {:.0f} MB is back within the VRAM budget {:.0f} MB",
                              static_cast<double>( MemoryWatch::LastDeviceLocalUsage() ) / MB,
                              static_cast<double>( MemoryWatch::LastDeviceLocalBudget() ) / MB );
                }
            }
        }
#else
        if ( const std::shared_ptr<Engine::Device> device = EngineContext::GetInstance().GetDevice() )
            GpuMemoryBudget::Get().Update( device->QueryMemory() );
#endif

        if ( const Common::BoolResultStr waited = Api().WaitFrameSlot(); !waited )
            return waited;
        // Every host begins its frames here, so no host has to remember the environment cache's readbacks
        // (the engine's client pumps them: EngineRendererClient::OnFrameSlot).
        m_Client->OnFrameSlot();
        return BOOLSUCCESS;
    }

    uint32_t Renderer::GetCurrentFrameIndex()
    {
        return EngineContext::GetInstance().GetCurrentFrameIndex();
    }

    Common::Memory::CommandBuffer& Renderer::GetRenderCommandQueue()
    {
        static Common::Memory::CommandBuffer cmdBuffer;
        return cmdBuffer;
    }

    // THESE TWO USED TO RETURN void AND THROW THE BACKEND'S RESULT AWAY. `RendererAPI` declares both as
    // Common::BoolResultStr; this link discarded it, `Window` declared its own pair void, and
    // `Application::Run` called them as statements — so a failure at submit or present had three separate
    // places to disappear before anyone could read it. That is the "a middle link drops a property" shape,
    // and its concrete cost was that a lost device could only ever be discovered one frame later, by
    // something else.
    Common::BoolResultStr Renderer::PresentFinalImage()
    {
        // The other half of the frame boundary (RenderTask::FrameEndSync): the submit takes the frame's list as
        // it is and FrameManager::NextFrame moves the slot index, so no job of this frame may still record.
        ::Desert::Engine::RenderTask::Get().FrameEndSync();
        auto&                       frames    = ::Desert::Engine::FrameManager::GetInstance();
        const Common::BoolResultStr presented = Api().PresentFinalImage();
        // The frame is on the queue: a job of it that starts later is torn (RenderTask::ThreadMain).
        ::Desert::Engine::RenderTask::Get().NoteFrameSubmitted( frames.GetAbsoluteFrameCount() );
        // Presented inline (the game thread's serial frame, a loading frame, a tool): the game thread moves to the
        // next frame here. As the render task's job "Frame: Submit/Present" the game thread moved on as it handed
        // the frame over (Application::Run step 6), and this thread keeps the frame's own count.
        if ( presented.IsSuccess() && !::Desert::Engine::RenderTask::OnRenderThread() )
            frames.NextFrame();
        return presented;
    }

    void Renderer::SubmitPulled( const GraphicsPipeline* pipeline, uint32_t vertexCount, float lineWidth,
                                 const MaterialExecutor* materialExecutor )
    {
        Api().SubmitPulled( pipeline, vertexCount, lineWidth, materialExecutor );
    }

    RDG::ShaderBindingLayout Renderer::GetBindingLayout( const Shader& shader )
    {
        return Api().GetBindingLayout( shader );
    }

    std::shared_ptr<const RDG::OtherRouteFill> Renderer::GetPipelineRouteFill( const ComputePipeline& pipeline )
    {
        return Api().GetPipelineRouteFill( pipeline );
    }

    Common::BoolResultStr Renderer::DispatchCompute( const RDG::PassBindings& bindings,
                                                     const ComputePipeline& pipeline, uint32_t groupCountX,
                                                     uint32_t groupCountY, uint32_t groupCountZ )
    {
        return Api().DispatchCompute( bindings, pipeline, groupCountX, groupCountY, groupCountZ );
    }

    Common::BoolResultStr Renderer::DispatchComputeIndirect( const RDG::PassBindings& bindings,
                                                             const ComputePipeline&   pipeline,
                                                             const RDG::BufferRef args, const uint64_t offset )
    {
        return Api().DispatchComputeIndirect( bindings, pipeline, args, offset );
    }

    Common::BoolResultStr Renderer::TraceRays( const RDG::PassBindings&                   bindings,
                                               const RayTracingPipeline&                  pipeline,
                                               const RayTracing::RayTracingHitGroupTable& hits, uint32_t width,
                                               uint32_t height )
    {
        return Api().TraceRays( bindings, pipeline, hits, width, height );
    }

    Common::BoolResultStr Renderer::DrawFullscreen( const RDG::PassBindings& bindings,
                                                    const GraphicsPipeline&  pipeline,
                                                    const MaterialExecutor*  material )
    {
        return DrawProcedural( bindings, pipeline, material, kFullscreenTriangleVertexCount, 1u );
    }

    Common::BoolResultStr Renderer::DrawProcedural( const RDG::PassBindings& bindings,
                                                    const GraphicsPipeline&  pipeline,
                                                    const MaterialExecutor* material, uint32_t vertexCount,
                                                    uint32_t instanceCount, const std::span<const std::byte> push )
    {
        return Api().DrawProcedural( bindings, pipeline, material, push, vertexCount, instanceCount );
    }

    Common::BoolResultStr Renderer::DrawProceduralIndirect( const RDG::PassBindings& bindings,
                                                            const GraphicsPipeline&  pipeline,
                                                            const MaterialExecutor*  material,
                                                            const RDG::BufferRef args, const uint64_t offset,
                                                            const std::span<const std::byte> push )
    {
        return Api().DrawProceduralIndirect( bindings, pipeline, material, push, args, offset );
    }

    Common::BoolResultStr Renderer::DrawProceduralIndexed( const RDG::PassBindings& bindings,
                                                           const GraphicsPipeline&  pipeline,
                                                           const MaterialExecutor*  material,
                                                           IndexBuffer&             indexBuffer,
                                                           const std::span<const ProceduralIndexedRange> ranges,
                                                           const std::span<const std::byte>              push )
    {
        return Api().DrawProceduralIndexed( bindings, pipeline, material, push, indexBuffer, ranges );
    }

    Common::BoolResultStr Renderer::DrawIndexed( const RDG::PassBindings& bindings,
                                                 const GraphicsPipeline&  pipeline,
                                                 const MaterialExecutor* material, VertexBuffer& vertexBuffer,
                                                 IndexBuffer& indexBuffer, uint32_t indexCount,
                                                 uint32_t firstIndex )
    {
        return Api().DrawIndexed( bindings, pipeline, material, vertexBuffer, indexBuffer, indexCount,
                                           firstIndex );
    }

    Common::BoolResultStr Renderer::RenderMesh( const RDG::PassBindings& bindings,
                                                const GraphicsPipeline& pipeline, const Mesh& mesh,
                                                const glm::mat4& transform, const MaterialExecutor& material,
                                                const std::span<const std::byte> push, uint32_t instanceCount,
                                                uint32_t firstInstance, SubmeshMask hiddenSubmeshMask,
                                                uint32_t lodLevel )
    {
        return Api().RenderMesh( bindings, pipeline, mesh, transform, material, push, instanceCount,
                                          firstInstance, hiddenSubmeshMask, lodLevel );
    }

    Common::BoolResultStr
    Renderer::RenderMeshIndirect( const RDG::PassBindings& bindings, const GraphicsPipeline& pipeline,
                                  const Mesh& mesh, const glm::mat4& transform, const MaterialExecutor& material,
                                  const std::span<const std::byte> push, const RDG::BufferRef args,
                                  const uint64_t offset, const SubmeshMask hiddenSubmeshMask,
                                  const MeshDrawArgumentsOrder order, const uint32_t groups )
    {
        return Api().RenderMeshIndirect( bindings, pipeline, mesh, transform, material, push, args,
                                                  offset, hiddenSubmeshMask, order, groups );
    }

    Common::BoolResultStr
    Renderer::RenderMeshIndirectCount( const RDG::PassBindings& bindings, const GraphicsPipeline& pipeline,
                                       const Mesh& mesh, const glm::mat4& transform,
                                       const MaterialExecutor& material, const std::span<const std::byte> push,
                                       const RDG::BufferRef args, const uint64_t offset,
                                       const RDG::BufferRef count, const uint64_t countOffset,
                                       const uint32_t maxDraws,
                                       const SubmeshMask hiddenSubmeshMask )
    {
        return Api().RenderMeshIndirectCount( bindings, pipeline, mesh, transform, material, push, args,
                                                       offset, count, countOffset, maxDraws, hiddenSubmeshMask );
    }

    Common::BoolResultStr Renderer::RenderMeshIndirectCountBins( const RDG::PassBindings&       bindings,
                                                                 const std::span<const BinDraw> bins )
    {
        return Api().RenderMeshIndirectCountBins( bindings, bins );
    }

    void Renderer::ComputeImageBeginWrite( Image* image )
    {
        Api().ComputeImageBeginWrite( image );
    }

    void Renderer::ComputeImageEndWrite( Image* image )
    {
        Api().ComputeImageEndWrite( image );
    }

    void Renderer::ComputeImageBeginRead( Image* image )
    {
        Api().ComputeImageBeginRead( image );
    }

    void Renderer::ComputeImageEndRead( Image* image )
    {
        Api().ComputeImageEndRead( image );
    }

    Common::BoolResultStr Renderer::CopyDepthImage( Image2D* src, Image2D* dst )
    {
        return Api().CopyDepthImage( src, dst );
    }

    Common::BoolResultStr Renderer::CopyGraphDepthToLayer( const RDG::PassContext&           context,
                                                           const RDG::TextureRef             source,
                                                           const RDG::TextureRef             target,
                                                           const uint32_t                    targetLayer,
                                                           const std::span<const glm::uvec4> regions )
    {
        return Api().CopyGraphDepthToLayer( context, source, target, targetLayer, regions );
    }

    Common::BoolResultStr Renderer::ScrollGraphDepthIntoLayer( const RDG::PassContext& context,
                                                               const RDG::TextureRef   source,
                                                               const RDG::TextureRef   target,
                                                               const uint32_t          targetLayer,
                                                               const int32_t           shiftX,
                                                               const int32_t           shiftY,
                                                               const float             clearDepth )
    {
        return Api().ScrollGraphDepthIntoLayer( context, source, target, targetLayer, shiftX, shiftY,
                                                         clearDepth );
    }

    Common::BoolResultStr Renderer::CopyGraphLayerToDepth( const RDG::PassContext& context,
                                                           const RDG::TextureRef   source,
                                                           const uint32_t          sourceLayer,
                                                           const RDG::TextureRef   target )
    {
        return Api().CopyGraphLayerToDepth( context, source, sourceLayer, target );
    }

    Common::BoolResultStr Renderer::CopyGraphBufferToTexture( const RDG::PassContext& context,
                                                              const RDG::BufferRef    source,
                                                              const RDG::TextureRef   target )
    {
        return Api().CopyGraphBufferToTexture( context, source, target );
    }

    void Renderer::SetScissor( int32_t x, int32_t y, uint32_t width, uint32_t height )
    {
        Api().SetScissor( x, y, width, height );
    }

    void Renderer::SetViewportRegion( int32_t x, int32_t y, uint32_t width, uint32_t height )
    {
        Api().SetViewportRegion( x, y, width, height );
    }

    void Renderer::ClearRegion( const Framebuffer& framebuffer, int32_t x, int32_t y, uint32_t width,
                                uint32_t height, const glm::vec4& color, float depth )
    {
        Api().ClearRegion( framebuffer, x, y, width, height, color, depth );
    }

    Common::BoolResultStr Renderer::BeginRenderPass( const RenderPass* renderPass, bool clearFrame )
    {
        return Api().BeginRenderPass( renderPass, clearFrame );
    }

    void Renderer::EndRenderPass()
    {
        Api().EndRenderPass();
    }

    Common::BoolResultStr Renderer::ExecuteGraph( RDG::Builder& graph )
    {
        // A graph the game thread records while the views record (a panel's preview) lands after them (RenderTask).
        ::Desert::Engine::RenderTask::RecordGuard( graph.GetName() );
        return Api().ExecuteGraph( graph );
    }

    Common::BoolResultStr Renderer::ImportImage( const std::shared_ptr<Image>& image, RDG::ExternalTexture& into )
    {
        return Api().ImportImage( image, into );
    }

    Common::BoolResultStr Renderer::ImportBackBuffer( RDG::ExternalTexture& into )
    {
        return Api().ImportBackBuffer( into );
    }

    Common::BoolResultStr Renderer::ImportBuffer( const std::shared_ptr<ShaderResources::StorageBuffer>& buffer,
                                                  RDG::ExternalBuffer&                                   into )
    {
        return Api().ImportBuffer( buffer, into );
    }

    Common::ResultStr<std::shared_ptr<SharedGpuBuffer>> Renderer::CreateSharedBuffer( const std::string_view name,
                                                                                      const uint64_t bytes )
    {
        return Api().CreateSharedBuffer( name, bytes );
    }

    Common::ResultStr<std::shared_ptr<SharedGpuTimeline>>
    Renderer::CreateSharedTimeline( const std::string_view name )
    {
        return Api().CreateSharedTimeline( name );
    }

    Common::BoolResultStr Renderer::ImportVertexBuffer( const std::shared_ptr<VertexBuffer>& buffer,
                                                        RDG::ExternalBuffer&                 into )
    {
        return Api().ImportVertexBuffer( buffer, into );
    }

    Common::BoolResultStr Renderer::ImportVertexBufferRange( const std::shared_ptr<VertexBuffer>& buffer,
                                                             const uint64_t offset, const uint64_t bytes,
                                                             RDG::ExternalBuffer& into )
    {
        return Api().ImportVertexBufferRange( buffer, offset, bytes, into );
    }

    void Renderer::InvalidateGeometryBinding()
    {
        Api().InvalidateGeometryBinding();
    }

    Common::BoolResultStr Renderer::ImportMeshVertices( const MeshGeometry& geometry, RDG::ExternalBuffer& into )
    {
        if ( !geometry.IsResident() )
            return Common::MakeError( "ImportMeshVertices: the mesh has no resident vertices" );
        return Api().ImportVertexBufferRange(
             geometry.Vertices, geometry.VertexByteOffset,
             uint64_t{ geometry.VertexCount } * uint64_t{ geometry.VertexStride }, into );
    }

    Common::BoolResultStr Renderer::ImportMeshStreams( const MeshGeometry& geometry, RDG::ExternalBuffer& into )
    {
        if ( !geometry.IsResident() || !geometry.Streams )
            return Common::MakeError( "ImportMeshStreams: the mesh has no resident stream buffer" );
        return Api().ImportVertexBufferRange(
             geometry.Streams, geometry.StreamByteOffset,
             uint64_t{ geometry.VertexCount } * uint64_t{ kGeometryStreamStride }, into );
    }

    void Renderer::BeginDebugLabel( const char* name )
    {
        Api().BeginDebugLabel( name );
    }

    void Renderer::EndDebugLabel()
    {
        Api().EndDebugLabel();
    }

    void Renderer::ResizeWindowEvent( uint32_t width, uint32_t height )
    {
        Api().ResizeWindowEvent( width, height );
    }

    void Renderer::WaitDeviceIdle()
    {
        Api().WaitDeviceIdle();
    }

    void Renderer::RecreateImageSamplers()
    {
        // Each image re-points at the sampler the new setting names (the device's VulkanSamplerCache); the old
        // sampler is never destroyed here. Idle first anyway: the image descriptors are rewritten next frame.
        WaitDeviceIdle();
        auto* imageService = Runtime::GetImageService();
        imageService->ForEachImage(
             []( Image& image )
             {
                 image.RecreateSampler();
             } );
        // Next frame, MaterialExecutor::Apply rebinds the new samplers into descriptor sets.
    }

    std::shared_ptr<Framebuffer> Renderer::GetCompositeFramebuffer()
    {
        return Api().GetCompositeFramebuffer();
    }

    const std::shared_ptr<Desert::Graphic::Texture2D>& Renderer::GetBRDFTexture() const
    {
        return m_BRDFTexture;
    }

    void Renderer::Shutdown()
    {
        // Mirror image of Init(), and called from ~Application so it runs inside main. Everything below
        // is owned by a static that outlives the Application object; a static destructor cannot be
        // ordered against the device, so the release has to be said out loud here instead.
        // THE ORDER (unchanged by BINT-RC, which moved the engine's steps behind IRendererClient):
        //   1. the engine drains its GPU work in flight (the environment cache's readbacks own staging buffers
        //      and command buffers: finished while the device is alive) - OnShutdownDrain;
        //   2. the BRDF LUT;
        //   3. the engine empties its resources: ResourceRegistry::ClearAll, then the shared primitive meshes -
        //      OnShutdownRelease;
        //   4. fallback textures, default textures, the geometry pool, the virtual mesh table, the backend.
        if ( !m_Client )
        {
            LOG_ERROR( "[Renderer] Shutdown without Init: no renderer client is attached, nothing to release" );
            return;
        }
        m_Client->OnShutdownDrain();

        m_BRDFTexture.reset();
        m_Client->OnShutdownRelease();

        if ( const auto released = FallbackTextures::Get().Release(); !released )
        {
            LOG_ERROR( "[Renderer] fallback textures were not released: {}", released.GetError() );
        }

        // The schema's own default textures (Graphic/DefaultTextures.hpp) are a separate table with the
        // same lifetime problem: they are 1x1 GPU images owned by a static, so the device would be gone
        // by the time a static destructor ran.
        if ( const auto released = DefaultTextures::Get().Release(); !released )
        {
            LOG_ERROR( "[Renderer] schema default textures were not released: {}", released.GetError() );
        }

        // The pool's arena buffers are device memory owned by a static: dropped here, before the device.
        GeometryPool::Get().RT_Shutdown();
        VirtualGeometry::VirtualMeshTable::Get().RT_Shutdown();

        m_RendererAPI->Shutdown();
        m_RendererAPI.reset();
    }

    Desert::Graphic::RendererAPI* Renderer::GetRendererAPI() const
    {
        return m_RendererAPI.get();
    }

    void Renderer::AttachRhi( std::unique_ptr<IRhi> rhi )
    {
        DESERT_VERIFY( rhi && !m_Rhi, "Renderer::AttachRhi: one backend per process, attached once" );
        m_Rhi = std::move( rhi );
    }

    IRhi& Renderer::Rhi()
    {
        return *m_Rhi;
    }
} // namespace Desert::Graphic

template <>
std::unique_ptr<Desert::Graphic::Renderer>& Common::Singleton<Desert::Graphic::Renderer>::Instance()
{
    static std::unique_ptr<Desert::Graphic::Renderer> instance;
    return instance;
}
