#include <Engine/ShaderResources/API/Vulkan/VulkanStorageBuffer.hpp>

#include <Common/Core/DevInstruments.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/API/Vulkan/DeviceCaps.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanVertexBuffer.hpp>

#include <cstring>

namespace Desert::ShaderResources::API::Vulkan
{
    namespace
    {
        // STORAGE for every reader and writer; INDIRECT because a storage buffer a compute pass fills is a draw's
        // indirect arguments: the particle Counters (ParticleCompact writes a VkDrawIndirectCommand per emitter,
        // ParticleRenderer::DrawPass reads it through Renderer::DrawProceduralIndirect); TRANSFER_DST because a
        // persistent buffer imported into a graph is filled by the graph's upload
        // (RDG::Builder::QueueBufferUpload, a copy) when its content changes, so a frame in flight never reads a
        // buffer the CPU is writing (GlobalDistanceField's mesh SDF records); TRANSFER_SRC because a pooled buffer
        // that grows is copied into its successor by the graph (RDG::Builder::AddCopyBufferPass, GrassRenderer
        // BufferPool::Commit), so the ranges it held keep their contents.
        constexpr VkBufferUsageFlags kUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                              VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                              VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        // GRASS-CULL-2: a storage buffer is also READ BY ADDRESS where the device has bufferDeviceAddress (its own
        // DeviceCaps row; the allocator is created with the device-address flag under the same row,
        // VulkanAllocator.cpp): a pass over many buffers names them in one table (GetDeviceAddress - the grass
        // variety table). A usage bit of a feature that is not enabled is invalid, so the bit follows the row.
        bool HasDeviceAddress()
        {
            const auto& allocator = SP_CAST( Desert::Graphic::API::Vulkan::VulkanContext,
                                             EngineContext::GetInstance().GetRendererContext() )
                                         ->GetVulkanAllocator();
            return allocator &&
                   allocator->GetDeviceCaps().Has( Desert::Graphic::API::Vulkan::Capability::BufferDeviceAddress );
        }

        VkBufferUsageFlags StorageUsage()
        {
            if ( HasDeviceAddress() )
                return kUsage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            return kUsage;
        }
    } // namespace

    namespace
    {
        // The bool the first constructor has always taken, in the lifetime vocabulary (BufferGrowth.hpp).
        Persistence LifetimeOf( const bool persistent )
        {
            if ( persistent )
                return Persistence::AcrossFrames;
            return Persistence::PerFrame;
        }
    } // namespace

    VulkanStorageBuffer::VulkanStorageBuffer( const std::string_view bufferName, uint32_t size, uint32_t binding,
                                              bool persistent )
         : VulkanStorageBuffer( bufferName, size, binding, LifetimeOf( persistent ) )
    {
    }

    VulkanStorageBuffer::VulkanStorageBuffer( const std::string_view bufferName, uint32_t size, uint32_t binding,
                                              const Persistence lifetime )
         : m_Lifetime( lifetime ), m_Binding( binding ), m_BufferName( bufferName )
    {
        NameLedgerRow( m_BufferName );
        if ( size == 0 )
            size = 1; // Vulkan requires size > 0; a per-frame buffer grows on its first SetData.

        if ( !IsPersistent() )
        {
            // No eager "frames x renderer slots" matrix: a copy exists only for a (view, frame in flight)
            // that actually wrote or bound this buffer, and its allocation refusal surfaces there.
            m_PerView = std::make_unique<ViewCopiedBlock>( size );
            m_Built   = BOOLSUCCESS;
            return;
        }

        m_SharedSize               = size;
        Common::BoolResultStr made = BOOLSUCCESS;
        if ( IsDeviceOnly() )
        {
            // GRASS-VIEW-LAW-1: device-local, never mapped, no CPU image (Persistence::DeviceOnly).
            made = MakeDeviceBufferCopy( std::format( "{}-StorageBuffer-DeviceOnly", m_BufferName ),
                                         StorageUsage(), size, m_Shared );
        }
        else
        {
            m_SharedImage.assign( size, 0 );
            made = MakeMappedBufferCopy( std::format( "{}-StorageBuffer-Persistent", m_BufferName ),
                                         StorageUsage(), size, m_Shared );
        }
        // A constructor has no channel, so the answer is kept and handed to every later question.
        m_Built = made.IsSuccess() ? Common::MakeSuccess( true )
                                   : Common::MakeFormattedError<bool>( "persistent storage buffer '{}': {}",
                                                                       m_BufferName, made.GetError() );
        if ( !m_Built.IsSuccess() )
            LOG_ERROR( "[StorageBuffer] '{}' was not built: {}", m_BufferName, m_Built.GetError() );
    }

