#pragma once

#include <Engine/Graphic/VertexBufferLayout.hpp>
#include <Engine/Graphic/RendererTypes.hpp>
#include <Engine/Graphic/DynamicResources.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>

// For NO_DISCARD below. Not implicit: this header is reached by translation units that never include
// Core.hpp on their own.
#include <Common/Core/Core.hpp>

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Desert::Graphic
{
    class VertexBuffer : public DynamicResources
    {
    public:
        // The ledger row, opened here so that every backend's vertex buffer is counted without any of them
        // remembering to. See Engine/Graphic/ResourceLedger.hpp.
        VertexBuffer() : m_Accounting( ResourceOwnership::Take( ResourceKind::VertexBuffer ) )
        {
        }

        virtual ~VertexBuffer() = default;

        /// Overwrite @p size bytes at @p offset. Refuses, having written nothing, when the buffer is not
        /// dynamic, when its memory is not mapped, or when the range does not fit.
        ///
        /// IT ANSWERS, AND THAT IS THE WHOLE POINT OF THIS SIGNATURE. It was `void` until Г13, which
        /// meant the refusal Г7-C had just made honest one layer down (Graphic/MappedMemory.hpp) could
        /// only ever reach a LOG_ERROR: the frame was then drawn from vertices that are not there and
        /// nothing above had anything to ask. A contract guarded by "somebody will read the log" is not
        /// guarded. NO_DISCARD because a caller who wants to ignore it must say so in writing.
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

        // The device address of this buffer's first byte, for a shader that reads it through a buffer reference
        // (the emissive light list's Build pass, a BLAS build input). 0: none - a device without
        // bufferDeviceAddress (the address table is filled only with the acceleration-structure row, so a device
        // with no hardware ray tracing has no addresses), or the buffer is not resident. Read when used, never
        // cached across frames: a pool arena's VkBuffer changes as it grows (VulkanGeometryAddress.hpp).
        [[nodiscard]] virtual uint64_t GetDeviceAddress() const
        {
            return 0;
        }

        [[nodiscard]] virtual unsigned int GetSize() const = 0;

        [[nodiscard]] virtual Common::BoolResultStr RT_Invalidate() = 0;

        // A Pooled buffer's range write (blocking one-off copy, like RT_Invalidate's upload). Refused by every
        // other usage. A Pooled buffer never grows: the GeometryPool adds a page instead.
        [[nodiscard]] virtual Common::BoolResultStr RT_WriteRange( uint64_t /*offset*/, const void* /*data*/,
                                                                   uint64_t /*size*/ )
        {
            return Common::MakeError<bool>( "RT_WriteRange: not a Pooled buffer" );
        }

        // @p file / @p line name the ledger row after the line that asked for the buffer: a vertex buffer has no
        // name of its own, and a row a closing view leaves behind has to say WHOSE it is (EDITOR-LOG-CLEAN).
        // __builtin_FILE/__builtin_LINE rather than std::source_location::current(): Apple clang 15 evaluates
        // the consteval current() in a default argument at the DECLARATION (every row read
        // "VertexBuffer.hpp:237"); the builtins are evaluated at the call site on clang, gcc and MSVC.
        static std::shared_ptr<VertexBuffer> Create( const void* data, uint32_t size,
                                                     BufferUsage usage = BufferUsage::Static,
                                                     const char* file  = __builtin_FILE(),
                                                     uint32_t    line  = __builtin_LINE() );
        static std::shared_ptr<VertexBuffer> Create( uint32_t size, BufferUsage usage = BufferUsage::Dynamic,
                                                     const char* file = __builtin_FILE(),
                                                     uint32_t    line = __builtin_LINE() );

    private:
        static std::shared_ptr<VertexBuffer> NamedAfter( std::shared_ptr<VertexBuffer> buffer, const char* file,
                                                         uint32_t line );

        ResourceOwnership m_Accounting;
    };
} // namespace Desert::Graphic
