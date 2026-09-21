#include <Common/Core/DevInstruments.hpp>
#include <Engine/Graphic/MemoryReadout.hpp>
#include <Engine/Graphic/BRDFLut.hpp>
#include <Engine/Graphic/Renderer.hpp>

#include <Engine/Graphic/RendererContext.hpp>
#include <Engine/Graphic/RendererAPI.hpp>
#include <Engine/Core/EngineContext.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanImage.hpp>

#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Geometry/PrimitiveMeshFactory.hpp>

namespace Desert::Graphic
{
    static RendererAPI* s_RendererAPI = nullptr;

    [[nodiscard]] Common::BoolResultStr Renderer::InitGraphicAPI()
    {
        switch ( RendererAPI::GetAPIType() )
        {
            case RendererAPIType::Vulkan:
            {
                s_RendererAPI = new Graphic::API::Vulkan::VulkanRendererAPI(
                     EngineContext::GetInstance().GetWindow() );

                break;
            }

            // The ONLY other value of the enum, and it used to fall straight through to the
            // dereference below with s_RendererAPI still null. It is unreachable today —
            // RendererAPI::s_RenderingAPI is a static inline fixed at Vulkan with no setter anywhere in
            // the tree — so this is a latent null dereference, not a live one. It is also exactly the
            // kind of latency that ends the day someone adds the setter that the enum's existence
            // promises. This function already returns BoolResultStr; refusing by name costs nothing.
            case RendererAPIType::None:
                return Common::MakeError( "Renderer::InitGraphicAPI: no rendering API selected "
                                          "(RendererAPIType::None)" );
        }
        s_RendererAPI->Init();

        return BOOLSUCCESS;
    }

