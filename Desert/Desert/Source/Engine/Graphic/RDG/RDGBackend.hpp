#pragma once

#include <Common/Core/ResultStr.hpp>

#include <Engine/Graphic/RDG/RDGCompileResult.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <cstdint>
#include <functional>
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

    // RDG-CONTRACTS A(1). The backend's transient memory (UE: IRHITransientResourceAllocator /
    // FRDGTransientResourceAllocator). It turns the AliasingPlan into physical resources PLACED in per-frame-slot
    // heaps, so two transients the plan gave the same bytes really share them.
    //
    // Responsibility: own one set of heaps per (frame slot, graph name, TransientHeapDesc); create each resource
    // at its Allocation's heap + Offset; destroy nothing a submitted frame may still read.
    // Who calls it: the frame loop calls BeginFrameSlot; IBackend::BeginGraph calls ReserveHeaps then Place* for
    // every used, non-extracted transient in AliasingPlan::Allocations order; IBackend::EndGraph /
    // AbandonGraph call EndGraph. Renderers never call it.
    // Lifetime model (the one VulkanRdgPool has today, generalised): a slot's heaps and placed resources are
    // reused only after the caller waited for that slot's previous submission (the frames-in-flight fence).
    // A heap that has to grow is retired, not freed: it is destroyed at the next BeginFrameSlot of the same
    // slot. Heaps are per GRAPH (keyed by Builder name) because two graphs of one frame record back to back
    // and the second graph's acquire barriers cannot see the first graph's accesses.
    // Never: place a resource outside [0, heap Bytes); hand out a placed resource after its slot's frame was
    // re-begun; place an extracted transient (it gets a dedicated allocation and outlives the graph).
    struct TransientAllocatorStats
    {
        uint64_t ReservedBytes   = 0; // all heaps of all slots
        uint32_t HeapCount       = 0;
        uint32_t PlacedResources = 0; // placed this frame, all graphs
    };

    class ITransientAllocator
    {
    public:
        virtual ~ITransientAllocator() = default;

        // The caller has waited for @p slot's previous submission; retires what that frame no longer needs.
        virtual void BeginFrameSlot( uint32_t slot ) = 0;
        // Ensures the current slot holds heaps for @p graph covering every @p heaps entry (same index order).
        virtual Common::BoolResultStr ReserveHeaps( std::string_view                   graph,
                                                    std::span<const TransientHeapDesc> heaps ) = 0;
        // A resource bound to heap @p allocation.Heap at @p allocation.Offset. The same (desc, usage, heap,
        // offset) in the same slot may return the same object as last time, but a caller must not rely on it.
        virtual Common::ResultStr<std::shared_ptr<IPhysicalTexture>> PlaceTexture( const Allocation&  allocation,
                                                                                   const TextureDesc& desc,
                                                                                   uint32_t           accessMask,
                                                                                   std::string_view   name ) = 0;
        virtual Common::ResultStr<std::shared_ptr<IPhysicalBuffer>>  PlaceBuffer( const Allocation& allocation,
                                                                                  const BufferDesc& desc,
                                                                                  uint32_t          accessMask,
                                                                                  std::string_view  name )  = 0;
        // The graph released its hold; placed resources stay alive until the slot is re-begun.
        virtual void                    EndGraph( std::string_view graph ) = 0;
        virtual TransientAllocatorStats GetStats() const                   = 0;
    };

    // RDG-CONTRACTS B(4). The decided async-compute fallback is announced exactly once per backend, not silently
    // taken and not logged every frame. Responsibility: turn CompileResult::DemotedAsyncPasses into one line
    // ("RDG: no separate compute queue family; AsyncCompute passes run on the graphics queue: <names>") the first
    // time any compile on this backend demoted a pass, and never again. Who calls it: Builder::Execute, after
    // Compile, when DemotedAsyncPasses is non-empty. The sink is the engine logger (Warning) in the product and a
    // line collector in the suite.
    class AsyncComputeFallbackLog
    {
    public:
        using Sink = std::function<void( std::string_view line )>;

        explicit AsyncComputeFallbackLog( Sink sink );

        // @p passNames: the names of the demoted passes of this compile. Returns true only on the call that
        // logged.
        bool     Report( std::span<const std::string_view> passNames );
        uint32_t GetLinesLogged() const;

    private:
        Sink     m_Sink;
        uint32_t m_LinesLogged = 0;
    };

    class IBackend
    {
    public:
        virtual ~IBackend() = default;

        virtual BackendKind                        GetKind() const               = 0;
        virtual const IMemoryRequirementsProvider& GetMemoryRequirements() const = 0;

        // ── RDG-CONTRACTS additions ──────────────────────────────────────────────────────────────────────
        // A(1): where BeginGraph places transients. Valid for the backend's whole life.
        virtual ITransientAllocator& GetTransientAllocator() = 0;
        // B(4): what Execute compiles against. Constant for the backend's life (it describes the device).
        virtual PipeCapabilities GetPipeCapabilities() const = 0;
        // B(4): the once-per-backend announcement of demoted AsyncCompute passes.
        virtual AsyncComputeFallbackLog& GetAsyncComputeFallbackLog() = 0;

        // B(3): per-pipe recording. Execute walks CompileResult::Segments in order and brackets each with
        // BeginPipeSegment / EndPipeSegment; every BeginPass .. EndPass, RecordBarriers and render pass in
        // between records onto segment.OnPipe. The backend ends a segment's command buffer at EndPipeSegment
        // and queues it for submission with the segment's WaitSyncs / SignalSyncs (Vulkan: binary semaphores,
        // one per CrossPipeSync, owned per frame slot). Never: a render pass open across a segment boundary;
        // a segment on AsyncCompute when GetPipeCapabilities().SeparateComputeFamily is false.
        virtual Common::BoolResultStr BeginPipeSegment( const PipeSegment& segment ) = 0;
        virtual Common::BoolResultStr EndPipeSegment( const PipeSegment& segment )   = 0;
        // B(3): the OwnershipRelease barriers of a pass, recorded after its EndRenderPass / exec lambda on the
        // pass's pipe. One batch; never called empty; every entry has BarrierType == OwnershipRelease.
        // (OwnershipAcquire and AliasAcquire barriers arrive through RecordBarriers, in the pass's one batch.)
        virtual void RecordEpilogueBarriers( std::span<const Barrier> barriers ) = 0;

        // Acquires a physical resource for every used transient and binds every used external. Called
        // once per Execute, before the first pass.
        // RDG-CONTRACTS A(1): a non-extracted transient is PLACED through GetTransientAllocator() at its
        // Allocation (ReserveHeaps with graph.Result->Aliasing.Heaps, then PlaceTexture / PlaceBuffer); only an
        // extracted transient gets a dedicated resource. Two transients sharing bytes are two distinct physical
        // resources over the same memory; their order is enforced only by the AliasAcquire barrier.
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
