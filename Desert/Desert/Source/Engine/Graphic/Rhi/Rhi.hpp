#pragma once

#include <Engine/Graphic/Rhi/RhiKind.hpp>
#include <Engine/Graphic/Rhi/RhiSelection.hpp>

#include <Engine/Graphic/Shader.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

struct GLFWwindow;

namespace Desert
{
    class Window;
} // namespace Desert

namespace Desert::Engine
{
    class Device;
} // namespace Desert::Engine

namespace Desert::Core::Formats
{
    struct Image2DSpecification;
    struct ImageCubeSpecification;
    struct Image3DSpecification;
} // namespace Desert::Core::Formats

namespace Desert::ShaderResources
{
    class StorageBuffer;
    class UniformBuffer;
    class UniformImage2D;
    class UniformImageCube;
} // namespace Desert::ShaderResources

namespace Desert::Graphic
{
    class RendererAPI;
    class RendererContext;
    class SwapChain;
    class Framebuffer;
    class GpuBatch;
    class Image2D;
    class ImageCube;
    class Image3D;
    class MipMapCubeGenerator;
    class IndexBuffer;
    class VertexBuffer;
    class GraphicsPipeline;
    class ComputePipeline;
    class RayTracingPipeline;
    class FallbackTextures;
    class MaterialBackend;
    struct FramebufferSpecification;
    struct GraphicsPipelineSpecification;
    struct ComputePipelineSpecification;
    struct RayTracingPipelineSpecification;
    enum class BufferUsage;
    enum class MipGenStrategy : uint8_t;
    enum class PipelineRole : uint8_t;

    namespace RayTracing
    {
        struct RayTracingMeshInput;
        class RayTracingGeometryResource;
    } // namespace RayTracing

    // THE BACKEND. One instance, owned by Renderer (Renderer::AttachRhi), created from the RhiSelection before the
    // window's surface. Every former per-API switch of a static Create is one virtual here with the signature
    // of the static Create it replaces; the statics stay as one-line forwards to Renderer::GetInstance().Rhi().
    // What a static did before or after the backend object (spec checks, naming, mip generation) stays in the
    // static.
    class IRhi
    {
    public:
        virtual ~IRhi() = default;

        [[nodiscard]] virtual RhiKind             Kind() const noexcept      = 0;
        [[nodiscard]] virtual const RhiSelection& Selection() const noexcept = 0;

        // --- the process's device and frame objects (Application, Window) --------------------------------------
        [[nodiscard]] virtual Common::ResultStr<std::shared_ptr<Engine::Device>> CreateDevice() = 0;
        [[nodiscard]] virtual std::unique_ptr<RendererAPI>
        CreateRendererAPI( const std::shared_ptr<Window>& window ) = 0;
        [[nodiscard]] virtual std::shared_ptr<RendererContext>
        CreateRendererContext( const std::shared_ptr<Window>& window )                               = 0;
        [[nodiscard]] virtual std::shared_ptr<SwapChain> CreateSwapChain( const GLFWwindow* window ) = 0;

        // --- images and targets. Null = the backend refused; it logged why. ------------------------------------
        [[nodiscard]] virtual std::shared_ptr<Image2D>
        CreateImage2D( const Core::Formats::Image2DSpecification& spec ) = 0;
        [[nodiscard]] virtual std::shared_ptr<ImageCube>
        CreateImageCube( const Core::Formats::ImageCubeSpecification& spec ) = 0;
        [[nodiscard]] virtual std::shared_ptr<Image3D>
        CreateImage3D( const Core::Formats::Image3DSpecification& spec ) = 0;
        [[nodiscard]] virtual std::unique_ptr<MipMapCubeGenerator>
        CreateMipMapCubeGenerator( MipGenStrategy strategy ) = 0;
        [[nodiscard]] virtual std::shared_ptr<Framebuffer>
        CreateFramebuffer( const FramebufferSpecification& spec ) = 0;

