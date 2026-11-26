#pragma once

#include <Engine/Graphic/Rhi/Rhi.hpp>

#include <memory>

namespace Desert::Graphic::API::Vulkan
{
    class VulkanContext;

    // THE VULKAN BACKEND (IRhi): every body the former `case RendererAPIType::Vulkan:` arms held, one per virtual.
    // Owns what the backend alone builds for the process lifetime (the fallback set) and remembers the context it
    // created, so the allocator questions (SceneWorkspace, DebugCommands) need no dynamic_cast of the engine's
    // RendererContext.
    class VulkanRhi final : public IRhi
    {
    public:
        explicit VulkanRhi( RhiSelection selection );
        ~VulkanRhi() override;

        VulkanRhi( const VulkanRhi& )            = delete;
        VulkanRhi& operator=( const VulkanRhi& ) = delete;

        [[nodiscard]] RhiKind             Kind() const noexcept override;
        [[nodiscard]] const RhiSelection& Selection() const noexcept override;

        [[nodiscard]] Common::ResultStr<std::shared_ptr<Engine::Device>> CreateDevice() override;
        [[nodiscard]] std::unique_ptr<RendererAPI>
        CreateRendererAPI( const std::shared_ptr<Window>& window ) override;
        [[nodiscard]] std::shared_ptr<RendererContext>
        CreateRendererContext( const std::shared_ptr<Window>& window ) override;
        [[nodiscard]] std::shared_ptr<SwapChain> CreateSwapChain( const GLFWwindow* window ) override;

        [[nodiscard]] std::shared_ptr<Image2D>
        CreateImage2D( const Core::Formats::Image2DSpecification& spec ) override;
        [[nodiscard]] std::shared_ptr<ImageCube>
        CreateImageCube( const Core::Formats::ImageCubeSpecification& spec ) override;
        [[nodiscard]] std::shared_ptr<Image3D>
        CreateImage3D( const Core::Formats::Image3DSpecification& spec ) override;
        [[nodiscard]] std::unique_ptr<MipMapCubeGenerator>
        CreateMipMapCubeGenerator( MipGenStrategy strategy ) override;
        [[nodiscard]] std::shared_ptr<Framebuffer>
        CreateFramebuffer( const FramebufferSpecification& spec ) override;

        [[nodiscard]] std::shared_ptr<IndexBuffer>  CreateIndexBuffer( const void* data, uint32_t size,
                                                                       BufferUsage usage ) override;
        [[nodiscard]] std::shared_ptr<IndexBuffer>  CreateIndexBuffer( uint32_t size, BufferUsage usage ) override;
        [[nodiscard]] std::shared_ptr<VertexBuffer> CreateVertexBuffer( const void* data, uint32_t size,
                                                                        BufferUsage usage ) override;
        [[nodiscard]] std::shared_ptr<VertexBuffer> CreateVertexBuffer( uint32_t    size,
                                                                        BufferUsage usage ) override;
        [[nodiscard]] std::shared_ptr<ShaderResources::StorageBuffer>
        CreateStorageBuffer( std::string_view name, uint32_t size, uint32_t binding, bool persistent ) override;
        [[nodiscard]] std::shared_ptr<ShaderResources::StorageBuffer>
        CreateReadbackStorageBuffer( std::string_view name, uint32_t size, uint32_t binding ) override;
        [[nodiscard]] std::shared_ptr<ShaderResources::StorageBuffer>
        CreateDeviceLocalStorageBuffer( std::string_view name, uint32_t size, uint32_t binding ) override;
        [[nodiscard]] std::shared_ptr<ShaderResources::UniformBuffer>
        CreateUniformBuffer( const ShaderResources::ShaderLayout::UniformBuffer& uniform ) override;
        [[nodiscard]] std::shared_ptr<ShaderResources::UniformImage2D>
        CreateUniformImage2D( std::string_view name, uint32_t binding ) override;
        [[nodiscard]] std::shared_ptr<ShaderResources::UniformImageCube>
        CreateUniformImageCube( std::string_view name, uint32_t binding ) override;

        [[nodiscard]] std::shared_ptr<Shader> CreateShader( const Assets::Asset<Assets::ShaderAsset>& asset,
                                                            const ShaderVariant&                      variant,
                                                            const std::string& pass ) override;
        [[nodiscard]] std::shared_ptr<Shader> CreateShaderFromMap( const Assets::Asset<Assets::ShaderAsset>& asset,
                                                                   const ShaderVariant&   variant,
                                                                   const std::string&     pass,
                                                                   const Core::ShaderMap& built ) override;
        [[nodiscard]] Common::ResultStr<std::shared_ptr<GraphicsPipeline>>
        CreateGraphicsPipeline( const GraphicsPipelineSpecification& spec ) override;
        [[nodiscard]] Common::ResultStr<std::shared_ptr<GraphicsPipeline>>
        CreateGraphicsPipelineAsync( const GraphicsPipelineSpecification& spec, PipelineRole role ) override;
        [[nodiscard]] Common::ResultStr<std::shared_ptr<ComputePipeline>>
        CreateComputePipeline( const ComputePipelineSpecification& spec ) override;
        [[nodiscard]] Common::ResultStr<std::shared_ptr<RayTracingPipeline>>
        CreateRayTracingPipeline( const RayTracingPipelineSpecification& spec ) override;
        [[nodiscard]] std::unique_ptr<MaterialBackend>
        CreateMaterialBackend( const std::shared_ptr<Shader>& shader ) override;

        [[nodiscard]] Common::ResultStr<std::unique_ptr<GpuBatch>> BeginGpuBatch() override;
        [[nodiscard]] FallbackTextures&                            Fallbacks() override;
        [[nodiscard]] Common::ResultStr<std::shared_ptr<RayTracing::RayTracingGeometryResource>>
        BuildStaticRayTracingGeometry( const RayTracing::RayTracingMeshInput& input ) override;

        std::size_t DrainDeferredDestruction() override;
        void        WriteAllocationLedger( std::string& out ) const override;
        void        BeginSceneOpenMeasure( std::string_view scene ) override;
        void        SampleSceneOpenMeasure() override;

    private:
        RhiSelection                      m_Selection;
        std::unique_ptr<FallbackTextures> m_Fallbacks;
        // The context CreateRendererContext made (the window owns it); its allocator answers the ledger questions.
        std::weak_ptr<VulkanContext> m_Context;
    };
} // namespace Desert::Graphic::API::Vulkan
