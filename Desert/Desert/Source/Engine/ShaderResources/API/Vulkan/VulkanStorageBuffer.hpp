#pragma once

#include <Engine/ShaderResources/StorageBuffer.hpp>
#include <Engine/ShaderResources/BufferGrowth.hpp>
#include <Engine/ShaderResources/API/Vulkan/VulkanMappedBufferCopy.hpp>

#include <memory>
#include <vector>

namespace Desert::ShaderResources::API::Vulkan
{
    // A storage buffer in one of two lifetimes (StorageBuffer::Create):
    //
    //   PerFrame     — one host-mapped copy per (view x frame in flight), made lazily the first time a view
    //                  writes or binds the buffer, exactly like VulkanUniformBuffer (ViewCopiedBlock). The
    //                  frame dimension keeps the CPU from rewriting what the GPU is still reading; the view
    //                  dimension keeps a preview from overwriting the per-object array or the skinning pose
    //                  the viewport's draws reference.
    //   AcrossFrames — ONE device buffer shared by every frame and every view, made at construction,
    //                  because the GPU is the author of its contents (a simulation that has been running
    //                  must not restart in a second view). One buffer, so one descriptor.
    class VulkanStorageBuffer : public StorageBuffer
    {
    public:
        VulkanStorageBuffer( const std::string_view bufferName, uint32_t size, uint32_t binding,
                             bool persistent = false );
        virtual ~VulkanStorageBuffer() = default;

        NO_DISCARD virtual Common::BoolResultStr EnsureMapped() override;

        NO_DISCARD virtual Common::BoolResultStr SetData( const void* data, uint32_t size,
                                                          uint32_t offset = 0 ) override;

        virtual uint32_t GetBinding() const override
        {
            return m_Binding;
        }

        virtual uint32_t GetSize() const override;

        // The descriptor for (@p frameIndex x ACTIVE VIEW), with the id of the copy it points at so a set
        // written for a copy that has since been dropped is rewritten (DescriptorCopyRecord). The view is
        // resolved here, so a write and the descriptor that points at it cannot disagree about it. A
        // persistent buffer answers with its one buffer for every frame and view.
        NO_DISCARD Common::BoolResultStr BindActiveCopy( uint32_t frameIndex, ViewCopyBinding& out );

        virtual const void* GetData() const override;

    private:
        bool IsPersistent() const
        {
            return m_Lifetime == Persistence::AcrossFrames;
        }

        // Allocates and maps one per-view copy at the block's CURRENT size.
        NO_DISCARD Common::BoolResultStr MakeCopy( std::string_view viewName, uint32_t frameIndex,
                                                   std::unique_ptr<IBlockCopy>& out ) const;

        // The copy a write or descriptor for (@p frameIndex x active view) goes to: the shared buffer when
        // persistent, the active view's (lazily made) copy otherwise.
        NO_DISCARD Common::BoolResultStr ActiveCopy( uint32_t frameIndex, MappedBufferCopy*& out );

    private:
        /// This REPLACES the `bool m_Persistent` that used to sit here, rather than sitting beside it. The
        /// growth decision (ShaderResources::ClassifyBufferWrite) is written in this vocabulary, and a bool
        /// and an enum saying the same thing are two things obliged to agree with nothing checking it.
        const Persistence m_Lifetime;

        // PerFrame: the per-view copies and the CPU image they are seeded from. Null when persistent.
        std::unique_ptr<ViewCopiedBlock> m_PerView;

        // AcrossFrames: the one buffer, and the CPU shadow of what was written into it (GetData). Empty
        // when per-frame; m_SharedImage.size() is the persistent buffer's size.
        std::unique_ptr<MappedBufferCopy> m_Shared;
        std::vector<uint8_t>              m_SharedImage;

        // The outcome of building the persistent buffer. The constructor has no channel, and a buffer that
        // failed to build must answer every later question with the CAUSE — see VulkanUniformBuffer.
        // Always success for a per-frame buffer: its copies are made, and refused, at the write or bind.
        Common::BoolResultStr m_Built = Common::MakeError<bool>( "storage buffer has not been built" );

        const uint32_t    m_Binding;
        const std::string m_BufferName;
    };
} // namespace Desert::ShaderResources::API::Vulkan
