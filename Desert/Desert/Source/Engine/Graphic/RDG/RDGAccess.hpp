#pragma once

#include <array>
#include <cstdint>
#include <string_view>

// The render graph's synchronisation vocabulary: what a pass does to a resource (Access), and the
// (stages, memory access, image layout) triple that access implies (AccessState).
//
// VULKAN-FREE ON PURPOSE. The graph's Compile runs without a device and is tested without the Vulkan SDK,
// so none of these enums carry VK_* numbers: the bits are the engine's own, packed densely, and the
// Vulkan executor (RDG2) translates each flag with an explicit table. Punning on VK_PIPELINE_STAGE_2_*
// values would compile here only because they are plain integers, and would silently make this header
// depend on one API's numbering.
namespace Desert::Graphic::RDG
{
    // One declared use of a resource by a pass. Every value maps to exactly one AccessState below.
    enum class Access : uint8_t
    {
        None,                  // no prior use: a fresh external, or a state nothing has touched yet
        SampledGraphics,       // sampled in a vertex or fragment shader
        SampledCompute,        // sampled in a compute shader
        StorageRead,           // storage image / storage buffer, read only
        StorageWrite,          // storage image / storage buffer, read-modify-write
        ColorTarget,           // colour attachment of a raster pass
        DepthWrite,            // depth attachment, tested and written
        DepthRead,             // depth attachment, tested only
        CopySrc,               // transfer source
        CopyDst,               // transfer destination
        IndirectArgs,          // draw / dispatch indirect arguments
        VertexIndex,           // vertex or index buffer
        UniformRead,           // uniform buffer
        Present,               // handed to the presentation engine
        HostRead,              // read back by the CPU after the frame's fence
        AccelStructBuildInput, // vertex/index/instance data consumed by an acceleration-structure build
        AccelStructBuildWrite, // the acceleration structure being built
        AccelStructRead,       // ray query from a fragment or compute shader
        Count
    };

    inline constexpr uint32_t kAccessCount = static_cast<uint32_t>( Access::Count );

    // Pipeline stages at synchronization2 granularity (the engine has no path without sync2).
    enum PipelineStage : uint32_t
    {
        PipelineStage_None                  = 0,
        PipelineStage_DrawIndirect          = 1u << 0,
        PipelineStage_VertexInput           = 1u << 1,
        PipelineStage_VertexShader          = 1u << 2,
        PipelineStage_FragmentShader        = 1u << 3,
        PipelineStage_EarlyFragmentTests    = 1u << 4,
        PipelineStage_LateFragmentTests     = 1u << 5,
        PipelineStage_ColorAttachmentOutput = 1u << 6,
        PipelineStage_ComputeShader         = 1u << 7,
        PipelineStage_Copy                  = 1u << 8,
        PipelineStage_Host                  = 1u << 9,
        PipelineStage_AccelStructBuild      = 1u << 10,
    };
    using PipelineStageFlags = uint32_t;

    enum MemoryAccess : uint32_t
    {
        MemoryAccess_None                 = 0,
        MemoryAccess_IndirectCommandRead  = 1u << 0,
        MemoryAccess_IndexRead            = 1u << 1,
        MemoryAccess_VertexAttributeRead  = 1u << 2,
        MemoryAccess_UniformRead          = 1u << 3,
        MemoryAccess_ShaderSampledRead    = 1u << 4,
        MemoryAccess_ShaderStorageRead    = 1u << 5,
        MemoryAccess_ShaderStorageWrite   = 1u << 6,
        MemoryAccess_ColorAttachmentRead  = 1u << 7,
        MemoryAccess_ColorAttachmentWrite = 1u << 8,
        MemoryAccess_DepthStencilRead     = 1u << 9,
        MemoryAccess_DepthStencilWrite    = 1u << 10,
        MemoryAccess_TransferRead         = 1u << 11,
        MemoryAccess_TransferWrite        = 1u << 12,
        MemoryAccess_HostRead             = 1u << 13,
        MemoryAccess_AccelStructRead      = 1u << 14,
        MemoryAccess_AccelStructWrite     = 1u << 15,
    };
    using MemoryAccessFlags = uint32_t;

