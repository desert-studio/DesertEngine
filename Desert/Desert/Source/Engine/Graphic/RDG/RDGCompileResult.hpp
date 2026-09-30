#pragma once

#include <Engine/Graphic/RDG/RDGAccess.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// What Builder::Compile hands to an executor: pure data, computed without a device. Every index named
// "Resource" is the index behind a TextureRef / BufferRef of the builder that produced the result; every
// "Pass" is an AddPass index (declaration order), and "Position" is an index into CompileResult::Passes.
namespace Desert::Graphic::RDG
{
    enum class ResourceKind : uint8_t
    {
        Texture,
        Buffer,
    };

    // RDG-CONTRACTS A(2) + B(2). What a Barrier does beyond a plain state change.
    //
    // AliasAcquire (UE: the "acquire" of a transient, FRDGTransientResourceAllocator + aliasing barrier). The
    // FIRST use of a transient whose Allocation lists AliasPredecessors. Compile emits it in the barrier batch
    // of the resource's FirstPosition pass (never as a separate batch), with DiscardContents = true,
    // Before.Layout = Undefined and Before.Stages / Before.Memory = the union of every predecessor's LAST access
    // (stages and accesses) on the pipe that last touched it. The backend records it as an ordinary
    // image / buffer memory barrier ON THE NEW RESOURCE from UNDEFINED: the source scope makes the predecessor's
    // work finish (and its writes available) before the new resource overwrites the shared bytes. Never:
    // a Before.Layout other than Undefined, an empty source scope when a predecessor was touched, or an acquire
    // emitted for an external / extracted resource. A transient with no predecessor gets a plain Transition
    // with DiscardContents = true and an empty source scope, as today.
    //
    // OwnershipRelease / OwnershipAcquire (UE: cross-pipe transitions; Vulkan queue family ownership transfer).
    // One QueueOwnershipTransfer is TWO barriers with the same Range: the release in
    // CompiledPass::EpilogueBarriers of the last pass on SrcPipe to touch the resource (Before = its last access,
    // After.Stages / Memory empty), the acquire in the barrier batch of the first pass on DstPipe (Before.Stages /
    // Memory empty, After = its first access); both carry the same Before.Layout -> After.Layout so the layout
    // changes exactly once. Only emitted when PipeCapabilities::SeparateComputeFamily; a resource whose contents
    // are discarded on the new pipe (DiscardContents) needs no transfer and gets none (Vulkan: undefined-layout
    // acquire without release).
    enum class BarrierKind : uint8_t
    {
        Transition,
        AliasAcquire,
        OwnershipRelease,
        OwnershipAcquire,
    };

    // One transition. For a buffer the range is always the single subresource {0, 1, 0, 1}.
    struct Barrier
    {
        BarrierKind BarrierType = BarrierKind::Transition;
        // Queue-family transfer endpoints; both Graphics (and ignored) unless BarrierType is Ownership*.
        Pipe             SrcPipe  = Pipe::Graphics;
        Pipe             DstPipe  = Pipe::Graphics;
        uint32_t         Resource = kInvalidResource;
        ResourceKind     Kind     = ResourceKind::Texture;
        SubresourceRange Range;
        AccessState      Before;
        AccessState      After;
        // The first use of transient memory: the old contents are not preserved (Before.Layout is
        // Undefined). When the memory belonged to an aliased predecessor, Before carries that
        // predecessor's last stages and accesses so its work finishes before this resource overwrites it.
        bool DiscardContents = false;
    };

    struct AttachmentDecision
    {
        uint32_t    Slot      = 0; // colour slot; unused for depth
        bool        IsDepth   = false;
        bool        IsResolve = false; // the single-sample image colour slot `Slot` resolves into
        Access      Usage     = Access::ColorTarget; // ColorTarget, DepthWrite or DepthRead: the attachment layout
        uint32_t    Resource  = kInvalidResource;
        uint32_t    Mip       = 0;
        uint32_t    BaseLayer = 0;
        uint32_t    LayerCount = 1;
        LoadAction  Load       = LoadAction::Load;
        StoreAction Store      = StoreAction::Store;
        ClearValue  Clear;
    };

