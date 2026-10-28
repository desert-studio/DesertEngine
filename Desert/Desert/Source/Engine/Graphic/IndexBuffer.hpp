#pragma once

#include <Engine/Graphic/RendererTypes.hpp>
#include <Engine/Graphic/DynamicResources.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>

// For NO_DISCARD on SetData below. Not implicit: this header is reached from Geometry/Mesh.hpp by
// translation units that never include Core.hpp on their own — the same reason VertexBuffer.hpp
// includes it explicitly.
#include <Common/Core/Core.hpp>

namespace Desert::Graphic
{
    class IndexBuffer : public DynamicResources
    {
    public:
        // The ledger row — see Engine/Graphic/ResourceLedger.hpp and the note on VertexBuffer.
        IndexBuffer() : m_Accounting( ResourceOwnership::Take( ResourceKind::IndexBuffer ) )
        {
        }

        virtual ~IndexBuffer() = default;

        /// Overwrite @p size bytes at @p offset. Refuses, having written nothing, when the buffer is not
        /// dynamic, when its memory is not mapped, or when the range does not fit. See
        /// VertexBuffer::SetData for why this answers instead of returning `void`.
        NO_DISCARD virtual Common::BoolResultStr SetData( void* data, uint32_t size, uint32_t offset = 0 ) = 0;

        void ClaimOwnership( const ResourceOwner owner, const Common::AssetHandle asset = Common::AssetHandle{} )
        {
            m_Accounting.Claim( owner, asset );
        }

        /// What this buffer costs on the device. Recorded by whoever knows — the base cannot ask
        /// GetSize() from its own constructor, the backend has not allocated yet at that point.
        void RecordDeviceBytes( const std::size_t bytes )
        {
            m_Accounting.RecordBytes( bytes );
        }

        virtual unsigned int GetSize() const  = 0;
        virtual unsigned int GetCount() const = 0;

        [[nodiscard]] virtual Common::BoolResultStr RT_Invalidate() = 0;

        // A Pooled buffer's range write (blocking one-off copy, like RT_Invalidate's upload) and in-place growth
        // (a new device buffer of @p newSize bytes holding the old contents; the old one retired on the frame
        // rule). Refused by every other usage.
        [[nodiscard]] virtual Common::BoolResultStr RT_WriteRange( uint64_t /*offset*/, const void* /*data*/,
                                                                   uint64_t /*size*/ )
        {
            return Common::MakeError<bool>( "RT_WriteRange: not a Pooled buffer" );
        }
        [[nodiscard]] virtual Common::BoolResultStr RT_Grow( uint32_t /*newSize*/ )
        {
            return Common::MakeError<bool>( "RT_Grow: not a Pooled buffer" );
        }

        static std::shared_ptr<IndexBuffer> Create( const void* data, uint32_t size,
                                                    BufferUsage usage = BufferUsage::Static );
        static std::shared_ptr<IndexBuffer> Create( uint32_t size, BufferUsage usage = BufferUsage::Dynamic );

    private:
        ResourceOwnership m_Accounting;
    };
} // namespace Desert::Graphic