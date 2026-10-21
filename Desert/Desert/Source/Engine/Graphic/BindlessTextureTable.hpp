#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Core/Formats/SamplerState.hpp>
#include <Engine/Core/ShaderCompiler/BindlessSetIndex.hpp>

#include <cstdint>
#include <optional>
#include <vector>

// THE GLOBAL BINDLESS TEXTURE TABLE (UE: FRHIDescriptorHandle over the Vulkan bindless descriptor heap,
// r.Vulkan.Bindless). Every texture created from uploaded pixels gets a SLOT for its GPU life; a shader reaches it
// as BindlessTextures[slot] and samples it with one of a small FIXED sampler set
// (Common/BindlessTextures.glslh). Sampled images and samplers are separate arrays, not combined image samplers:
// Metal (MoltenVK) caps samplers at ~1-2k, so a sampler per texture cannot scale, and a sampler is a property of
// the material slot anyway.
//
// One table per device, not per frame x view: its entries are immutable uploaded images in their shader-read
// layout, never written by the render graph, so no view or frame owns a copy (Docs/RENDERER_FRAME_STATE.md).
// An image with storage or attachment usage never gets a slot: the graph cannot order a bindless read it never
// saw declared.
namespace Desert::Graphic
{
    class Image2D;

    // A slot in the table. Stable for the image's GPU life. 0 is reserved: the table's own "missing" texture
    // (magenta), never a silent white — a record that names slot 0 is a defect, visible on screen.
    struct BindlessTextureSlot
    {
        uint32_t Index = 0;

        [[nodiscard]] constexpr bool IsValid() const
        {
            return Index != 0;
        }
        bool operator==( const BindlessTextureSlot& ) const = default;
    };

    // An index into the fixed sampler array: one sampler per Core::Formats::SamplerState, so every material
    // slot's state has its exact sampler and the array never grows.
    struct BindlessSamplerSlot
    {
        uint32_t Index                                          = 0;
        bool     operator==( const BindlessSamplerSlot& ) const = default;
    };

    // The sampler set's size is shader-visible (DESERT_BINDLESS_SAMPLER_COUNT), so it lives with the other shader
    // defines (Core/ShaderCompiler/BindlessSetIndex.hpp).
    using ::Desert::Core::kBindlessSamplerFilterCount;
    using ::Desert::Core::kBindlessSamplerStateCount;
    using ::Desert::Core::kBindlessSamplerWrapCount;

    // The sampler-array index of @p state; the shader header spells the same numbering (BINDLESS_SAMPLER_*).
    [[nodiscard]] constexpr BindlessSamplerSlot BindlessSamplerFor( const Core::Formats::SamplerState& state )
    {
        return {
             static_cast<uint32_t>( state.WrapU ) +
             ( kBindlessSamplerWrapCount * static_cast<uint32_t>( state.WrapV ) ) +
             ( kBindlessSamplerWrapCount * kBindlessSamplerWrapCount * static_cast<uint32_t>( state.Filter ) ) };
    }

    // The inverse: the state the sampler at @p index samples with (the table fills its sampler array from it).
    [[nodiscard]] constexpr Core::Formats::SamplerState BindlessSamplerState( uint32_t index )
    {
        Core::Formats::SamplerState state;
        state.WrapU  = static_cast<Core::Formats::SamplerWrap>( index % kBindlessSamplerWrapCount );
        state.WrapV  = static_cast<Core::Formats::SamplerWrap>( ( index / kBindlessSamplerWrapCount ) %
                                                                kBindlessSamplerWrapCount );
        state.Filter = static_cast<Core::Formats::SamplerFilter>(
             index / ( kBindlessSamplerWrapCount * kBindlessSamplerWrapCount ) );
        return state;
    }

    // PURE: the slot numbering. A released slot is reusable only once every frame that could still read its old
    // descriptor has completed (the FrameRetireQueue rule: frame >= released + framesInFlight + 1), which is what
    // makes UPDATE_UNUSED_WHILE_PENDING writes into the live table safe without per-frame copies of it.
    class BindlessSlotAllocator
    {
    public:
        // @p capacity counts slot 0, which is reserved and never handed out.
        explicit BindlessSlotAllocator( uint32_t capacity );

