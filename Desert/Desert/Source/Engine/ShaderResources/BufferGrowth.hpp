#pragma once

#include <cstdint>

namespace Desert::ShaderResources
{
    // ------------------------------------------------------------------------------------------------
    // WHETHER A WRITE MAY RESIZE THE BUFFER IT IS WRITING INTO — a decision, extracted so it can be
    // asserted without a device, and so that it is taken in ONE place instead of at each write.
    //
    // The defect this exists to make unwritable: `VulkanStorageBuffer::SetData` grew by calling
    // `RT_Invalidate()`, which destroys every VkBuffer the object owns and allocates fresh ones. For a
    // buffer created with `persistent = true` that is the GPU simulation state gone — the state whose own
    // comment two files away says it "must survive across frames, and a second view must not get a fresh
    // copy of a simulation that has been running". It happened silently, from the per-frame write path,
    // and the caller was told nothing because `SetData` returned `void`.
    //
    // GROWING AND WRITING ARE DIFFERENT OPERATIONS AND THE OLD CODE SPELLED THEM THE SAME. Growing a
    // per-frame buffer is free of consequence: the GPU re-reads the whole thing next frame anyway, and
    // the reflection path deliberately allocates a placeholder 36 bytes that the first real write is
    // expected to replace (ShaderResourcesManager). Growing a buffer that carries state ACROSS frames
    // cannot be done at all — there is nothing to copy the old contents from that would still be
    // correct, because the GPU, not the CPU, is the author of those bytes. So the honest answer for that
    // case is a REFUSAL WITH A NAME, which is only expressible now that SetData answers.
    //
    // WHY THE LIFETIME IS AN ENUM AND NOT THE `bool persistent` IT REPLACES AT THIS SEAM. `false` at a
    // call site says nothing; `Persistence::PerFrame` says which of the two things is being claimed. The
    // buffer's own constructor keeps the bool it has always had — this type is about the DECISION, and a
    // decision taken from an unlabelled boolean is how the wrong branch gets chosen and read as correct.
    // ------------------------------------------------------------------------------------------------

    /// How long the contents of a buffer are required to remain valid.
    enum class Persistence : uint8_t
    {
        /// One copy per (frame in flight x renderer slot); the CPU rewrites it before every read.
        PerFrame = 0,
        /// One shared copy whose contents the GPU itself authors and which must survive across frames.
        AcrossFrames = 1,
        /// GRASS-VIEW-LAW-1 (UE: a GPU-authored FRDGPooledBuffer kept across graphs): AcrossFrames in
        /// device-local memory that the host never maps - no CPU image, no host write, read by address or
        /// bound like any storage buffer. The BAR heap (256 MB without resizable BAR) is not spent on it.
        DeviceOnly = 2,
    };

    /// What a write of @c size bytes at @c offset may do to a buffer of the current capacity.
    enum class BufferWriteVerdict : uint8_t
    {
        Fits                              = 0, ///< Write it; the buffer is big enough as it stands.
        Grow                              = 1, ///< Re-create at the larger size, then write.
        RefuseWouldDestroyPersistentState = 2, ///< Growing would throw away state only the GPU has.
        RefuseWouldNotFitAnyBuffer        = 3, ///< size + offset does not fit in the size type at all.
    };

    /// The bytes a buffer must hold for a write of @p size at @p offset, or 0 when the sum cannot be
    /// represented. Zero is unambiguous as a failure here because a write requiring zero bytes always
    /// fits any buffer, so the two never need telling apart by the caller — ClassifyBufferWrite is the
    /// only reader that has to, and it asks before it computes.
    [[nodiscard]] constexpr uint32_t RequiredBufferSize( uint32_t size, uint32_t offset )
    {
        // UNSIGNED ADDITION WRAPS, AND A WRAPPED REQUIREMENT LOOKS SMALL. `size + offset > m_Size` was
        // the whole bound, so a pair that overflows 32 bits produced a tiny number, compared as "it
        // fits", and the write then ran past the end of the mapping. Г7-C's MappedMemory refuses that
        // write today, but it refuses it as an unexplained size mismatch; refusing it HERE names it.
        if ( offset > UINT32_MAX - size )
            return 0;
        return size + offset;
    }

    /// The whole relation, as one pure function so it can be asserted without a device.
    ///
    ///   * a write inside the current capacity always Fits, whatever the lifetime;
    ///   * an overflowing write GROWS a per-frame buffer — nothing is lost, the GPU re-reads it;
    ///   * an overflowing write is REFUSED on a buffer whose contents outlive the frame;
    ///   * a size + offset that cannot be represented is refused before anything is compared.
    [[nodiscard]] constexpr BufferWriteVerdict ClassifyBufferWrite( uint32_t currentSize, uint32_t size,
                                                                    uint32_t offset, Persistence life )
    {
        const uint32_t required = RequiredBufferSize( size, offset );
        if ( required == 0 && ( size != 0 || offset != 0 ) )
            return BufferWriteVerdict::RefuseWouldNotFitAnyBuffer;
        if ( required <= currentSize )
            return BufferWriteVerdict::Fits;
        if ( life != Persistence::PerFrame )
            return BufferWriteVerdict::RefuseWouldDestroyPersistentState;
        return BufferWriteVerdict::Grow;
    }

    /// Name used in the refusal message. Read in a log beside a buffer name, so it is short and says
    /// what happened rather than which enumerator it was.
    ///
    /// No `default:` — a fifth verdict must stop the compiler here rather than print "unknown" in the
    /// one message somebody will be reading at the time.
    [[nodiscard]] constexpr const char* BufferWriteVerdictName( BufferWriteVerdict verdict )
    {
        switch ( verdict )
        {
            case BufferWriteVerdict::Fits:
                return "fits";
            case BufferWriteVerdict::Grow:
                return "needs a larger buffer";
            case BufferWriteVerdict::RefuseWouldDestroyPersistentState:
                return "would destroy state the GPU authored and that must survive the frame";
            case BufferWriteVerdict::RefuseWouldNotFitAnyBuffer:
                return "asks for more bytes than a buffer size can express";
        }
        return "";
    }
} // namespace Desert::ShaderResources
