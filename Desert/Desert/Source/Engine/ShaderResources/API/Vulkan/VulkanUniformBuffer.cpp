#include "VulkanUniformBuffer.hpp"

#include <Engine/Core/EngineContext.hpp>

namespace Desert::ShaderResources::API::Vulkan
{
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
        std::unique_ptr<MappedBufferCopy> copy;
        const auto                        made = MakeMappedBufferCopy(
             std::format( "{}-UniformBuffer-{}-Frame{}", m_UniformModel.Name, viewName, frameIndex ),
             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, m_UniformModel.Size, copy );
        if ( !made.IsSuccess() )
            return Common::MakeFormattedError<bool>( "uniform buffer '{}' copy for view '{}', frame {}: {}",
                                                     m_UniformModel.Name, viewName, frameIndex, made.GetError() );
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
        auto        resolved = ActiveCopy( frameIndex, copy );
        if ( !resolved.IsSuccess() )
            return resolved;

        // Only MakeCopy creates copies under this block's key, and it only creates MappedBufferCopy.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        out = static_cast<const MappedBufferCopy*>( copy )->Binding();
        return BOOLSUCCESS;
    }
} // namespace Desert::ShaderResources::API::Vulkan
