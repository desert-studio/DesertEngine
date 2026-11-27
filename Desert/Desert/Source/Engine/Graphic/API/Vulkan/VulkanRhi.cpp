#include <Engine/Graphic/API/Vulkan/VulkanRhi.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanFallbackTextures.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanFramebuffer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanGpuBatch.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanImage.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanIndexBuffer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanMaterialBackend.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanMipMapGenerator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanPipeline.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanPipelineCompute.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRayTracingGeometry.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRayTracingPipeline.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanShader.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanSwapChain.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanVertexBuffer.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/ShaderResources/API/Vulkan/VulkanStorageBuffer.hpp>
#include <Engine/ShaderResources/API/Vulkan/VulkanUniformBuffer.hpp>
#include <Engine/ShaderResources/API/Vulkan/VulkanUniformImage2D.hpp>
#include <Engine/ShaderResources/API/Vulkan/VulkanUniformImageCube.hpp>

#include <Common/Core/Logger.hpp>

#include <format>

namespace Desert::Graphic::API::Vulkan
{
    namespace VulkanSR = ::Desert::ShaderResources::API::Vulkan;

    VulkanRhi::VulkanRhi( RhiSelection selection ) : m_Selection( std::move( selection ) )
    {
    }

    VulkanRhi::~VulkanRhi() = default;

    RhiKind VulkanRhi::Kind() const noexcept
    {
        return RhiKind::Vulkan;
    }

    const RhiSelection& VulkanRhi::Selection() const noexcept
    {
        return m_Selection;
    }

    Common::ResultStr<std::shared_ptr<Engine::Device>> VulkanRhi::CreateDevice()
    {
        auto device = VulkanLogicalDevice::Create( m_Selection );
        if ( !device )
            return Common::MakeError<std::shared_ptr<Engine::Device>>( device.GetError() );
        return Common::MakeSuccess<std::shared_ptr<Engine::Device>>( device.ExtractValue() );
    }

    std::unique_ptr<RendererAPI> VulkanRhi::CreateRendererAPI( const std::shared_ptr<Window>& window )
    {
        return std::make_unique<VulkanRendererAPI>( window );
    }

    std::shared_ptr<RendererContext> VulkanRhi::CreateRendererContext( const std::shared_ptr<Window>& window )
    {
        auto context = std::make_shared<VulkanContext>( window );
        m_Context    = context;
        return context;
    }

    std::shared_ptr<SwapChain> VulkanRhi::CreateSwapChain( const GLFWwindow* window )
    {
        return std::make_shared<VulkanSwapChain>( window );
    }

    std::shared_ptr<Image2D> VulkanRhi::CreateImage2D( const Core::Formats::Image2DSpecification& spec )
    {
        const auto& image  = std::make_shared<VulkanImage2D>( spec );
        const auto  result = image->RT_Invalidate();
        if ( !result.IsSuccess() )
        {
            // ASKED, NOT ASSUMED: handing back a half-built image pushes the failure into the first pass that
            // samples it, with no connection to the allocation that actually failed. Null is inside
            // Image2D::Create's declared contract.
            LOG_ERROR( "Image2D::Create: image {}x{} failed: {}", spec.Width, spec.Height, result.GetError() );
            return nullptr;
        }
        // THE SIZE IS RECORDED BY THE BACKEND'S ALLOCATION, NOT HERE: seven sites (2 in VulkanFramebuffer.cpp, 5
        // in VulkanFallbackTextures.cpp) construct the backend image directly, which is how 320 MiB of shadow
        // cascades once reported zero bytes. See VulkanImage.cpp, beside the allocation.
        return image;
    }

    std::shared_ptr<ImageCube> VulkanRhi::CreateImageCube( const Core::Formats::ImageCubeSpecification& spec )
    {
        const auto& image  = std::make_shared<VulkanImageCube>( spec );
        const auto  result = image->RT_Invalidate();
        if ( !result.IsSuccess() )
        {
            LOG_ERROR( "ImageCube::Create: cube '{}' with a {}-texel face failed: {}", spec.Tag, spec.FaceSize,
                       result.GetError() );
            return nullptr;
        }
        // Size recorded by the allocation — see VulkanImage.cpp and the note in CreateImage2D.
        return image;
    }

