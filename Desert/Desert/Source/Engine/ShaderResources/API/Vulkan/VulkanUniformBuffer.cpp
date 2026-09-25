#include "VulkanUniformBuffer.hpp"

#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>

#include <Engine/Core/EngineContext.hpp>

namespace Desert::ShaderResources::API::Vulkan
{
    namespace
    {
        Desert::Graphic::API::Vulkan::VulkanAllocator* Allocator()
        {
            return SP_CAST( Desert::Graphic::API::Vulkan::VulkanContext,
                            EngineContext::GetInstance().GetRendererContext() )
                 ->GetVulkanAllocator()
                 .get();
        }

        // One view's copy for one frame in flight. Owns its buffer from the moment it is allocated, so a
        // copy that fails to map is released by the same destructor as one a view drops.
        class UniformCopy final : public IBlockCopy
        {
        public:
            UniformCopy( VkBuffer buffer, VmaAllocation allocation, VkDeviceSize size )
                 : Buffer( buffer ), Allocation( allocation ), Size( size )
            {
            }

            ~UniformCopy() override
            {
                // Unmap BEFORE the buffer is queued for destruction; RT_DestroyBuffer defers the release,
                // which is what makes dropping a copy legal at any point of a frame.
                Mapping.Unmap();
                Allocator()->RT_DestroyBuffer( Buffer, Allocation );
            }

            UniformCopy( const UniformCopy& )            = delete;
            UniformCopy& operator=( const UniformCopy& ) = delete;

            Common::BoolResultStr Write( const void* data, uint32_t size, uint32_t offset ) override
            {
                return Mapping.Write( data, size, offset );
            }

            VkBuffer                      Buffer;
            VmaAllocation                 Allocation;
            VkDeviceSize                  Size;
            Desert::Graphic::MappedMemory Mapping; // persistent for the copy's life; CPU_TO_GPU stays mappable
        };
    } // namespace

    VulkanUniformBuffer::VulkanUniformBuffer( const ShaderLayout::UniformBuffer& uniform )
         : UniformBuffer( uniform ), m_Block( uniform.Size )
    {
        // A constructor has no channel, so the answer is KEPT rather than logged and forgotten: every
        // SetData and EnsureMapped afterwards hands the caller this refusal, with its original reason.
        m_Built = RT_Invalidate();
        if ( !m_Built.IsSuccess() )
            LOG_ERROR( "[UniformBuffer] '{}' was not built: {}", m_UniformModel.Name, m_Built.GetError() );
    }

