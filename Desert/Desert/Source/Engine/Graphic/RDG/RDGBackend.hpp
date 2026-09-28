#pragma once

#include <Common/Core/ResultStr.hpp>

#include <Engine/Graphic/RDG/RDGCompileResult.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

// The two seams between the device-free core and a device (lead's answers to RDG1, 2026-09-27).
//
// Compile needs REAL memory requirements to plan aliasing and compute a view's peak: an estimate living
// next to the device's own numbers would be a second source of truth for the same budget. It asks an
// IMemoryRequirementsProvider, which the Vulkan executor answers from the device and the suite answers
// with a fixed estimate.
//
// Execute stays in the core (pass order, what runs, the barrier batch, write-back of external states)
// and hands every device operation to an IBackend: acquiring physical resources, one barrier batch per
// pass, begin/end of the render pass, and the label + timestamp pair around each pass - so profiling and labels
// live in exactly one place, the backend's BeginPass/EndPass.
namespace Desert::Graphic::RDG
{
    struct MemoryRequirements
    {
        uint64_t Size           = 0;
        uint64_t Alignment      = 1;
        uint32_t MemoryTypeBits = ~0u; // memory types the resource may live in; disjoint sets never alias
    };

    class IMemoryRequirementsProvider
    {
    public:
        virtual ~IMemoryRequirementsProvider() = default;

        // @p accessMask is the DerivedUsage of the resource (bit i = 1 << Access i): usage decides
        // tiling and compression, and therefore the size a device reports.
        virtual Common::ResultStr<MemoryRequirements> GetTextureRequirements( const TextureDesc& desc,
                                                                              uint32_t accessMask ) const = 0;
        virtual Common::ResultStr<MemoryRequirements> GetBufferRequirements( const BufferDesc& desc,
                                                                             uint32_t accessMask ) const  = 0;
    };

    // One resource of the graph as the backend sees it when it acquires physical resources.
    struct ResourceView
    {
        uint32_t           Resource = kInvalidResource;
        std::string_view   Name;
        ResourceKind       Kind        = ResourceKind::Texture;
        const TextureDesc* Texture     = nullptr; // set for a texture
        const BufferDesc*  Buffer      = nullptr; // set for a buffer
        ExternalTexture*   ExternalTex = nullptr; // registered external (carries its own physical resource)
        ExternalBuffer*    ExternalBuf = nullptr;
        bool               Extracted   = false; // a transient that must outlive the graph
        bool               Used        = false; // touched by an executed pass
        uint32_t           AccessMask  = 0;     // DerivedUsage of a used transient
    };

    struct GraphView
    {
        std::string_view              Name;
        std::span<const ResourceView> Resources; // indexed by resource
        const CompileResult*          Result = nullptr;
    };

    class IBackend
    {
    public:
        virtual ~IBackend() = default;

        virtual BackendKind                        GetKind() const               = 0;
        virtual const IMemoryRequirementsProvider& GetMemoryRequirements() const = 0;

        // Acquires a physical resource for every used transient and binds every used external. Called
        // once per Execute, before the first pass.
        virtual Common::BoolResultStr BeginGraph( const GraphView& graph ) = 0;

        // Label and timestamp open around the pass.
        virtual void BeginPass( const CompiledPass& pass ) = 0;
        // ONE batch of transitions (one vkCmdPipelineBarrier); never called with an empty batch.
        virtual void RecordBarriers( std::span<const Barrier> barriers ) = 0;
        // Only for a raster pass that declared attachments.
        virtual Common::BoolResultStr BeginRenderPass( const CompiledPass& pass ) = 0;
        virtual void                  EndRenderPass()                             = 0;
        virtual void                  EndPass( const CompiledPass& pass )         = 0;

        // Final transitions (extraction / final accesses; may be empty), then the backend releases this
        // graph's hold on its transients.
        virtual Common::BoolResultStr EndGraph( std::span<const Barrier> finalBarriers ) = 0;
        // A pass failed: close whatever is open without recording further work.
        virtual void AbandonGraph() = 0;

        // Physical resource of a used resource, valid between BeginGraph and EndGraph / AbandonGraph.
        virtual std::shared_ptr<IPhysicalTexture> GetPhysicalTexture( uint32_t resource ) const = 0;
        virtual std::shared_ptr<IPhysicalBuffer>  GetPhysicalBuffer( uint32_t resource ) const  = 0;
    };
} // namespace Desert::Graphic::RDG
