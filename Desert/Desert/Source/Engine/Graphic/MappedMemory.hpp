#pragma once

#include <Common/Core/Core.hpp>
#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace Desert::Graphic
{
    // A LIVE MAPPING OF DEVICE MEMORY, OR A NAMED REFUSAL — AND THERE IS NO WAY TO REACH THE BYTES
    // WITHOUT THE ANSWER ARRIVING WITH THEM.
    //
    // The defect this type exists to make unwritable: `VulkanAllocator::MapMemory` used to hand back a
    // `uint8_t*`, and sixteen call sites took it. One turned a null into a refusal. Ten went straight into
    // `memcpy( mapped, source, size )`. A `memcpy` through null is not an exception and not a failed
    // operation — it is memory corruption or an immediate, unexplained death of the process, and it is the
    // same class Ф1 closed for file reads ("a primitive that aborts kills every soft branch above it")
    // left open for GPU memory. Making the mapping SAY why it failed (which it now does) changed nothing
    // about that: the failure merely became loud before writing through null anyway.
    //
    // WHY A TYPE AND NOT THIRTEEN `if`s. Thirteen hand-written checks are thirteen places for a fourteenth
    // to be forgotten, and the fourteenth is written by somebody who never read this paragraph. The same
    // move as Ф3's typed file read: the OLD SHAPE MUST NOT COMPILE. There is no `Data()`, no `Get()`, no
    // conversion to `void*` and no `operator*` here — deliberately, and asserted by `MappedMemoryGuard`
    // rather than merely intended — so `memcpy( mapping, src, n )` is a compile error at every call site
    // that could ever be written, not a crash at the one that was.
    //
    // AND IT BUYS WHAT Ф3's RETURN TYPE COULD NOT. `Common::ResultStr<T>::GetValue()` on a FAILED result
    // returns `T{}`, which for a pointer is a null the caller cannot tell from a mapped one — so
    // `ResultStr<uint8_t*>` would have been the same defect one method call further down (that is task Ф4,
    // filed separately). The guarantee here does not rest on anybody's discipline with an unwrap, because
    // there is no unwrap.
    //
    // WHAT YOU DO INSTEAD is name the transfer: `Write`, `ReadInto`, `Fill`. Each answers
    // `Common::BoolResultStr` and each is NO_DISCARD, so a caller that ignores the answer is a warning in
    // a workspace that has built with `warnings "Extra"` since 2026-09-06.
    //
    // THE BOUNDS ARE CHECKED TOO, and that is a second defect closed on the way past. Every one of the ten
    // `memcpy` sites trusted a size computed somewhere else against a buffer allocated somewhere else —
    // `VulkanVertexBuffer::SetData` wrote `dst + offset` for a caller-supplied offset with no bound of any
    // kind. A write that runs off the end of a mapping corrupts whatever VMA placed after it, with no
    // symptom at the site. Here it is a named refusal carrying both numbers.
    //
    // RAII, so the unmap cannot be forgotten or skipped by an early return: the mapping unmaps itself when
    // it goes out of scope, and the thirteen hand-paired `UnmapMemory` calls are gone with the pointer they
    // guarded. Move-only, because two owners would unmap twice.
    //
    // NO VULKAN IN THIS HEADER, ON PURPOSE. It sits beside `DeviceLost.hpp` for the same reason that one
    // does: the contract is about the engine, the implementation happens to be Vulkan's. The unmap is a
    // function pointer supplied by whoever made the mapping, so `Engine/Graphic` stays free of VMA and the
    // suite that proves the guarantees needs neither a device nor a driver to run.
    //
    // A WRITE REACHES THE DEVICE, A READ SEES THE DEVICE — ON ANY HEAP. Host-visible memory without
    // HOST_COHERENT keeps what the host wrote in the CPU's caches until the range is flushed, and keeps
    // stale lines over what the device wrote until it is invalidated; unmapping does neither (vmaUnmapMemory
    // flushes nothing). So the transfer that touched the bytes is the one that answers for them: `Write` and
    // `Fill` flush exactly the range they wrote, `ReadInto` invalidates exactly the range it reads, and the
    // owner of the memory supplies both hooks only when the allocation is NOT coherent — on coherent memory
    // they are null and nothing is called. Per transfer rather than at unmap, because the uniform and
    // storage copies stay mapped for their whole life and are never unmapped between frames (UE VulkanRHI:
    // FlushMappedMemoryRanges on unlock of a non-coherent range; VMA aligns the range to nonCoherentAtomSize).
    //
    // A PASS OF MANY WRITES FLUSHES ONCE: MappedWriteBatch (below) holds the mappings of one write pass,
    // records the ranges its writes touched instead of flushing each, and flushes them all in one call
    // into the owner (vmaFlushAllocations) — see there.
    class MappedWriteBatch;

    class MappedMemory
    {
    public:
        /// How the owner of the memory releases it. Receives the opaque allocation handle given to Live().
        using UnmapFn = void ( * )( void* allocation );

        /// How the owner of non-coherent memory flushes a range the host wrote, or invalidates a range the
        /// host is about to read: [@p offset, @p offset + @p bytes) of @p allocation, while it is mapped.
        using RangeFn = Common::BoolResultStr ( * )( void* allocation, std::size_t offset, std::size_t bytes );

        /// How the owner of non-coherent memory flushes @p count ranges the host wrote, in ONE call: range i is
        /// [@p offsets[i], @p offsets[i] + @p sizes[i]) of @p allocations[i], each mapped. MappedWriteBatch's
        /// flush; 64-bit offsets and sizes because that is what the device takes (VkDeviceSize).
        using RangesFn = Common::BoolResultStr ( * )( void* const* allocations, const uint64_t* offsets,
                                                      const uint64_t* sizes, std::size_t count );

        /// An unmapped mapping that refuses everything. The default state is a refusal rather than an
        /// empty success: §1.4 of the contract — "not ready" and "nothing to do" must not be one value.
        MappedMemory() = default;

        /// The mapping did not happen, and this is why. @p reason is what the caller will print.
        static MappedMemory Refused( std::string reason )
        {
            MappedMemory refused;
            if ( !reason.empty() )
            {
                refused.m_Refusal = std::move( reason );
                refused.m_Reason  = nullptr; // the specific reason wins over the generic one
            }
            return refused;
        }

        /// A live mapping of @p size bytes at @p bytes, released through @p unmap( @p allocation ). @p flush,
        /// @p invalidate and @p flushRanges are null for coherent memory and set for memory that is not (see
        /// above); @p flushRanges is the many-range flush a MappedWriteBatch ends its pass with.
        static MappedMemory Live( void* allocation, uint8_t* bytes, std::size_t size, UnmapFn unmap,
                                  RangeFn flush = nullptr, RangeFn invalidate = nullptr,
                                  RangesFn flushRanges = nullptr )
        {
            if ( bytes == nullptr )
                return Refused( "the mapping reported success and handed back no address" );

            MappedMemory live;
            live.m_Allocation = allocation;
            live.m_Bytes      = bytes;
            live.m_Size       = size;
            live.m_Unmap      = unmap;
            live.m_Flush      = flush;
            live.m_Invalidate  = invalidate;
            live.m_FlushRanges = flushRanges;
            live.m_Reason      = nullptr;
            return live;
        }

        ~MappedMemory()
        {
            Unmap();
        }

        MappedMemory( MappedMemory&& other ) noexcept
        {
            Adopt( std::move( other ) );
        }

        MappedMemory& operator=( MappedMemory&& other ) noexcept
        {
            if ( this != &other )
            {
                Unmap();
                Adopt( std::move( other ) );
            }
            return *this;
        }

        MappedMemory( const MappedMemory& )            = delete;
        MappedMemory& operator=( const MappedMemory& ) = delete;

        bool IsMapped() const noexcept
        {
            return m_Bytes != nullptr;
        }

        /// Explicit on purpose. An implicit one would let `memcpy( mapping, ... )` find a conversion.
        explicit operator bool() const noexcept
        {
            return IsMapped();
        }

        std::size_t GetSize() const noexcept
        {
            return m_Size;
        }

        /// Why there is nothing to write to. Empty while the mapping is live.
        ///
        /// By value, and the reason is worth a line: a mapping is created on the hot upload path (every
        /// dynamic vertex and index buffer, every frame), so the SUCCESS path must not allocate. Holding
        /// the three standing reasons as `const char*` and only the formatted one as a `std::string`
        /// keeps a live mapping allocation-free from construction to destruction — the first draft
        /// default-initialised a 35-character `std::string`, which is one malloc/free per upload for a
        /// sentence nobody was ever going to read.
        std::string GetRefusal() const
        {
            if ( !m_Refusal.empty() )
                return m_Refusal;
            return m_Reason != nullptr ? std::string( m_Reason ) : std::string();
        }

        /// @p bytes from @p source into the mapping at @p offset. Refuses, writing nothing, when the
        /// mapping is not live or the range does not fit.
        NO_DISCARD Common::BoolResultStr Write( const void* source, std::size_t bytes, std::size_t offset = 0 )
        {
            const auto copied = Copy( source, bytes, offset );
            if ( !copied.IsSuccess() || bytes == 0 )
                return copied;
            return Flushed( offset, bytes );
        }

        /// @p bytes out of the mapping at @p offset into @p destination. The readback direction — the
        /// unchecked form of this one dereferenced null as the SOURCE, which reads as a segfault in
        /// `memcpy` with no line of ours anywhere near it.
        NO_DISCARD Common::BoolResultStr ReadInto( void* destination, std::size_t bytes,
                                                   std::size_t offset = 0 ) const
        {
            if ( destination == nullptr && bytes != 0 )
                return Common::MakeError<bool>( "a mapped read was given a null destination" );

            const auto room = Admits( bytes, offset, "read" );
            if ( !room.IsSuccess() )
                return room;

            if ( bytes == 0 )
                return Common::MakeSuccess( true );
            if ( m_Invalidate != nullptr )
            {
                const auto invalidated = m_Invalidate( m_Allocation, offset, bytes );
                if ( !invalidated.IsSuccess() )
                    return invalidated;
            }
            std::memcpy( destination, m_Bytes + offset, bytes );
            return Common::MakeSuccess( true );
        }

        /// @p bytes of @p value into the mapping at @p offset.
        NO_DISCARD Common::BoolResultStr Fill( uint8_t value, std::size_t bytes, std::size_t offset = 0 )
        {
            const auto room = Admits( bytes, offset, "fill" );
            if ( !room.IsSuccess() )
                return room;

            if ( bytes == 0 )
                return Common::MakeSuccess( true );
            std::memset( m_Bytes + offset, value, bytes );
            return Flushed( offset, bytes );
        }

        /// Release early. Idempotent, and the destructor's own path — a mapping that has been unmapped
        /// refuses every transfer afterwards rather than writing into memory the driver has taken back.
        void Unmap() noexcept
        {
            // A mapping that was never live keeps the reason it already had — "the map failed because X"
            // is more use to the next reader than "released".
            const bool wasLive = m_Bytes != nullptr;
            if ( wasLive && m_Unmap != nullptr )
                m_Unmap( m_Allocation );
            m_Allocation = nullptr;
            m_Bytes      = nullptr;
            m_Size       = 0;
            m_Unmap      = nullptr;
            m_Flush       = nullptr;
            m_Invalidate  = nullptr;
            m_FlushRanges = nullptr;
            if ( wasLive )
            {
                m_Refusal.clear();
                m_Reason = kReleased;
            }
        }

    private:
        // The batch writes through Copy and flushes what it recorded in one call; nothing else may skip the flush.
        friend class MappedWriteBatch;

        static constexpr const char* kNeverAttempted = "the mapping was never attempted";
        static constexpr const char* kReleased       = "the mapping has already been released";
        static constexpr const char* kMovedOut       = "the mapping was moved out of this object";

        void Adopt( MappedMemory&& other ) noexcept
        {
            m_Allocation = other.m_Allocation;
            m_Bytes      = other.m_Bytes;
            m_Size       = other.m_Size;
            m_Unmap      = other.m_Unmap;
            m_Flush      = other.m_Flush;
            m_Invalidate  = other.m_Invalidate;
            m_FlushRanges = other.m_FlushRanges;
            m_Reason      = other.m_Reason;
            m_Refusal    = std::move( other.m_Refusal );

            other.m_Allocation = nullptr;
            other.m_Bytes      = nullptr;
            other.m_Size       = 0;
            other.m_Unmap      = nullptr;
            other.m_Flush      = nullptr;
            other.m_Invalidate  = nullptr;
            other.m_FlushRanges = nullptr;
            other.m_Reason      = kMovedOut;
            other.m_Refusal.clear();
        }

        // The one place the two questions are asked, so a new transfer cannot answer only one of them.
        // `offset > m_Size` is tested BEFORE `m_Size - offset` for the reason the subtraction is unsigned:
        // the other order wraps and admits the write it was meant to refuse.
        Common::BoolResultStr Admits( std::size_t bytes, std::size_t offset, const char* what ) const
        {
            if ( !IsMapped() )
                return Common::MakeFormattedError<bool>( "a mapped {} of {} byte(s) was refused: {}", what, bytes,
                                                         GetRefusal() );
            if ( offset > m_Size || bytes > m_Size - offset )
                return Common::MakeFormattedError<bool>(
                     "a mapped {} of {} byte(s) at offset {} does not fit a {}-byte mapping", what, bytes, offset,
                     m_Size );
            return Common::MakeSuccess( true );
        }

        // The copy half of Write, without the flush: the bytes land in the mapping and the caller answers for
        // making them visible — Write by flushing the range at once, MappedWriteBatch by recording it.
        Common::BoolResultStr Copy( const void* source, std::size_t bytes, std::size_t offset )
        {
            if ( source == nullptr && bytes != 0 )
                return Common::MakeError<bool>( "a mapped write was given a null source" );

            const auto room = Admits( bytes, offset, "write" );
            if ( !room.IsSuccess() )
                return room;

            if ( bytes != 0 )
                std::memcpy( m_Bytes + offset, source, bytes );
            return Common::MakeSuccess( true );
        }

        // The range a write has just touched, made visible to the device; nothing to do on coherent memory.
        Common::BoolResultStr Flushed( std::size_t offset, std::size_t bytes ) const
        {
            if ( m_Flush == nullptr )
                return Common::MakeSuccess( true );
            return m_Flush( m_Allocation, offset, bytes );
        }

        void*       m_Allocation  = nullptr;
        uint8_t*    m_Bytes       = nullptr;
        std::size_t m_Size        = 0;
        UnmapFn     m_Unmap       = nullptr;
        RangeFn     m_Flush       = nullptr; // null: coherent memory, a write needs no flush
        RangeFn     m_Invalidate  = nullptr; // null: coherent memory, a read needs no invalidate
        RangesFn    m_FlushRanges = nullptr; // null: coherent memory, a batch of writes needs no flush
        // The standing reason, or null while the mapping is live. A pointer rather than a string so that
        // a successful map costs no allocation at all (see GetRefusal).
        const char* m_Reason = kNeverAttempted;
        // The reason that had to be built — the driver's own words. Empty unless one was given.
        std::string m_Refusal;
    };

    // ONE WRITE PASS, ONE FLUSH. A pass that writes many disjoint ranges — the shader binding table's runs of
    // changed records and the material rows' runs of changed rows, every trace — would flush each range with
    // its own driver call if it went through MappedMemory::Write. The batch HOLDS the pass's mappings, copies
    // each write without flushing, records the range only when the memory is not coherent, and Flush() hands
    // every recorded range of every held mapping to the owner in one call (vmaFlushAllocations; UE VulkanRHI
    // and VMA flush a set of written ranges with one vkFlushMappedMemoryRanges the same way). On coherent memory
    // nothing is recorded and Flush() calls nothing.
    //
    // WHY THE BATCH OWNS THE MAPPINGS. A flush is only defined while the memory is mapped, so the ranges must be
    // flushed before any mapping they name is released. Holding the mappings makes that the class's own order:
    // the destructor flushes what is still pending, then the members unmap. Flush() is the call that REPORTS;
    // the destructor's flush is the backstop for a pass that left early (its error path already answered) and
    // its failure is noted by the owner's hook (device loss) — it is never the path a finished pass relies on.
    class MappedWriteBatch
    {
    public:
        MappedWriteBatch() = default;

        ~MappedWriteBatch()
        {
            (void)Flush();
        }

        MappedWriteBatch( const MappedWriteBatch& )            = delete;
        MappedWriteBatch& operator=( const MappedWriteBatch& ) = delete;
        MappedWriteBatch( MappedWriteBatch&& )                 = delete;
        MappedWriteBatch& operator=( MappedWriteBatch&& )      = delete;

        /// Keeps @p mapping until the pass ends and answers the slot its writes name. A refused mapping is held
        /// as what it is: every write through its slot refuses with the mapping's own reason.
        std::size_t Hold( MappedMemory&& mapping )
        {
            m_Mappings.push_back( std::move( mapping ) );
            return m_Mappings.size() - 1u;
        }

        /// @p bytes from @p source into the mapping held at @p slot, at @p offset — the bounds and refusals of
        /// MappedMemory::Write — with the flush left to Flush().
        NO_DISCARD Common::BoolResultStr Write( std::size_t slot, const void* source, std::size_t bytes,
                                                std::size_t offset )
        {
            if ( slot >= m_Mappings.size() )
                return Common::MakeFormattedError<bool>( "a batched write named mapping {} of the {} held", slot,
                                                         m_Mappings.size() );
            MappedMemory& mapping = m_Mappings[slot];
            if ( mapping.m_Flush != nullptr )
            {
                if ( mapping.m_FlushRanges == nullptr )
                    return Common::MakeError<bool>( "a batched write to non-coherent memory was refused: the mapping's "
                                                    "owner gave no many-range flush" );
                if ( m_FlushRanges != nullptr && m_FlushRanges != mapping.m_FlushRanges )
                    return Common::MakeError<bool>( "a batched write was refused: its mapping is flushed by another "
                                                    "owner than the batch's other mappings" );
            }

            const auto copied = mapping.Copy( source, bytes, offset );
            if ( !copied.IsSuccess() || bytes == 0 || mapping.m_Flush == nullptr )
                return copied;

            m_FlushRanges = mapping.m_FlushRanges;
            m_Slots.push_back( slot );
            m_Offsets.push_back( static_cast<uint64_t>( offset ) );
            m_Sizes.push_back( static_cast<uint64_t>( bytes ) );
            return Common::MakeSuccess( true );
        }

        /// Every range written since the last flush, made visible to the device in ONE call into the owner; on
        /// coherent memory nothing was recorded and nothing is called. The held mappings stay mapped.
        NO_DISCARD Common::BoolResultStr Flush()
        {
            if ( m_Slots.empty() )
                return Common::MakeSuccess( true );

            std::vector<void*> allocations;
            allocations.reserve( m_Slots.size() );
            for ( const std::size_t slot : m_Slots )
                allocations.push_back( m_Mappings[slot].m_Allocation );
            const auto flushed = m_FlushRanges( allocations.data(), m_Offsets.data(), m_Sizes.data(), m_Slots.size() );
            m_Slots.clear();
            m_Offsets.clear();
            m_Sizes.clear();
            return flushed;
        }

    private:
        std::vector<MappedMemory> m_Mappings; // indexed by the slot Hold answered; moved on growth, never re-held
        // The ranges awaiting the flush, one entry per write to non-coherent memory: mapping slot, offset, bytes.
        std::vector<std::size_t>  m_Slots;
        std::vector<uint64_t>     m_Offsets;
        std::vector<uint64_t>     m_Sizes;
        MappedMemory::RangesFn    m_FlushRanges = nullptr; // the owner every recorded range is flushed through
    };
} // namespace Desert::Graphic