    Common::BoolResultStr VulkanUniformBuffer::RT_Invalidate()
    {
        // No eager "frames x renderer slots" matrix any more: a copy exists only for a (view, frame in
        // flight) that actually wrote or bound this buffer. What is left to decide up front is whether a
        // copy could be made at all — Vulkan refuses a zero-sized buffer, and saying so here keeps the
        // meaning m_Built always had.
        if ( m_UniformModel.Size == 0 )
            return Common::MakeFormattedError<bool>( "uniform buffer '{}' declares a zero-byte block",
                                                     m_UniformModel.Name );

        m_Block.Reset( m_UniformModel.Size );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanUniformBuffer::MakeCopy( std::string_view viewName, uint32_t frameIndex,
                                                         std::unique_ptr<IBlockCopy>& out ) const
    {
        VkBufferCreateInfo bufferInfo = {};
        bufferInfo.sType              = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.usage              = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bufferInfo.size               = m_UniformModel.Size;

        auto*    allocator = Allocator();
        VkBuffer buffer    = VK_NULL_HANDLE;

        const auto allocatedBuffer = allocator->RT_AllocateBuffer(
             std::format( "{}-UniformBuffer-{}-Frame{}", m_UniformModel.Name, viewName, frameIndex ), bufferInfo,
             VMA_MEMORY_USAGE_CPU_TO_GPU, buffer );

        // REFUSED, NOT SKIPPED. The eager loop this replaced once answered a failed allocation with a
        // bare `continue`, leaving a VK_NULL_HANDLE copy and a zeroed VkDescriptorBufferInfo that was
        // then written into a set — undefined behaviour in the driver, visible only as one frame in
        // flight rendering wrong. A copy that did not allocate never reaches a view now.
        if ( !allocatedBuffer.IsSuccess() )
            return Common::MakeFormattedError<bool>( "uniform buffer '{}' copy for view '{}', frame {}: {}",
                                                     m_UniformModel.Name, viewName, frameIndex,
                                                     allocatedBuffer.GetError() );

        auto copy     = std::make_unique<UniformCopy>( buffer, allocatedBuffer.GetValue(), m_UniformModel.Size );
        copy->Mapping = allocator->MapMemory( copy->Allocation );

        // A COPY THAT DID NOT MAP IS THE SAME FAILURE AS ONE THAT DID NOT ALLOCATE: every write to it
        // would refuse for its whole life. Refused here, and the copy's destructor gives the buffer back.
        if ( !copy->Mapping.IsMapped() )
            return Common::MakeFormattedError<bool>( "uniform buffer '{}' copy for view '{}', frame {}: {}",
                                                     m_UniformModel.Name, viewName, frameIndex,
                                                     copy->Mapping.GetRefusal() );

        out = std::move( copy );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanUniformBuffer::ActiveCopy( uint32_t frameIndex, IBlockCopy*& out )
    {
        // THE CAUSE, NOT THE CONSEQUENCE: a buffer that failed to build answers with the constructor's
        // own reason.
        if ( !m_Built.IsSuccess() )
            return Common::MakeFormattedError<bool>( "uniform buffer '{}' was never built: {}",
                                                     m_UniformModel.Name, m_Built.GetError() );

        const auto resolved = m_Block.Resolve(
             frameIndex, [this]( std::string_view view, uint32_t frame, std::unique_ptr<IBlockCopy>& made )
             { return MakeCopy( view, frame, made ); }, out );
        if ( !resolved.IsSuccess() )
            return Common::MakeFormattedError<bool>( "uniform buffer '{}': {}", m_UniformModel.Name,
                                                     resolved.GetError() );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanUniformBuffer::SetData( const void* data, uint32_t size, uint32_t offset )
    {
        if ( !m_Built.IsSuccess() )
            return Common::MakeFormattedError<bool>( "uniform buffer '{}' was never built: {}",
                                                     m_UniformModel.Name, m_Built.GetError() );

        // NO LOG ON THIS PATH: it runs per frame per material; the caller receives the refusal and
        // decides once, where it knows what the write was for.
        const auto wrote =
             m_Block.Write( data, size, offset, EngineContext::GetInstance().GetCurrentFrameIndex(),
                            [this]( std::string_view view, uint32_t frame, std::unique_ptr<IBlockCopy>& made )
                            { return MakeCopy( view, frame, made ); } );
        if ( !wrote.IsSuccess() )
            return Common::MakeFormattedError<bool>( "uniform buffer '{}': {}", m_UniformModel.Name,
                                                     wrote.GetError() );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanUniformBuffer::EnsureMapped()
    {
        // "Can a write land now?" — which, with lazy copies, means "does the active view have a mapped
        // copy for this frame, or can one be made". MakeCopy refuses an unmapped copy, so a copy that
        // exists is mapped; there is no second state to check for.
        IBlockCopy* copy = nullptr;
        return ActiveCopy( EngineContext::GetInstance().GetCurrentFrameIndex(), copy );
    }

    Common::BoolResultStr VulkanUniformBuffer::BindActiveCopy( uint32_t frameIndex, ViewCopyBinding& out )
    {
        IBlockCopy* copy     = nullptr;
        const auto  resolved = ActiveCopy( frameIndex, copy );
        if ( !resolved.IsSuccess() )
            return resolved;

        // Only MakeCopy creates copies under this block's key, and it only creates UniformCopy.
        const auto* uniform = static_cast<const UniformCopy*>( copy );
        out.Info.buffer     = uniform->Buffer;
        out.Info.offset     = 0;
        out.Info.range      = uniform->Size;
        out.CopyId          = uniform->GetId();
        return BOOLSUCCESS;
    }
} // namespace Desert::ShaderResources::API::Vulkan