    struct CompiledPass
    {
        uint32_t    Pass = 0;
        std::string Name;
        PassFlags   Flags = PassFlags::None;
        // ALL transitions this pass needs, recorded as one batch (one vkCmdPipelineBarrier in the Vulkan backend).
        std::vector<Barrier>            Barriers;
        std::vector<AttachmentDecision> Attachments;
        // Render-pass merging. A raster pass on exactly the attachments of the executed pass before it, loading
        // every one of them and needing no barrier, records inside the render pass that pass opened.
        bool ContinuesRenderPass = false; // no BeginRenderPass: the previous pass's render pass is still open
        bool KeepsRenderPassOpen = false; // no EndRenderPass: the next pass continues this render pass

        // RDG-CONTRACTS B(2). The pipe this pass records on. Graphics unless the pass declared AsyncCompute AND
        // the compile had PipeCapabilities::SeparateComputeFamily. Render-pass merging never crosses a pipe or a
        // sync point: a pass that waits or signals neither continues nor keeps open a render pass.
        Pipe OnPipe = Pipe::Graphics;
        // Indices into CompileResult::Syncs. WaitSyncs are waited on before this pass's barrier batch;
        // SignalSyncs are signalled after its EpilogueBarriers. Empty on a single-pipe compile.
        std::vector<uint32_t> WaitSyncs;
        std::vector<uint32_t> SignalSyncs;
        // Recorded after the exec lambda (and after EndRenderPass), before any signal: the OwnershipRelease
        // half of every QueueOwnershipTransfer whose ReleasePosition is this pass. Never contains a Transition.
        std::vector<Barrier> EpilogueBarriers;
    };

    // RDG-CONTRACTS B(2). A fork or a join between the two pipes (UE: FRDGPass cross-pipeline sync; Vulkan: one
    // binary semaphore signalled by one submission and waited by one later submission on the other queue).
    //   * fork: the Graphics pass at SignalPosition is the LAST graphics producer (writer, in AddPass order) of
    //     anything the async segment starting at WaitPosition reads; if no graphics pass precedes, there is no
    //     fork and the segment waits on nothing inside the graph;
    //   * join: the Graphics pass at WaitPosition is the FIRST graphics pass that reads, writes or aliases
    //     memory of anything the async segment touched (RAW, WAR and WAW all join). An async segment whose writes
    //     leave the graph with no graphics consumer joins before the graph's FinalBarriers (WaitPosition =
    //     kJoinAtGraphEnd), so every graph ends with both pipes idle relative to the graphics queue.
    // Passes are never reordered: the scheduler only picks the pipe and places syncs.
    struct CrossPipeSync
    {
        static constexpr uint32_t kJoinAtGraphEnd = ~0u;
        // Amendment B: the SignalPosition of a fork no pass signals. The async run consumes externals no earlier
        // pass of the graph touched (it may sit at position 0): the graphics pipe opens with a prologue
        // PipeSegment (FirstPosition == LastPosition == kForkAtGraphStart, no passes) that records
        // CompileResult::PrologueBarriers (the OwnershipRelease of those externals) and signals this fork.
        static constexpr uint32_t kForkAtGraphStart = ~0u - 1u;

        Pipe     SignalPipe     = Pipe::Graphics;
        uint32_t SignalPosition = 0; // CompileResult::Passes index that signals after its epilogue
        Pipe     WaitPipe       = Pipe::AsyncCompute;
        uint32_t WaitPosition =
             0; // CompileResult::Passes index that waits before its barriers, or kJoinAtGraphEnd
        // Stages of the waiting pipe that must not start before the signal (Vulkan pWaitDstStageMask): the
        // stages of every first access on WaitPipe that depends on the signalling side.
        PipelineStageFlags WaitStages = PipelineStage_None;
        bool               IsFork     = true; // Graphics -> AsyncCompute; false: AsyncCompute -> Graphics
    };