    Common::BoolResultStr Renderer::Init()
    {
        // Pull in the generated reflection TU (static lib would otherwise strip it) so all
        // REFLECT()-annotated types are registered before any editor / shader-upload use.
        Reflection::ForceLinkGeneratedReflection();
        LOG_INFO( "[Reflection] {} reflected type(s) registered",
                  Reflection::ReflectionRegistry::Get().All().size() );

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

            Graphic::TextureSpecification spec;
            spec.GenerateMips = false;

            constexpr uint32_t kLutSize    = 256;
            constexpr uint32_t kLutSamples = 512;
            // A failed LUT used to become a null texture here and travel on: IBL specular then sampled
            // nothing for the whole session, which is the same silent degradation the comment above
            // describes for the old on-disk BRDF_LUT.tga. Init can say so — it returns a result.
            auto lut = Texture2D::Create( spec, "BRDF_LUT (generated)", kLutSize, kLutSize,
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
        return s_RendererAPI->EndFrame();
    }

    [[nodiscard]] Common::BoolResultStr Renderer::BeginFrame()
    {
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
        // AND IT IS GATED AT THE CALL SITE, unlike the draw counter and the load ledger, because the
        // ARGUMENT is the instrument: `TakeFrameSample()` asks the device for its heap budgets and reads
        // the process's resident set. An empty `SampleFrame` would still pay for the query every frame.
        // The paragraph above says this reading is taken "in the shipped path" — it was written when
        // there was no configuration in which anything was NOT in the shipped path. A player's frame does
        // not pay for a number no player can read.
#if DESERT_DEV_INSTRUMENTS
        MemoryWatch::SampleFrame( MemoryReadout::TakeFrameSample() );
#endif

        return s_RendererAPI->BeginFrame();
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
        return s_RendererAPI->PresentFinalImage();
    }

    void Renderer::SubmitFullscreenQuad( const GraphicsPipeline* pipeline, const MaterialExecutor* materialExecutor )
    {
        s_RendererAPI->SubmitFullscreenQuad( pipeline, materialExecutor );
    }

    void Renderer::SubmitIndexed( const GraphicsPipeline* pipeline, VertexBuffer* vertexBuffer,
                                  IndexBuffer* indexBuffer, uint32_t indexCount, uint32_t firstIndex,
                                  const MaterialExecutor* materialExecutor )
    {
        s_RendererAPI->SubmitIndexed( pipeline, vertexBuffer, indexBuffer, indexCount, firstIndex,
                                      materialExecutor );
    }

    void Renderer::SubmitLines( const GraphicsPipeline* pipeline, uint32_t vertexCount, float lineWidth,
                                const MaterialExecutor* materialExecutor )
    {
        s_RendererAPI->SubmitLines( pipeline, vertexCount, lineWidth, materialExecutor );
    }

    void Renderer::SubmitVertices( const GraphicsPipeline* pipeline, uint32_t vertexCount,
                                   const MaterialExecutor* materialExecutor )
    {
        s_RendererAPI->SubmitVertices( pipeline, vertexCount, materialExecutor );
    }

    void Renderer::DispatchComputeInFrame( const ComputePipeline* pipeline, uint32_t groupCountX,
                                           uint32_t groupCountY, uint32_t groupCountZ )
    {
        s_RendererAPI->DispatchComputeInFrame( pipeline, groupCountX, groupCountY, groupCountZ );
    }

    void Renderer::DispatchComputeCull( const ComputePipeline* pipeline, uint32_t groupCountX,
                                        uint32_t groupCountY, uint32_t groupCountZ )
    {
        s_RendererAPI->DispatchComputeCull( pipeline, groupCountX, groupCountY, groupCountZ );
    }

    void Renderer::ComputeImageBeginWrite( Image* image )
    {
        s_RendererAPI->ComputeImageBeginWrite( image );
    }

    void Renderer::ComputeImageEndWrite( Image* image )
    {
        s_RendererAPI->ComputeImageEndWrite( image );
    }

    void Renderer::ComputeImageBeginRead( Image* image )
    {
        s_RendererAPI->ComputeImageBeginRead( image );
    }

    void Renderer::ComputeImageEndRead( Image* image )
    {
        s_RendererAPI->ComputeImageEndRead( image );
    }

    void Renderer::CopyDepthImage( Image2D* src, Image2D* dst )
    {
        s_RendererAPI->CopyDepthImage( src, dst );
    }

    void Renderer::SetScissor( int32_t x, int32_t y, uint32_t width, uint32_t height )
    {
        s_RendererAPI->SetScissor( x, y, width, height );
    }

    void Renderer::BeginRenderPass( const RenderPass* renderPass, bool clearFrame )
    {
        s_RendererAPI->BeginRenderPass( renderPass, clearFrame );
    }

    void Renderer::BeginSwapChainRenderPass()
    {
        s_RendererAPI->BeginSwapChainRenderPass();
    }

    void Renderer::EndRenderPass()
    {
        s_RendererAPI->EndRenderPass();
    }

    void Renderer::BeginDebugLabel( const char* name )
    {
        s_RendererAPI->BeginDebugLabel( name );
    }

    void Renderer::EndDebugLabel()
    {
        s_RendererAPI->EndDebugLabel();
    }

    void Renderer::ResizeWindowEvent( uint32_t width, uint32_t height )
    {
        s_RendererAPI->ResizeWindowEvent( width, height );
    }

    void Renderer::WaitDeviceIdle()
    {
        s_RendererAPI->WaitDeviceIdle();
    }

    void Renderer::RecreateImageSamplers()
    {
        // Idle first: we destroy/recreate VkSamplers that in-flight frames may still reference.
        WaitDeviceIdle();
        auto* imageService = Runtime::ResourceRegistry::GetImageService();
        for ( const auto& image : imageService->All() )
        {
            if ( auto* vulkanImage = dynamic_cast<API::Vulkan::IVulkanImage*>( image.get() ) )
                vulkanImage->RecreateSampler();
        }
        // Next frame, MaterialExecutor::Apply rebinds the new samplers into descriptor sets.
    }

    std::shared_ptr<Framebuffer> Renderer::GetCompositeFramebuffer()
    {
        return s_RendererAPI->GetCompositeFramebuffer();
    }

    void Renderer::RenderMesh( const GraphicsPipeline* pipeline, const Mesh* mesh, const glm::mat4 transform,
                               const MaterialExecutor* materialExecutor, uint32_t instanceCount,
                               uint32_t firstInstance, uint64_t hiddenSubmeshMask, uint32_t lodLevel )
    {
        s_RendererAPI->RenderMesh( pipeline, mesh, transform, materialExecutor, instanceCount, firstInstance,
                                   hiddenSubmeshMask, lodLevel );
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
        m_BRDFTexture.reset();
        Runtime::ResourceRegistry::ClearAll();
        Geometry::PrimitiveMeshFactory::ReleaseShared();

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

        s_RendererAPI->Shutdown();
        delete s_RendererAPI;
        s_RendererAPI = nullptr;
    }

    Desert::Graphic::RendererAPI* Renderer::GetRendererAPI() const
    {
        return s_RendererAPI;
    }
} // namespace Desert::Graphic