    std::shared_ptr<Image3D> VulkanRhi::CreateImage3D( const Core::Formats::Image3DSpecification& spec )
    {
        const auto& image  = std::make_shared<VulkanImage3D>( spec );
        const auto  result = image->RT_Invalidate();
        if ( !result.IsSuccess() )
        {
            // Handing back a half-built volume would push the failure into the first dispatch that binds it, with
            // no connection to the allocation that actually failed.
            LOG_ERROR( "Image3D::Create: volume '{}' {}x{}x{} failed: {}", spec.Tag, spec.Width, spec.Height,
                       spec.Depth, result.GetError() );
            return nullptr;
        }
        // Size recorded by the allocation — see VulkanImage.cpp and the note in CreateImage2D.
        return image;
    }

    std::unique_ptr<MipMapCubeGenerator> VulkanRhi::CreateMipMapCubeGenerator( const MipGenStrategy strategy )
    {
        if ( strategy == MipGenStrategy::ComputeShader )
        {
            DESERT_VERIFY( false, "ComputeShader was not impl" );
        }
        return std::make_unique<VulkanMipMapCubeGeneratorTO>();
    }

    std::shared_ptr<Framebuffer> VulkanRhi::CreateFramebuffer( const FramebufferSpecification& spec )
    {
        return std::make_shared<VulkanFramebuffer>( spec );
    }

    std::shared_ptr<IndexBuffer> VulkanRhi::CreateIndexBuffer( const void* data, const uint32_t size,
                                                               const BufferUsage usage )
    {
        return std::make_shared<VulkanIndexBuffer>( data, size, usage );
    }

    std::shared_ptr<IndexBuffer> VulkanRhi::CreateIndexBuffer( const uint32_t size, const BufferUsage usage )
    {
        return std::make_shared<VulkanIndexBuffer>( size, usage );
    }

    std::shared_ptr<VertexBuffer> VulkanRhi::CreateVertexBuffer( const void* data, const uint32_t size,
                                                                 const BufferUsage usage )
    {
        return std::make_shared<VulkanVertexBuffer>( data, size, usage );
    }

    std::shared_ptr<VertexBuffer> VulkanRhi::CreateVertexBuffer( const uint32_t size, const BufferUsage usage )
    {
        return std::make_shared<VulkanVertexBuffer>( size, usage );
    }

    std::shared_ptr<ShaderResources::StorageBuffer> VulkanRhi::CreateStorageBuffer( const std::string_view name,
                                                                                    const uint32_t         size,
                                                                                    const uint32_t         binding,
                                                                                    const bool persistent )
    {
        // A BUFFER THAT DID NOT BUILD DOES NOT LEAVE THIS FUNCTION. Its constructor allocates and maps every copy
        // and refuses as a whole (Г13), so an object that failed has no device buffer, no mapping and no
        // descriptor to offer; every caller already tests the result for null. Handing one back would put the
        // failure at the descriptor write instead of here, where the size and the name are.
        auto       buffer = std::make_shared<VulkanSR::VulkanStorageBuffer>( name, size, binding, persistent );
        const auto usable = buffer->EnsureMapped();
        if ( !usable.IsSuccess() )
        {
            LOG_ERROR( "[StorageBuffer] '{}' ({} bytes, binding {}) is not usable and was not created: {}", name,
                       size, binding, usable.GetError() );
            return nullptr;
        }
        return buffer;
    }

    std::shared_ptr<ShaderResources::StorageBuffer>
    VulkanRhi::CreateReadbackStorageBuffer( const std::string_view name, const uint32_t size,
                                            const uint32_t binding )
    {
        // As CreateStorageBuffer: a buffer whose active copy cannot be made and mapped does not leave here.
        auto buffer = std::make_shared<VulkanSR::VulkanStorageBuffer>(
             name, size, binding, ShaderResources::Persistence::PerFrameReadback );
        const auto usable = buffer->EnsureMapped();
        if ( !usable.IsSuccess() )
        {
            LOG_ERROR(
                 "[StorageBuffer] read-back '{}' ({} bytes, binding {}) is not usable and was not created: {}",
                 name, size, binding, usable.GetError() );
            return nullptr;
        }
        return buffer;
    }