    // RDG-CONTRACTS B(2). A resource used on both pipes with contents that must survive the move (a release on
    // the source queue, an acquire on the destination queue - VK_SHARING_MODE_EXCLUSIVE is the only sharing mode
    // the graph creates, as in UE). External resources transfer too; an external that ends the graph owned by
    // AsyncCompute is transferred back to Graphics before FinalBarriers, so the next graph always finds every
    // external owned by Graphics.
    struct QueueOwnershipTransfer
    {
        uint32_t         Resource = kInvalidResource;
        ResourceKind     Kind     = ResourceKind::Texture;
        SubresourceRange Range;
        Pipe             From            = Pipe::Graphics;
        Pipe             To              = Pipe::AsyncCompute;
        // Its OwnershipRelease is in this pass's EpilogueBarriers; kForkAtGraphStart: in PrologueBarriers.
        uint32_t ReleasePosition = 0;
        // Its OwnershipAcquire is in this pass's Barriers; kJoinAtGraphEnd (amendment A): in FinalBarriers,
        // recorded after the graphics queue waited on the graph-end join.
        uint32_t AcquirePosition = 0;
        uint32_t         Sync            = 0; // the CrossPipeSync ordering the two (index into Syncs)
    };

    // RDG-CONTRACTS B(2)/(3). A maximal run of consecutive executed passes on ONE pipe that no sync splits: the
    // unit the backend records into one command buffer and submits once. Graphics segments are split at every
    // pass that waits on a join or signals a fork; an AsyncCompute segment is the passes between one fork and
    // its join. Segments are in submission order: a segment is submitted only after every segment it waits on.
    struct PipeSegment
    {
        Pipe                  OnPipe        = Pipe::Graphics;
        uint32_t              FirstPosition = 0;
        uint32_t              LastPosition  = 0;
        std::vector<uint32_t> WaitSyncs;   // union of WaitSyncs of its passes
        std::vector<uint32_t> SignalSyncs; // union of SignalSyncs of its passes
    };

    enum class DependencyKind : uint8_t
    {
        ReadAfterWrite,
        WriteAfterRead,
        WriteAfterWrite,
    };

    struct DependencyEdge
    {
        uint32_t       From     = 0; // earlier pass (AddPass index)
        uint32_t       To       = 0; // later pass
        DependencyKind Kind     = DependencyKind::ReadAfterWrite;
        uint32_t       Resource = kInvalidResource;
    };

    // Where a graph-owned resource is live in the compiled plan: from the first to the last EXECUTED pass that
    // declares any subresource of it. This is the contract the aliasing plan (step 6 of Compile) is built on, and
    // the one a later async-compute scheduler or a cross-graph transient pool must build on too:
    //   * only transients have an entry - created with CreateTexture / CreateBuffer, extracted or not. An external
    //     resource has none: its memory belongs to its owner and outlives every graph;
    //   * only executed passes count. A culled pass neither opens nor extends a lifetime, so culling shrinks the
    //     range, and a transient whose every user was culled has no entry and allocates nothing;
    //   * FirstPass / LastPass are AddPass indices (what PassFlags, names and CulledPasses use); FirstPosition /
    //     LastPosition index CompileResult::Passes and name the same two passes. First <= Last;
    //   * an extracted transient is still live after LastPass (it leaves the graph), so it never shares memory;
    //     any other transient's memory is free for reuse by a pass after LastPosition.
    struct ResourceLifetime
    {
        uint32_t Resource      = kInvalidResource;
        uint32_t FirstPosition = 0;
        uint32_t LastPosition  = 0;
        uint32_t FirstPass     = 0;
        uint32_t LastPass      = 0;
        // RDG-CONTRACTS B(2). The range the ALIASING PLAN uses (UE extends async-compute lifetimes to the fork /
        // join for the same reason). Equal to First/LastPosition when every user is on Graphics. When any user
        // runs on AsyncCompute, positions stop being a timeline: a graphics pass after it in AddPass order may run
        // before the async work finishes. The range is then widened to [position of the fork the resource's first
        // async user waits on, position of the join its last async user signals] (kJoinAtGraphEnd = the last
        // executed position), so no graphics transient can be placed in its bytes while the other queue may
        // still touch them. Never narrower than [FirstPosition, LastPosition].
        uint32_t AliasFirstPosition = 0;
        uint32_t AliasLastPosition  = 0;
        bool     UsedOnAsyncCompute = false;
    };

    // RDG-CONTRACTS A(1). One heap the backend's ITransientAllocator must provide for the graph, per memory class
    // and memory-type set: allocations are placed inside it at Allocation::Offset. Compile groups allocations
    // whose MemoryTypeBits intersect into one heap (MemoryTypeBits = the intersection); disjoint sets get
    // separate heaps and never alias. Bytes is the heap's high-water mark: max over allocations of Offset + Size.
    struct TransientHeapDesc
    {
        MemoryClass Class          = MemoryClass::Texture;
        uint32_t    MemoryTypeBits = ~0u;
        uint64_t    Bytes          = 0;
        uint64_t    Alignment      = 1; // max alignment of its allocations
    };

