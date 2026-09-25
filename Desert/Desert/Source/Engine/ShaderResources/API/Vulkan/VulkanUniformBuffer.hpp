#pragma once

#include <Engine/ShaderResources/UniformBuffer.hpp>
#include <Engine/ShaderResources/ViewCopiedBlock.hpp>
#include <Common/Core/Memory/Buffer.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>

namespace Desert::ShaderResources::API::Vulkan
{
    class VulkanUniformBuffer : public UniformBuffer
    {
    public:
        VulkanUniformBuffer( const ShaderLayout::UniformBuffer& uniform );
        virtual ~VulkanUniformBuffer() = default;

        NO_DISCARD virtual Common::BoolResultStr SetData( const void* data, uint32_t size,
                                                          uint32_t offset = 0 ) override;

        NO_DISCARD virtual Common::BoolResultStr EnsureMapped() override;

        // What a descriptor write for (@p frameIndex x ACTIVE VIEW) needs: the buffer, and the id of the
        // copy it belongs to, which the material backend compares with what the set was last written with
        // (ViewCopiedBlock.hpp, DescriptorCopyRecord). The view is resolved here rather than passed in for
        // the reason the slot used to be: every caller wants the copy of the view that is recording.
        struct ViewCopyBinding
        {
            VkDescriptorBufferInfo Info{};
            uint64_t               CopyId = 0;
        };
        NO_DISCARD Common::BoolResultStr BindActiveCopy( uint32_t frameIndex, ViewCopyBinding& out );

        virtual const void* GetData() const override
        {
            return nullptr;
        }

    private:
        /// Checks the block can be built and drops every copy of the previous one. Copies themselves are
        /// made lazily, per (view x frame in flight), the first time a view writes or binds this buffer —
        /// so an allocation or mapping refusal surfaces at that write or bind, with its cause.
        NO_DISCARD Common::BoolResultStr RT_Invalidate();

        // Allocates and maps one copy. Refuses as a whole: a copy that did not allocate or did not map is
        // never handed to a view (the history of the `continue` this replaced is in the .cpp).
        NO_DISCARD Common::BoolResultStr MakeCopy( std::string_view viewName, uint32_t frameIndex,
                                                   std::unique_ptr<IBlockCopy>& out ) const;

        NO_DISCARD Common::BoolResultStr ActiveCopy( uint32_t frameIndex, IBlockCopy*& out );

    private:
        ViewCopiedBlock m_Block;

        // WHY THE REFUSAL IS STORED. The constructor is the only caller of RT_Invalidate and a
        // constructor has no channel, so without this the answer would be produced and dropped one line
        // after it was introduced. Kept, so every later question — SetData, EnsureMapped — answers with
        // the CAUSE rather than with a true but useless consequence.
        Common::BoolResultStr m_Built = Common::MakeError<bool>( "uniform buffer has not been built" );
    };
} // namespace Desert::ShaderResources::API::Vulkan
