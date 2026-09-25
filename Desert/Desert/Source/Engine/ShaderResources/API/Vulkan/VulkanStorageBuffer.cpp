#include <Engine/ShaderResources/API/Vulkan/VulkanStorageBuffer.hpp>

#include <Engine/Core/EngineContext.hpp>

#include <cstring>

namespace Desert::ShaderResources::API::Vulkan
{
    namespace
    {
        // STORAGE ONLY. Every storage buffer also carried INDIRECT until Г25, granted for ONE caller: the
        // grass cull compute wrote a VkDrawIndirectCommand into a storage buffer and the grass draw read it
        // back through vkCmdDrawIndirect. That generator is gone and no draw in this engine is indirect any
        // more, so the capability is granted to nobody - and a usage bit nothing uses is a claim about this
        // buffer that is not true.
        constexpr VkBufferUsageFlags kUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    } // namespace

    VulkanStorageBuffer::VulkanStorageBuffer( const std::string_view bufferName, uint32_t size, uint32_t binding,
                                              bool persistent )
         : m_Lifetime( persistent ? Persistence::AcrossFrames : Persistence::PerFrame ), m_Binding( binding ),
           m_BufferName( bufferName )
    {
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

        m_SharedImage.assign( size, 0 );
        const auto made = MakeMappedBufferCopy( std::format( "{}-StorageBuffer-Persistent", m_BufferName ), kUsage,
                                                size, m_Shared );
        // A constructor has no channel, so the answer is kept and handed to every later question.
        m_Built = made.IsSuccess() ? Common::MakeSuccess( true )
                                   : Common::MakeFormattedError<bool>( "persistent storage buffer '{}': {}",
                                                                       m_BufferName, made.GetError() );
        if ( !m_Built.IsSuccess() )
            LOG_ERROR( "[StorageBuffer] '{}' was not built: {}", m_BufferName, m_Built.GetError() );
    }

    uint32_t VulkanStorageBuffer::GetSize() const
    {
        return m_PerView ? m_PerView->GetSize() : static_cast<uint32_t>( m_SharedImage.size() );
    }

    const void* VulkanStorageBuffer::GetData() const
    {
        return m_PerView ? static_cast<const void*>( m_PerView->GetContents() )
                         : static_cast<const void*>( m_SharedImage.data() );
    }

    Common::BoolResultStr VulkanStorageBuffer::MakeCopy( std::string_view viewName, uint32_t frameIndex,
                                                         std::unique_ptr<IBlockCopy>& out ) const
    {
        std::unique_ptr<MappedBufferCopy> copy;
        const auto                        made = MakeMappedBufferCopy(
             std::format( "{}-StorageBuffer-{}-Frame{}", m_BufferName, viewName, frameIndex ), kUsage, GetSize(),
             copy );
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

    Common::BoolResultStr VulkanStorageBuffer::SetData( const void* data, uint32_t size, uint32_t offset )
    {
        if ( !m_Built.IsSuccess() )
            return Common::MakeFormattedError<bool>( "storage buffer '{}' is not usable: {}", m_BufferName,
                                                     m_Built.GetError() );

        // Asked before anything is written: the persistent path copies into its CPU shadow with memcpy,
        // which does not refuse a null source the way MappedMemory::Write does (Г7-C).
        if ( data == nullptr && size != 0 )
            return Common::MakeFormattedError<bool>( "storage buffer '{}' was given {} byte(s) from a null "
                                                     "source",
                                                     m_BufferName, size );

        // GROWING IS NOT WRITING. For a PERSISTENT buffer a resize is the GPU simulation state gone, so the
        // decision has a name and lives in one pure function a test can reach without a device.
        switch ( ClassifyBufferWrite( GetSize(), size, offset, m_Lifetime ) )
        {
            case BufferWriteVerdict::Fits:
                break;

            case BufferWriteVerdict::Grow:
                // Per-frame only (ClassifyBufferWrite never grows a persistent buffer). Nothing is
                // allocated here: every view's copy is dropped and re-made at the new size by the write
                // below or by the view's next bind, seeded from the CPU image, which keeps its contents.
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
        // unmapped one), so there is no second state to check.
        MappedBufferCopy* copy = nullptr;
        return ActiveCopy( EngineContext::GetInstance().GetCurrentFrameIndex(), copy );
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
} // namespace Desert::ShaderResources::API::Vulkan