        // The lowest free slot; nullopt when every slot is live or still retiring — the table then grows.
        [[nodiscard]] std::optional<uint32_t> Allocate();
        // Parks @p slot until Retire sees @p frame retired. A slot that is not live is a defect (verified).
        void Release( uint32_t slot, uint64_t frame );
        // Makes every slot parked at or before frame - framesInFlight - 1 allocatable again; returns how many.
        uint32_t Retire( uint64_t frame, uint32_t framesInFlight );
        // Raises the capacity; every live slot keeps its index. A smaller capacity is a defect (verified).
        void Grow( uint32_t newCapacity );

        [[nodiscard]] uint32_t Live() const
        {
            return m_Live;
        }
        [[nodiscard]] uint32_t Capacity() const
        {
            return m_Capacity;
        }
        [[nodiscard]] uint32_t Retiring() const
        {
            return static_cast<uint32_t>( m_Parked.size() );
        }

    private:
        struct Parked
        {
            uint32_t Slot  = 0;
            uint64_t Frame = 0;
        };

        uint32_t              m_Capacity = 0;
        uint32_t              m_Next     = 1; // slots below it were handed out at least once
        uint32_t              m_Live     = 0;
        std::vector<uint32_t> m_Free;   // released and retired, reusable now
        std::vector<Parked>   m_Parked; // released, still possibly read by a frame in flight
        std::vector<bool>     m_IsLive;
    };

    // The largest capacity the table may grow to under a device ceiling (the smaller of the per-stage and
    // per-set update-after-bind sampled-image limits): @p initial doubled while the doubled size stays at most
    // half the ceiling. The other half is left to the rest of a pipeline layout — a stage's sampled images are
    // counted across ALL its sets against the same per-stage limit (VUID-VkPipelineLayoutCreateInfo-
    // descriptorType-03022), so a table sized to the whole ceiling would make every layout that includes it
    // invalid. 0 when even @p initial does not fit that way.
    [[nodiscard]] constexpr uint32_t BindlessCapacityCeiling( uint32_t initial, uint32_t deviceCeiling )
    {
        if ( initial == 0 || static_cast<uint64_t>( initial ) * 2 > deviceCeiling )
            return 0;
        uint64_t capacity = initial;
        while ( capacity * 4 <= deviceCeiling )
            capacity *= 2;
        return static_cast<uint32_t>( capacity );
    }

    class BindlessTextureTable
    {
    public:
        virtual ~BindlessTextureTable() = default;

        // A slot for @p image, written into the table. Refused (error naming the image) for an image with storage
        // or attachment usage, a multisampled one, or one without a sampling view; and when the table is at its
        // device ceiling (error with live count and ceiling).
        [[nodiscard]] virtual Common::ResultStr<BindlessTextureSlot> Register( const Image2D& image ) = 0;
        // Gives @p slot back; it is reused only after the frames in flight have retired (BindlessSlotAllocator).
        virtual void Release( BindlessTextureSlot slot ) = 0;
        // Once per frame, after the frame counter advanced: retires released slots and the descriptor sets that
        // growth or a sampler change replaced.
        virtual void EndFrame( uint64_t frame, uint32_t framesInFlight ) = 0;

        [[nodiscard]] virtual uint32_t LiveSlots() const = 0; ///< overlay + memory readout
        [[nodiscard]] virtual uint32_t Capacity() const  = 0;
    };

    // The device's table (VulkanLogicalDevice owns it; made on first use, after the device and its caps exist).
    [[nodiscard]] BindlessTextureTable& GetBindlessTextureTable();
    // Null once the device has released it: an image destroyed after the device's teardown has no slot to give
    // back.
    [[nodiscard]] BindlessTextureTable* TryGetBindlessTextureTable();
} // namespace Desert::Graphic