    std::shared_ptr<ShaderResources::StorageBuffer>
    VulkanRhi::CreateDeviceLocalStorageBuffer( const std::string_view name, const uint32_t size,
                                               const uint32_t binding )
    {
        // As CreateStorageBuffer: a buffer that did not build does not leave here. It has no mapping to ask about
        // (EnsureMapped refuses by design), so the question is whether its one device buffer was made -
        // PrepareForRecording answers it for a buffer shared by every frame.
        auto       buffer = std::make_shared<VulkanSR::VulkanStorageBuffer>( name, size, binding,
                                                                             ShaderResources::Persistence::DeviceOnly );
        const auto usable = buffer->PrepareForRecording();
        if ( !usable.IsSuccess() )
        {
            LOG_ERROR( "[StorageBuffer] device-local '{}' ({} bytes, binding {}) is not usable and was not "
                       "created: {}",
                       name, size, binding, usable.GetError() );
            return nullptr;
        }
        return buffer;
    }

    std::shared_ptr<ShaderResources::UniformBuffer>
    VulkanRhi::CreateUniformBuffer( const ShaderResources::ShaderLayout::UniformBuffer& uniform )
    {
        // Refused rather than handed back half-built — CreateStorageBuffer's argument, word for word.
        auto       buffer = std::make_shared<VulkanSR::VulkanUniformBuffer>( uniform );
        const auto usable = buffer->EnsureMapped();
        if ( !usable.IsSuccess() )
        {
            LOG_ERROR( "[UniformBuffer] '{}' ({} bytes) is not usable and was not created: {}", uniform.Name,
                       uniform.Size, usable.GetError() );
            return nullptr;
        }
        return buffer;
    }

    std::shared_ptr<ShaderResources::UniformImage2D> VulkanRhi::CreateUniformImage2D( const std::string_view name,
                                                                                      const uint32_t binding )
    {
        return std::make_shared<VulkanSR::VulkanUniformImage2D>( name, binding );
    }

    std::shared_ptr<ShaderResources::UniformImageCube>
    VulkanRhi::CreateUniformImageCube( const std::string_view name, const uint32_t binding )
    {
        return std::make_shared<VulkanSR::VulkanUniformImageCube>( name, binding );
    }

    std::shared_ptr<Shader> VulkanRhi::CreateShader( const Assets::Asset<Assets::ShaderAsset>& asset,
                                                     const ShaderVariant& variant, const std::string& pass )
    {
        return std::make_shared<VulkanShader>( asset, variant, pass );
    }

    std::shared_ptr<Shader> VulkanRhi::CreateShaderFromMap( const Assets::Asset<Assets::ShaderAsset>& asset,
                                                            const ShaderVariant& variant, const std::string& pass,
                                                            const Core::ShaderMap& built )
    {
        return std::make_shared<VulkanShader>( asset, variant, pass, built );
    }

    Common::ResultStr<std::shared_ptr<GraphicsPipeline>>
    VulkanRhi::CreateGraphicsPipeline( const GraphicsPipelineSpecification& spec )
    {
        auto pipeline = std::make_shared<VulkanPipeline>( spec );
        pipeline->Invalidate();
        // THE HANDLE, NOT A FLAG. Invalidate's own refusal (a program with no vertex stage, e.g. a compute shader
        // reached by name) leaves the VkPipeline null, and asking the object what it built is the one question
        // that cannot drift away from what it did.
        if ( pipeline->GetVkPipeline() == VK_NULL_HANDLE )
        {
            return Common::MakeError<std::shared_ptr<GraphicsPipeline>>(
                 "GraphicsPipeline '" + spec.DebugName +
                 "': the Vulkan pipeline was not built (see the error above)." );
        }
        return Common::MakeSuccess<std::shared_ptr<GraphicsPipeline>>( std::move( pipeline ) );
    }

    Common::ResultStr<std::shared_ptr<GraphicsPipeline>>
    VulkanRhi::CreateGraphicsPipelineAsync( const GraphicsPipelineSpecification& spec, const PipelineRole role )
    {
        auto pipeline = std::make_shared<VulkanPipeline>( spec );
        pipeline->InvalidateAsync( role );
        // UNBUILT here means the leaf refused before any compile was handed out (its reason is logged);
        // Compiling/Built/Failed all mean the driver has it.
        if ( pipeline->GetBuildState() == VulkanPipeline::BuildState::Unbuilt )
        {
            return Common::MakeError<std::shared_ptr<GraphicsPipeline>>(
                 "GraphicsPipeline '" + spec.DebugName +
                 "': the Vulkan pipeline was not built (see the error above)." );
        }
        return Common::MakeSuccess<std::shared_ptr<GraphicsPipeline>>( std::move( pipeline ) );
    }