    // Every bit above that makes an access a WRITE. A state whose access mask has none of these is
    // read-only, and consecutive read-only uses in one layout are merged into one state.
    inline constexpr MemoryAccessFlags kWriteAccessMask =
         MemoryAccess_ShaderStorageWrite | MemoryAccess_ColorAttachmentWrite | MemoryAccess_DepthStencilWrite |
         MemoryAccess_TransferWrite | MemoryAccess_AccelStructWrite;

    enum class ImageLayout : uint8_t
    {
        Undefined, // contents are discarded by the transition; also the "layout" of every buffer access
        General,   // storage images
        ColorAttachment,
        DepthStencilAttachment,
        DepthStencilReadOnly,
        ShaderReadOnly,
        TransferSrc,
        TransferDst,
        Present,
    };

    // Which kind of resource an access may be declared on. A declaration outside this set is refused by
    // the PassBuilder rather than translated into something plausible.
    enum AccessTarget : uint8_t
    {
        AccessTarget_Texture = 1u << 0,
        AccessTarget_Buffer  = 1u << 1,
        AccessTarget_Both    = AccessTarget_Texture | AccessTarget_Buffer,
    };

    struct AccessState
    {
        PipelineStageFlags Stages = PipelineStage_None;
        MemoryAccessFlags  Memory = MemoryAccess_None;
        ImageLayout        Layout = ImageLayout::Undefined;

        constexpr bool IsReadOnly() const
        {
            return ( Memory & kWriteAccessMask ) == 0;
        }
        constexpr bool operator==( const AccessState& ) const = default;
    };

    struct AccessInfo
    {
        Access           Value;
        std::string_view Name;
        AccessState      State;
        uint8_t          Targets;
    };

    inline constexpr PipelineStageFlags kGraphicsShaderStages =
         PipelineStage_VertexShader | PipelineStage_FragmentShader;
    inline constexpr PipelineStageFlags kAllShaderStages = kGraphicsShaderStages | PipelineStage_ComputeShader;
    inline constexpr PipelineStageFlags kDepthTestStages =
         PipelineStage_EarlyFragmentTests | PipelineStage_LateFragmentTests;