    uint32_t VulkanStorageBuffer::GetSize() const
    {
        return m_PerView ? m_PerView->GetSize() : m_SharedSize;
    }

    const void* VulkanStorageBuffer::GetData() const
    {
        if ( IsDeviceOnly() )
            return nullptr; // no CPU image: the GPU alone authors it
        return m_PerView ? static_cast<const void*>( m_PerView->GetContents() )
                         : static_cast<const void*>( m_SharedImage.data() );
    }

    Common::BoolResultStr VulkanStorageBuffer::MakeCopy( std::string_view viewName, uint32_t frameIndex,
                                                         std::unique_ptr<IBlockCopy>& out ) const
    {
        std::unique_ptr<MappedBufferCopy> copy;
        // MEM-3: a read-back buffer's copies are host-cached (Persistence::PerFrameReadback).
        const MappedCopyMemory memory =
             m_Lifetime == Persistence::PerFrameReadback ? MappedCopyMemory::Readback : MappedCopyMemory::Upload;
        const auto made = MakeMappedBufferCopy(
             std::format( "{}-StorageBuffer-{}-Frame{}", m_BufferName, viewName, frameIndex ), StorageUsage(),
             GetSize(), copy, memory );
        if ( !made.IsSuccess() )
            return Common::MakeFormattedError<bool>( "storage buffer '{}' copy for view '{}', frame {}: {}",
                                                     m_BufferName, viewName, frameIndex, made.GetError() );
        out = std::move( copy );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanStorageBuffer::ActiveCopy( uint32_t frameIndex, MappedBufferCopy*& out )
    {
        if ( !m_Built.IsSuccess() )
            return Common::MakeFormattedError<bool>( "storage buffer '{}' is not usable: {}", m_BufferName,
                                                     m_Built.GetError() );
        if ( IsPersistent() )
        {
            out = m_Shared.get();
            return BOOLSUCCESS;
        }

        IBlockCopy* copy     = nullptr;
        const auto  resolved = m_PerView->Resolve(
             frameIndex, [this]( std::string_view view, uint32_t frame, std::unique_ptr<IBlockCopy>& made )
             { return MakeCopy( view, frame, made ); }, copy );
        if ( !resolved.IsSuccess() )
            return Common::MakeFormattedError<bool>( "storage buffer '{}': {}", m_BufferName,
                                                     resolved.GetError() );
        // Only MakeCopy creates copies under this block's key, and it only creates MappedBufferCopy.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        out = static_cast<MappedBufferCopy*>( copy );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanStorageBuffer::MakeRoom( const uint32_t size, const uint32_t offset )
    {
        // GROWING IS NOT WRITING. For a PERSISTENT buffer a resize is the GPU simulation state gone, so the
        // decision has a name and lives in one pure function a test can reach without a device.
        switch ( ClassifyBufferWrite( GetSize(), size, offset, m_Lifetime ) )
        {
            case BufferWriteVerdict::Fits:
                break;

            case BufferWriteVerdict::Grow:
                // RDG-PARALLEL-6: never while a pass records. The growth drops every view's copy - the one this
                // frame's descriptors already point at - and the re-make is a VkBuffer on a recording worker.
                if ( Desert::Graphic::RDG::RecordingSeal::IsSealed() )
                    return Common::MakeFormattedError<bool>(
                         "storage buffer '{}' holds {} bytes and a write of {} at offset {} would grow it while a "
                         "pass records; the pass reserves it in its setup (StorageBuffer::Reserve) and declares it "
                         "(PassBuilder::Prepares), so it grows before the graph records",
                         m_BufferName, GetSize(), size, offset );
                // Per-frame only (ClassifyBufferWrite never grows a persistent buffer). Nothing is
                // allocated here: every view's copy is dropped and re-made at the new size by the next write
                // or by the view's next bind, seeded from the CPU image, which keeps its contents.
                // An allocation that cannot be made is refused at that point, with its size.
                m_PerView->Grow( RequiredBufferSize( size, offset ) );
                break;

            case BufferWriteVerdict::RefuseWouldDestroyPersistentState:
                return Common::MakeFormattedError<bool>(
                     "storage buffer '{}' holds {} bytes and a write of {} at offset {} {}. The buffer was "
                     "created persistent, so its contents are authored by the GPU and there is nothing on "
                     "the CPU to restore them from -- create it at the size it needs instead of letting a "
                     "write resize it",
                     m_BufferName, GetSize(), size, offset,
                     BufferWriteVerdictName( BufferWriteVerdict::RefuseWouldDestroyPersistentState ) );

            case BufferWriteVerdict::RefuseWouldNotFitAnyBuffer:
                return Common::MakeFormattedError<bool>(
                     "storage buffer '{}': a write of {} bytes at offset {} {}", m_BufferName, size, offset,
                     BufferWriteVerdictName( BufferWriteVerdict::RefuseWouldNotFitAnyBuffer ) );
        }
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanStorageBuffer::Reserve( const uint32_t size )
    {
        if ( !m_Built.IsSuccess() )
            return Common::MakeFormattedError<bool>( "storage buffer '{}' is not usable: {}", m_BufferName,
                                                     m_Built.GetError() );
        // A reservation is the room a write of @p size bytes at offset 0 needs: the same verdict, the same refusals.
        return MakeRoom( size, 0 );
    }

    Common::BoolResultStr VulkanStorageBuffer::PrepareForRecording() const
    {
        if ( !m_Built.IsSuccess() )
            return Common::MakeFormattedError<bool>( "storage buffer '{}' is not usable: {}", m_BufferName,
                                                     m_Built.GetError() );
        if ( IsPersistent() )
            return BOOLSUCCESS; // one buffer, made at construction
        IBlockCopy* copy     = nullptr;
        const auto  resolved = m_PerView->Resolve(
             EngineContext::GetInstance().GetCurrentFrameIndex(),
             [this]( std::string_view view, uint32_t frame, std::unique_ptr<IBlockCopy>& made )
             { return MakeCopy( view, frame, made ); }, copy );
        if ( !resolved.IsSuccess() )
            return Common::MakeFormattedError<bool>( "storage buffer '{}': {}", m_BufferName, resolved.GetError() );
        return BOOLSUCCESS;
    }

    uint64_t VulkanStorageBuffer::ActiveAppliedVersion() const
    {
        if ( IsPersistent() || !m_PerView )
            return 0; // one device buffer, no per-copy record: its writer writes every time
        return m_PerView->ActiveAppliedVersion( EngineContext::GetInstance().GetCurrentFrameIndex() );
    }

    void VulkanStorageBuffer::NoteActiveApplied( const uint64_t version )
    {
        if ( IsPersistent() || !m_PerView )
            return;
        m_PerView->NoteActiveApplied( EngineContext::GetInstance().GetCurrentFrameIndex(), version );
    }

    Common::BoolResultStr VulkanStorageBuffer::SetData( const void* data, uint32_t size, uint32_t offset )
    {
        if ( !m_Built.IsSuccess() )
            return Common::MakeFormattedError<bool>( "storage buffer '{}' is not usable: {}", m_BufferName,
                                                     m_Built.GetError() );
        if ( IsDeviceOnly() )
            return Common::MakeFormattedError<bool>(
                 "storage buffer '{}' is device-local and GPU-authored (StorageBuffer::CreateDeviceLocal): the "
                 "host "
                 "never maps it, so a write of {} byte(s) at offset {} cannot land; a pass writes it",
                 m_BufferName, size, offset );

        // Asked before anything is written: the persistent path copies into its CPU shadow with memcpy,
        // which does not refuse a null source the way MappedMemory::Write does (Г7-C).
        if ( data == nullptr && size != 0 )
            return Common::MakeFormattedError<bool>( "storage buffer '{}' was given {} byte(s) from a null "
                                                     "source",
                                                     m_BufferName, size );

        if ( const auto room = MakeRoom( size, offset ); !room.IsSuccess() )
            return room;

        if ( !IsPersistent() )
        {
            // NO LOG ON THIS PATH: it runs per frame per object; StorageBufferProperty names the refusal.
            const auto wrote = m_PerView->Write(
                 data, size, offset, EngineContext::GetInstance().GetCurrentFrameIndex(),
                 [this]( std::string_view view, uint32_t frame, std::unique_ptr<IBlockCopy>& made )
                 { return MakeCopy( view, frame, made ); } );
            if ( !wrote.IsSuccess() )
                return Common::MakeFormattedError<bool>( "storage buffer '{}': {}", m_BufferName,
                                                         wrote.GetError() );
            return BOOLSUCCESS;
        }

        // RDG-PARALLEL-6: the shadow and the mapped bytes as one unit (m_SharedLock).
        std::scoped_lock lock( m_SharedLock );
        if ( size != 0 )
            std::memcpy( m_SharedImage.data() + offset, data, size );
        const auto wrote = m_Shared->Write( data, size, offset );
        if ( !wrote.IsSuccess() )
            return Common::MakeFormattedError<bool>( "persistent storage buffer '{}': {}", m_BufferName,
                                                     wrote.GetError() );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanStorageBuffer::EnsureMapped()
    {
        // "Can a write land now?" — for a per-frame buffer, "does the active view have a mapped copy for
        // this frame, or can one be made". A copy that exists is mapped (MakeMappedBufferCopy refuses an
        // unmapped one), so there is no second state to check. A device-only buffer has no mapping at all.
        if ( IsDeviceOnly() )
            return Common::MakeFormattedError<bool>(
                 "storage buffer '{}' is device-local and GPU-authored: it has no host mapping", m_BufferName );
        MappedBufferCopy* copy = nullptr;
        return ActiveCopy( EngineContext::GetInstance().GetCurrentFrameIndex(), copy );
    }

    Common::BoolResultStr VulkanStorageBuffer::ReadBack( void* destination, uint32_t size, uint32_t offset )
    {
        if ( IsPersistent() )
            return Common::MakeFormattedError<bool>(
                 "storage buffer '{}' is persistent: a frame still in flight "
                 "may be writing its one device buffer, so it is not read back",
                 m_BufferName );
        MappedBufferCopy* copy     = nullptr;
        const auto        resolved = ActiveCopy( EngineContext::GetInstance().GetCurrentFrameIndex(), copy );
        if ( !resolved.IsSuccess() )
            return resolved;
        if ( const auto read = copy->Read( destination, size, offset ); !read.IsSuccess() )
            return Common::MakeFormattedError<bool>( "storage buffer '{}': {}", m_BufferName, read.GetError() );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanStorageBuffer::BindActiveCopy( uint32_t frameIndex, ViewCopyBinding& out )
    {
        MappedBufferCopy* copy     = nullptr;
        auto              resolved = ActiveCopy( frameIndex, copy );
        if ( !resolved.IsSuccess() )
            return resolved;
        out = copy->Binding();
        return BOOLSUCCESS;
    }
    Common::ResultStr<uint64_t> VulkanStorageBuffer::GetDeviceAddress()
    {
        // CPU-GRASS-VARIETY-1: the copy's address, read once when the copy was made (MappedBufferCopy::
        // DeviceAddress): no driver call and no device cast per question. The device's row is asked only on
        // the refusal, to name why.
        MappedBufferCopy* copy     = nullptr;
        const auto        resolved = ActiveCopy( EngineContext::GetInstance().GetCurrentFrameIndex(), copy );
        if ( !resolved.IsSuccess() )
            return Common::MakeFormattedError<uint64_t>( "{}", resolved.GetError() );
        const uint64_t address = copy->DeviceAddress();
        if ( address == 0 && !HasDeviceAddress() )
            return Common::MakeFormattedError<uint64_t>(
                 "storage buffer '{}' has no device address: the device has no bufferDeviceAddress",
                 m_BufferName );
        if ( address == 0 )
            return Common::MakeFormattedError<uint64_t>( "storage buffer '{}': its buffer has no device address",
                                                         m_BufferName );
#if DESERT_DEV_INSTRUMENTS
        // FPS-DEVLOST17: an address fetched inside a graph pass's exec is a read of that range by the pass.
        CheckExecutingPassAddress( copy->Binding().Info );
#endif
        return Common::MakeSuccess( address );
    }

    VulkanStorageBuffer::GraphRecord* VulkanStorageBuffer::FindRecord( const uint64_t copyId ) noexcept
    {
        if ( copyId == 0 )
            return nullptr;
        for ( GraphRecord& record : m_GraphRecords )
        {
            if ( record.CopyId == copyId )
                return &record;
        }
        return nullptr;
    }

    VulkanStorageBuffer::GraphRecord& VulkanStorageBuffer::RecordForCopy( const uint64_t copyId ) noexcept
    {
        GraphRecord* record = FindRecord( copyId );
        if ( record == nullptr )
        {
            // The least recently imported slot (an empty one has LastUse 0, so it goes first).
            record = &m_GraphRecords[0];
            for ( GraphRecord& candidate : m_GraphRecords )
            {
                if ( candidate.LastUse < record->LastUse )
                    record = &candidate;
            }
            record->CopyId = copyId;
            record->Physical.reset();
            record->State = Desert::Graphic::RDG::AccessState{};
        }
        record->LastUse = ++m_GraphRecordClock;
        return *record;
    }
} // namespace Desert::ShaderResources::API::Vulkan
