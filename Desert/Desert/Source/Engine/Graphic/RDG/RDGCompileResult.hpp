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

    // One transition. For a buffer the range is always the single subresource {0, 1, 0, 1}.
    struct Barrier
    {
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

    // Only transients used by an executed pass have a lifetime; a transient whose every user was culled
    // allocates nothing.
    struct ResourceLifetime
    {
        uint32_t Resource      = kInvalidResource;
        uint32_t FirstPosition = 0;
        uint32_t LastPosition  = 0;
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
    };

    struct AliasingPlan
    {
        // Transients that neither are external nor extracted, in placement order.
        std::vector<Allocation>                 Allocations;
        std::array<uint64_t, kMemoryClassCount> PeakBytes{};
        uint64_t                                TotalPeakBytes = 0; // the view's transient memory peak
        uint64_t                                UnaliasedBytes = 0; // the same set without any sharing
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
        std::vector<DependencyEdge>     Edges;         // between executed passes
        std::vector<Barrier>            FinalBarriers; // after the last pass: extraction / final accesses
        std::vector<ResourceLifetime>   Lifetimes;
        std::vector<DerivedUsage>       Usages;
        std::vector<ExternalFinalState> ExternalFinalStates;
        AliasingPlan                    Aliasing;

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