        // --- buffers ------------------------------------------------------------------------------------------
        [[nodiscard]] virtual std::shared_ptr<IndexBuffer>  CreateIndexBuffer( const void* data, uint32_t size,
                                                                               BufferUsage usage )  = 0;
        [[nodiscard]] virtual std::shared_ptr<IndexBuffer>  CreateIndexBuffer( uint32_t    size,
                                                                               BufferUsage usage )  = 0;
        [[nodiscard]] virtual std::shared_ptr<VertexBuffer> CreateVertexBuffer( const void* data, uint32_t size,
                                                                                BufferUsage usage ) = 0;
        [[nodiscard]] virtual std::shared_ptr<VertexBuffer> CreateVertexBuffer( uint32_t    size,
                                                                                BufferUsage usage ) = 0;
        [[nodiscard]] virtual std::shared_ptr<ShaderResources::StorageBuffer>
        CreateStorageBuffer( std::string_view name, uint32_t size, uint32_t binding, bool persistent ) = 0;
        [[nodiscard]] virtual std::shared_ptr<ShaderResources::StorageBuffer>
        CreateReadbackStorageBuffer( std::string_view name, uint32_t size, uint32_t binding ) = 0;
        [[nodiscard]] virtual std::shared_ptr<ShaderResources::StorageBuffer>
        CreateDeviceLocalStorageBuffer( std::string_view name, uint32_t size, uint32_t binding ) = 0;
        [[nodiscard]] virtual std::shared_ptr<ShaderResources::UniformBuffer>
        CreateUniformBuffer( const ShaderResources::ShaderLayout::UniformBuffer& uniform ) = 0;
        [[nodiscard]] virtual std::shared_ptr<ShaderResources::UniformImage2D>
        CreateUniformImage2D( std::string_view name, uint32_t binding ) = 0;
        [[nodiscard]] virtual std::shared_ptr<ShaderResources::UniformImageCube>
        CreateUniformImageCube( std::string_view name, uint32_t binding ) = 0;

        // --- programs and pipelines ---------------------------------------------------------------------------
        [[nodiscard]] virtual std::shared_ptr<Shader>
        CreateShader( const Assets::Asset<Assets::ShaderAsset>& asset, const ShaderVariant& variant,
                      const std::string& pass ) = 0;
        [[nodiscard]] virtual std::shared_ptr<Shader>
        CreateShaderFromMap( const Assets::Asset<Assets::ShaderAsset>& asset, const ShaderVariant& variant,
                             const std::string& pass, const Core::ShaderMap& built ) = 0;
        // The spec was checked by the static (Check*PipelineSpecification); the backend builds or refuses by name.
        [[nodiscard]] virtual Common::ResultStr<std::shared_ptr<GraphicsPipeline>>
        CreateGraphicsPipeline( const GraphicsPipelineSpecification& spec ) = 0;
        [[nodiscard]] virtual Common::ResultStr<std::shared_ptr<GraphicsPipeline>>
        CreateGraphicsPipelineAsync( const GraphicsPipelineSpecification& spec, PipelineRole role ) = 0;
        [[nodiscard]] virtual Common::ResultStr<std::shared_ptr<ComputePipeline>>
        CreateComputePipeline( const ComputePipelineSpecification& spec ) = 0;
        [[nodiscard]] virtual Common::ResultStr<std::shared_ptr<RayTracingPipeline>>
        CreateRayTracingPipeline( const RayTracingPipelineSpecification& spec ) = 0;
        [[nodiscard]] virtual std::unique_ptr<MaterialBackend>
        CreateMaterialBackend( const std::shared_ptr<Shader>& shader ) = 0;

        // --- one-shot work, shared sets -----------------------------------------------------------------------
        [[nodiscard]] virtual Common::ResultStr<std::unique_ptr<GpuBatch>> BeginGpuBatch() = 0;
        // The device's fallback set, built on first use (owned here: it lives exactly as long as the backend).
        [[nodiscard]] virtual FallbackTextures& Fallbacks() = 0;
        // The blocking BLAS build of one static mesh's resident buffers (RayTracingGeometryVariants' production
        // build). Null in the success value = no BLAS by design (see the backend); an error = the build failed.
        [[nodiscard]] virtual Common::ResultStr<std::shared_ptr<RayTracing::RayTracingGeometryResource>>
        BuildStaticRayTracingGeometry( const RayTracing::RayTracingMeshInput& input ) = 0;

        // --- editor and diagnostics that read the device allocator (SceneWorkspace, DebugCommands) --------------
        // Releases every deferred GPU destruction now (the caller waited for the device); returns how many.
        virtual std::size_t DrainDeferredDestruction() = 0;
        // A total line, then one line per allocation tag, appended to @p out ("[AllocLedger] ...", one per line).
        virtual void WriteAllocationLedger( std::string& out ) const = 0;
        // The device-memory window of one scene open: started at the top of an open, sampled at its end.
        virtual void BeginSceneOpenMeasure( std::string_view scene ) = 0;
        virtual void SampleSceneOpenMeasure()                        = 0;
    };

    // THE ONE SWITCH on the backend kind. Refuses d3d12 by name until its renderer is finished; never falls back
    // to another backend.
    [[nodiscard]] Common::ResultStr<std::unique_ptr<IRhi>> CreateRhi( const RhiSelection& selection );
} // namespace Desert::Graphic