    // THE table. Indexed by Access; the static_assert below proves every row sits at its own index, so a
    // row inserted out of order fails the build instead of shifting every later state by one.
    inline constexpr std::array<AccessInfo, kAccessCount> kAccessTable = { {
         { Access::None,
           "None",
           { PipelineStage_None, MemoryAccess_None, ImageLayout::Undefined },
           AccessTarget_Both },
         { Access::SampledGraphics,
           "SampledGraphics",
           { kGraphicsShaderStages, MemoryAccess_ShaderSampledRead, ImageLayout::ShaderReadOnly },
           AccessTarget_Texture },
         { Access::SampledCompute,
           "SampledCompute",
           { PipelineStage_ComputeShader, MemoryAccess_ShaderSampledRead, ImageLayout::ShaderReadOnly },
           AccessTarget_Texture },
         { Access::StorageRead,
           "StorageRead",
           { kAllShaderStages, MemoryAccess_ShaderStorageRead, ImageLayout::General },
           AccessTarget_Both },
         { Access::StorageWrite,
           "StorageWrite",
           { kAllShaderStages, MemoryAccess_ShaderStorageRead | MemoryAccess_ShaderStorageWrite,
             ImageLayout::General },
           AccessTarget_Both },
         { Access::ColorTarget,
           "ColorTarget",
           { PipelineStage_ColorAttachmentOutput,
             MemoryAccess_ColorAttachmentRead | MemoryAccess_ColorAttachmentWrite, ImageLayout::ColorAttachment },
           AccessTarget_Texture },
         { Access::DepthWrite,
           "DepthWrite",
           { kDepthTestStages, MemoryAccess_DepthStencilRead | MemoryAccess_DepthStencilWrite,
             ImageLayout::DepthStencilAttachment },
           AccessTarget_Texture },
         { Access::DepthRead,
           "DepthRead",
           { kDepthTestStages, MemoryAccess_DepthStencilRead, ImageLayout::DepthStencilReadOnly },
           AccessTarget_Texture },
         { Access::CopySrc,
           "CopySrc",
           { PipelineStage_Copy, MemoryAccess_TransferRead, ImageLayout::TransferSrc },
           AccessTarget_Both },
         { Access::CopyDst,
           "CopyDst",
           { PipelineStage_Copy, MemoryAccess_TransferWrite, ImageLayout::TransferDst },
           AccessTarget_Both },
         { Access::IndirectArgs,
           "IndirectArgs",
           { PipelineStage_DrawIndirect, MemoryAccess_IndirectCommandRead, ImageLayout::Undefined },
           AccessTarget_Buffer },
         { Access::VertexIndex,
           "VertexIndex",
           { PipelineStage_VertexInput, MemoryAccess_IndexRead | MemoryAccess_VertexAttributeRead,
             ImageLayout::Undefined },
           AccessTarget_Buffer },
         { Access::UniformRead,
           "UniformRead",
           { kAllShaderStages, MemoryAccess_UniformRead, ImageLayout::Undefined },
           AccessTarget_Buffer },
         // The presentation engine is outside every pipeline stage: sync2 expresses the hand-off as stage
         // NONE with access NONE, the layout change being the only thing the barrier does.
         { Access::Present,
           "Present",
           { PipelineStage_None, MemoryAccess_None, ImageLayout::Present },
           AccessTarget_Texture },
         { Access::HostRead,
           "HostRead",
           { PipelineStage_Host, MemoryAccess_HostRead, ImageLayout::Undefined },
           AccessTarget_Buffer },
         { Access::AccelStructBuildInput,
           "AccelStructBuildInput",
           { PipelineStage_AccelStructBuild, MemoryAccess_ShaderStorageRead, ImageLayout::Undefined },
           AccessTarget_Buffer },
         { Access::AccelStructBuildWrite,
           "AccelStructBuildWrite",
           { PipelineStage_AccelStructBuild, MemoryAccess_AccelStructRead | MemoryAccess_AccelStructWrite,
             ImageLayout::Undefined },
           AccessTarget_Buffer },
         { Access::AccelStructRead,
           "AccelStructRead",
           { PipelineStage_FragmentShader | PipelineStage_ComputeShader, MemoryAccess_AccelStructRead,
             ImageLayout::Undefined },
           AccessTarget_Buffer },
    } };

    namespace AccessTableDetail
    {
        constexpr bool RowsSitAtTheirIndex()
        {
            for ( uint32_t i = 0; i < kAccessCount; ++i )
            {
                if ( static_cast<uint32_t>( kAccessTable[i].Value ) != i )
                    return false;
            }
            return true;
        }
    } // namespace AccessTableDetail
    static_assert( AccessTableDetail::RowsSitAtTheirIndex(), "kAccessTable row order must match enum Access" );

    constexpr const AccessInfo& GetAccessInfo( Access access )
    {
        return kAccessTable[static_cast<uint32_t>( access )];
    }

    constexpr AccessState GetAccessState( Access access )
    {
        return GetAccessInfo( access ).State;
    }

    constexpr std::string_view GetAccessName( Access access )
    {
        return GetAccessInfo( access ).Name;
    }

    constexpr bool IsWriteAccess( Access access )
    {
        return !GetAccessState( access ).IsReadOnly();
    }

    // Union of two read-only states in ONE layout: the state a merged group of reads transitions into.
    constexpr AccessState MergeReadStates( const AccessState& a, const AccessState& b )
    {
        return AccessState{ a.Stages | b.Stages, a.Memory | b.Memory, a.Layout };
    }

    // True when a resource already in @p current needs no barrier to be used in @p next: both are
    // read-only, share a layout, and everything @p next reads was already made visible to @p current's
    // stages and accesses by the barrier that entered @p current.
    constexpr bool IsCoveredReadState( const AccessState& current, const AccessState& next )
    {
        return current.IsReadOnly() && next.IsReadOnly() && current.Layout == next.Layout &&
               ( next.Stages & ~current.Stages ) == 0 && ( next.Memory & ~current.Memory ) == 0;
    }
} // namespace Desert::Graphic::RDG