    Common::ResultStr<std::shared_ptr<ComputePipeline>>
    VulkanRhi::CreateComputePipeline( const ComputePipelineSpecification& spec )
    {
        auto pipeline = std::make_shared<VulkanPipelineCompute>( spec );
        pipeline->Invalidate();
        // THE HANDLE, NOT A FLAG: Invalidate's own refusal (a shader with no compute stage) leaves it null.
        if ( pipeline->GetVkPipeline() == VK_NULL_HANDLE )
        {
            return Common::MakeError<std::shared_ptr<ComputePipeline>>(
                 std::string( "ComputePipeline '" ) + spec.DebugName +
                 "': the Vulkan pipeline was not built (see the error above)." );
        }
        return Common::MakeSuccess<std::shared_ptr<ComputePipeline>>( std::move( pipeline ) );
    }

    Common::ResultStr<std::shared_ptr<RayTracingPipeline>>
    VulkanRhi::CreateRayTracingPipeline( const RayTracingPipelineSpecification& spec )
    {
        using Result  = std::shared_ptr<RayTracingPipeline>;
        auto pipeline = std::make_shared<VulkanRayTracingPipeline>( spec );
        if ( const Common::BoolResultStr built = pipeline->Build(); !built )
            return Common::MakeError<Result>( built.GetError() );
        return Common::MakeSuccess<Result>( std::move( pipeline ) );
    }

    std::unique_ptr<MaterialBackend> VulkanRhi::CreateMaterialBackend( const std::shared_ptr<Shader>& shader )
    {
        return std::make_unique<VulkanMaterialBackend>( shader );
    }

    Common::ResultStr<std::unique_ptr<GpuBatch>> VulkanRhi::BeginGpuBatch()
    {
        return VulkanGpuBatch::Begin();
    }

    FallbackTextures& VulkanRhi::Fallbacks()
    {
        if ( !m_Fallbacks )
        {
            // The device's own, whichever view first asked for one (RT-LEDGER-4): the set outlives every view.
            const ResourceAttributionScope owned( ResourceOwner::Device );
            m_Fallbacks = std::make_unique<VulkanFallbackTextures>();
        }
        return *m_Fallbacks;
    }

    Common::ResultStr<std::shared_ptr<RayTracing::RayTracingGeometryResource>>
    VulkanRhi::BuildStaticRayTracingGeometry( const RayTracing::RayTracingMeshInput& input )
    {
        using Result = std::shared_ptr<RayTracing::RayTracingGeometryResource>;
        auto built   = VulkanRayTracingGeometry::BuildStatic( input );
        if ( !built )
            return Common::MakeError<Result>( built.GetError() );
        return Common::MakeSuccess( Result( built.GetValue() ) );
    }

    std::size_t VulkanRhi::DrainDeferredDestruction()
    {
        const auto context = m_Context.lock();
        // No context = no allocator was ever made, so nothing was ever queued for destruction.
        if ( !context || !context->GetVulkanAllocator() )
            return 0;
        return context->GetVulkanAllocator()->DrainDeletionQueue();
    }

    void VulkanRhi::WriteAllocationLedger( std::string& out ) const
    {
        const auto context = m_Context.lock();
        if ( !context || !context->GetVulkanAllocator() )
        {
            out += "[AllocLedger] the Vulkan context has no allocator yet; nothing is allocated\n";
            return;
        }
        const auto& ledger = context->GetVulkanAllocator()->Ledger();
        out += std::format( "[AllocLedger] {} live allocation(s), {:.2f} MiB\n", ledger.LiveCount(),
                            static_cast<double>( ledger.LiveBytes() ) / ( 1024.0 * 1024.0 ) );
        for ( const auto& row : ledger.ByTag() )
            out += std::format( "[AllocLedger] tag '{}': {} x, {} B\n", row.Tag, row.Count, row.Bytes );
    }

    void VulkanRhi::BeginSceneOpenMeasure( const std::string_view scene )
    {
        if ( const auto context = m_Context.lock(); context && context->GetVulkanAllocator() )
            context->GetVulkanAllocator()->BeginSceneOpen( scene );
    }

    void VulkanRhi::SampleSceneOpenMeasure()
    {
        if ( const auto context = m_Context.lock(); context && context->GetVulkanAllocator() )
            context->GetVulkanAllocator()->SampleSceneOpen();
    }
} // namespace Desert::Graphic::API::Vulkan