    struct Allocation
    {
        uint32_t    Resource       = kInvalidResource;
        MemoryClass Class          = MemoryClass::Texture;
        uint64_t    Offset         = 0;
        uint64_t    Size           = 0;
        uint64_t    Alignment      = 1;
        uint32_t    MemoryTypeBits = ~0u; // from the IMemoryRequirementsProvider
        // Earlier transients whose bytes this one reuses (their lifetimes ended before it starts).
        std::vector<uint32_t> AliasPredecessors;
        // RDG-CONTRACTS A(1). Index into AliasingPlan::Heaps; Offset is relative to that heap.
        uint32_t Heap = 0;
    };

    struct AliasingPlan
    {
        // Transients that neither are external nor extracted, in placement order.
        std::vector<Allocation>                 Allocations;
        std::array<uint64_t, kMemoryClassCount> PeakBytes{};
        uint64_t                                TotalPeakBytes = 0; // the view's transient memory peak
        uint64_t                                UnaliasedBytes = 0; // the same set without any sharing
        // RDG-CONTRACTS A(1). What the backend's ITransientAllocator reserves for this graph (ReserveHeaps).
        std::vector<TransientHeapDesc> Heaps;
    };

    // The state an external or extracted resource is left in, per subresource (layer-major); Execute
    // writes it back into the ExternalTexture / ExternalBuffer.
    struct ExternalFinalState
    {
        uint32_t                 Resource = kInvalidResource;
        ResourceKind             Kind     = ResourceKind::Texture;
        std::vector<AccessState> SubresourceStates;
    };

    // Usage the backend must create a transient with, OR-ed from every access declared on it by an
    // executed pass (bit i = 1 << Access i).
    struct DerivedUsage
    {
        uint32_t Resource   = kInvalidResource;
        uint32_t AccessMask = 0;
    };

    struct CompileResult
    {
        std::vector<CompiledPass>       Passes;        // executed passes, in AddPass order (never reordered)
        std::vector<uint32_t>           CulledPasses;  // AddPass indices that do not execute
        std::vector<std::string>        CulledPassNames; // their names, same order: what a log or debug view shows
        bool PassCulling = true;                         // false: Builder::SetPassCulling(false) kept every pass
        std::vector<DependencyEdge>     Edges;         // between executed passes
        std::vector<Barrier>            FinalBarriers; // after the last pass: extraction / final accesses
        // Amendment B: the releases the graphics prologue segment records (fork at kForkAtGraphStart).
        std::vector<Barrier> PrologueBarriers;
        std::vector<ResourceLifetime>   Lifetimes;
        std::vector<DerivedUsage>       Usages;
        std::vector<ExternalFinalState> ExternalFinalStates;
        AliasingPlan                    Aliasing;

        // RDG-CONTRACTS B(2). The async-compute schedule. On a compile with SeparateComputeFamily == false (or
        // with no AsyncCompute pass) Syncs, OwnershipTransfers are empty and Segments is one Graphics segment
        // spanning every executed pass - the exact result the graph had before async compute existed.
        PipeCapabilities                    Pipes;
        std::vector<CrossPipeSync>          Syncs;
        std::vector<QueueOwnershipTransfer> OwnershipTransfers;
        std::vector<PipeSegment>            Segments;
        // RDG-CONTRACTS B(4). AddPass indices of executed passes that declared AsyncCompute but run on Graphics
        // because Pipes.SeparateComputeFamily is false. Not an error: the decided fallback. Execute hands a
        // non-empty list to IBackend::GetAsyncComputeFallbackLog(), which logs it once per backend.
        std::vector<uint32_t> DemotedAsyncPasses;

        const CompiledPass* FindPass( std::string_view name ) const
        {
            for ( const CompiledPass& pass : Passes )
            {
                if ( pass.Name == name )
                    return &pass;
            }
            return nullptr;
        }

        const Allocation* FindAllocation( uint32_t resource ) const
        {
            for ( const Allocation& allocation : Aliasing.Allocations )
            {
                if ( allocation.Resource == resource )
                    return &allocation;
            }
            return nullptr;
        }
    };
} // namespace Desert::Graphic::RDG
